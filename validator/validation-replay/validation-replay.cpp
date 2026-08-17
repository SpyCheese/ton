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
#include "checksum.h"
#include "fabric.h"
#include "top-shard-descr.hpp"
#include "utils.h"
#include "validation-replay.h"

namespace ton::validator {

namespace {

struct RunInfo {
  size_t idx = 0;
  std::string description;
  std::string status;
};

class ValidationReplayerImpl : public ValidationReplayer {
 public:
  ValidationReplayerImpl(td::actor::ActorId<ValidatorManager> manager, Ref<ValidatorManagerOptions> opts)
      : manager_(std::move(manager)), opts_(std::move(opts)) {
  }

  td::actor::Task<std::string> run_command(std::string command) override {
    LOG(INFO) << "Command: " << command;
    std::vector<std::string> tokens = tokenize(command);

    if (tokens.empty()) {
      co_return "Validation replayer. `vrp help` for more info";
    }
    size_t idx = 1;
    auto eoln = [&]() -> bool { return idx == tokens.size(); };
    auto next = [&]() -> td::Result<std::string> {
      if (eoln()) {
        return td::Status::Error("unexpected eoln");
      }
      return tokens[idx++];
    };
    auto check_eoln = [&]() -> td::Status {
      if (!eoln()) {
        return td::Status::Error("extra data in command");
      }
      return td::Status::OK();
    };
    if (tokens[0] == "help") {
      CO_TRY(check_eoln());
      co_return command_help();
    }
    if (tokens[0] == "show") {
      CO_TRY(check_eoln());
      co_return command_show();
    }
    if (tokens[0] == "cancel") {
      CO_TRY(check_eoln());
      co_return command_cancel();
    }
    if (tokens[0] == "run") {
      ReplayMode mode = ReplayMode::validate;
      bool log_work_time = false;
      std::vector<std::string> params;
      while (!eoln()) {
        std::string token = CO_TRY(next());
        if (token == "--mode") {
          mode = CO_TRY(parse_mode(CO_TRY(next())));
        } else if (token == "--log-work-time") {
          log_work_time = true;
        } else if (token[0] == '-') {
          co_return td::Status::Error(PSTRING() << "unknown flag " << token);
        } else {
          params.push_back(std::move(token));
        }
      }
      if (params.empty()) {
        co_return td::Status::Error("Expected at least one block id");
      }
      std::vector<BlockId> block_ids;
      for (const std::string& s : params) {
        block_ids.push_back(CO_TRY(BlockId::from_str(s)));
      }
      command_run(std::move(block_ids), mode, log_work_time).start().detach_silent();
      co_return "Started. `vrp show` to see results.";
    }
    if (tokens[0] == "run-range") {
      ReplayMode mode = ReplayMode::validate;
      bool log_work_time = false;
      td::uint32 max_jobs = 1;
      std::optional<WorkchainId> wc;
      std::vector<std::string> params;
      while (!eoln()) {
        std::string token = CO_TRY(next());
        if (token == "--mode") {
          mode = CO_TRY(parse_mode(CO_TRY(next())));
        } else if (token == "--wc") {
          wc = CO_TRY(td::to_integer_safe<WorkchainId>(CO_TRY(next())));
        } else if (token == "--max-jobs") {
          max_jobs = CO_TRY(td::to_integer_safe<td::uint32>(CO_TRY(next())));
        } else if (token == "--log-work-time") {
          log_work_time = true;
        } else if (token[0] == '-') {
          co_return td::Status::Error(PSTRING() << "unknown flag " << token);
        } else {
          params.push_back(std::move(token));
        }
      }
      if (params.size() != 2) {
        co_return td::Status::Error("Expected two seqnos");
      }
      BlockSeqno start = CO_TRY(td::to_integer_safe<BlockSeqno>(params[0]));
      BlockSeqno end = CO_TRY(td::to_integer_safe<BlockSeqno>(params[1]));
      if (end <= start) {
        co_return td::Status::Error("Invalid range, start should be earlier than end");
      }
      if (max_jobs == 0) {
        co_return td::Status::Error("max-jobs is 0");
      }
      command_run_range(start, end, mode, log_work_time, wc, max_jobs).start().detach_silent();
      co_return "Started. `vrp show` to see results.";
    }
    co_return td::Status::Error("Unknown command " + tokens[0]);
  }

  void update_options(Ref<ValidatorManagerOptions> opts) override {
    opts_ = opts;
  }

 private:
  td::actor::ActorId<ValidatorManager> manager_;
  Ref<ValidatorManagerOptions> opts_;

  std::vector<RunInfo> past_runs_;
  bool busy_ = false;
  RunInfo current_run_;
  std::queue<td::Promise<>> run_queue_;
  size_t next_run_idx_ = 0;
  td::CancellationTokenSource cancellation_;

  std::string command_help() {
    return "Validation replayer - replays collation and validation for old blocks\n"
           "vrp help\tshow help\n"
           "vrp show\tshow runs\n"
           "vrp run [--mode mode] <block_id> ...\tprocess given blocks. Id format: (0,8000000000000000,123456) (no "
           "hashes)\n"
           "\t--mode mode\tcollate/validate/both (default: validate)\n"
           "\t--log-work-time\tshow detailed work time stats\n"
           "vrp run-range [--mode mode] <start> <end>\tprocess all blocks between mc seqnos <start> and <end>\n"
           "\t--mode mode\tcollate/validate/both (default: validate)\n"
           "\t--log-work-time\tshow detailed work time stats\n"
           "\t--workchain <wc>\tprocess blocks from <wc> - 0 or -1 (default: both)\n"
           "\t--max-jobs <wc>\tmaximum number of blocks processed in parallel (default: 1)\n"
           "vrp cancel\tcancel current run\n";
  }

  std::string command_show() {
    td::StringBuilder sb;
    if (past_runs_.empty()) {
      sb << "No past runs\n";
    } else {
      sb << "Past runs (" << past_runs_.size() << "):\n";
      for (const auto& run : past_runs_) {
        sb << "  #" << run.idx << ": " << run.description << "\n";
        sb << "    ";
        for (char c : run.status) {
          sb << c << (c == '\n' ? "    " : "");
        }
        sb << "\n";
      }
    }
    if (busy_) {
      sb << "\nCurrent run #" << current_run_.idx << ": " << current_run_.description << "\n";
      sb << "  ";
      for (char c : current_run_.status) {
        sb << c << (c == '\n' ? "  " : "");
      }
      sb << "\n";
    }
    if (!run_queue_.empty()) {
      sb << "\nRuns in queue: " << run_queue_.size() << "\n";
    }
    return sb.as_cslice().str();
  }

  std::string command_cancel() {
    if (!busy_) {
      return "Nothing to cancel";
    }
    cancellation_.cancel();
    return "Cancelled";
  }

  td::actor::Task<> command_run(std::vector<BlockId> block_ids, ReplayMode mode, bool log_work_time) {
    std::string description;
    CHECK(!block_ids.empty());
    if (block_ids.size() == 1) {
      description = PSTRING() << "Block " << block_ids[0] << ", mode=" << mode_to_str(mode);
    } else {
      description = PSTRING() << block_ids.size() << " blocks, mode=" << mode_to_str(mode);
    }
    co_await run_start(description);
    auto result = co_await command_run_inner(std::move(block_ids), mode, log_work_time).wrap();
    if (result.is_error()) {
      LOG(ERROR) << "ERROR run #" << current_run_.idx << ": " << result.error();
      current_run_.status = PSTRING() << "ERROR: " << result.error().to_string()
                                      << "\nLast status: " << current_run_.status;
    }
    run_end();
    co_return {};
  }

  td::actor::Task<> command_run_inner(std::vector<BlockId> block_ids, ReplayMode mode, bool log_work_time) {
    auto cancellation_token = cancellation_.get_cancellation_token();
    ProcessBlockResult total;
    size_t processed_ok = 0;
    for (size_t i = 0; i < block_ids.size(); ++i) {
      BlockId block_id = block_ids[i];
      auto handle = co_await get_block_by_id(manager_, block_id);
      current_run_.status = "Processing block " + block_id.to_str();
      auto R = co_await process_block(handle, mode).wrap();
      if (R.is_ok()) {
        total += R.ok();
        ++processed_ok;
      } else {
        LOG(ERROR) << "ERROR run #" << current_run_.idx << " " << handle->id().id << ": " << R.error();
      }
      td::StringBuilder sb;
      if (block_ids.size() == 1) {
        if (R.is_error()) {
          sb << "ERROR: " << R.error().message();
        } else {
          sb << total.to_str(1.0, log_work_time);
        }
      } else {
        sb << "Processed " << i + 1 << "/" << block_ids.size() << " blocks, " << processed_ok << " ok, "
           << i + 1 - processed_ok << " errors";
        if (processed_ok > 0) {
          sb << "\n" << total.to_str((double)processed_ok, log_work_time);
        }
      }
      current_run_.status = sb.as_cslice().str();
      CO_TRY(cancellation_token.check());
    }

    co_return {};
  }

  td::actor::Task<> command_run_range(BlockSeqno mc_seqno_start, BlockSeqno mc_seqno_end, ReplayMode mode,
                                      bool log_work_time, std::optional<WorkchainId> wc, td::uint32 max_jobs) {
    std::string description = PSTRING() << "MC range " << mc_seqno_start << " to " << mc_seqno_end
                                        << ", mode=" << mode_to_str(mode);
    if (wc) {
      description += PSTRING() << ", wc=" << *wc;
    }
    co_await run_start(std::move(description));
    auto result =
        co_await command_run_range_inner(mc_seqno_start, mc_seqno_end, mode, log_work_time, wc, max_jobs).wrap();
    if (result.is_error()) {
      LOG(ERROR) << "ERROR run #" << current_run_.idx << ": " << result.error();
      current_run_.status = PSTRING() << "ERROR: " << result.error().to_string()
                                      << "\nLast status: " << current_run_.status;
    }
    run_end();
    co_return {};
  }

  td::actor::Task<> command_run_range_inner(BlockSeqno mc_seqno_start, BlockSeqno mc_seqno_end, ReplayMode mode,
                                            bool log_work_time, std::optional<WorkchainId> wc, td::uint32 max_jobs) {
    auto cancellation_token = cancellation_.get_cancellation_token();
    struct State {
      size_t processed_total = 0;
      size_t processed_ok = 0;
      ProcessBlockResult total;
      td::uint32 running_jobs = 0;
      td::Promise<> wait_for_job_finish;
      td::Promise<> wait_for_all_jobs;
    };
    auto state = std::make_shared<State>();
    BlockSeqno current_mc_seqno = mc_seqno_start;
    td::Timer timer;

    auto update_status = [&] {
      td::StringBuilder sb;
      sb << "mc_seqno=" << current_mc_seqno << " ("
         << td::StringBuilder::FixedDouble(
                (double)(current_mc_seqno - mc_seqno_start) / (double)(mc_seqno_end - mc_seqno_start) * 100.0, 1)
         << "%), processed " << state->processed_total << " blocks, " << state->processed_ok << " ok, "
         << state->processed_total - state->processed_ok << " errors";
      if (state->running_jobs > 0) {
        sb << "; " << state->running_jobs << " in progress";
      }
      sb << "; running for " << td::StringBuilder::FixedDouble(timer.elapsed(), 1) << "s";
      if (state->processed_ok > 0) {
        sb << "\n" << state->total.to_str((double)state->processed_ok, log_work_time);
      }
      current_run_.status = sb.as_cslice().str();
    };

    auto process_block_outer = [](ValidationReplayerImpl* self, size_t run_idx, std::shared_ptr<State> state,
                                  ReplayMode mode, ConstBlockHandle handle) -> td::actor::Task<> {
      auto R = co_await self->process_block(handle, mode).wrap();
      ++state->processed_total;
      if (R.is_ok()) {
        ++state->processed_ok;
        state->total += R.move_as_ok();
      } else {
        LOG(ERROR) << "ERROR run #" << run_idx << " " << handle->id().id << ": " << R.error();
      }
      --state->running_jobs;
      if (state->wait_for_job_finish) {
        state->wait_for_job_finish.set_value(td::Unit{});
      }
      if (state->running_jobs == 0 && state->wait_for_all_jobs) {
        state->wait_for_all_jobs.set_value(td::Unit{});
      }
      co_return {};
    };

    auto process_block = [&](ConstBlockHandle handle, BlockSeqno cur_mc_seqno) -> td::actor::Task<> {
      current_mc_seqno = cur_mc_seqno;
      if (!wc || handle->id().id.workchain == *wc) {
        if (state->running_jobs == max_jobs) {
          auto [task, promise] = td::actor::StartedTask<>::make_bridge();
          state->wait_for_job_finish = std::move(promise);
          co_await std::move(task);
          CHECK(state->running_jobs < max_jobs);
        }
        ++state->running_jobs;
        process_block_outer(this, current_run_.idx, state, mode, handle).start().detach_silent();
      }
      update_status();
      CO_TRY(cancellation_token.check());
      co_return {};
    };

    co_await process_all_blocks(manager_, mc_seqno_start, mc_seqno_end, !wc || *wc == basechainId,
                                std::move(process_block));

    if (state->running_jobs > 0) {
      auto [task, promise] = td::actor::StartedTask<>::make_bridge();
      state->wait_for_all_jobs = std::move(promise);
      co_await std::move(task);
    }
    update_status();

    co_return {};
  }

  td::actor::Task<> run_start(std::string desc) {
    if (busy_) {
      auto [task, promise] = td::actor::StartedTask<>::make_bridge();
      run_queue_.push(std::move(promise));
      co_await std::move(task);
    }
    CHECK(!busy_);
    busy_ = true;
    current_run_ = RunInfo{.idx = next_run_idx_++, .description = std::move(desc), .status = "Started"};
    co_return {};
  }

  void run_end() {
    CHECK(busy_);
    busy_ = false;
    past_runs_.push_back(std::move(current_run_));
    if (!run_queue_.empty()) {
      auto promise = std::move(run_queue_.front());
      run_queue_.pop();
      promise.set_value(td::Unit{});
    }
  }

  struct ProcessBlockResult {
    double block_size = 0;
    struct Collate {
      double new_block_size = 0;
      double new_collated_data_size = 0;
      double time = 0.0;
      CollationStats::WorkTimeStats work_time;
    };
    std::optional<Collate> collate;
    struct Validate {
      double time = 0.0;
      ValidationStats::WorkTimeStats work_time;
    };
    std::optional<Validate> validate;

    ProcessBlockResult& operator+=(const ProcessBlockResult& r) {
      block_size += r.block_size;
      if (!collate) {
        collate = r.collate;
      } else if (r.collate) {
        collate->new_block_size += r.collate->new_block_size;
        collate->new_collated_data_size += r.collate->new_collated_data_size;
        collate->time += r.collate->time;
        collate->work_time += r.collate->work_time;
      }
      if (!validate) {
        validate = r.validate;
      } else if (r.validate) {
        validate->time += r.validate->time;
        validate->work_time += r.validate->work_time;
      }
      return *this;
    }

    std::string to_str(double n, bool log_work_time) const {
      using Fixed = td::StringBuilder::FixedDouble;
      td::StringBuilder sb;
      if (collate) {
        sb << (n == 1.0 ? "" : "Avg ") << "Collate: size=" << Fixed(collate->new_block_size / n, 0) << "/"
           << Fixed(block_size / n, 0) << ", cdata_size=" << Fixed(collate->new_collated_data_size / n, 0)
           << ", time=" << Fixed(collate->time / n, 6);
        if (log_work_time) {
          auto wt = collate->work_time;
          wt *= 1.0 / n;
          auto s1 = tokenize(wt.to_str(false));
          auto s2 = tokenize(wt.to_str(true));
          for (size_t i = 0; i < s1.size(); ++i) {
            sb << "\n  " << "real_" << s1[i] << " / " << "cpu_" << s2[i];
          }
        }
      } else {
        sb << (n == 1.0 ? "" : "Avg ") << "Block size = " << Fixed(block_size / n, 0);
      }
      if (validate) {
        sb << "\n" << (n == 1.0 ? "" : "Avg ") << "Validate: time=" << Fixed(validate->time / n, 6);
        if (log_work_time) {
          auto wt = validate->work_time;
          wt *= 1.0 / n;
          auto s1 = tokenize(wt.to_str(false));
          auto s2 = tokenize(wt.to_str(true));
          for (size_t i = 0; i < s1.size(); ++i) {
            sb << "\n  " << "real_" << s1[i] << " / " << "cpu_" << s2[i];
          }
        }
      }
      return sb.as_cslice().str();
    }
  };
  td::actor::Task<ProcessBlockResult> process_block(ConstBlockHandle handle, ReplayMode mode) {
    Ref<BlockData> block = co_await td::actor::ask(manager_, &ValidatorManager::get_block_data_from_db, handle);
    ProcessBlockResult result;
    result.block_size = (double)block->data().size();

    BlockIdExt block_id = block->block_id();
    UnpackedBlock unpacked = unpack_block(block);
    BlockIdExt min_mc_block_id =
        (co_await get_block_by_id(manager_, BlockId{masterchainId, shardIdAll, unpacked.min_mc_ref_seqno}))->id();

    Ref<MasterchainState> prev_mc_state{
        co_await td::actor::ask(manager_, &ValidatorManager::get_shard_state_from_db_short, unpacked.mc_block_id)};
    auto validator_set = prev_mc_state->get_validator_set(block_id.shard_full());

    std::vector<Ref<ShardTopBlockDescription>> shard_blocks;
    if (block_id.is_masterchain()) {
      Ref<MasterchainState> new_mc_state{
          co_await td::actor::ask(manager_, &ValidatorManager::get_shard_state_from_db_short, block_id)};
      shard_blocks = co_await get_shard_block_descriptions(new_mc_state, prev_mc_state, manager_);
    }

    BlockCandidate candidate;
    if (mode == ReplayMode::collate || mode == ReplayMode::both) {
      auto [task, promise] = td::actor::StartedTask<BlockCandidate>::make_bridge();
      auto [stats_task, stats_promise] = td::actor::StartedTask<CollationStats>::make_bridge();
      LOG(WARNING) << "Collating block " << block_id.id;
      td::Timer timer;
      run_collate_query(
          CollateParams{
              .shard = block_id.shard_full(),
              .min_masterchain_block_id = min_mc_block_id,
              .prev = unpacked.prev,
              .creator = unpacked.creator,
              .validator_set = validator_set,
              .collator_opts = opts_->get_collator_options(),
              .utime = (double)unpacked.gen_utime,
              .hard_timeout = td::Timestamp::in(10.0),
              .is_replay = true,
              .in_top_mc_block_id = unpacked.mc_block_id,
              .in_external_messages = unpacked.ext_msgs,
              .in_shard_blocks = shard_blocks,
              .in_rand_seed = unpacked.rand_seed,
              .store_stats_to = std::move(stats_promise),
          },
          manager_, {}, std::move(promise));
      candidate = co_await std::move(task).trace("collate " + block_id.id.to_str());
      LOG(WARNING) << "Collating block " << block_id.id << ": done, size=" << candidate.data.size() << "/"
                   << block->data().size() << ", cdata_size=" << candidate.collated_data.size()
                   << ", time=" << timer.elapsed();
      auto stats = co_await std::move(stats_task);
      result.collate = ProcessBlockResult::Collate{
          .new_block_size = (double)candidate.data.size(),
          .new_collated_data_size = (double)candidate.collated_data.size(),
          .time = timer.elapsed(),
          .work_time = stats.work_time,
      };
    } else {
      block::gen::ConsensusExtraData::Record rec;
      rec.flags = 0;
      rec.gen_utime_ms = (td::uint64)unpacked.gen_utime * 1000;
      Ref<vm::Cell> cell;
      CHECK(block::gen::pack_cell(cell, rec));
      std::vector<Ref<vm::Cell>> roots = {cell};
      if (!shard_blocks.empty()) {
        roots.push_back(create_collated_data_shard_block_descr(shard_blocks));
      }
      td::BufferSlice collated_data = vm::std_boc_serialize_multi(std::move(roots), 2).ensure().move_as_ok();
      candidate = BlockCandidate{unpacked.creator, block_id, td::sha256_bits256(collated_data), block->data(),
                                 collated_data.clone()};
    }

    if (mode == ReplayMode::validate || mode == ReplayMode::both) {
      LOG(WARNING) << "Validating block " << block_id.id;
      auto [task, promise] = td::actor::StartedTask<ValidateCandidateResult>::make_bridge();
      auto [stats_task, stats_promise] = td::actor::StartedTask<ValidationStats>::make_bridge();
      td::Timer timer;
      run_validate_query(std::move(candidate),
                         ValidateParams{
                             .shard = block_id.shard_full(),
                             .min_masterchain_block_id = min_mc_block_id,
                             .prev = unpacked.prev,
                             .validator_set = validator_set,
                             .is_replay = true,
                             .store_stats_to = std::move(stats_promise),
                         },
                         manager_, td::Timestamp::in(30.0), std::move(promise));
      auto verdict = co_await std::move(task).trace("validate " + block_id.id.to_str());
      if (verdict.has<CandidateReject>()) {
        co_return td::Status::Error(PSTRING()
                                    << "REJECT " << block_id.id << ": " << verdict.get<CandidateReject>().reason);
      }
      LOG(WARNING) << "Validating block " << block_id.id << ": done, time=" << timer.elapsed();
      auto stats = co_await std::move(stats_task);
      result.validate = ProcessBlockResult::Validate{
          .time = timer.elapsed(),
          .work_time = stats.work_time,
      };
    }
    co_return result;
  }
};

}  // namespace

td::actor::ActorOwn<ValidationReplayer> ValidationReplayer::create(td::actor::ActorId<ValidatorManager> manager,
                                                                   Ref<ValidatorManagerOptions> opts) {
  return td::actor::create_actor<ValidationReplayerImpl>("VRP", manager, opts);
}

}  // namespace ton::validator
