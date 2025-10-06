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
#include "download-state.hpp"
#include "ton/ton-tl.hpp"
#include "ton/ton-io.hpp"
#include "td/utils/overloaded.h"
#include "full-node.h"
#include "td/actor/coro_utils.h"

namespace ton {

namespace validator {

namespace fullnode {

DownloadState::DownloadState(BlockIdExt block_id, BlockIdExt masterchain_block_id, PersistentStateType type,
                             adnl::AdnlNodeIdShort local_id, overlay::OverlayIdShort overlay_id,
                             adnl::AdnlNodeIdShort download_from, td::uint32 priority, td::Timestamp timeout,
                             td::actor::ActorId<ValidatorManagerInterface> validator_manager,
                             td::actor::ActorId<adnl::AdnlSenderInterface> rldp,
                             td::actor::ActorId<overlay::Overlays> overlays, td::actor::ActorId<adnl::Adnl> adnl,
                             td::actor::ActorId<adnl::AdnlExtClient> client, td::Promise<td::BufferSlice> promise)
    : block_id_(block_id)
    , masterchain_block_id_(masterchain_block_id)
    , type_(type)
    , effective_shard_(persistent_state_to_effective_shard(block_id_.shard_full(), type))
    , local_id_(local_id)
    , overlay_id_(overlay_id)
    , download_from_(download_from)
    , priority_(priority)
    , timeout_(timeout)
    , validator_manager_(validator_manager)
    , rldp_(rldp)
    , overlays_(overlays)
    , adnl_(adnl)
    , client_(client)
    , promise_(std::move(promise)) {
  CHECK(masterchain_block_id_.is_valid() || effective_shard_ == 0);
}

void DownloadState::abort_query(td::Status reason) {
  if (promise_) {
    LOG(WARNING) << "failed to download state " << block_id_.to_str() << " from " << download_from_ << ": " << reason;
    promise_.set_error(std::move(reason));
  }
  stop();
}

void DownloadState::alarm() {
  abort_query(td::Status::Error(ErrorCode::timeout, "timeout"));
}

void DownloadState::finish_query(td::BufferSlice state) {
  if (promise_) {
    LOG(WARNING) << "finished downloading state " << block_id_.to_str() << ": " << td::format::as_size(state.size());
    promise_.set_value(std::move(state));
  }
  stop();
}

void DownloadState::start_up() {
  alarm_timestamp() = timeout_;
  run()
      .start()
      .then([SelfId = actor_id(this)](td::Result<td::BufferSlice> R) mutable -> td::actor::Task<td::Unit> {
        if (R.is_error()) {
          td::actor::send_closure(SelfId, &DownloadState::abort_query, R.move_as_error());
        } else {
          td::actor::send_closure(SelfId, &DownloadState::finish_query, R.move_as_ok());
        }
        co_return td::Unit();
      })
      .detach();
}

td::actor::Task<td::BufferSlice> DownloadState::run() {
  status_ = ProcessStatus(validator_manager_, "process.download_state_net");

  td::Result<td::BufferSlice> state_from_disk;
  if (block_id_.seqno() == 0) {
    state_from_disk =
        co_await td::actor::ask(validator_manager_, &ValidatorManagerInterface::get_zero_state, block_id_).wrap();
  } else {
    state_from_disk = co_await td::actor::ask(validator_manager_, &ValidatorManagerInterface::get_persistent_state,
                                              block_id_, masterchain_block_id_, type_)
                          .wrap();
  }
  if (state_from_disk.is_ok()) {
    LOG(WARNING) << "got block state from disk: " << block_id_.to_str();
    co_return state_from_disk.move_as_ok();
  }

  BlockHandle handle =
      co_await td::actor::ask(validator_manager_, &ValidatorManagerInterface::get_block_handle, block_id_, true);
  if (download_from_.is_zero() && client_.empty()) {
    auto peers =
        co_await td::actor::ask(overlays_, &overlay::Overlays::get_overlay_random_peers, local_id_, overlay_id_, 1);
    if (peers.empty()) {
      co_return td::Status::Error(ErrorCode::notready, "no nodes");
    }
    download_from_ = peers[0];
  }

  LOG(WARNING) << "downloading state " << block_id_.to_str() << " ("
               << persistent_state_type_to_string(block_id_.shard_full(), type_) << ") from " << download_from_;
  if (effective_shard_ == 0) {
    td::BufferSlice query;
    if (masterchain_block_id_.is_valid()) {
      query = create_serialize_tl_object<ton_api::tonNode_preparePersistentState>(
          create_tl_block_id(block_id_), create_tl_block_id(masterchain_block_id_));
    } else {
      query = create_serialize_tl_object<ton_api::tonNode_prepareZeroState>(create_tl_block_id(block_id_));
    }
    td::BufferSlice response = co_await send_query("get_prepare", std::move(query), td::Timestamp::in(1.0));
    auto f = co_await fetch_tl_object<ton_api::tonNode_PreparedState>(std::move(response), true);
    if (f->get_id() == ton_api::tonNode_notFoundState::ID) {
      co_return td::Status::Error(ErrorCode::notready, "state not found");
    }
    if (!masterchain_block_id_.is_valid()) {
      status_.set_status(PSTRING() << block_id_.id.to_str() << " : download started");
      td::BufferSlice data = co_await send_query(
          "download_zero_state",
          create_serialize_tl_object<ton_api::tonNode_downloadZeroState>(create_tl_block_id(block_id_)),
          td::Timestamp::in(3.0));
      co_return data;
    }
    co_await request_total_size();
  } else {
    td::BufferSlice response = co_await send_query(
        "get_prepare",
        create_serialize_tl_object<ton_api::tonNode_getPersistentStateSizeV2>(
            create_tl_object<ton_api::tonNode_persistentStateIdV2>(
                create_tl_block_id(block_id_), create_tl_block_id(masterchain_block_id_), effective_shard_)),
        td::Timestamp::in(1.0));
    auto f = co_await fetch_tl_object<ton_api::tonNode_PersistentStateSize>(std::move(response), true);
    if (f->get_id() == ton_api::tonNode_persistentStateSizeNotFound::ID) {
      co_return td::Status::Error(ErrorCode::notready, "state not found");
    }
    total_size_ = move_tl_object_as<ton_api::tonNode_persistentStateSize>(std::move(f))->size_;
  }

  td::uint64 sum = 0;
  td::uint32 part_size = 1 << 21;
  std::vector<td::BufferSlice> parts;
  td::uint64 prev_logged_sum = 0;
  td::Timer prev_logged_timer;
  while (true) {
    td::BufferSlice query;
    if (effective_shard_ == 0) {
      query = create_serialize_tl_object<ton_api::tonNode_downloadPersistentStateSlice>(
          create_tl_block_id(block_id_), create_tl_block_id(masterchain_block_id_), sum, part_size);
    } else {
      query = create_serialize_tl_object<ton_api::tonNode_downloadPersistentStateSliceV2>(
          create_tl_object<ton_api::tonNode_persistentStateIdV2>(
              create_tl_block_id(block_id_), create_tl_block_id(masterchain_block_id_), effective_shard_),
          sum, part_size);
    }
    td::BufferSlice data = co_await send_query("download_state", std::move(query), td::Timestamp::in(20.0));
    bool last_part = data.size() < part_size;
    sum += data.size();
    parts.push_back(std::move(data));
    double elapsed = prev_logged_timer.elapsed();
    if (elapsed > 5.0) {
      prev_logged_timer = td::Timer();
      auto speed = (td::uint64)((double)(sum - prev_logged_sum) / elapsed);
      td::StringBuilder sb;
      sb << td::format::as_size(sum);
      if (total_size_) {
        sb << "/" << td::format::as_size(total_size_);
      }
      sb << " (" << td::format::as_size(speed) << "/s";
      if (total_size_) {
        sb << ", " << td::StringBuilder::FixedDouble((double)sum / (double)total_size_ * 100.0, 2) << "%";
        if (speed > 0 && total_size_ >= sum) {
          td::uint64 rem = (total_size_ - sum) / speed;
          sb << ", " << rem << "s remaining";
        }
      }
      sb << ")";
      LOG(WARNING) << "downloading state " << block_id_.to_str() << " : " << sb.as_cslice();
      status_.set_status(PSTRING() << block_id_.id.to_str() << " : " << sb.as_cslice());
      prev_logged_sum = sum;
    }
    if (last_part) {
      break;
    }
  }

  status_.set_status(PSTRING() << block_id_.id.to_str() << " : " << sum << " bytes, finishing");
  td::BufferSlice res{td::narrow_cast<std::size_t>(sum)};
  auto S = res.as_slice();
  for (auto &p : parts) {
    S.copy_from(p.as_slice());
    S.remove_prefix(p.size());
  }
  parts.clear();
  CHECK(S.empty());
  co_return res;
}

td::actor::StartedTask<td::BufferSlice> DownloadState::send_query(std::string name, td::BufferSlice query,
                                                                  td::Timestamp timeout) {
  auto [task, promise] = td::actor::StartedTask<td::BufferSlice>::make_bridge();
  if (client_.empty()) {
    td::actor::send_closure(overlays_, &overlay::Overlays::send_query, download_from_, local_id_, overlay_id_,
                            std::move(name), std::move(promise), timeout, std::move(query));
  } else {
    td::actor::send_closure(client_, &adnl::AdnlExtClient::send_query, std::move(name),
                            create_serialize_tl_object_suffix<ton_api::tonNode_query>(std::move(query)), timeout,
                            std::move(promise));
  }
  return std::move(task);
}

td::actor::Task<td::Unit> DownloadState::request_total_size() {
  td::BufferSlice query;
  if (effective_shard_ == 0) {
    query = create_serialize_tl_object<ton_api::tonNode_getPersistentStateSize>(
        create_tl_block_id(block_id_), create_tl_block_id(masterchain_block_id_));
  } else {
    query = create_serialize_tl_object<ton_api::tonNode_getPersistentStateSizeV2>(
        create_tl_object<ton_api::tonNode_persistentStateIdV2>(
            create_tl_block_id(block_id_), create_tl_block_id(masterchain_block_id_), effective_shard_));
  }
  auto R = co_await send_query("get_size", std::move(query), td::Timestamp::in(3.0)).wrap();
  if (R.is_error()) {
    co_return td::Unit{};
  }
  auto res = fetch_tl_object<ton_api::tonNode_persistentStateSize>(R.move_as_ok(), true);
  if (res.is_error()) {
    co_return td::Unit{};
  }
  total_size_ = res.ok()->size_;
  co_return td::Unit{};
}

}  // namespace fullnode

}  // namespace validator

}  // namespace ton
