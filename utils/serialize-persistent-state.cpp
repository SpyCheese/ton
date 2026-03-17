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
#include "td/utils/port/rlimit.h"
#include "td/utils/port/signals.h"
#include "td/utils/tl_storers.h"
#include "tl-utils/lite-utils.hpp"
#include "ton/lite-tl.hpp"
#include "vm/boc.h"
#include "vm/cells/MerkleProof.h"

#include "fabric.h"
#include "git.h"

using namespace ton;

static BlockIdExt in_mc_block_id;
static std::vector<ShardIdFull> in_shards;
static std::vector<std::pair<ShardIdFull, std::string>> in_preload_states;
static std::string db_root;
static std::string out_dir;
static td::optional<td::uint32> force_split_depth;

static bool need_serialize(const ShardIdFull& shard) {
  return in_shards.empty() || std::any_of(in_shards.begin(), in_shards.end(), [&](const ShardIdFull& in_shard) {
           return shard_intersects(in_shard, shard);
         });
}

class CachedCellDbReader : public vm::CellDbReader {
 public:
  explicit CachedCellDbReader(std::shared_ptr<vm::CellDbReader> parent, vm::CellHashSet cache = {})
      : parent_(std::move(parent)), cache_(std::move(cache)) {
  }
  td::Result<td::Ref<vm::DataCell>> load_cell(td::Slice hash) override {
    ++total_reqs_;
    DCHECK(hash.size() == 32);
    auto it = cache_.find(hash);
    if (it != cache_.end()) {
      ++cached_reqs_;
      TRY_RESULT(loaded_cell, (*it)->load_cell());
      return loaded_cell.data_cell;
    }
    return parent_->load_cell(hash);
  }
  td::Result<std::vector<td::Ref<vm::DataCell>>> load_bulk(td::Span<td::Slice> hashes) override {
    total_reqs_ += hashes.size();
    std::vector<td::Slice> missing_hashes;
    std::vector<size_t> missing_indices;
    std::vector<td::Ref<vm::DataCell>> res(hashes.size());
    for (size_t i = 0; i < hashes.size(); i++) {
      auto it = cache_.find(hashes[i]);
      if (it != cache_.end()) {
        ++cached_reqs_;
        TRY_RESULT(loaded_cell, (*it)->load_cell());
        res[i] = loaded_cell.data_cell;
        continue;
      }
      missing_hashes.push_back(hashes[i]);
      missing_indices.push_back(i);
    }
    if (missing_hashes.empty()) {
      return std::move(res);
    }
    TRY_RESULT(missing_cells, parent_->load_bulk(missing_hashes));
    for (size_t i = 0; i < missing_indices.size(); i++) {
      res[missing_indices[i]] = missing_cells[i];
    }
    return res;
  };
  void print_stats() const {
    LOG(WARNING) << "CachedCellDbReader stats : " << total_reqs_ << " reads, " << cached_reqs_ << " cached, "
                 << bulk_reqs_ << " bulk reqs";
  }

  void add_new_cells(const td::Ref<vm::Cell>& cell) {
    if (!cell->is_loaded()) {
      return;
    }
    if (parent_->load_cell(cell->get_hash().as_slice()).is_ok()) {
      return;
    }

    auto [_, inserted] = cache_.insert(cell);
    if (!inserted) {
      return;
    }

    vm::CellSlice cs{vm::NoVm{}, cell};
    for (unsigned i = 0; i < cs.size_refs(); ++i) {
      add_new_cells(cs.prefetch_ref(i));
    }
  }

 private:
  std::shared_ptr<vm::CellDbReader> parent_;
  vm::CellHashSet cache_;

  td::uint64 total_reqs_ = 0;
  td::uint64 cached_reqs_ = 0;
  td::uint64 bulk_reqs_ = 0;
};

struct SerializablePart {
  validator::PersistentStateType type;
  td::Ref<vm::Cell> cell;
};

static std::vector<SerializablePart> split_shard_state(ShardId shard_id, td::Ref<vm::Cell> shard_state_cell,
                                                       int split_depth) {
  CHECK(split_depth <= 63);
  int shard_prefix_length = shard_pfx_len(shard_id);
  if (shard_prefix_length >= static_cast<int>(split_depth)) {
    return {{validator::UnsplitStateType{}, std::move(shard_state_cell)}};
  }

  block::gen::ShardStateUnsplit::Record unsplit_shard_state;
  bool rc = tlb::unpack_cell(shard_state_cell, unsplit_shard_state);
  CHECK(rc);

  std::vector<SerializablePart> result;

  auto unwrapped_accounts_root = unsplit_shard_state.accounts;
  auto accounts_cut = std::make_shared<vm::CellUsageTree>();
  auto accounts_root = vm::UsageCell::create(unwrapped_accounts_root, accounts_cut->root_ptr());

  // NOTE: Ref<Cell> constructor expects caller to unwrap HashMapAugE.
  vm::AugmentedDictionary accounts{
      vm::load_cell_slice_ref(accounts_root),
      256,
      block::tlb::aug_ShardAccounts,
      false,
  };

  // Build account dict parts
  ShardId effective_shard = shard_id ^ (1ULL << (63 - shard_prefix_length)) ^ (1ULL << (63 - split_depth));
  ShardId increment = 1ULL << (64 - split_depth);

  for (int i = 0; i < (1 << (split_depth - shard_prefix_length)); ++i, effective_shard += increment) {
    td::BitArray<64> prefix;
    prefix.store_ulong(effective_shard);
    auto account_dict_part = accounts;
    account_dict_part.cut_prefix_subdict(prefix.bits(), split_depth);

    if (!account_dict_part.is_empty()) {
      result.push_back({validator::SplitAccountStateType{effective_shard}, account_dict_part.get_wrapped_dict_root()});
    }
  }

  auto accounts_proof = vm::MerkleProof::generate_raw(unwrapped_accounts_root, accounts_cut.get());

  // Build header
  unsplit_shard_state.accounts = accounts_proof;
  vm::CellBuilder unsplit_shard_state_cb;
  rc = tlb::pack(unsplit_shard_state_cb, unsplit_shard_state);
  CHECK(rc);

  auto header = unsplit_shard_state_cb.finalize();
  CHECK(header->get_level() <= 1 && header->get_hash(0) == shard_state_cell->get_hash());

  result.push_back({validator::SplitPersistentStateType{}, vm::CellBuilder::create_merkle_proof(header)});

  return result;
}

class SerializePersistentState : public td::actor::Actor {
 public:
  void start_up() override {
    LOG_IF(FATAL, in_mc_block_id.id.workchain == workchainIdNotYet) << "masterchain block id is not set";
    LOG_IF(FATAL, !in_mc_block_id.is_masterchain_ext()) << "block id is not a valid masterchain id";
    LOG_IF(FATAL, db_root.empty()) << "celldb directory is not set";

    td::Ref<validator::ValidatorManagerOptions> opts = validator::ValidatorManagerOptions::create(
        BlockIdExt{masterchainId, shardIdAll, 0, RootHash::zero(), FileHash::zero()},
        BlockIdExt{masterchainId, shardIdAll, 0, RootHash::zero(), FileHash::zero()});
    opts.write().set_readonly_celldb(true);
    cell_db_ = td::actor::create_actor<validator::CellDb>("celldb", td::actor::ActorId<validator::RootDb>{}, db_root,
                                                          std::move(opts));

    td::actor::send_closure(cell_db_, &validator::CellDb::get_cell_db_reader,
                            td::promise_send_closure(actor_id(this), &SerializePersistentState::got_cell_db_reader));
  }

  void got_cell_db_reader(td::Result<std::shared_ptr<vm::CellDbReader>> R) {
    R.ensure();
    cell_db_reader_ = R.move_as_ok();
    td::actor::send_closure(cell_db_, &validator::CellDb::load_state_root, in_mc_block_id,
                            td::promise_send_closure(actor_id(this), &SerializePersistentState::got_masterchain_state));
  }

  void got_masterchain_state(td::Result<td::Ref<vm::DataCell>> R) {
    R.ensure();
    auto r_state = validator::create_shard_state(in_mc_block_id, R.move_as_ok());
    r_state.ensure();
    td::Ref<validator::MasterchainState> state{r_state.move_as_ok()};
    LOG(ERROR) << "Loaded masterchain state " << in_mc_block_id.to_str();
    if (need_serialize(in_mc_block_id.shard_full())) {
      LOG(ERROR) << "State to serialize: " << in_mc_block_id.to_str();
    }
    for (const auto& shard : state->get_shards()) {
      if (need_serialize(shard->shard())) {
        td::uint32 split_depth = force_split_depth ? force_split_depth.value()
                                                   : state->persistent_state_split_depth(shard->shard().workchain);
        shards_.emplace_back(shard->top_block_id(), split_depth);
        LOG(ERROR) << "State to serialize: " << shard->top_block_id().to_str() << ", split_depth=" << split_depth;
      }
    }
    if (shards_.empty() && in_mc_block_id.seqno() == 0) {
      for (const auto& [_, wc] : state->get_workchain_list()) {
        shards_.emplace_back(BlockIdExt{wc->workchain, shardIdAll, 0, wc->zerostate_root_hash, wc->zerostate_file_hash},
                             0);
        LOG(ERROR) << "State to serialize: " << shards_.back().block_id.to_str() << ", split_depth=0";
      }
    }

    if (need_serialize(in_mc_block_id.shard_full())) {
      serialize_state(in_mc_block_id, state->root_cell(), 0);
    }
    get_next_shard();
  }

  void get_next_shard() {
    if (shard_idx_ == shards_.size()) {
      LOG(ERROR) << "Done";
      auto r_stat = td::mem_stat();
      if (r_stat.is_error()) {
        LOG(WARNING) << "Mem stat: " << r_stat.error();
      } else {
        LOG(WARNING) << "Peak memory usage: " << r_stat.ok().resident_size_peak_;
      }
      exit(0);
    }
    td::actor::send_closure(cell_db_, &validator::CellDb::load_state_root, shards_[shard_idx_].block_id,
                            td::promise_send_closure(actor_id(this), &SerializePersistentState::got_shard_state));
  }

  void got_shard_state(td::Result<td::Ref<vm::DataCell>> R) {
    R.ensure();
    serialize_state(shards_[shard_idx_].block_id, R.move_as_ok(), shards_[shard_idx_].split_depth);
    ++shard_idx_;
    get_next_shard();
  }

  void serialize_state(const BlockIdExt& block_id, td::Ref<vm::Cell> root_cell, td::uint32 split_depth) {
    std::vector<SerializablePart> parts = split_shard_state(block_id.id.shard, root_cell, split_depth);
    LOG(ERROR) << "Serializing state " << block_id.to_str() << " (" << parts.size() << " parts)";

    for (auto& [type, cell] : parts) {
      validator::FileReferenceShort file_id;
      type.visit(td::overloaded(
          [&](const validator::UnsplitStateType&) {
            if (in_mc_block_id.seqno() == 0) {
              file_id = validator::fileref::ZeroStateShort{block_id.id.workchain, block_id.file_hash};
            } else {
              file_id = validator::fileref::PersistentStateShort::create(block_id, in_mc_block_id);
            }
          },
          [&](const validator::SplitAccountStateType& account_state) {
            file_id = validator::fileref::SplitAccountState::create(block_id, in_mc_block_id,
                                                                    account_state.effective_shard_id);
          },
          [&](const validator::SplitPersistentStateType&) {
            file_id = validator::fileref::SplitPersistentState::create(block_id, in_mc_block_id);
          }));
      std::string file = PSTRING() << out_dir << "/" << file_id.filename_short();

      LOG(ERROR) << "serializing shard state " << block_id.id.to_str() << " ("
                 << persistent_state_type_to_string(block_id.shard_full(), type) << ") to " << file;
      prepare_cache(block_id.shard_full(), type);
      cached_cell_db_reader_->add_new_cells(cell);

      std::string file_tmp = file + ".tmp";
      td::mkpath(out_dir + "/").ensure();
      if (in_mc_block_id.seqno() == 0 && split_depth == 0) {
        td::BufferSlice data = vm::std_boc_serialize(cell, 31).move_as_ok();
        td::write_file(file_tmp, data).ensure();
      } else {
        auto r_fd = td::FileFd::open(file_tmp, td::FileFd::Write | td::FileFd::Create | td::FileFd::Truncate);
        auto fd = r_fd.move_as_ok();
        r_fd.ensure();
        vm::boc_serialize_to_file_large(cached_cell_db_reader_, cell->get_hash(), fd, 31, {}).ensure();
        fd.sync().ensure();
        fd.close();
      }
      td::rename(file_tmp, file).ensure();
      cached_cell_db_reader_->print_stats();
      LOG(ERROR) << "Written file " << file;
    }
  }

  void prepare_cache(ShardIdFull shard, const validator::PersistentStateType& type) {
    if (type.get_offset() == type.offset<validator::SplitPersistentStateType>()) {
      // Header of a split state is small, so not caching it is fine.
      cached_cell_db_reader_ = std::make_shared<CachedCellDbReader>(cell_db_reader_);
      return;
    }
    if (type.get_offset() == type.offset<validator::SplitAccountStateType>()) {
      shard = {shard.workchain, type.get<validator::SplitAccountStateType>().effective_shard_id};
    }

    std::vector<std::string> new_cache_files;
    for (const auto& [prev_shard, file] : in_preload_states) {
      if (shard_intersects(shard, prev_shard)) {
        new_cache_files.push_back(file);
      }
    }
    if (new_cache_files.empty()) {
      LOG(WARNING) << "No cache files for shard " << shard.to_str();
      current_cache_files_.clear();
      cached_cell_db_reader_ = std::make_shared<CachedCellDbReader>(cell_db_reader_);
      return;
    }
    current_cache_files_ = new_cache_files;
    LOG(WARNING) << "Preloading previous persistent state for shard " << shard.to_str() << " ("
                 << new_cache_files.size() << " files)";
    vm::CellHashSet cells;
    std::function<void(td::Ref<vm::Cell>)> dfs = [&](td::Ref<vm::Cell> cell) {
      if (!cells.insert(cell).second) {
        return;
      }
      bool is_special;
      vm::CellSlice cs = vm::load_cell_slice_special(cell, is_special);
      for (unsigned i = 0; i < cs.size_refs(); ++i) {
        dfs(cs.prefetch_ref(i));
      }
    };
    for (const auto& file : new_cache_files) {
      auto r_data = td::read_file(file);
      if (r_data.is_error()) {
        LOG(WARNING) << "Preloading state file " << file << " : " << r_data.move_as_error();
        continue;
      }
      LOG(WARNING) << "Preloaded state file " << file << " : " << td::format::as_size(r_data.ok().size());
      auto r_root = vm::std_boc_deserialize(r_data.move_as_ok());
      if (r_root.is_error()) {
        LOG(WARNING) << "Deserialize error : " << r_root.move_as_error();
        continue;
      }
      r_data.clear();
      dfs(r_root.move_as_ok());
    }
    LOG(WARNING) << "Preloaded previous state: " << cells.size() << " cells";
    cached_cell_db_reader_ = std::make_shared<CachedCellDbReader>(cell_db_reader_, std::move(cells));
  }

 private:
  td::actor::ActorOwn<validator::CellDb> cell_db_;
  std::shared_ptr<vm::CellDbReader> cell_db_reader_;

  std::shared_ptr<CachedCellDbReader> cached_cell_db_reader_;
  std::vector<std::string> current_cache_files_;

  struct ShardInfo {
    BlockIdExt block_id;
    td::uint32 split_depth;

    ShardInfo() = default;
    ShardInfo(BlockIdExt block_id, td::uint32 split_depth) : block_id(block_id), split_depth(split_depth) {
    }
  };
  std::vector<ShardInfo> shards_;
  size_t shard_idx_ = 0;
};

static void add_preload_state(td::Slice arg) {
  auto walk_status = td::WalkPath::run(arg.str(), [&](td::CSlice path, td::WalkPath::Type type) {
    if (type == td::WalkPath::Type::NotDir) {
      auto idx = path.rfind('/');
      td::Slice name = idx == td::Slice::npos ? path : path.substr(idx + 1);
      auto R = validator::FileReferenceShort::create(name.str());
      if (R.is_error()) {
        return td::WalkPath::Action::Continue;
      }
      auto ref = R.move_as_ok();
      in_preload_states.emplace_back(ref.shard(), path.str());
      LOG(WARNING) << "State to preload: " << ref.shard().to_str() << " " << path;
    }
    return td::WalkPath::Action::Continue;
  });
}

int main(int argc, char* argv[]) {
  SET_VERBOSITY_LEVEL(verbosity_INFO);

  td::unique_ptr<td::LogInterface> logger_;
  SCOPE_EXIT {
    td::log_interface = td::default_log_interface;
  };

  LOG_STATUS(td::change_maximize_rlimit(td::RlimitType::nofile, 786432));

  td::OptionParser p;
  p.set_description("Serialize the specified shard state from celldb\n");
  p.add_option('v', "verbosity", "set verbosity level", [&](td::Slice arg) {
    int v = VERBOSITY_NAME(FATAL) + (td::to_integer<int>(arg));
    SET_VERBOSITY_LEVEL(v);
  });
  p.add_option('V', "version", "show build information", [&]() {
    std::cout << "prepare-ls-slice-config build information: [ Commit: " << GitMetadata::CommitSHA1()
              << ", Date: " << GitMetadata::CommitDate() << "]\n";
    std::exit(0);
  });
  p.add_option('h', "help", "print help", [&]() {
    char b[10240];
    td::StringBuilder sb(td::MutableSlice{b, 10000});
    sb << p;
    std::cout << sb.as_cslice().c_str();
    std::exit(2);
  });
  p.add_checked_option('m', "mc-block", "masterchain block id", [&](td::Slice arg) -> td::Status {
    std::string s = arg.str();
    TRY_RESULT_ASSIGN(in_mc_block_id, BlockIdExt::from_str(s));
    return td::Status::OK();
  });
  p.add_checked_option('s', "shard", "shard in format 0:8000000000000000 (default: all shards)",
                       [&](td::Slice arg) -> td::Status {
                         TRY_RESULT(shard, ShardIdFull::parse(arg));
                         if (!shard.is_valid_ext()) {
                           return td::Status::Error(PSTRING() << "invalid shard " << arg);
                         }
                         in_shards.push_back(shard);
                         return td::Status::OK();
                       });
  p.add_option('D', "db", "celldb directory", [&](td::Slice arg) { db_root = arg.str(); });
  p.add_option('o', "out-dir", "directory for output files (default: .)", [&](td::Slice arg) { out_dir = arg.str(); });
  p.add_option('p', "preload-state", "persistent state file or directory to preload (used to speed up serialization)",
               [&](td::Slice arg) { add_preload_state(arg); });
  p.add_checked_option('\0', "force-split-depth", "override persistent state split depth for shards",
                       [&](td::Slice arg) -> td::Status {
                         TRY_RESULT(x, td::to_integer_safe<td::uint32>(arg));
                         if (x >= 64) {
                           return td::Status::Error(PSTRING() << "too big split depth " << x);
                         }
                         force_split_depth = x;
                         return td::Status::OK();
                       });

  p.run(argc, argv).ensure();
  td::actor::Scheduler scheduler({3});

  scheduler.run_in_context([&] { td::actor::create_actor<SerializePersistentState>("main").release(); });
  while (scheduler.run(1)) {
  }
}
