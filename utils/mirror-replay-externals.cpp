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
#include <algorithm>
#include <fstream>
#include <list>
#include <queue>

#include "auto/tl/ton_api_json.h"
#include "auto/tl/tonlib_api.h"
#include "auto/tl/tonlib_api.hpp"
#include "auto/tl/tonlib_api_json.h"
#include "common/delay.h"
#include "emulator/transaction-emulator.h"
#include "keys/encryptor.h"
#include "overlay/overlays.h"
#include "td/actor/MultiPromise.h"
#include "td/utils/JsonBuilder.h"
#include "td/utils/OptionParser.h"
#include "td/utils/Random.h"
#include "td/utils/filesystem.h"
#include "td/utils/overloaded.h"
#include "td/utils/port/signals.h"
#include "tl/tl_json.h"
#include "tonlib/tonlib/TonlibClient.h"
#include "tonlib/tonlib/TonlibClientWrapper.h"

#include "git.h"

namespace ton {

std::string global_config, private_config;
td::uint32 start_mc_seqno = 0;
double private_time_shift = 0;

static auto to_tonlib_api(const BlockIdExt& blk) {
  return tonlib_api::make_object<tonlib_api::ton_blockIdExt>(
      blk.id.workchain, blk.id.shard, blk.id.seqno, blk.root_hash.as_slice().str(), blk.file_hash.as_slice().str());
}

static BlockIdExt to_block_id(const tl_object_ptr<tonlib_api::ton_blockIdExt>& b) {
  return BlockIdExt(b->workchain_, b->shard_, b->seqno_,
                    td::Bits256{td::ConstBitPtr{(const unsigned char*)b->root_hash_.data()}},
                    td::Bits256{td::ConstBitPtr{(const unsigned char*)b->file_hash_.data()}});
}

class Replayer : public td::actor::Actor {
 public:
  Replayer() = default;

  void run() {
    CHECK(!global_config.empty());
    CHECK(!private_config.empty());
    CHECK(start_mc_seqno != 0);
    {
      auto conf_data = td::read_file(global_config).move_as_ok();
      auto tonlib_options = tonlib_api::make_object<tonlib_api::options>(
          tonlib_api::make_object<tonlib_api::config>(conf_data.as_slice().str(), "", false, false),
          tonlib_api::make_object<tonlib_api::keyStoreTypeInMemory>());
      tonlib_client_ = td::actor::create_actor<tonlib::TonlibClientWrapper>("tonlibclient", std::move(tonlib_options));
    }
    {
      auto conf_data = td::read_file(private_config).move_as_ok();
      auto tonlib_options = tonlib_api::make_object<tonlib_api::options>(
          tonlib_api::make_object<tonlib_api::config>(conf_data.as_slice().str(), "", false, false),
          tonlib_api::make_object<tonlib_api::keyStoreTypeInMemory>());
      private_tonlib_client_ =
          td::actor::create_actor<tonlib::TonlibClientWrapper>("tonlibclient", std::move(tonlib_options));
    }

    td::actor::send_closure(tonlib_client_, &tonlib::TonlibClientWrapper::send_request<tonlib_api::sync>,
                            create_tl_object<tonlib_api::sync>(),
                            [SelfId = actor_id(this)](td::Result<tl_object_ptr<tonlib_api::ton_blockIdExt>> R) {
                              R.ensure();
                              td::actor::send_closure(SelfId, &Replayer::run_cont);
                            });
  }

  void get_shards(td::uint32 mc_seqno, td::Promise<std::vector<BlockIdExt>> promise) {
    td::actor::send_closure(
        tonlib_client_, &tonlib::TonlibClientWrapper::send_request<tonlib_api::blocks_lookupBlock>,
        create_tl_object<tonlib_api::blocks_lookupBlock>(
            1, create_tl_object<tonlib_api::ton_blockId>(-1, shardIdAll, mc_seqno), 0, 0),
        [SelfId = actor_id(this), client = tonlib_client_.get(), mc_seqno,
         promise = std::move(promise)](td::Result<tl_object_ptr<tonlib_api::ton_blockIdExt>> R) mutable {
          if (R.is_error()) {
            LOG(ERROR) << "Lookup mc block #" << mc_seqno << " error: " << R.move_as_error();
            delay_action(
                [=, promise = std::move(promise)]() mutable {
                  td::actor::send_closure(SelfId, &Replayer::get_shards, mc_seqno, std::move(promise));
                },
                td::Timestamp::in(1.0));
            return;
          }
          auto mc_block_obj = R.move_as_ok();
          auto mc_block_id = to_block_id(mc_block_obj);
          td::actor::send_closure(
              client, &tonlib::TonlibClientWrapper::send_request<tonlib_api::blocks_getShards>,
              create_tl_object<tonlib_api::blocks_getShards>(std::move(mc_block_obj)),
              [=, promise = std::move(promise)](td::Result<tl_object_ptr<tonlib_api::blocks_shards>> R) mutable {
                if (R.is_error()) {
                  LOG(ERROR) << "Get shards for mc block #" << mc_seqno << " error: " << R.move_as_error();
                  delay_action(
                      [=, promise = std::move(promise)]() mutable {
                        td::actor::send_closure(SelfId, &Replayer::get_shards, mc_seqno, std::move(promise));
                      },
                      td::Timestamp::in(1.0));
                  return;
                }
                auto f = R.move_as_ok();
                std::vector<BlockIdExt> blocks;
                blocks.push_back(mc_block_id);
                for (auto& b : f->shards_) {
                  blocks.push_back(to_block_id(b));
                }
                promise.set_result(std::move(blocks));
              });
        });
  }

  void get_header(BlockIdExt block_id, td::Promise<tl_object_ptr<tonlib_api::blocks_header>> promise) {
    td::actor::send_closure(
        tonlib_client_, &tonlib::TonlibClientWrapper::send_request<tonlib_api::blocks_getBlockHeader>,
        create_tl_object<tonlib_api::blocks_getBlockHeader>(to_tonlib_api(block_id)),
        [SelfId = actor_id(this), block_id,
         promise = std::move(promise)](td::Result<tl_object_ptr<tonlib_api::blocks_header>> R) mutable {
          if (R.is_error()) {
            LOG(ERROR) << "Get block header for " << block_id.id.to_str() << " error: " << R.move_as_error();
            delay_action(
                [=, promise = std::move(promise)]() mutable {
                  td::actor::send_closure(SelfId, &Replayer::get_header, block_id, std::move(promise));
                },
                td::Timestamp::in(1.0));
            return;
          }
          promise.set_result(R.move_as_ok());
        });
  }

  void get_transactions(BlockIdExt block_id, td::Bits256 after_account, td::uint64 after_lt,
                        td::Promise<tl_object_ptr<tonlib_api::blocks_transactionsExt>> promise) {
    td::actor::send_closure(
        tonlib_client_, &tonlib::TonlibClientWrapper::send_request<tonlib_api::blocks_getTransactionsExt>,
        create_tl_object<tonlib_api::blocks_getTransactionsExt>(
            to_tonlib_api(block_id), 128, 256,
            create_tl_object<tonlib_api::blocks_accountTransactionId>(after_account.as_slice().str(), after_lt)),
        [=, SelfId = actor_id(this),
         promise = std::move(promise)](td::Result<tl_object_ptr<tonlib_api::blocks_transactionsExt>> R) mutable {
          if (R.is_error()) {
            LOG(ERROR) << "Get transactions for " << block_id.id.to_str() << " error: " << R.move_as_error();
            delay_action(
                [=, promise = std::move(promise)]() mutable {
                  td::actor::send_closure(SelfId, &Replayer::get_transactions, block_id, after_account, after_lt,
                                          std::move(promise));
                },
                td::Timestamp::in(1.0));
            return;
          }
          promise.set_result(R.move_as_ok());
        });
  }

  void run_cont() {
    LOG(WARNING) << "Synced";
    cur_mc_seqno_ = start_mc_seqno;
    get_shards(cur_mc_seqno_, [SelfId = actor_id(this)](td::Result<std::vector<BlockIdExt>> R) {
      R.ensure();
      td::actor::send_closure(SelfId, &Replayer::got_shards_0, R.move_as_ok());
    });
    alarm();
  }

  void got_shards_0(std::vector<BlockIdExt> shards) {
    LOG(INFO) << "Got shards for starting MC block #" << cur_mc_seqno_ << ":";
    for (auto& b : shards) {
      processed_blocks_.insert(b);
      LOG(INFO) << "  " << b.id.to_str();
    }
    process_next_mc_block();
  }

  void process_next_mc_block() {
    if ((double)max_block_ts_ > td::Clocks::system() + private_time_shift + 3600.0) {
      delay_action([SelfId = actor_id(this)]() { td::actor::send_closure(SelfId, &Replayer::process_next_mc_block); },
                   td::Timestamp::in(1.0));
      return;
    }
    ++cur_mc_seqno_;
    get_shards(cur_mc_seqno_, [SelfId = actor_id(this)](td::Result<std::vector<BlockIdExt>> R) {
      R.ensure();
      td::actor::send_closure(SelfId, &Replayer::got_shards, R.move_as_ok());
    });
  }

  void got_shards(std::vector<BlockIdExt> shards) {
    LOG(INFO) << "Got shards for MC block #" << cur_mc_seqno_ << "";
    for (auto& b : shards) {
      if (!processed_blocks_.count(b)) {
        blocks_queue_.push(b);
        processed_blocks_.insert(b);
      }
    }
    process_next_block();
  }

  void process_next_block() {
    if (blocks_queue_.empty()) {
      process_next_mc_block();
      return;
    }
    BlockIdExt block_id = blocks_queue_.front();
    blocks_queue_.pop();
    get_header(block_id, [SelfId = actor_id(this), block_id](td::Result<tl_object_ptr<tonlib_api::blocks_header>> R) {
      R.ensure();
      td::actor::send_closure(SelfId, &Replayer::got_block_header, block_id, R.move_as_ok());
    });
  }

  void got_block_header(BlockIdExt block_id, tl_object_ptr<tonlib_api::blocks_header> header) {
    LOG(INFO) << "Processing block " << block_id.id.to_str();
    for (auto& prev : header->prev_blocks_) {
      BlockIdExt b = to_block_id(prev);
      if (!processed_blocks_.count(b)) {
        blocks_queue_.push(b);
        processed_blocks_.insert(b);
      }
    }
    auto block_utime = (td::uint32)header->gen_utime_;
    max_block_ts_ = std::max(max_block_ts_, block_utime);
    get_transactions(block_id, td::Bits256::zero(), 0,
                     [=, SelfId = actor_id(this)](td::Result<tl_object_ptr<tonlib_api::blocks_transactionsExt>> R) {
                       R.ensure();
                       td::actor::send_closure(SelfId, &Replayer::got_transactions, block_id, block_utime,
                                               R.move_as_ok());
                     });
  }

  void got_transactions(BlockIdExt block_id, td::uint32 block_utime,
                        tl_object_ptr<tonlib_api::blocks_transactionsExt> f) {
    td::Bits256 last_acc_addr;
    for (auto& trans : f->transactions_) {
      last_acc_addr = block::StdAddress::parse(trans->address_->account_address_).move_as_ok().addr;
      td::Ref<vm::Cell> trans_root = vm::std_boc_deserialize(trans->data_).move_as_ok();
      block::gen::Transaction::Record rec;
      CHECK(block::tlb::unpack_cell(trans_root, rec));
      if (rec.r1.in_msg->size_refs() == 0) {
        continue;
      }
      vm::CellSlice cs = vm::load_cell_slice(rec.r1.in_msg->prefetch_ref());
      if (cs.fetch_ulong(2) != 0b10) {
        continue;
      }
      std::string str = PSTRING() << block_id.id.workchain << ":" << rec.account_addr.to_hex() << " " << rec.lt << " "
                                  << trans_root->get_hash().to_hex();
      pending_ext_msgs_.emplace(block_utime,
                                std::pair<td::BufferSlice, std::string>{
                                    vm::std_boc_serialize(rec.r1.in_msg->prefetch_ref()).move_as_ok(), std::move(str)});
    }

    if (f->incomplete_) {
      CHECK(!f->transactions_.empty());
      get_transactions(block_id, last_acc_addr, f->transactions_.back()->transaction_id_->lt_,
                       [=, SelfId = actor_id(this)](td::Result<tl_object_ptr<tonlib_api::blocks_transactionsExt>> R) {
                         R.ensure();
                         td::actor::send_closure(SelfId, &Replayer::got_transactions, block_id, block_utime,
                                                 R.move_as_ok());
                       });
    } else {
      process_next_block();
    }
  }

  void alarm() {
    alarm_timestamp() = td::Timestamp::in(0.5);
    while (!pending_ext_msgs_.empty()) {
      auto it = pending_ext_msgs_.begin();
      if ((double)it->first < td::Clocks::system() + private_time_shift + 1.0) {
        send_ext_msg(it->first, std::move(it->second.first), std::move(it->second.second));
        pending_ext_msgs_.erase(it);
      } else {
        break;
      }
    }
  }

  void send_ext_msg(td::uint32 ts, td::BufferSlice msg, std::string str, int max_retries = 4) {
    td::actor::send_closure(private_tonlib_client_,
                            &tonlib::TonlibClientWrapper::send_request<tonlib_api::raw_sendMessage>,
                            create_tl_object<tonlib_api::raw_sendMessage>(msg.as_slice().str()),
                            [=, SelfId = actor_id(this), msg = msg.clone(),
                             str = std::move(str)](td::Result<tl_object_ptr<tonlib_api::ok>> R) mutable {
                              if (R.is_error()) {
                                if (max_retries == 0) {
                                  LOG(ERROR) << " [[ SEND ERROR : " << str << " ]] ";
                                  return;
                                }
                                delay_action(
                                    [=, msg = std::move(msg), str = std::move(str)]() mutable {
                                      td::actor::send_closure(SelfId, &Replayer::send_ext_msg, ts, std::move(msg),
                                                              std::move(str), max_retries - 1);
                                    },
                                    td::Timestamp::in(4.0));
                                return;
                              }
                              LOG(WARNING) << "Sent message, ts = " << ts;
                            });
  }

 private:
  td::actor::ActorOwn<tonlib::TonlibClientWrapper> tonlib_client_;
  td::actor::ActorOwn<tonlib::TonlibClientWrapper> private_tonlib_client_;

  std::set<BlockIdExt> processed_blocks_;
  std::queue<BlockIdExt> blocks_queue_;
  td::uint32 cur_mc_seqno_;
  td::uint32 max_block_ts_ = 0;

  std::multimap<td::uint32, std::pair<td::BufferSlice, std::string>> pending_ext_msgs_;
};

int run(int argc, char* argv[]) {
  SET_VERBOSITY_LEVEL(verbosity_INFO);
  td::set_default_failure_signal_handler().ensure();

  td::actor::ActorOwn<Replayer> x;
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
  p.add_option('h', "help", "prints a help message", [&]() {
    char b[10240];
    td::StringBuilder sb(td::MutableSlice{b, 10000});
    sb << p;
    std::cout << sb.as_cslice().c_str();
    std::exit(2);
  });
  p.add_option('C', "global-config", "global TON configuration file",
               [&](td::Slice arg) { global_config = arg.str(); });
  p.add_option('c', "private-config", "TON configuration file for the private network",
               [&](td::Slice arg) { private_config = arg.str(); });
  p.add_checked_option('s', "start-mc-seqno", "start MC seqno (in public net)", [&](td::Slice arg) -> td::Status {
    TRY_RESULT_ASSIGN(start_mc_seqno, td::to_integer_safe<td::uint32>(arg));
    return td::Status::OK();
  });
  p.add_option('T', "private-time-shift", "time shift for the private net",
               [&](td::Slice arg) { private_time_shift = td::to_double(arg); });

  td::actor::Scheduler scheduler({7});

  scheduler.run_in_context([&] { x = td::actor::create_actor<Replayer>("replayer"); });

  scheduler.run_in_context([&] { p.run(argc, argv).ensure(); });
  scheduler.run_in_context([&] { td::actor::send_closure(x, &Replayer::run); });
  while (scheduler.run(1)) {
  }

  return 0;
}

}  // namespace ton

int main(int argc, char* argv[]) {
  return ton::run(argc, argv);
}
