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
#include "bench-simulate-serializer.h"
#include "common/delay.h"

namespace ton::validator {

void BenchSimulateSerializer::start_up() {
  LOG(ERROR) << "Starting BenchSimulateSerializer";
  get_masterchain_state();
}

void BenchSimulateSerializer::get_masterchain_state() {
  td::actor::send_closure(
      manager_, &ValidatorManager::get_top_masterchain_state,
      [SelfId = actor_id(this)](td::Result<td::Ref<MasterchainState>> R) {
        if (R.is_error() || R.ok()->get_seqno() == 0) {
          delay_action([=]() { td::actor::send_closure(SelfId, &BenchSimulateSerializer::get_masterchain_state); },
                       td::Timestamp::in(2.0));
        } else {
          td::actor::send_closure(SelfId, &BenchSimulateSerializer::got_masterchain_state, R.move_as_ok());
        }
      });
}

void BenchSimulateSerializer::got_masterchain_state(td::Ref<MasterchainState> state) {
  blocks_.clear();
  state_root_hashes_.clear();
  blocks_.push_back(state->get_block_id());
  state_root_hashes_.push_back(state->root_cell()->get_hash().bits());
  for (const auto& shard : state->get_shards()) {
    blocks_.push_back(shard->top_block_id());
  }
  get_shard_states();
}

void BenchSimulateSerializer::get_shard_states() {
  if (blocks_.size() == state_root_hashes_.size()) {
    get_celldb_reader();
    return;
  }
  size_t i = state_root_hashes_.size();
  td::actor::send_closure(
      manager_, &ValidatorManager::wait_block_state_short, blocks_[i], 0, td::Timestamp::in(5.0),
      [SelfId = actor_id(this)](td::Result<td::Ref<ShardState>> R) {
        if (R.is_error()) {
          delay_action([=]() { td::actor::send_closure(SelfId, &BenchSimulateSerializer::get_shard_states); },
                       td::Timestamp::in(2.0));
        } else {
          td::actor::send_closure(SelfId, &BenchSimulateSerializer::got_shard_state, R.move_as_ok());
        }
      });
}

void BenchSimulateSerializer::got_shard_state(td::Ref<ShardState> state) {
  state_root_hashes_.push_back(state->root_cell()->get_hash().bits());
  get_shard_states();
}

void BenchSimulateSerializer::get_celldb_reader() {
  td::actor::send_closure(
      manager_, &ValidatorManager::get_cell_db_reader,
      [SelfId = actor_id(this)](td::Result<std::shared_ptr<vm::CellDbReader>> R) {
        if (R.is_error()) {
          delay_action([=]() { td::actor::send_closure(SelfId, &BenchSimulateSerializer::get_celldb_reader); },
                       td::Timestamp::in(2.0));
        } else {
          td::actor::send_closure(SelfId, &BenchSimulateSerializer::got_celldb_reader, R.move_as_ok());
        }
      });
}

void BenchSimulateSerializer::got_celldb_reader(std::shared_ptr<vm::CellDbReader> reader) {
  for (size_t i = 0; i < blocks_.size(); ++i) {
    LOG(INFO) << "serializing " << blocks_[i].id.to_str();
    td::FileFd fd = td::FileFd::open(db_root_ + "/tmp/tmp-bench-shard-state",
                                     td::FileFd::Truncate | td::FileFd::Create | td::FileFd::Write)
                        .move_as_ok();
    td::Status S =
        vm::std_boc_serialize_to_file_large(reader, vm::CellHash::from_slice(state_root_hashes_[i].as_slice()), fd, 31);
    fd.close();
    if (S.is_error()) {
      LOG(WARNING) << "Serialize error: " << S.move_as_error();
    }
  }
  get_masterchain_state();
}

}  // namespace ton::validator
