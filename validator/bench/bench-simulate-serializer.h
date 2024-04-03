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

#include "validator/interfaces/validator-manager.h"
#include "validator/interfaces/shard.h"

namespace ton::validator {

class BenchSimulateSerializer : public td::actor::Actor {
 private:
  td::actor::ActorId<ValidatorManager> manager_;
  std::string db_root_;

  std::vector<BlockIdExt> blocks_;
  std::vector<td::Bits256> state_root_hashes_;

 public:
  BenchSimulateSerializer(td::actor::ActorId<ValidatorManager> manager, std::string db_root)
      : manager_(manager), db_root_(db_root) {
  }

  void start_up() override;
  void get_masterchain_state();
  void got_masterchain_state(td::Ref<MasterchainState> state);
  void get_shard_states();
  void got_shard_state(td::Ref<ShardState> state);
  void get_celldb_reader();
  void got_celldb_reader(std::shared_ptr<vm::CellDbReader> reader);
};

}  // namespace ton::validator
