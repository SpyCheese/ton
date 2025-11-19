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

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>
#include "adnl/adnl.h"
#include "auto/tl/ton_api.h"
#include "dumb-overlay.hpp"
#include "overlays.h"
#include "td/actor/actor.h"

namespace ton {

namespace overlay {

DumbOverlayImpl::DumbOverlayImpl(adnl::AdnlNodeIdShort local_id,
                                 OverlayIdShort overlay_id,
                                 std::unique_ptr<Overlays::Callback> callback,
                                 td::actor::ActorId<Overlays> manager,
                                 td::actor::ActorId<rldp2::Rldp> rldp2,
                                 std::vector<adnl::AdnlNodeIdShort> nodes)
  : local_id_(local_id)
  , overlay_id_(overlay_id)
  , callback_(std::move(callback))
  , manager_(std::move(manager))
  , rldp2_(std::move(rldp2))
  , other_nodes_(std::move(nodes))
{
  other_nodes_.erase(std::remove(other_nodes_.begin(), other_nodes_.end(), local_id_), other_nodes_.end());
}

void DumbOverlayImpl::receive_message(adnl::AdnlNodeIdShort src,
                                      tl_object_ptr<ton_api::overlay_messageExtra> extra,
                                      td::BufferSlice data) {
  auto R = fetch_tl_object<ton_api::dumbOverlay_broadcast>(data, true);
  if (R.is_error()) {
    callback_->receive_message(src, overlay_id_, std::move(data));
  } else {
    auto B = R.move_as_ok();
    callback_->receive_broadcast(PublicKeyHash(B->send_as_), overlay_id_, std::move(B->data_));
  }
}

void DumbOverlayImpl::receive_query(adnl::AdnlNodeIdShort src,
                                    tl_object_ptr<ton_api::overlay_messageExtra> extra,
                                    td::BufferSlice data,
                                    td::Promise<td::BufferSlice> promise) {
  callback_->receive_query(src, overlay_id_, std::move(data), std::move(promise));
}

void DumbOverlayImpl::send_broadcast(PublicKeyHash send_as, td::uint32 flags, td::BufferSlice data) {
  td::BufferSlice obj = create_serialize_tl_object<ton_api::dumbOverlay_broadcast>(send_as.bits256_value(), std::move(data));
  for (auto &dst : other_nodes_) {
    td::actor::send_closure(manager_, &Overlays::send_message_via, dst, local_id_, overlay_id_, obj.clone(), rldp2_);
  }
}

}  // namespace overlay

}  // namespace ton
