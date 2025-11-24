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
#pragma once

#include <map>

#include "adnl/adnl-peer-table.h"
#include "td/actor/coro.h"
#include "td/net/Pipe.h"
#include "td/net/TcpListener.h"

#include "atcp-connection.hpp"

namespace ton::atcp {

class Atcp : public adnl::AdnlSenderInterface {
 public:
  explicit Atcp(td::actor::ActorId<adnl::AdnlPeerTable> adnl) : adnl_(adnl) {
  }

  td::actor::Task<> add_id(adnl::AdnlNodeIdShort local_id);

  void send_message(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::BufferSlice data) override;
  void send_query(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, std::string name,
                  td::Promise<td::BufferSlice> promise, td::Timestamp timeout, td::BufferSlice data) override {
    send_query_ex(src, dst, std::move(name), std::move(promise), timeout, std::move(data), DEFAULT_MTU);
  }
  void send_query_ex(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, std::string name,
                     td::Promise<td::BufferSlice> promise, td::Timestamp timeout, td::BufferSlice data,
                     td::uint64 max_answer_size) override;
  void get_conn_ip_str(adnl::AdnlNodeIdShort l_id, adnl::AdnlNodeIdShort p_id,
                       td::Promise<td::string> promise) override {
    td::actor::send_closure(adnl_, &adnl::AdnlPeerTable::get_conn_ip_str, l_id, p_id, std::move(promise));
  }
  void set_mtu(adnl::AdnlNodeIdShort local_id, td::uint64 mtu);

  static constexpr td::uint64 DEFAULT_MTU = 1024;

 private:
  td::actor::ActorId<adnl::AdnlPeerTable> adnl_;

  struct TcpListener {
    td::actor::ActorOwn<td::TcpInfiniteListener> actor;
    int refcnt = 0;
  };
  std::map<int, TcpListener> tcp_listeners_;

  struct Connection {
    td::actor::ActorOwn<AtcpConnection> actor;
    bool closing_soon = false;
  };
  struct PeerPair {
    td::IPAddress ip;
    std::map<td::uint64, Connection> connections;
    std::vector<std::shared_ptr<td::Promise<td::actor::ActorId<AtcpConnection>>>> connection_waiters;
    bool connection_pending = false;
  };
  struct LocalId {
    std::map<adnl::AdnlNodeIdShort, PeerPair> peers;
    td::uint64 mtu = DEFAULT_MTU;
    std::set<int> listening_ports;
  };
  std::map<adnl::AdnlNodeIdShort, LocalId> local_ids_;
  td::uint64 next_connection_id_ = 0;

  td::actor::Task<> accept_connection(td::SocketPipe pipe);
  void get_connection(adnl::AdnlNodeIdShort local_id, adnl::AdnlNodeIdShort peer_id, td::Timestamp timeout,
                      td::Promise<td::actor::ActorId<AtcpConnection>> promise);
  td::actor::Task<td::actor::ActorId<AtcpConnection>> create_connection(adnl::AdnlNodeIdShort local_id,
                                                                        adnl::AdnlNodeIdShort peer_id);
  void after_create_connection(adnl::AdnlNodeIdShort local_id, adnl::AdnlNodeIdShort peer_id,
                               td::Result<td::actor::ActorId<AtcpConnection>> R);

  td::actor::Task<adnl::AdnlNodeIdFull> get_local_id_full(adnl::AdnlNodeIdShort local_id);
  void connection_status_changed(td::uint64 connection_id, adnl::AdnlNodeIdShort local_id,
                                 adnl::AdnlNodeIdShort peer_id, bool closing_soon);
  void close_connection(td::uint64 connection_id, adnl::AdnlNodeIdShort local_id, adnl::AdnlNodeIdShort peer_id);

  friend AtcpConnection;
};

}  // namespace ton::atcp
