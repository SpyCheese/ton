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
#include "adnl/adnl-peer-table.h"
#include "td/net/Pipe.h"

namespace ton::atcp {

class Atcp;

class AtcpConnection : public td::actor::Actor {
 public:
  AtcpConnection(td::uint64 connection_id, td::SocketPipe pipe, td::actor::ActorId<Atcp> atcp,
                 td::actor::ActorId<adnl::AdnlPeerTable> adnl,
                 td::Promise<std::pair<adnl::AdnlNodeIdFull, adnl::AdnlNodeIdShort>> init_promise)
      : connection_id_(connection_id)
      , inbound_(true)
      , pipe_(std::move(pipe))
      , atcp_(std::move(atcp))
      , adnl_(std::move(adnl))
      , init_promise_(std::move(init_promise)) {
  }

  AtcpConnection(td::uint64 connection_id, adnl::AdnlNodeIdFull local_id_full, adnl::AdnlNodeIdShort peer_id,
                 td::IPAddress outbound_ip, td::actor::ActorId<Atcp> atcp, td::actor::ActorId<adnl::AdnlPeerTable> adnl,
                 td::Promise<std::pair<adnl::AdnlNodeIdFull, adnl::AdnlNodeIdShort>> init_promise)
      : connection_id_(connection_id)
      , inbound_(false)
      , outbound_ip_(outbound_ip)
      , atcp_(std::move(atcp))
      , adnl_(std::move(adnl))
      , init_promise_(std::move(init_promise))
      , local_id_(local_id_full.compute_short_id())
      , local_id_full_(local_id_full)
      , peer_id_(peer_id) {
  }

  void start_up() override;
  void tear_down() override;
  void loop() override;

  void send_message(td::BufferSlice data);
  void send_query(std::string name, td::Promise<td::BufferSlice> promise, td::Timestamp timeout, td::BufferSlice data,
                  td::uint64 max_answer_size);

 private:
  td::uint64 connection_id_;
  bool inbound_;
  td::SocketPipe pipe_;
  td::IPAddress outbound_ip_;
  td::actor::ActorId<Atcp> atcp_;
  td::actor::ActorId<adnl::AdnlPeerTable> adnl_;
  td::Promise<std::pair<adnl::AdnlNodeIdFull, adnl::AdnlNodeIdShort>> init_promise_;

  adnl::AdnlNodeIdShort local_id_ = adnl::AdnlNodeIdShort::zero();
  adnl::AdnlNodeIdFull local_id_full_ = {};
  adnl::AdnlNodeIdShort peer_id_ = adnl::AdnlNodeIdShort::zero();
  adnl::AdnlNodeIdFull peer_id_full_ = {};
  bool inited_ = false;

  td::Promise<td::Unit> fd_read_waiter_;
  size_t fd_read_waiter_size_ = 0;

  struct OutQuery {
    std::string name;
    td::Promise<td::BufferSlice> promise;
  };
  std::map<td::Bits256, OutQuery> out_queries_;

  td::actor::Task<> run();
  td::actor::Task<> run_inner();

  void on_query_timeout(td::Bits256 query_id);
  void send_query_answer(td::Bits256 query_id, td::BufferSlice data);

  td::actor::Task<td::BufferSlice> read_message();
  td::actor::Task<td::BufferSlice> read_bytes(size_t size);
  void send_message_internal(td::BufferSlice data);

  void abort(td::Status S);
};

}  // namespace ton::atcp
