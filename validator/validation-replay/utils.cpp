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
#include "ton/ton-io.hpp"

#include "block-auto.h"
#include "block-parse.h"
#include "fabric.h"
#include "top-shard-descr.hpp"
#include "utils.h"

namespace ton::validator {

std::vector<std::string> tokenize(const std::string& s) {
  std::vector<std::string> tokens;
  for (size_t i = 0; i < s.size();) {
    while (i < s.size() && std::isspace(s[i])) {
      ++i;
    }
    if (i == s.size()) {
      break;
    }
    size_t start = i;
    while (i < s.size() && !std::isspace(s[i])) {
      ++i;
    }
    tokens.push_back(s.substr(start, i - start));
  }
  return tokens;
}

td::Result<ReplayMode> parse_mode(const std::string& s) {
  if (s == "collate") {
    return ReplayMode::collate;
  }
  if (s == "validate") {
    return ReplayMode::validate;
  }
  if (s == "both") {
    return ReplayMode::both;
  }
  return td::Status::Error(PSTRING() << "invalid mode " << s);
}

std::string mode_to_str(ReplayMode mode) {
  switch (mode) {
    case ReplayMode::collate:
      return "collate";
    case ReplayMode::validate:
      return "validate";
    case ReplayMode::both:
      return "both";
  }
  UNREACHABLE();
}

UnpackedBlock unpack_block(Ref<BlockData> block) {
  UnpackedBlock unpacked;
  block::unpack_block_prev_blk_try(block->root_cell(), block->block_id(), unpacked.prev, unpacked.mc_block_id,
                                   unpacked.after_split)
      .ensure();

  block::gen::Block::Record rec;
  block::gen::BlockInfo::Record info;
  block::gen::BlockExtra::Record extra;
  CHECK(block::gen::unpack_cell(block->root_cell(), rec));
  CHECK(block::gen::unpack_cell(rec.info, info));
  CHECK(block::gen::unpack_cell(rec.extra, extra));

  unpacked.creator = Ed25519_PublicKey{extra.created_by};
  unpacked.min_mc_ref_seqno = info.min_ref_mc_seqno;
  unpacked.rand_seed = extra.rand_seed;
  unpacked.gen_utime = info.gen_utime;

  std::vector<std::pair<LogicalTime, Ref<ExtMessage>>> ext_msgs_lt;
  vm::AugmentedDictionary in_msg_dict{vm::load_cell_slice_ref(extra.in_msg_descr), 256,
                                      block::tlb::aug_InMsgDescrDefault};
  in_msg_dict.check_for_each_extra([&](Ref<vm::CellSlice> value, Ref<vm::CellSlice>, td::ConstBitPtr, int) {
    int tag = block::gen::t_InMsg.get_tag(*value);
    if (tag == block::gen::InMsg::msg_import_ext) {
      block::gen::InMsg::Record_msg_import_ext rec;
      CHECK(block::gen::csr_unpack(value, rec));
      auto data = vm::std_boc_serialize(rec.msg).move_as_ok();
      auto msg = create_ext_message(std::move(data), block::SizeLimitsConfig::ExtMsgLimits{}).move_as_ok();

      block::gen::Transaction::Record trans;
      CHECK(block::gen::unpack_cell(rec.transaction, trans));
      ext_msgs_lt.emplace_back(trans.lt, msg);
    }
    return true;
  });
  std::sort(ext_msgs_lt.begin(), ext_msgs_lt.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  for (auto& [_, m] : ext_msgs_lt) {
    unpacked.ext_msgs.push_back(m);
  }

  return unpacked;
}

td::actor::Task<std::vector<Ref<ShardTopBlockDescription>>> get_shard_block_descriptions(
    Ref<MasterchainState> new_state, Ref<MasterchainState> prev_state, td::actor::ActorId<ValidatorManager> manager) {
  std::set<BlockIdExt> prev_top_blocks;
  for (const auto& desc : prev_state->get_shards()) {
    prev_top_blocks.insert(desc->top_block_id());
  }
  std::vector<Ref<ShardTopBlockDescription>> result;
  for (const auto& desc : new_state->get_shards()) {
    if (prev_top_blocks.contains(desc->top_block_id())) {
      continue;
    }

    std::vector<Ref<vm::Cell>> proof_roots;
    BlockIdExt cur_block_id = desc->top_block_id();
    while (true) {
      auto handle = co_await td::actor::ask(manager, &ValidatorManager::get_block_handle, cur_block_id, false);
      auto data = co_await td::actor::ask(manager, &ValidatorManager::get_block_proof_link, handle);
      auto root = CO_TRY(vm::std_boc_deserialize(data));
      block::gen::BlockProof::Record proof;
      CHECK(block::gen::unpack_cell(root, proof));
      proof_roots.push_back(proof.root);

      if (cur_block_id.seqno() == 1) {
        break;
      }
      CHECK(handle->inited_prev());
      if (handle->prev().size() == 2) {
        break;
      }
      cur_block_id = handle->one_prev(true);
      if (prev_top_blocks.contains(cur_block_id)) {
        break;
      }
    }

    int n = (int)proof_roots.size();
    CHECK(n <= 8);
    Ref<vm::Cell> root;
    for (int i = n - 1; i > 0; i--) {
      vm::CellBuilder cb;
      CHECK(cb.store_ref_bool(proof_roots[i]) && (root.is_null() || cb.store_ref_bool(root)) && cb.finalize_to(root));
    }

    Ref<block::ValidatorSet> validator_set = prev_state->get_validator_set(desc->shard());
    vm::CellBuilder cb2;
    Ref<vm::Cell> signatures;
    CHECK(cb2.store_long_bool(0x11, 8)  // block_signatures#11
          && cb2.store_long_bool(validator_set->get_validator_set_hash(),
                                 32)  // validator_info$_ validator_set_hash_short:uint32
          && cb2.store_long_bool(validator_set->get_catchain_seqno(),
                                 32)     //   validator_set_ts:uint32
          && cb2.store_long_bool(0, 32)  // sig_count:uint32
          && cb2.store_long_bool(0, 64)  // sig_weight:uint32
          && cb2.store_bool_bool(false)  // (HashmapE 16 CryptoSignaturePair)
          && cb2.finalize_to(signatures));

    vm::CellBuilder cb;
    Ref<vm::Cell> td_cell;
    CHECK(cb.store_long_bool(0xd5, 8)                                 // top_block_descr#d5
          && block::tlb::t_BlockIdExt.pack(cb, desc->top_block_id())  // proof_for:BlockIdExt
          && cb.store_bool_bool(true)                                 // signatures:(Maybe
          && cb.store_ref_bool(signatures)                            //   ^BlockSignatures)
          && cb.store_long_bool(n, 8)                                 // len:(## 8)
          && n <= 8                                                   // { len <= 8 }
          && cb.store_ref_bool(proof_roots[0])                        // chain:(ProofChain len)
          && (root.is_null() || cb.store_ref_bool(std::move(root))) && cb.finalize_to(td_cell));
    CHECK(block::gen::t_TopBlockDescr.validate_ref(td_cell));
    auto top_block_descr_data = vm::std_boc_serialize(td_cell, 0).ensure().move_as_ok();
    result.push_back(ShardTopBlockDescrQ::fetch(std::move(top_block_descr_data), true).ensure().move_as_ok());
  }
  co_return result;
}

Ref<vm::Cell> create_collated_data_shard_block_descr(const std::vector<Ref<ShardTopBlockDescription>>& shard_blocks) {
  vm::Dictionary dict{96};
  for (const auto& descr : shard_blocks) {
    auto shard = descr->shard();
    td::BitArray<96> key;
    key.bits().store_int(shard.workchain, 32);
    (key.bits() + 32).store_uint(shard.shard, 64);
    CHECK(dict.set_ref(key, Ref<ShardTopBlockDescrQ>{descr}->get_root(), vm::Dictionary::SetMode::Add));
  }
  block::gen::TopBlockDescrSet::Record rec;
  Ref<vm::Cell> cell;
  rec.collection = std::move(dict).extract_root();
  CHECK(tlb::pack_cell(cell, rec));
  return cell;
}

td::actor::Task<ConstBlockHandle> get_block_by_id(td::actor::ActorId<ValidatorManager> manager, BlockId block_id) {
  auto handle = co_await td::actor::ask(manager, &ValidatorManager::get_block_by_seqno_from_db,
                                        AccountIdPrefixFull{block_id.workchain, block_id.shard}, block_id.seqno);
  if (handle->id().id != block_id) {
    co_return td::Status::Error(PSTRING() << "Block " << block_id << " not in db");
  }
  co_return handle;
}

td::actor::Task<> process_all_blocks(td::actor::ActorId<ValidatorManager> manager, BlockSeqno mc_seqno_start,
                                     BlockSeqno mc_seqno_end, bool with_shards,
                                     std::function<td::actor::Task<>(ConstBlockHandle, BlockSeqno)> f) {
  auto mc_handle = co_await get_block_by_id(manager, BlockId{masterchainId, shardIdAll, mc_seqno_start});
  Ref<MasterchainState> mc_state{
      co_await td::actor::ask(manager, &ValidatorManager::get_shard_state_from_db, mc_handle)};

  while (mc_handle->id().seqno() < mc_seqno_end) {
    if (!mc_handle->inited_next()) {
      co_return td::Status::Error(PSTRING() << "MC block " << mc_handle->id().seqno() << " has no known next");
    }
    auto next_mc_handle =
        co_await td::actor::ask(manager, &ValidatorManager::get_block_handle, mc_handle->one_next(true), false);
    Ref<MasterchainState> next_mc_state{
        co_await td::actor::ask(manager, &ValidatorManager::get_shard_state_from_db, next_mc_handle)};

    std::set<BlockIdExt> visited_shard_blocks;
    for (const auto& desc : mc_state->get_shards()) {
      visited_shard_blocks.insert(desc->top_block_id());
    }
    std::function<td::actor::Task<>(BlockIdExt)> dfs = [&](BlockIdExt block_id) -> td::actor::Task<> {
      if (!visited_shard_blocks.insert(block_id).second || block_id.seqno() == 0) {
        co_return {};
      }
      auto handle = co_await td::actor::ask(manager, &ValidatorManager::get_block_handle, block_id, false);
      if (!handle->inited_prev()) {
        co_return td::Status::Error(PSTRING() << "Block " << block_id.id.to_str() << " has no known prev");
      }
      for (const BlockIdExt& prev : handle->prev()) {
        co_await dfs(prev);
      }
      co_await f(handle, mc_handle->id().seqno());
      co_return {};
    };
    if (with_shards) {
      for (const auto& desc : next_mc_state->get_shards()) {
        co_await dfs(desc->top_block_id());
      }
    }
    mc_handle = next_mc_handle;
    mc_state = next_mc_state;
    co_await f(mc_handle, mc_handle->id().seqno());
  }
  co_return {};
}

}  // namespace ton::validator
