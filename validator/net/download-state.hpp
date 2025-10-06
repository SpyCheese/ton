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

#include "overlay/overlays.h"
#include "ton/ton-types.h"
#include "validator/validator.h"
#include "adnl/adnl-ext-client.h"
#include "td/actor/coro_task.h"

#include <stats-provider.h>

namespace ton {

namespace validator {

namespace fullnode {

class DownloadState : public td::actor::Actor {
 public:
  DownloadState(BlockIdExt block_id, BlockIdExt masterchain_block_id, PersistentStateType type,
                adnl::AdnlNodeIdShort local_id, overlay::OverlayIdShort overlay_id, adnl::AdnlNodeIdShort download_from,
                td::uint32 priority, td::Timestamp timeout,
                td::actor::ActorId<ValidatorManagerInterface> validator_manager,
                td::actor::ActorId<adnl::AdnlSenderInterface> rldp, td::actor::ActorId<overlay::Overlays> overlays,
                td::actor::ActorId<adnl::Adnl> adnl, td::actor::ActorId<adnl::AdnlExtClient> client,
                td::Promise<td::BufferSlice> promise);

  void abort_query(td::Status reason);
  void alarm() override;
  void finish_query(td::BufferSlice state);

  void start_up() override;
  td::actor::Task<td::BufferSlice> run();
  td::actor::StartedTask<td::BufferSlice> send_query(std::string name, td::BufferSlice query, td::Timestamp timeout);
  td::actor::Task<td::Unit> request_total_size();

 private:
  BlockIdExt block_id_;
  BlockIdExt masterchain_block_id_;
  PersistentStateType type_;
  ShardId effective_shard_;
  adnl::AdnlNodeIdShort local_id_;
  overlay::OverlayIdShort overlay_id_;

  adnl::AdnlNodeIdShort download_from_ = adnl::AdnlNodeIdShort::zero();

  td::uint32 priority_;

  td::Timestamp timeout_;
  td::actor::ActorId<ValidatorManagerInterface> validator_manager_;
  td::actor::ActorId<adnl::AdnlSenderInterface> rldp_;
  td::actor::ActorId<overlay::Overlays> overlays_;
  td::actor::ActorId<adnl::Adnl> adnl_;
  td::actor::ActorId<adnl::AdnlExtClient> client_;
  td::Promise<td::BufferSlice> promise_;

  td::uint64 total_size_ = 0;
  ProcessStatus status_;
};

}  // namespace fullnode

}  // namespace validator

}  // namespace ton
