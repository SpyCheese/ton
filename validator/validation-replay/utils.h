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
#include "interfaces/validator-manager.h"

namespace ton::validator {

enum class ReplayMode { collate, validate, both };

std::vector<std::string> tokenize(const std::string& s);
td::Result<ReplayMode> parse_mode(const std::string& s);
std::string mode_to_str(ReplayMode mode);

struct UnpackedBlock {
  std::vector<BlockIdExt> prev;
  BlockIdExt mc_block_id;
  bool after_split;
  Ed25519_PublicKey creator;
  BlockSeqno min_mc_ref_seqno;
  td::Bits256 rand_seed;
  UnixTime gen_utime;

  std::vector<Ref<ExtMessage>> ext_msgs;
};

UnpackedBlock unpack_block(Ref<BlockData> block);

td::actor::Task<std::vector<Ref<ShardTopBlockDescription>>> get_shard_block_descriptions(
    Ref<MasterchainState> new_state, Ref<MasterchainState> prev_state, td::actor::ActorId<ValidatorManager> manager);
Ref<vm::Cell> create_collated_data_shard_block_descr(const std::vector<Ref<ShardTopBlockDescription>>& shard_blocks);

td::actor::Task<ConstBlockHandle> get_block_by_id(td::actor::ActorId<ValidatorManager> manager, BlockId block_id);

td::actor::Task<> process_all_blocks(td::actor::ActorId<ValidatorManager> manager, BlockSeqno mc_seqno_start,
                                     BlockSeqno mc_seqno_end, bool with_shards,
                                     std::function<td::actor::Task<>(ConstBlockHandle, BlockSeqno)> f);

}  // namespace ton::validator
