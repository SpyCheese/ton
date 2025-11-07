/*
    This file is part of TON Blockchain source code.

    TON Blockchain is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License
    as published by the Free Software Foundation; either version 2
    of the License, or (at your option) any later version.

    TON Blockchain is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with TON Blockchain.  If not, see <http://www.gnu.org/licenses/>.
*/
#include <fstream>
#include <iostream>
#include <ton/ton-tl.hpp>

#include "adnl/adnl.h"
#include "auto/tl/lite_api.h"
#include "auto/tl/ton_api.h"
#include "auto/tl/ton_api_json.h"
#include "block/block-auto.h"
#include "block/block-parse.h"
#include "block/mc-config.h"
#include "db/celldb.hpp"
#include "db/fileref.hpp"
#include "lite-client/ext-client.h"
#include "td/actor/MultiPromise.h"
#include "td/actor/actor.h"
#include "td/utils/FileLog.h"
#include "td/utils/OptionParser.h"
#include "td/utils/Random.h"
#include "td/utils/filesystem.h"
#include "td/utils/overloaded.h"
#include "td/utils/port/IPAddress.h"
#include "td/utils/port/path.h"
#include "td/utils/port/signals.h"
#include "td/utils/tl_storers.h"
#include "tl-utils/lite-utils.hpp"
#include "ton/lite-tl.hpp"
#include "vm/boc.h"
#include "vm/cells/MerkleProof.h"

#include "fabric.h"
#include "git.h"

using namespace ton;

static std::string db_root;
std::vector<char*> in_files;

class MirrorPrepareState : public td::actor::Actor {
 public:
  void start_up() override {
    LOG_IF(FATAL, db_root.empty()) << "celldb directory is not set";
    LOG_IF(FATAL, in_files.empty()) << "input files are not set";

    td::Ref<validator::ValidatorManagerOptions> opts = validator::ValidatorManagerOptions::create(
        BlockIdExt{masterchainId, shardIdAll, 0, RootHash::zero(), FileHash::zero()},
        BlockIdExt{masterchainId, shardIdAll, 0, RootHash::zero(), FileHash::zero()});
    cell_db_ = td::actor::create_actor<validator::CellDb>("celldb", td::actor::ActorId<validator::RootDb>{}, db_root,
                                                          std::move(opts));

    LOG(ERROR) << "Storing " << in_files.size() << " states";
    store_state(0);
  }

  BlockIdExt get_block_id(td::Ref<vm::Cell> state_root) {
    block::gen::ShardStateUnsplit::Record state;
    CHECK(block::tlb::unpack_cell(state_root, state));
    auto shard = block::ShardId{state.shard_id};
    return BlockIdExt{shard.workchain_id, (ShardId)shard.shard_pfx, state.seq_no, state_root->get_hash().bits(),
                      td::Bits256::zero()};
  }

  void store_state(size_t idx) {
    if (idx == in_files.size()) {
      finish();
      return;
    }

    td::Slice path{in_files[idx], strlen(in_files[idx])};
    auto pos = path.rfind('/');
    td::Slice name = pos == td::Slice::npos ? path : path.substr(pos + 1);
    auto ref = validator::FileReferenceShort::create(name.str()).ensure().move_as_ok();
    ShardIdFull shard = ref.shard();
    LOG(ERROR) << "Storing state #" << idx + 1 << "/" << in_files.size() << " : " << path << " " << shard.to_str();

    auto root = vm::std_boc_deserialize(td::read_file((path.str())).ensure().move_as_ok()).ensure().move_as_ok();
    td::actor::send_closure(cell_db_, &validator::CellDb::store_cell, get_block_id(root), root,
                            [=, SelfId = actor_id(this)](td::Result<td::Ref<vm::DataCell>> R) {
                              td::actor::send_closure(SelfId, &MirrorPrepareState::stored, R.ensure().move_as_ok(), idx,
                                                      shard);
                            });
  }

  void stored(td::Ref<vm::Cell> root, size_t idx, ShardIdFull shard) {
    states_.emplace_back(shard, root);
    store_state(idx + 1);
  }

  void finish() {
    LOG(ERROR) << "Stored all states";
    std::sort(states_.begin(), states_.end(),
              [](const std::pair<ShardIdFull, td::Ref<vm::Cell>>& a,
                 const std::pair<ShardIdFull, td::Ref<vm::Cell>>& b) { return a.first < b.first; });
    std::vector<std::pair<ShardIdFull, td::Ref<vm::Cell>>> st;
    for (const auto& p : states_) {
      st.push_back(p);
      while (st.size() >= 2 && shard_is_sibling(st[st.size() - 2].first, st[st.size() - 1].first)) {
        auto n = merge_states(st[st.size() - 2], st[st.size() - 1]);
        st.pop_back();
        st.pop_back();
        st.push_back(std::move(n));
      }
    }
    CHECK(st.size() == 1);
    CHECK(st[0].first.shard == shardIdAll);

    auto state = process_final_state(st[0].second);
    auto block_id = get_block_id(state);
    LOG(WARNING) << "block_id = " << block_id.to_str();
    td::actor::send_closure(cell_db_, &validator::CellDb::store_cell, block_id, state,
                            [=, SelfId = actor_id(this)](td::Result<td::Ref<vm::DataCell>> R) {
                              R.ensure();
                              LOG(ERROR) << "DONE";
                              exit(0);
                            });
  }

  std::pair<ShardIdFull, td::Ref<vm::Cell>> merge_states(const std::pair<ShardIdFull, td::Ref<vm::Cell>>& p1,
                                                         const std::pair<ShardIdFull, td::Ref<vm::Cell>>& p2) {
    ShardIdFull new_shard = shard_parent(p1.first);
    LOG(ERROR) << "Merging " << p1.first.to_str() << " and " << p2.first.to_str() << " into " << new_shard.to_str();

    block::ShardState s1, s2;
    s1.unpack_state(get_block_id(p1.second), p1.second).ensure();
    s2.unpack_state(get_block_id(p2.second), p2.second).ensure();
    s1.merge_with(s2);

    block::gen::ShardStateUnsplit::Record state;
    CHECK(block::tlb::unpack_cell(p1.second, state));
    vm::CellBuilder cb;
    CHECK(block::ShardId{s1.id_.shard_full()}.serialize(cb));
    state.shard_id = cb.as_cellslice_ref();
    state.seq_no = s1.id_.seqno();
    state.gen_utime = s1.utime_;
    state.gen_lt = s1.lt_;
    state.min_ref_mc_seqno = s1.min_ref_mc_seqno_;

    cb.reset();
    s1.out_msg_queue_->append_dict_to_bool(cb);
    s1.processed_upto_->pack(cb);
    s1.ihr_pending_->append_dict_to_bool(cb);
    state.out_msg_queue_info = cb.finalize();

    state.before_split = false;

    cb.reset();
    s1.account_dict_->append_dict_to_bool(cb);
    state.accounts = cb.finalize();

    state.r1.overload_history = s1.overload_history_;
    state.r1.underload_history = s1.underload_history_;
    state.r1.total_balance = s1.total_balance_.pack();
    state.r1.total_validator_fees = s1.total_validator_fees_.pack();
    state.r1.libraries = s1.shard_libraries_->get_root();

    cb.reset();
    cb.store_long(1, 1);
    cb.store_long_bool(s1.mc_blk_lt_, 64);           // end_lt:uint64
    cb.store_long_bool(s1.mc_blk_ref_.seqno(), 32);  // seq_no:uint32
    cb.store_bits_bool(s1.mc_blk_ref_.root_hash);    // root_hash:bits256
    cb.store_bits_bool(s1.mc_blk_ref_.file_hash);    // file_hash:bits256
    state.r1.master_ref = cb.as_cellslice_ref();

    td::Ref<vm::Cell> state_root;
    CHECK(block::tlb::pack_cell(state_root, state));
    return {new_shard, state_root};
  }

  td::Ref<vm::Cell> process_final_state(td::Ref<vm::Cell> state_root) {
    block::gen::ShardStateUnsplit::Record state;
    CHECK(block::tlb::unpack_cell(state_root, state));

    state.seq_no = 0;
    state.vert_seq_no = 0;
    state.min_ref_mc_seqno = -1;
    state.before_split = false;
    state.r1.master_ref = vm::CellBuilder{}.store_zeroes(1).as_cellslice_ref();
    LOG(WARNING) << "lt = " << state.gen_lt << "\n";
    state.out_msg_queue_info = vm::CellBuilder{}.store_zeroes(67).finalize();

    CHECK(block::tlb::pack_cell(state_root, state));
    return state_root;
  }

 private:
  td::actor::ActorOwn<validator::CellDb> cell_db_;
  std::vector<std::pair<ShardIdFull, td::Ref<vm::Cell>>> states_;
};

int main(int argc, char* argv[]) {
  SET_VERBOSITY_LEVEL(verbosity_INFO);

  td::unique_ptr<td::LogInterface> logger_;
  SCOPE_EXIT {
    td::log_interface = td::default_log_interface;
  };

  td::OptionParser p;
  p.set_description("a b c d e f g\n");
  p.add_option('v', "verbosity", "set verbosity level", [&](td::Slice arg) {
    int v = VERBOSITY_NAME(FATAL) + (td::to_integer<int>(arg));
    SET_VERBOSITY_LEVEL(v);
  });
  p.add_option('h', "help", "print help", [&]() {
    char b[10240];
    td::StringBuilder sb(td::MutableSlice{b, 10000});
    sb << p;
    std::cout << sb.as_cslice().c_str();
    std::exit(2);
  });
  p.add_option('D', "db", "celldb directory", [&](td::Slice arg) { db_root = arg.str(); });

  auto res = p.run(argc, argv);
  res.ensure();
  in_files = res.move_as_ok();
  td::actor::Scheduler scheduler({3});

  scheduler.run_in_context([&] { td::actor::create_actor<MirrorPrepareState>("main").release(); });
  while (scheduler.run(1)) {
  }
}
