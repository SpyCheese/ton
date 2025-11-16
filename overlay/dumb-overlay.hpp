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

    Copyright 2017-2020 Telegram Systems LLP
*/
#pragma once

#include <memory>
#include <utility>
#include <vector>
#include "adnl/adnl.h"
#include "overlay/overlay.h"
#include "overlay/overlays.h"
#include "rldp2/rldp.h"
#include "td/actor/actor.h"
#include "td/utils/buffer.h"
#include "tl/generate/auto/tl/ton_api.h"

namespace ton {

namespace overlay {

class DumbOverlayImpl : public Overlay {
 public:
  DumbOverlayImpl(adnl::AdnlNodeIdShort local_id, OverlayIdShort overlay_id,
                  std::unique_ptr<Overlays::Callback> callback,
                  td::actor::ActorId<Overlays> manager,
                  td::actor::ActorId<rldp2::Rldp> rldp2,
                  std::vector<adnl::AdnlNodeIdShort> nodes);
  virtual void update_dht_node(td::actor::ActorId<dht::Dht> dht) {}
  virtual void receive_message(adnl::AdnlNodeIdShort src, tl_object_ptr<ton_api::overlay_messageExtra> extra,
                               td::BufferSlice data);
  virtual void receive_query(adnl::AdnlNodeIdShort src, tl_object_ptr<ton_api::overlay_messageExtra> extra,
                             td::BufferSlice data, td::Promise<td::BufferSlice> promise);
  virtual void send_message_to_neighbours(td::BufferSlice data) {}
  virtual void send_broadcast(PublicKeyHash send_as, td::uint32 flags, td::BufferSlice data);
  virtual void send_broadcast_fec(PublicKeyHash send_as, td::uint32 flags, td::BufferSlice data) {
      send_broadcast(send_as, flags, std::move(data));
  }
  virtual void print(td::StringBuilder &sb) {}
  virtual void get_overlay_random_peers(td::uint32 max_peers,
                                        td::Promise<std::vector<adnl::AdnlNodeIdShort>> promise) {}
  virtual void add_certificate(PublicKeyHash key, std::shared_ptr<Certificate>) {}
  virtual void set_privacy_rules(OverlayPrivacyRules rules) {}
  virtual void receive_nodes_from_db(tl_object_ptr<ton_api::overlay_nodes> nodes) {}
  virtual void receive_nodes_from_db_v2(tl_object_ptr<ton_api::overlay_nodesV2> nodes) {}
  virtual void get_stats(td::Promise<tl_object_ptr<ton_api::engine_validator_overlayStats>> promise) {}
  virtual void update_throughput_out_ctr(adnl::AdnlNodeIdShort peer_id, td::uint64 msg_size, bool is_query,
                                         bool is_response) {}
  virtual void update_throughput_in_ctr(adnl::AdnlNodeIdShort peer_id, td::uint64 msg_size, bool is_query,
                                        bool is_response) {}
  virtual void update_peer_ip_str(adnl::AdnlNodeIdShort peer_id, td::string ip_str) {}
  virtual void update_member_certificate(OverlayMemberCertificate cert) {}
  virtual void update_root_member_list(std::vector<adnl::AdnlNodeIdShort> ids,
                                       std::vector<PublicKeyHash> root_public_keys, OverlayMemberCertificate cert) {}
  virtual void forget_peer(adnl::AdnlNodeIdShort peer_id) {}

 private:
  adnl::AdnlNodeIdShort local_id_;
  OverlayIdShort overlay_id_;
  std::unique_ptr<Overlays::Callback> callback_;
  td::actor::ActorId<Overlays> manager_;
  td::actor::ActorId<rldp2::Rldp> rldp2_;
  std::vector<adnl::AdnlNodeIdShort> other_nodes_;
};

}  // namespace overlay

}  // namespace ton
