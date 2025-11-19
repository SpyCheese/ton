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
#include "td/actor/coro_utils.h"
#include "td/net/Pipe.h"

#include "atcp-connection.hpp"
#include "atcp.hpp"

namespace ton::atcp {

void Atcp::start_up() {
  CHECK(addr_.is_ipv4());
  CHECK(addr_.get_port() != 0);
  class Callback : public td::TcpListener::Callback {
   public:
    explicit Callback(td::actor::ActorId<Atcp> atcp) : atcp_(std::move(atcp)) {
    }

    void accept(td::SocketFd fd) override {
      td::actor::ask(atcp_, &Atcp::accept_connection, td::make_socket_pipe(std::move(fd))).detach();
    }

   private:
    td::actor::ActorId<Atcp> atcp_;
  };
  LOG(INFO) << "Starting ATCP server on port " << addr_.get_port();
  tcp_listener_ = td::actor::create_actor<td::TcpInfiniteListener>("TcpListener", addr_.get_port(),
                                                                   std::make_unique<Callback>(actor_id(this)));
}

void Atcp::add_id(adnl::AdnlNodeIdShort local_id) {
  if (!local_ids_.emplace(local_id, LocalId{}).second) {
    return;
  }

  class Callback : public adnl::Adnl::Callback {
   public:
    Callback(td::actor::ActorId<Atcp> atcp) : atcp_(std::move(atcp)) {
    }
    void receive_message(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::BufferSlice data) override {
    }
    void receive_query(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::BufferSlice data,
                       td::Promise<td::BufferSlice> promise) override {
      td::actor::send_closure(atcp_, &Atcp::receive_adnl_query, std::move(data), std::move(promise));
    }

   private:
    td::actor::ActorId<Atcp> atcp_;
  };
  td::actor::send_closure(adnl_, &adnl::Adnl::subscribe, local_id,
                          adnl::Adnl::int_to_bytestring(ton_api::atcp_getTcpAddr::ID),
                          std::make_unique<Callback>(actor_id(this)));
}

void Atcp::send_message(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::BufferSlice data) {
  get_connection(src, dst, [data = std::move(data)](td::Result<td::actor::ActorId<AtcpConnection>> R) mutable {
    if (R.is_error()) {
      LOG(DEBUG) << "Cannot get connection for send_message : " << R.error();
      return;
    }
    td::actor::send_closure(R.move_as_ok(), &AtcpConnection::send_message, std::move(data));
  });
}

void Atcp::send_query_ex(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, std::string name,
                         td::Promise<td::BufferSlice> promise, td::Timestamp timeout, td::BufferSlice data,
                         td::uint64 max_answer_size) {
  get_connection(src, dst,
                 [data = std::move(data), name = std::move(name), promise = std::move(promise), timeout,
                  max_answer_size](td::Result<td::actor::ActorId<AtcpConnection>> R) mutable {
                   if (R.is_error()) {
                     LOG(DEBUG) << "Cannot get connection for send_query \"" << name << "\" : " << R.error();
                     promise.set_error(R.move_as_error());
                     return;
                   }
                   td::actor::send_closure(R.move_as_ok(), &AtcpConnection::send_query, std::move(name),
                                           std::move(promise), timeout, std::move(data), max_answer_size);
                 });
}

td::actor::Task<> Atcp::accept_connection(td::SocketPipe pipe) {
  td::uint64 connection_id = next_connection_id_++;
  LOG(INFO) << "Received inbound connection #" << connection_id;
  auto [init_task, init_promise] =
      td::actor::StartedTask<std::pair<adnl::AdnlNodeIdFull, adnl::AdnlNodeIdShort>>::make_bridge();
  auto connection =
      td::actor::create_actor<AtcpConnection>(PSTRING() << "atcp-conn#" << connection_id, connection_id,
                                              std::move(pipe), actor_id(this), adnl_, std::move(init_promise));
  auto [peer_id_full, local_id] = co_await std::move(init_task);
  auto it = local_ids_.find(local_id);
  if (it == local_ids_.end()) {
    co_return td::Status::Error("no such local id");
  }
  adnl::AdnlNodeIdShort peer_id = peer_id_full.compute_short_id();
  PeerPair& peer_pair = it->second.peers[peer_id];
  auto res = connection.get();
  peer_pair.connections[connection_id] = std::move(connection);
  after_create_connection(local_id, peer_id, res);
  co_return td::Unit{};
}

void Atcp::get_connection(adnl::AdnlNodeIdShort local_id, adnl::AdnlNodeIdShort peer_id,
                          td::Promise<td::actor::ActorId<AtcpConnection>> promise) {
  auto it = local_ids_.find(local_id);
  if (it == local_ids_.end()) {
    promise.set_error(td::Status::Error("no such local id"));
    return;
  }
  PeerPair& peer_pair = it->second.peers[peer_id];
  if (!peer_pair.connections.empty()) {
    promise.set_value(peer_pair.connections.begin()->second.get());
    return;
  }
  peer_pair.connection_waiters.push_back(std::move(promise));
  if (!peer_pair.connection_pending) {
    peer_pair.connection_pending = true;
    td::actor::send_closure(
        actor_id(this), &Atcp::create_connection, local_id, peer_id,
        td::PromiseCreator::lambda([=, SelfId = actor_id(this)](td::Result<td::actor::ActorId<AtcpConnection>> R) {
          td::actor::send_closure(SelfId, &Atcp::after_create_connection, local_id, peer_id, std::move(R));
        }));
  }
}

td::actor::Task<td::actor::ActorId<AtcpConnection>> Atcp::create_connection(adnl::AdnlNodeIdShort local_id,
                                                                            adnl::AdnlNodeIdShort peer_id) {
  adnl::AdnlNodeIdFull local_id_full = (co_await td::actor::ask(adnl_, &adnl::Adnl::get_self_node, local_id)).pub_id();
  td::IPAddress peer_addr = local_ids_[local_id].peers[peer_id].ip;
  if (!peer_addr.is_valid()) {
    for (int attempt = 0;; ++attempt) {
      auto [task, promise] = td::actor::StartedTask<td::BufferSlice>::make_bridge();
      td::actor::send_closure(adnl_, &Adnl::send_query, local_id, peer_id, "getTcpAddr", std::move(promise),
                              td::Timestamp::in(1.0), create_serialize_tl_object<ton_api::atcp_getTcpAddr>());
      auto R = co_await std::move(task).wrap();
      if (R.is_error()) {
        LOG(DEBUG) << "getTcpAddr failed : " << R.error();
        if (attempt == 4) {
          co_return R.move_as_error_prefix("cannot get tcp address: ");
        }
      } else {
        auto f = co_await fetch_tl_object<ton_api::atcp_tcpAddr>(R.move_as_ok(), true);
        co_await peer_addr.init_ipv4_port(td::IPAddress::ipv4_to_str(f->ip_), f->port_);
        break;
      }
    }
    local_ids_[local_id].peers[peer_id].ip = peer_addr;
  }
  td::uint64 connection_id = next_connection_id_++;
  LOG(INFO) << "Trying to create connection #" << connection_id << " to " << peer_addr.get_ip_str() << ":"
            << peer_addr.get_port() << " (" << local_id << " -> " << peer_id << ")";
  auto [init_task, init_promise] =
      td::actor::StartedTask<std::pair<adnl::AdnlNodeIdFull, adnl::AdnlNodeIdShort>>::make_bridge();
  auto connection =
      td::actor::create_actor<AtcpConnection>(PSTRING() << "atcp-conn#" << connection_id, connection_id, local_id_full,
                                              peer_id, peer_addr, actor_id(this), adnl_, std::move(init_promise));
  auto [peer_id_full, _] = co_await std::move(init_task);
  auto it = local_ids_.find(local_id);
  if (it == local_ids_.end()) {
    co_return td::Status::Error("no such local id");
  }
  PeerPair& peer_pair = it->second.peers[peer_id];
  auto res = connection.get();
  peer_pair.connections[connection_id] = std::move(connection);
  co_return res;
}

void Atcp::after_create_connection(adnl::AdnlNodeIdShort local_id, adnl::AdnlNodeIdShort peer_id,
                                   td::Result<td::actor::ActorId<AtcpConnection>> R) {
  auto it = local_ids_.find(local_id);
  if (it == local_ids_.end()) {
    return;
  }
  auto it2 = it->second.peers.find(peer_id);
  if (it2 == it->second.peers.end()) {
    return;
  }
  PeerPair& peer_pair = it2->second;
  peer_pair.connection_pending = false;
  for (auto& promise : peer_pair.connection_waiters) {
    promise.set_result(R.clone());
  }
  peer_pair.connection_waiters.clear();
}

td::actor::Task<adnl::AdnlNodeIdFull> Atcp::get_local_id_full(adnl::AdnlNodeIdShort local_id) {
  if (!local_ids_.contains(local_id)) {
    co_return td::Status::Error("no such local id");
  }
  co_return (co_await td::actor::ask(adnl_, &adnl::Adnl::get_self_node, local_id)).pub_id();
}

void Atcp::close_connection(td::uint64 connection_id, adnl::AdnlNodeIdShort local_id, adnl::AdnlNodeIdShort peer_id) {
  auto it = local_ids_.find(local_id);
  if (it == local_ids_.end()) {
    return;
  }
  auto it2 = it->second.peers.find(peer_id);
  if (it2 == it->second.peers.end()) {
    return;
  }
  if (it2->second.connections.erase(connection_id)) {
    LOG(DEBUG) << "Closed connection #" << connection_id << " : l_id= << " << local_id << " p_id=" << peer_id;
  }
}

td::actor::Task<td::BufferSlice> Atcp::receive_adnl_query(td::BufferSlice query) {
  co_await fetch_tl_object<ton_api::atcp_getTcpAddr>(query, true);
  co_return create_serialize_tl_object<ton_api::atcp_tcpAddr>(addr_.get_ipv4(), addr_.get_port());
}

}  // namespace ton::atcp
