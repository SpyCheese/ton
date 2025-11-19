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
#include "td/net/TcpListener.h"
#include "td/actor/coro.h"
#include "td/net/Pipe.h"
#include "atcp-connection.hpp"

namespace ton::atcp {

class Atcp : public adnl::AdnlSenderInterface {
 public:
  Atcp(td::actor::ActorId<adnl::AdnlPeerTable> adnl, td::IPAddress addr) : adnl_(adnl), addr_(addr) {
  }

  void start_up() override;

  void add_id(adnl::AdnlNodeIdShort local_id);

  void send_message(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, td::BufferSlice data) override;
  void send_query(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, std::string name,
                  td::Promise<td::BufferSlice> promise, td::Timestamp timeout, td::BufferSlice data) override {
    send_query_ex(src, dst, std::move(name), std::move(promise), timeout, std::move(data), adnl::Adnl::get_mtu());
  }
  void send_query_ex(adnl::AdnlNodeIdShort src, adnl::AdnlNodeIdShort dst, std::string name,
                     td::Promise<td::BufferSlice> promise, td::Timestamp timeout, td::BufferSlice data,
                     td::uint64 max_answer_size) override;
  void get_conn_ip_str(adnl::AdnlNodeIdShort l_id, adnl::AdnlNodeIdShort p_id,
                       td::Promise<td::string> promise) override {
    td::actor::send_closure(adnl_, &adnl::AdnlPeerTable::get_conn_ip_str, l_id, p_id, std::move(promise));
  }

 private:
  td::actor::ActorId<adnl::AdnlPeerTable> adnl_;
  td::IPAddress addr_;
  td::actor::ActorOwn<td::TcpInfiniteListener> tcp_listener_;

  struct PeerPair {
    td::IPAddress ip;
    std::map<td::uint64, td::actor::ActorOwn<AtcpConnection>> connections;
    std::vector<td::Promise<td::actor::ActorId<AtcpConnection>>> connection_waiters;
    bool connection_pending = false;
  };
  struct LocalId {
    std::map<adnl::AdnlNodeIdShort, PeerPair> peers;
  };
  std::map<adnl::AdnlNodeIdShort, LocalId> local_ids_;
  td::uint64 next_connection_id_ = 0;

  td::actor::Task<> accept_connection(td::SocketPipe pipe);
  void get_connection(adnl::AdnlNodeIdShort local_id, adnl::AdnlNodeIdShort peer_id,
                      td::Promise<td::actor::ActorId<AtcpConnection>> promise);
  td::actor::Task<td::actor::ActorId<AtcpConnection>> create_connection(adnl::AdnlNodeIdShort local_id,
                                                                        adnl::AdnlNodeIdShort peer_id);
  void after_create_connection(adnl::AdnlNodeIdShort local_id, adnl::AdnlNodeIdShort peer_id,
                               td::Result<td::actor::ActorId<AtcpConnection>> R);

  td::actor::Task<adnl::AdnlNodeIdFull> get_local_id_full(adnl::AdnlNodeIdShort local_id);
  void close_connection(td::uint64 connection_id, adnl::AdnlNodeIdShort local_id, adnl::AdnlNodeIdShort peer_id);

  td::actor::Task<td::BufferSlice> receive_adnl_query(td::BufferSlice query);

  friend AtcpConnection;
};

}  // namespace ton::atcp
