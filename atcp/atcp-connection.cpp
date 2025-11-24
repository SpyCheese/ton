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
#include "ton/ton-types.h"

#include "atcp-connection.hpp"
#include "atcp.hpp"
#include "delay.h"

namespace ton::atcp {

void AtcpConnection::start_up() {
  mtu_ = Atcp::DEFAULT_MTU;
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
  alarm_timestamp().relax(init_timeout_ = td::Timestamp::in(5.0));
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
  init_timeout_ = {};
  update_timeout();

  while (true) {
    auto data = co_await read_message();
    if (data.empty()) {
      continue;
    }
    auto res = fetch_tl_object<ton_api::atcp_Message>(data, true);
    if (res.is_error()) {
      LOG(DEBUG) << "Receive : " << res.error() << ", size=" << data.size();
      continue;
    }
    auto f = res.move_as_ok();
    ton_api::downcast_call(
        *f, td::overloaded(
                [&](ton_api::atcp_customMessage& message) {
                  if (message.data_.size() > mtu_) {
                    LOG(DEBUG) << "Dropping to big message, size=" << message.data_.size() << ", mtu=" << mtu_;
                    return;
                  }
                  LOG(DEBUG) << "Received message, size=" << message.data_.size();
                  td::actor::send_closure(adnl_, &adnl::AdnlPeerTable::deliver, peer_id_, local_id_,
                                          std::move(message.data_));
                },
                [&](ton_api::atcp_query& query) {
                  if (query.data_.size() > mtu_) {
                    LOG(DEBUG) << "Dropping to big query, size=" << query.data_.size() << ", mtu=" << mtu_;
                    return;
                  }
                  auto max_answer_size = (td::uint64)query.max_answer_size_;
                  auto timeout = (UnixTime)query.timeout_;
                  LOG(DEBUG) << "Received query, size=" << query.data_.size() << ", max_answer_size=" << max_answer_size
                             << ", timeout="
                             << (timeout ? td::to_string((double)timeout - td::Clocks::system()) : "none");
                  td::actor::send_closure(
                      adnl_, &adnl::AdnlPeerTable::deliver_query, peer_id_, local_id_, std::move(query.data_),
                      [=, SelfId = actor_id(this), query_id = query.query_id_](td::Result<td::BufferSlice> R) {
                        td::actor::send_closure(SelfId, &AtcpConnection::send_query_answer, query_id, max_answer_size,
                                                timeout, std::move(R));
                      });
                },
                [&](ton_api::atcp_answer& answer) {
                  finish_query(answer.query_id_, std::move(answer.data_));
                },
                [&](const ton_api::atcp_queryError& query_error) {
                  finish_query(query_error.query_id_, td::Status::Error("query rejected"));
                },
                [&](ton_api::atcp_nop&) { LOG(DEBUG) << "Received adnl.nop"; }));
    update_timeout();
  }
}

td::actor::Task<td::BufferSlice> AtcpConnection::read_message() {
  td::BufferSlice size_str = co_await read_bytes(8);
  td::uint64 size = td::as<td::uint64>(size_str.data());
  td::uint64 current_mtu = mtu_;
  if (!out_queries_max_answer_sizes_.empty()) {
    current_mtu = std::max(current_mtu, *out_queries_max_answer_sizes_.rbegin());
  }
  if (size >= MAX_MESSAGE_HEADER_SIZE && size - MAX_MESSAGE_HEADER_SIZE > current_mtu) {
    LOG(DEBUG) << "Dropping too long message : " << size << " > " << current_mtu + MAX_MESSAGE_HEADER_SIZE;
    co_await skip_bytes(size);
    co_return td::BufferSlice{};
  }
  co_return co_await read_bytes(size);
}

td::actor::Task<td::BufferSlice> AtcpConnection::read_bytes(size_t size) {
  co_await wait_read(size);
  td::BufferSlice result{size};
  size_t read = pipe_.input_buffer().advance(size, result.as_slice());
  CHECK(read == size);
  co_return result;
}

td::actor::Task<> AtcpConnection::skip_bytes(size_t size) {
  while (size > 0) {
    co_await wait_read(1);
    size_t x = std::min(size, pipe_.left_unread());
    pipe_.input_buffer().advance(x);
    size -= x;
  }
  co_return td::Unit{};
}

td::actor::Task<> AtcpConnection::wait_read(size_t size) {
  co_await pipe_.flush_read();
  while (pipe_.left_unread() < size) {
    CHECK(!fd_read_waiter_);
    auto [task, promise] = td::actor::StartedTask<>::make_bridge();
    fd_read_waiter_ = std::move(promise);
    fd_read_waiter_size_ = size;
    co_await std::move(task);
  }
  co_return td::Unit{};
}

void AtcpConnection::send_message(td::BufferSlice data) {
  LOG(DEBUG) << "Send message, size=" << data.size();
  send_message_internal(create_serialize_tl_object<ton_api::atcp_customMessage>(std::move(data)));
  update_timeout();
}

void AtcpConnection::send_query(std::string name, td::Promise<td::BufferSlice> promise, td::Timestamp timeout,
                                td::BufferSlice data, td::uint64 max_answer_size) {
  LOG(DEBUG) << "Send query \"" << name << "\", size=" << data.size();
  td::Bits256 query_id;
  td::Random::secure_bytes(query_id.as_slice());
  auto [_, inserted] = out_queries_.emplace(
      query_id, OutQuery{.name = std::move(name), .max_answer_size = max_answer_size, .promise = std::move(promise)});
  CHECK(inserted);
  out_queries_max_answer_sizes_.insert(max_answer_size);
  send_message_internal(create_serialize_tl_object<ton_api::atcp_query>(
      query_id, max_answer_size, timeout ? (int)timeout.at_unix() + 1 : 0, std::move(data)));
  if (timeout) {
    delay_action(
        [SelfId = actor_id(this), query_id]() {
          td::actor::send_closure(SelfId, &AtcpConnection::finish_query, query_id,
                                  td::Status::Error(ErrorCode::timeout, "timeout"));
        },
        timeout);
  }
  update_timeout();
}

void AtcpConnection::send_query_answer(td::Bits256 query_id, td::uint64 max_answer_size, UnixTime timeout,
                                       td::Result<td::BufferSlice> R) {
  if (timeout && td::Clocks::system() > (double)timeout) {
    LOG(DEBUG) << "Inbound query timeout";
    return;
  }
  if (R.is_error()) {
    LOG(DEBUG) << "Send query reject : " << R.move_as_error();
    send_message_internal(create_serialize_tl_object<ton_api::atcp_queryError>(query_id));
  } else if (R.ok().size() <= max_answer_size) {
    LOG(DEBUG) << "Send query answer, size=" << R.ok().size();
    send_message_internal(create_serialize_tl_object<ton_api::atcp_answer>(query_id, R.move_as_ok()));
  } else {
    LOG(DEBUG) << "Send query reject : answer too big (" << R.ok().size() << " > " << max_answer_size << ")";
    send_message_internal(create_serialize_tl_object<ton_api::atcp_queryError>(query_id));
  }
  update_timeout();
}

void AtcpConnection::finish_query(td::Bits256 query_id, td::Result<td::BufferSlice> R) {
  auto it = out_queries_.find(query_id);
  if (it == out_queries_.end()) {
    return;
  }
  if (R.is_error()) {
    LOG(DEBUG) << "Query \"" << it->second.name << "\" : " << R.error();
  } else if (R.ok().size() > it->second.max_answer_size) {
    LOG(DEBUG) << "Query \"" << it->second.name << "\" : answer too big (" << R.ok().size() << " > "
               << it->second.max_answer_size << ")";
    R = td::Status::Error("query answer is too big");
  } else {
    LOG(DEBUG) << "Query \"" << it->second.name << "\" : received answer, size=" << R.ok().size();
  }
  it->second.promise.set_result(std::move(R));
  out_queries_max_answer_sizes_.erase(out_queries_max_answer_sizes_.find(it->second.max_answer_size));
  out_queries_.erase(it);
}

void AtcpConnection::send_message_internal(td::BufferSlice data) {
  td::uint64 size = data.size();
  td::BufferSlice str{8 + data.size()};
  td::MutableSlice s = str.as_slice();
  s.copy_from(td::Slice(reinterpret_cast<const td::uint8*>(&size), 8));
  s.remove_prefix(8);
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

void AtcpConnection::alarm() {
  if (init_timeout_ && init_timeout_.is_in_past()) {
    abort(td::Status::Error(timeout, "connection init timeout"));
    return;
  }
  if (send_nop_at_) {
    LOG(DEBUG) << "Send atcp.nop";
    send_message_internal(create_serialize_tl_object<ton_api::atcp_nop>());
    send_nop_at_ = {};
    update_timeout();
  }
  if (prepare_close_at_ && prepare_close_at_.is_in_past()) {
    LOG(INFO) << "Closing connection by timeout in " << CLOSE_TIMEOUT << " seconds";
    prepare_close_at_ = {};
    closing_soon_ = true;
    close_at_ = td::Timestamp::in(CLOSE_TIMEOUT);
    td::actor::send_closure(atcp_, &Atcp::connection_status_changed, connection_id_, local_id_, peer_id_, true);
  }
  if (close_at_ && close_at_.is_in_past()) {
    close_at_ = {};
    abort(td::Status::Error(timeout, "connection timeout"));
    return;
  }
  alarm_timestamp().relax(init_timeout_);
  alarm_timestamp().relax(send_nop_at_);
  alarm_timestamp().relax(prepare_close_at_);
  alarm_timestamp().relax(close_at_);
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

void AtcpConnection::update_timeout() {
  if (closing_soon_) {
    LOG(INFO) << "Abort closing connection by timeout";
    close_at_ = {};
    closing_soon_ = false;
    td::actor::send_closure(atcp_, &Atcp::connection_status_changed, connection_id_, local_id_, peer_id_, false);
  }
  alarm_timestamp().relax(prepare_close_at_ = td::Timestamp::in(PREPARE_CLOSE_TIMEOUT));
  if (out_queries_.empty()) {
    send_nop_at_ = {};
  } else {
    send_nop_at_ = td::Timestamp::in(SEND_NOP_PERIOD);
  }
}

}  // namespace ton::atcp
