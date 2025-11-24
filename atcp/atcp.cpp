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
#include "delay.h"

namespace ton::atcp {

td::actor::Task<> Atcp::add_id(adnl::AdnlNodeIdShort local_id) {
  auto [_, added] = local_ids_.emplace(local_id, LocalId{});
  if (added) {
    LOG(INFO) << "Added local id " << local_id;
  }
  adnl::AdnlNode node = co_await td::actor::ask(adnl_, &adnl::Adnl::get_self_node, local_id);
  std::set<int> ports;
  for (const auto& addr : node.addr_list().addrs()) {
    auto r_ip = addr->to_ip_address();
    if (r_ip.is_ok() && r_ip.ok().get_port() != 0) {
      ports.insert(r_ip.ok().get_port());
    }
  }
  if (ports.empty()) {
    LOG(WARNING) << "Local id " << local_id << " has no ports";
  }
  LocalId& info = local_ids_[local_id];
  if (info.listening_ports == ports) {
    co_return td::Unit{};
  }

  class TcpListenerCallback : public td::TcpListener::Callback {
   public:
    explicit TcpListenerCallback(td::actor::ActorId<Atcp> atcp) : atcp_(std::move(atcp)) {
    }

    void accept(td::SocketFd fd) override {
      td::actor::ask(atcp_, &Atcp::accept_connection, td::make_socket_pipe(std::move(fd))).detach();
    }

   private:
    td::actor::ActorId<Atcp> atcp_;
  };
  for (int port : ports) {
    if (tcp_listeners_[port].refcnt++ == 0) {
      LOG(INFO) << "Starting TCP server on port " << port;
      tcp_listeners_[port].actor = td::actor::create_actor<td::TcpInfiniteListener>(
          "TcpListener", port, std::make_unique<TcpListenerCallback>(actor_id(this)));
    }
  }
  for (int port : info.listening_ports) {
    if (--tcp_listeners_[port].refcnt == 0) {
      LOG(INFO) << "Closing TCP server on port " << port;
      tcp_listeners_.erase(port);
    }
  }
  info.listening_ports = std::move(ports);

  co_return td::Unit{};
}

void Atcp::send_message(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::BufferSlice data) {
  get_connection(src, dst, td::Timestamp::in(10.0),
                 [data = std::move(data)](td::Result<td::actor::ActorId<AtcpConnection>> R) mutable {
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
  get_connection(src, dst, timeout,
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

void Atcp::set_mtu(adnl::AdnlNodeIdShort local_id, td::uint64 mtu) {
  auto it = local_ids_.find(local_id);
  CHECK(it != local_ids_.end());
  it->second.mtu = mtu;
  for (auto& [_, peer_pair] : it->second.peers) {
    for (auto& [_, conn] : peer_pair.connections) {
      td::actor::send_closure(conn.actor, &AtcpConnection::set_mtu, mtu);
    }
  }
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
  td::actor::send_closure(connection, &AtcpConnection::set_mtu, it->second.mtu);
  adnl::AdnlNodeIdShort peer_id = peer_id_full.compute_short_id();
  PeerPair& peer_pair = it->second.peers[peer_id];
  auto res = connection.get();
  peer_pair.connections[connection_id].actor = std::move(connection);
  after_create_connection(local_id, peer_id, res);
  co_return td::Unit{};
}

void Atcp::get_connection(adnl::AdnlNodeIdShort local_id, adnl::AdnlNodeIdShort peer_id, td::Timestamp timeout,
                          td::Promise<td::actor::ActorId<AtcpConnection>> promise) {
  auto it = local_ids_.find(local_id);
  if (it == local_ids_.end()) {
    promise.set_error(td::Status::Error("no such local id"));
    return;
  }
  PeerPair& peer_pair = it->second.peers[peer_id];
  for (auto& [_, conn] : peer_pair.connections) {
    if (!conn.closing_soon) {
      promise.set_value(conn.actor.get());
      return;
    }
  }
  auto promise_ptr = std::make_shared<td::Promise<td::actor::ActorId<AtcpConnection>>>(std::move(promise));
  peer_pair.connection_waiters.push_back(promise_ptr);
  if (timeout) {
    delay_action(
        [SelfId = actor_id(this), promise_ptr]() {
          td::actor::send_lambda(SelfId, [promise_ptr]() mutable {
            if (*promise_ptr) {
              promise_ptr->set_error(td::Status::Error(ErrorCode::timeout, "timeout"));
            }
          });
        },
        timeout);
  }
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
    adnl::AdnlNode peer_node = co_await td::actor::ask(adnl_, &adnl::Adnl::get_peer_node, local_id, peer_id);
    for (const auto& addr : peer_node.addr_list().addrs()) {
      auto r_ip = addr->to_ip_address();
      if (r_ip.is_ok()) {
        peer_addr = r_ip.move_as_ok();
      }
    }
    if (!peer_addr.is_valid()) {
      co_return td::Status::Error("no ip address for peer");
    }
    local_ids_[local_id].peers[peer_id].ip = peer_addr;
  }
  td::uint64 connection_id = next_connection_id_++;
  LOG(INFO) << "Trying to create connection #" << connection_id << " to " << peer_addr.get_ip_str() << ":"
            << peer_addr.get_port() << " (" << local_id << " -> " << peer_id << ")";
  auto [init_task, init_promise] =
      td::actor::StartedTask<std::pair<adnl::AdnlNodeIdFull, adnl::AdnlNodeIdShort>>::make_bridge();
  auto connection = td::actor::create_actor<AtcpConnection>(PSTRING() << "atcp-conn#" << connection_id, connection_id,
                                                            local_id_full, peer_id, peer_addr,
                                                            actor_id(this), adnl_, std::move(init_promise));
  auto [peer_id_full, _] = co_await std::move(init_task);
  auto it = local_ids_.find(local_id);
  if (it == local_ids_.end()) {
    co_return td::Status::Error("no such local id");
  }
  td::actor::send_closure(connection, &AtcpConnection::set_mtu, it->second.mtu);
  PeerPair& peer_pair = it->second.peers[peer_id];
  auto res = connection.get();
  peer_pair.connections[connection_id].actor = std::move(connection);
  co_return res;
}

void Atcp::after_create_connection(adnl::AdnlNodeIdShort local_id, adnl::AdnlNodeIdShort peer_id,
                                   td::Result<td::actor::ActorId<AtcpConnection>> R) {
  if (R.is_error()) {
    LOG(INFO) << "Cannot create connection to " << peer_id << " : " << R.error();
  }
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
    if (*promise) {
      promise->set_result(R.clone());
    }
  }
  peer_pair.connection_waiters.clear();
}

td::actor::Task<adnl::AdnlNodeIdFull> Atcp::get_local_id_full(adnl::AdnlNodeIdShort local_id) {
  if (!local_ids_.contains(local_id)) {
    co_return td::Status::Error("no such local id");
  }
  co_return (co_await td::actor::ask(adnl_, &adnl::Adnl::get_self_node, local_id)).pub_id();
}

void Atcp::connection_status_changed(td::uint64 connection_id, adnl::AdnlNodeIdShort local_id,
                                     adnl::AdnlNodeIdShort peer_id, bool closing_soon) {
  auto it = local_ids_.find(local_id);
  if (it == local_ids_.end()) {
    return;
  }
  auto it2 = it->second.peers.find(peer_id);
  if (it2 == it->second.peers.end()) {
    return;
  }
  auto it3 = it2->second.connections.find(connection_id);
  if (it3 == it2->second.connections.end()) {
    return;
  }
  it3->second.closing_soon = closing_soon;
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

}  // namespace ton::atcp
