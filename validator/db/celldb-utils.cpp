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
#include "block/block-auto.h"
#include "td/actor/MultiPromise.h"
#include "td/utils/HashMap.h"
#include "vm/db/CellStorage.h"
#include "vm/dict.h"

#include "block-parse.h"
#include "celldb-utils.h"

namespace ton::validator {

void calculate_permanent_celldb_update(const std::map<BlockIdExt, td::Ref<BlockData>>& blocks,
                                       std::shared_ptr<vm::DynamicBagOfCellsDb::AsyncExecutor> executor,
                                       td::Promise<std::vector<PermanentCellDbUpdate>> promise) {
  td::MultiPromise mp;
  auto ig = mp.init_guard();
  auto updates = std::make_shared<std::vector<PermanentCellDbUpdate>>();
  updates->reserve(blocks.size());
  for (auto& [_, block] : blocks) {
    executor->execute_async([block = block, updates, executor,
                             promise = std::make_shared<td::Promise<td::Unit>>(ig.get_promise())]() mutable {
      block::gen::Block::Record rec;
      if (!block::gen::unpack_cell(block->root_cell(), rec)) {
        promise->set_error(td::Status::Error("cannot unpack Block record"));
        return;
      }
      bool spec;
      vm::CellSlice update_cs = vm::load_cell_slice_special(rec.state_update, spec);
      if (update_cs.special_type() != vm::CellTraits::SpecialType::MerkleUpdate) {
        promise->set_error(td::Status::Error("invalid Merkle update in block"));
        return;
      }
      td::Ref<vm::Cell> new_state_root = update_cs.prefetch_ref(1);
      td::HashMap<vm::CellHash, int> visited;
      PermanentCellDbUpdate update{.block_id = block->block_id(),
                                   .state_root_hash = new_state_root->get_hash(0).bits()};
      std::function<void(const td::Ref<vm::Cell>&, int)> dfs = [&](const td::Ref<vm::Cell>& cell, int merkle_depth) {
        int& vis = visited[cell->get_hash()];
        if (vis & (1 << merkle_depth)) {
          return;
        }
        vis |= (1 << merkle_depth);
        vm::CellSlice cs{vm::NoVm(), cell};
        if (cs.special_type() == vm::CellTraits::SpecialType::PrunnedBranch && cell->get_level() == merkle_depth + 1) {
          return;
        }
        update.to_store.emplace_back(
            cell->get_hash(merkle_depth),
            vm::CellStorer::serialize_value(1 << 29, cell->load_cell().move_as_ok().data_cell, false, merkle_depth));
        merkle_depth = cs.child_merkle_depth(merkle_depth);
        for (unsigned i = 0; i < cs.size_refs(); ++i) {
          dfs(cs.prefetch_ref(i), merkle_depth);
        }
      };
      dfs(new_state_root, 0);
      executor->execute_sync(
          [update = std::move(update), updates = std::move(updates), promise = std::move(promise)]() mutable {
            updates->push_back(std::move(update));
            promise->set_result(td::Unit());
          });
    });
  }

  ig.add_promise([updates, promise = std::move(promise)](td::Result<td::Unit> R) mutable {
    TRY_STATUS_PROMISE(promise, R.move_as_status());
    promise.set_value(std::move(*updates));
  });
}

static constexpr int GC_SPLIT_DEPTH = 8;

std::vector<std::pair<BlockIdExt, td::Ref<vm::Cell>>> split_state_for_celldb_gc(BlockIdExt block_id,
                                                                                td::Ref<vm::Cell> state_root) {
  int shard_prefix_length = block_id.shard_full().pfx_len();
  if (shard_prefix_length >= GC_SPLIT_DEPTH) {
    return {};
  }

  block::gen::ShardStateUnsplit::Record state;
  if (!tlb::unpack_cell(state_root, state)) {
    LOG(ERROR) << "GC split : failed to parse state " << block_id.to_str();
    return {};
  }

  std::vector<std::pair<BlockIdExt, td::Ref<vm::Cell>>> result;
  vm::AugmentedDictionary accounts{
      vm::load_cell_slice_ref(state.accounts),
      256,
      block::tlb::aug_ShardAccounts,
      false,
  };

  // Build account dict parts
  ShardId effective_shard = block_id.id.shard ^ (1ULL << (63 - shard_prefix_length)) ^ (1ULL << (63 - GC_SPLIT_DEPTH));
  ShardId increment = 1ULL << (64 - GC_SPLIT_DEPTH);

  for (int i = 0; i < (1 << (GC_SPLIT_DEPTH - shard_prefix_length)); ++i, effective_shard += increment) {
    td::BitArray<64> prefix;
    prefix.store_ulong(effective_shard);
    auto account_dict_part = accounts;
    account_dict_part.cut_prefix_subdict(prefix.bits(), GC_SPLIT_DEPTH);
    if (!account_dict_part.is_empty()) {
      BlockIdExt new_block_id = block_id;
      new_block_id.id.shard = effective_shard;
      result.emplace_back(new_block_id, account_dict_part.get_wrapped_dict_root());
    }
  }
  return result;
}

}  // namespace ton::validator
