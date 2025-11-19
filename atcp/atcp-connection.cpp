/*
    This file is part of TON Blockchain Library.

    TON Blockchain Library is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    TON Blockchain Library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with TON Blockchain Library.  If not, see <http://www.gnu.org/licenses/>.
*/
#include "auto/tl/ton_api.hpp"
#include "td/actor/coro_utils.h"
#include "td/utils/Random.h"
#include "td/utils/as.h"
#include "td/utils/overloaded.h"

#include "atcp-connection.hpp"
#include "atcp.hpp"
#include "delay.h"

namespace ton::atcp {

void AtcpConnection::start_up() {
  run().start().detach();
}

void AtcpConnection::tear_down() {
  for (auto& [_, query] : out_queries_) {
    query.promise.set_error(td::Status::Error("atcp connection closed"));
  }
  if (inited_) {
    td::actor::send_closure(atcp_, &Atcp::close_connection, connection_id_, local_id_, peer_id_);
  }
}

td::actor::Task<> AtcpConnection::run() {
  auto res = co_await run_inner().wrap();
  if (res.is_ok()) {
    abort(td::Status::Error("finished"));
  } else {
    abort(res.move_as_error());
  }
  co_return td::Unit{};
}

td::actor::Task<> AtcpConnection::run_inner() {
  if (!inbound_) {
    pipe_ = td::make_socket_pipe(co_await td::SocketFd::open(outbound_ip_));
  }
  pipe_.subscribe();

  if (inbound_) {
    td::BufferSlice init_message = co_await read_message();
    auto init = co_await fetch_tl_object<ton_api::atcp_init>(init_message, true);
    peer_id_full_ = co_await adnl::AdnlNodeIdFull::create(init->local_id_);
    peer_id_ = peer_id_full_.compute_short_id();
    local_id_ = adnl::AdnlNodeIdShort{std::move(init->peer_id_)};
    local_id_full_ = co_await td::actor::ask(atcp_, &Atcp::get_local_id_full, local_id_);
    send_message_internal(create_serialize_tl_object<ton_api::atcp_init>(local_id_full_.tl(), peer_id_.tl()));
    LOG(INFO) << "New inbound connection : " << peer_id_ << " -> " << local_id_;
  } else {
    send_message_internal(create_serialize_tl_object<ton_api::atcp_init>(local_id_full_.tl(), peer_id_.tl()));
    td::BufferSlice init_message = co_await read_message();
    auto init = co_await fetch_tl_object<ton_api::atcp_init>(init_message, true);
    peer_id_full_ = co_await adnl::AdnlNodeIdFull::create(init->local_id_);
    if (peer_id_full_.compute_short_id() != peer_id_) {
      co_return td::Status::Error("peer_id mismatch in init packet");
    }
    if (local_id_.bits256_value() != init->peer_id_->id_) {
      co_return td::Status::Error("local_id mismatch in init packet");
    }
    LOG(INFO) << "New outbound connection : " << local_id_ << " -> " << peer_id_;
  }
  init_promise_.set_value({peer_id_full_, local_id_});
  inited_ = true;

  while (true) {
    auto data = co_await read_message();
    auto res = fetch_tl_object<ton_api::atcp_Message>(data, true);
    if (res.is_error()) {
      LOG(DEBUG) << "Receive : " << res.error() << ", size=" << data.size();
      continue;
    }
    auto f = res.move_as_ok();
    ton_api::downcast_call(
        *f, td::overloaded(
                [&](ton_api::atcp_customMessage& message) {
                  LOG(DEBUG) << "Received message, size=" << message.data_.size();
                  td::actor::send_closure(adnl_, &adnl::AdnlPeerTable::deliver, peer_id_, local_id_,
                                          std::move(message.data_));
                },
                [&](ton_api::atcp_query& query) {
                  LOG(DEBUG) << "Received query, size=" << query.data_.size();
                  td::actor::send_closure(
                      adnl_, &adnl::AdnlPeerTable::deliver_query, peer_id_, local_id_, std::move(query.data_),
                      [SelfId = actor_id(this), query_id = query.query_id_](td::Result<td::BufferSlice> R) {
                        if (R.is_error()) {
                          LOG(DEBUG) << "Query error: " << R.error();
                        } else {
                          td::actor::send_closure(SelfId, &AtcpConnection::send_query_answer, query_id, R.move_as_ok());
                        }
                      });
                },
                [&](ton_api::atcp_answer& answer) {
                  auto it = out_queries_.find(answer.query_id_);
                  if (it == out_queries_.end()) {
                    LOG(DEBUG) << "Received answer to unknown query, size=" << answer.data_.size();
                  } else {
                    LOG(DEBUG) << "Received answer to \"" << it->second.name << "\", size=" << answer.data_.size();
                    it->second.promise.set_value(std::move(answer.data_));
                    out_queries_.erase(it);
                  }
                }));
  }
}

td::actor::Task<td::BufferSlice> AtcpConnection::read_message() {
  td::BufferSlice size_str = co_await read_bytes(4);
  td::uint32 size = td::as<td::uint32>(size_str.data());
  co_return co_await read_bytes(size);
}

td::actor::Task<td::BufferSlice> AtcpConnection::read_bytes(size_t size) {
  co_await pipe_.flush_read();
  while (pipe_.left_unread() < size) {
    CHECK(!fd_read_waiter_);
    auto [task, promise] = td::actor::StartedTask<>::make_bridge();
    fd_read_waiter_ = std::move(promise);
    fd_read_waiter_size_ = size;
    co_await std::move(task);
  }
  td::BufferSlice result{size};
  size_t read = pipe_.input_buffer().advance(size, result.as_slice());
  CHECK(read == size);
  co_return result;
}

void AtcpConnection::send_message(td::BufferSlice data) {
  LOG(DEBUG) << "Send message, size=" << data.size();
  send_message_internal(create_serialize_tl_object<ton_api::atcp_customMessage>(std::move(data)));
}

void AtcpConnection::send_query(std::string name, td::Promise<td::BufferSlice> promise, td::Timestamp timeout,
                                td::BufferSlice data, td::uint64 max_answer_size) {
  LOG(DEBUG) << "Send query \"" << name << "\", size=" << data.size();
  td::Bits256 query_id;
  td::Random::secure_bytes(query_id.as_slice());
  out_queries_[query_id] = {.name = std::move(name), .promise = std::move(promise)};
  send_message_internal(create_serialize_tl_object<ton_api::atcp_query>(query_id, std::move(data)));
  if (timeout) {
    delay_action([SelfId = actor_id(this),
                  query_id]() { td::actor::send_closure(SelfId, &AtcpConnection::on_query_timeout, query_id); },
                 timeout);
  }
}

void AtcpConnection::on_query_timeout(td::Bits256 query_id) {
  auto it = out_queries_.find(query_id);
  if (it != out_queries_.end()) {
    LOG(DEBUG) << "Query \"" << it->second.name << "\" timeout";
    it->second.promise.set_error(td::Status::Error(timeout, "timeout"));
    out_queries_.erase(it);
  }
}

void AtcpConnection::send_query_answer(td::Bits256 query_id, td::BufferSlice data) {
  LOG(DEBUG) << "Send query answer, size=" << data.size();
  send_message_internal(create_serialize_tl_object<ton_api::atcp_answer>(query_id, std::move(data)));
}

void AtcpConnection::send_message_internal(td::BufferSlice data) {
  td::uint32 size = td::narrow_cast<td::uint32>(data.size());
  td::BufferSlice str{4 + data.size()};
  td::MutableSlice s = str.as_slice();
  s.copy_from(td::Slice(reinterpret_cast<const td::uint8*>(&size), 4));
  s.remove_prefix(4);
  s.copy_from(data.as_slice());
  pipe_.output_buffer().append(std::move(str));
  yield();  // run loop()
}

void AtcpConnection::loop() {
  auto S = pipe_.flush_read();
  if (S.is_error()) {
    abort(std::move(S));
    return;
  }
  S = pipe_.flush_write();
  if (S.is_error()) {
    abort(std::move(S));
    return;
  }
  if (fd_read_waiter_) {
    if (pipe_.left_unread() >= fd_read_waiter_size_) {
      fd_read_waiter_.set_value(td::Unit{});
    }
  }
}

void AtcpConnection::abort(td::Status S) {
  S.ensure_error();
  if (inited_) {
    LOG(INFO) << "Connection closed (l_id= << " << local_id_ << " p_id=" << peer_id_ << ") : " << S;
  } else {
    LOG(INFO) << "Connection closed (uninit) : " << S;
  }
  if (init_promise_) {
    init_promise_.set_error(std::move(S));
  }
  stop();
}

}  // namespace ton::atcp
