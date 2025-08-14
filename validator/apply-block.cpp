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
#include "apply-block.hpp"

#include "block-auto.h"
#include "block-parse.h"
#include "adnl/utils.hpp"
#include "ton/ton-io.hpp"
#include "validator/invariants.hpp"
#include "td/actor/MultiPromise.h"
#include "validator/fabric.h"

namespace ton {

namespace validator {

static bool need_write_stats() {
  static bool x = []() -> bool {
    const char* s = getenv("TON_STORE_APPLY_STATS");
    return s != nullptr && strlen(s) > 0;
  }();
  return x;
}

void ApplyBlock::abort_query(td::Status reason) {
  if (promise_) {
    VLOG(VALIDATOR_WARNING) << "aborting apply block query for " << id_ << ": " << reason;
    promise_.set_error(std::move(reason));
  }
  stop();
}

void ApplyBlock::finish_query() {
  VLOG(VALIDATOR_DEBUG) << "successfully finishing apply block query";
  handle_->set_processed();
  ValidatorInvariants::check_post_apply(handle_);

  if (promise_) {
    promise_.set_value(td::Unit());
  }
  stop();
}

void ApplyBlock::alarm() {
  abort_query(td::Status::Error(ErrorCode::timeout, "timeout"));
}

void ApplyBlock::start_up() {
  VLOG(VALIDATOR_DEBUG) << "running apply_block for " << id_;

  if (id_.is_masterchain()) {
    masterchain_block_id_ = id_;
  }

  alarm_timestamp() = timeout_;

  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<BlockHandle> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &ApplyBlock::abort_query, R.move_as_error());
    } else {
      td::actor::send_closure(SelfId, &ApplyBlock::got_block_handle, R.move_as_ok());
    }
  });

  td::actor::send_closure(manager_, &ValidatorManager::get_block_handle, id_, true, std::move(P));
}

void ApplyBlock::got_block_handle(BlockHandle handle) {
  handle_ = std::move(handle);

  if (handle_->is_applied() && (!handle_->id().is_masterchain() || handle_->processed())) {
    finish_query();
    return;
  }

  if (handle_->is_applied()) {
    auto P =
        td::PromiseCreator::lambda([SelfId = actor_id(this), seqno = handle_->id().id.seqno](td::Result<BlockIdExt> R) {
          R.ensure();
          auto h = R.move_as_ok();
          if (h.id.seqno < seqno) {
            td::actor::send_closure(SelfId, &ApplyBlock::written_block_data, td::Ref<BlockData>{});
          } else {
            td::actor::send_closure(SelfId, &ApplyBlock::finish_query);
          }
        });
    td::actor::send_closure(manager_, &ValidatorManager::get_top_masterchain_block, std::move(P));
    return;
  }

  if (handle_->id().id.seqno == 0) {
    written_block_data();
    return;
  }

  if (handle_->is_archived()) {
    finish_query();
    return;
  }

  if (handle_->received() && !(need_write_stats() && block_.is_null())) {
    written_block_data();
    return;
  }

  if (block_.not_null()) {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Unit> R) {
      if (R.is_error()) {
        td::actor::send_closure(SelfId, &ApplyBlock::abort_query, R.move_as_error());
      } else {
        td::actor::send_closure(SelfId, &ApplyBlock::written_block_data, td::Ref<BlockData>{});
      }
    });

    td::actor::send_closure(manager_, &ValidatorManager::set_block_data, handle_, block_, std::move(P));
  } else {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), handle = handle_](td::Result<td::Ref<BlockData>> R) {
      CHECK(handle->received());
      if (R.is_error()) {
        td::actor::send_closure(SelfId, &ApplyBlock::abort_query, R.move_as_error());
      } else {
        td::actor::send_closure(SelfId, &ApplyBlock::written_block_data, R.move_as_ok());
      }
    });

    td::actor::send_closure(manager_, &ValidatorManager::wait_block_data, handle_, apply_block_priority(), timeout_,
                            std::move(P));
  }
}

void ApplyBlock::written_block_data(td::Ref<BlockData> data) {
  if (data.not_null()) {
    block_ = data;
  }
  VLOG(VALIDATOR_DEBUG) << "apply block: written block data for " << id_;
  if (!handle_->id().seqno()) {
    CHECK(handle_->inited_split_after());
    CHECK(handle_->inited_state_root_hash());
    CHECK(handle_->inited_logical_time());
  } else {
    if (handle_->id().is_masterchain() && !handle_->inited_proof()) {
      abort_query(td::Status::Error(ErrorCode::notready, "proof is absent"));
      return;
    }
    if (!handle_->id().is_masterchain() && !handle_->inited_proof_link()) {
      abort_query(td::Status::Error(ErrorCode::notready, "proof link is absent"));
      return;
    }
    CHECK(handle_->inited_merge_before());
    CHECK(handle_->inited_split_after());
    CHECK(handle_->inited_prev());
    CHECK(handle_->inited_state_root_hash());
    CHECK(handle_->inited_logical_time());
  }
  if (handle_->is_applied() && handle_->processed()) {
    finish_query();
  } else {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Ref<ShardState>> R) {
      if (R.is_error()) {
        td::actor::send_closure(SelfId, &ApplyBlock::abort_query, R.move_as_error());
      } else {
        td::actor::send_closure(SelfId, &ApplyBlock::got_cur_state, R.move_as_ok());
      }
    });

    td::actor::send_closure(manager_, &ValidatorManager::wait_block_state, handle_, apply_block_priority(), timeout_,
                            true, std::move(P));
  }
}

void ApplyBlock::got_cur_state(td::Ref<ShardState> state) {
  VLOG(VALIDATOR_DEBUG) << "apply block: received state for " << id_;
  state_ = std::move(state);
  CHECK(handle_->received_state());
  written_state();
}

void ApplyBlock::written_state() {
  if (handle_->is_applied() && handle_->processed()) {
    finish_query();
    return;
  }
  VLOG(VALIDATOR_DEBUG) << "apply block: setting next for parents of " << id_;

  if (handle_->id().id.seqno != 0 && !handle_->is_applied()) {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Unit> R) {
      if (R.is_error()) {
        td::actor::send_closure(SelfId, &ApplyBlock::abort_query, R.move_as_error());
      } else {
        td::actor::send_closure(SelfId, &ApplyBlock::written_next);
      }
    });

    td::MultiPromise mp;
    auto g = mp.init_guard();
    g.add_promise(std::move(P));

    td::actor::send_closure(manager_, &ValidatorManager::set_next_block, handle_->one_prev(true), id_, g.get_promise());
    if (handle_->merge_before()) {
      td::actor::send_closure(manager_, &ValidatorManager::set_next_block, handle_->one_prev(false), id_,
                              g.get_promise());
    }
  } else {
    written_next();
  }
}

void ApplyBlock::written_next() {
  if (handle_->is_applied() && handle_->processed()) {
    finish_query();
    return;
  }

  VLOG(VALIDATOR_DEBUG) << "apply block: applying parents of " << id_;

  if (handle_->id().id.seqno != 0 && !handle_->is_applied()) {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Unit> R) {
      if (R.is_error()) {
        td::actor::send_closure(SelfId, &ApplyBlock::abort_query, R.move_as_error_prefix("prev: "));
      } else {
        td::actor::send_closure(SelfId, &ApplyBlock::applied_prev);
      }
    });

    td::MultiPromise mp;
    auto g = mp.init_guard();
    g.add_promise(std::move(P));
    BlockIdExt m = masterchain_block_id_;
    if (id_.is_masterchain()) {
      m = id_;
    }
    run_apply_block_query(handle_->one_prev(true), td::Ref<BlockData>{}, m, manager_, timeout_, g.get_promise());
    if (handle_->merge_before()) {
      run_apply_block_query(handle_->one_prev(false), td::Ref<BlockData>{}, m, manager_, timeout_, g.get_promise());
    }
  } else {
    applied_prev();
  }
}

void ApplyBlock::applied_prev() {
  VLOG(VALIDATOR_DEBUG) << "apply block: waiting manager's confirm for " << id_;
  if (!id_.is_masterchain()) {
    handle_->set_masterchain_ref_block(masterchain_block_id_.seqno());
  }
  auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Unit> R) {
    if (R.is_error()) {
      td::actor::send_closure(SelfId, &ApplyBlock::abort_query, R.move_as_error());
    } else {
      td::actor::send_closure(SelfId, &ApplyBlock::applied_set);
    }
  });
  td::actor::send_closure(manager_, &ValidatorManager::new_block, handle_, state_, std::move(P));
}

void ApplyBlock::applied_set() {
  VLOG(VALIDATOR_DEBUG) << "apply block: setting apply bit for " << id_;
  handle_->set_applied();
  if (handle_->id().seqno() > 0) {
    CHECK(handle_->handle_moved_to_archive());
    CHECK(handle_->moved_to_archive());
  }

  if (need_write_stats() && handle_->id().seqno() > 0) {
    auto stats = create_tl_object<ton_api::validatorStats_appliedBlockStats>();
    stats->block_id_ = create_tl_block_id(block_->block_id());
    stats->applied_at_ = td::Clocks::system();
    block::gen::Block::Record rec;
    block::gen::BlockInfo::Record info;
    block::gen::BlockExtra::Record extra;
    CHECK(block::gen::unpack_cell(block_->root_cell(), rec) && block::gen::unpack_cell(rec.info, info) &&
          block::gen::unpack_cell(rec.extra, extra));
    stats->block_timestamp_ = info.gen_utime;

    vm::AugmentedDictionary acc_dict{vm::load_cell_slice_ref(extra.account_blocks), 256,
                                     block::tlb::aug_ShardAccountBlocks};
    acc_dict.check_for_each([&](td::Ref<vm::CellSlice> value, td::ConstBitPtr key, int n) -> bool {
      block::gen::CurrencyCollection::Record skip;
      block::gen::csr_unpack_skip(value, skip);
      block::gen::AccountBlock::Record acc_blk;
      CHECK(tlb::csr_unpack(std::move(value), acc_blk));
      vm::AugmentedDictionary trans_dict{vm::DictNonEmpty(), std::move(acc_blk.transactions), 64,
                                         block::tlb::aug_AccountTransactions};
      trans_dict.check_for_each_extra([&](td::Ref<vm::CellSlice> value, td::Ref<vm::CellSlice>, td::ConstBitPtr, int) -> bool {
        ++stats->transactions_;
        block::gen::Transaction::Record trans;
        CHECK(tlb::unpack_cell(value->prefetch_ref(), trans));
        if (trans.r1.in_msg->size_refs() > 0) {
          vm::CellSlice msg{vm::NoVm(), trans.r1.in_msg->prefetch_ref()};
          if (msg.prefetch_long(1)) {
            ++stats->ext_msgs_;
          }
        }
        return true;
      });
      return true;
    });
    td::actor::send_closure(manager_, &ValidatorManager::log_applied_block_stats, std::move(stats));
  }

  if (handle_->need_flush()) {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this)](td::Result<td::Unit> R) {
      if (R.is_error()) {
        td::actor::send_closure(SelfId, &ApplyBlock::abort_query, R.move_as_error());
      } else {
        td::actor::send_closure(SelfId, &ApplyBlock::finish_query);
      }
    });
    handle_->flush(manager_, handle_, std::move(P));
  } else {
    finish_query();
  }
}

}  // namespace validator

}  // namespace ton
