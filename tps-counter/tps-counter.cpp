#include "lite-client/ext-client.h"
#include "td/utils/port/signals.h"
#include "td/utils/OptionParser.h"
#include "td/utils/Random.h"
#include "common/delay.h"
#include <fstream>
#include "overlay/overlays.h"

#include "lite-client/ext-client.h"

#include <algorithm>
#include <list>
#include "git.h"
#include "td/utils/filesystem.h"
#include "keys/encryptor.h"
#include "td/actor/MultiPromise.h"
#include "auto/tl/lite_api.h"
#include "auto/tl/ton_api_json.h"
#include "td/utils/JsonBuilder.h"
#include "tl-utils/lite-utils.hpp"
#include "ton/lite-tl.hpp"
#include "mc-config.h"
#include "vm/cells/MerkleProof.h"

#include <block-auto.h>

using namespace ton;

std::string global_config;
int msplit = 0;
int duration = 16;
int start_delay = 30;
int max_shards = 1000000000;

td::Bits256 to_bits256(const std::string& s) {
  td::Bits256 x;
  CHECK(x.from_hex(s) == 256);
  return x;
}

td::Ref<vm::Cell> to_lib_cell(const std::string& s) {
  vm::CellBuilder b;
  b.store_long(2, 8);
  b.store_bytes(to_bits256(s).as_slice());
  return b.finalize(true);
}

template <class Type, class... Args>
td::BufferSlice create_query(Args&&... args) {
  Type object(std::forward<Args>(args)...);
  return create_serialize_tl_object<lite_api::liteServer_query>(serialize_tl_object(&object, true));
}

const td::uint32 STEP = 60;

class TpsCounter : public td::actor::Actor {
 public:
  TpsCounter() = default;

  void run() {
    auto G = td::read_file(global_config).move_as_ok();
    auto gc_j = td::json_decode(G.as_slice()).move_as_ok();
    ton::ton_api::liteclient_config_global gc;
    ton::ton_api::from_json(gc, gc_j.get_object()).ensure();
    std::vector<liteclient::LiteServerConfig> servers = liteclient::LiteServerConfig::parse_global_config(gc).move_as_ok();

    client_ = liteclient::ExtClient::create(std::move(servers), make_callback());
    delay_action([SelfId = actor_id(this)]() { td::actor::send_closure(SelfId, &TpsCounter::send_get_mc_info); },
                 td::Timestamp::in(0));
  }

  void send_get_mc_info() {
    td::actor::send_closure(
        client_, &liteclient::ExtClient::send_query, "q", create_query<lite_api::liteServer_getMasterchainInfo>(),
        td::Timestamp::in(2.0), [SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
          if (R.is_error()) {
            LOG(WARNING) << R.move_as_error();
            td::actor::send_closure(SelfId, &TpsCounter::send_get_mc_info);
            return;
          }
          auto res = fetch_tl_object<lite_api::liteServer_masterchainInfo>(R.move_as_ok(), true).move_as_ok();
          td::actor::send_closure(SelfId, &TpsCounter::start, create_block_id(res->last_));
        });
  }

  td::unique_ptr<liteclient::ExtClient::Callback> make_callback() {
    class Callback : public liteclient::ExtClient::Callback {
     public:
    };

    return td::make_unique<Callback>();
  }

  void start(BlockIdExt block) {
    alarm_timestamp() = td::Timestamp::in(60.0);
    interval_end_ = (td::uint32)td::Clocks::system() - start_delay;
    LOG(INFO) << "Last mc block: " << block.id.to_str();
    td::actor::send_closure(
        client_, &liteclient::ExtClient::send_query, "q",
        create_query<lite_api::liteServer_getAllShardsInfo>(create_tl_lite_block_id(block)),
        td::Timestamp::in(20.0), [SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
          R.ensure();
          auto res = fetch_tl_object<lite_api::liteServer_allShardsInfo>(R.move_as_ok(), true).move_as_ok();
          delay_action(
              [=, res = std::move(res)]() mutable {
                td::actor::send_closure(SelfId, &TpsCounter::got_shards, std::move(res));
              },
              td::Timestamp::in(5.0));
        });
  }

  void got_shards(tl_object_ptr<lite_api::liteServer_allShardsInfo> res) {
    alarm_timestamp() = td::Timestamp::in(60.0);
    LOG(INFO) << "Last shard blocks:";
    auto root = vm::std_boc_deserialize(res->data_).move_as_ok();
    block::ShardConfig sh_conf;
    CHECK(sh_conf.unpack(vm::load_cell_slice_ref(root)));
    auto ids = sh_conf.get_shard_hash_ids(true);
    int rem = max_shards;
    for (auto id : ids) {
      if (rem == 0) {
        break;
      }
      auto ref = sh_conf.get_shard_hash(ton::ShardIdFull(id));
      if (ref.not_null()) {
        --rem;
        auto block = ref->top_block_id();
        LOG(INFO) << "  " << block.id.to_str();
        add_id(block);
        msplit = std::max(msplit, block.id.shard_full().pfx_len());
      }
    }
    cur_stat_ = {};
    add_id(create_block_id(res->id_));
    myloop();
  }

  void add_id(BlockIdExt id) {
    if (visited_.count(id)) {
      return;
    }
    visited_.insert(id);
    ++waiting_;
    send_query_retr(create_query<lite_api::liteServer_getBlockHeader>(create_tl_lite_block_id(id), (1 << 30)),
                    [id, SelfId = actor_id(this)](td::Result<td::BufferSlice> R) {
                      R.ensure();
                      auto res = fetch_tl_object<lite_api::liteServer_blockHeader>(R.move_as_ok(), true).move_as_ok();
                      td::actor::send_closure(SelfId, &TpsCounter::got_block, id, std::move(res));
                    });
  }

  void send_query_retr(td::BufferSlice q, td::Promise<td::BufferSlice> promise) {
    auto q2 = q.clone();
    td::actor::send_closure(
        client_, &liteclient::ExtClient::send_query, "q", std::move(q), td::Timestamp::in(20.0),
        [SelfId = actor_id(this), q2 = std::move(q2),
         promise = std::move(promise)](td::Result<td::BufferSlice> R) mutable {
          if (R.is_error()) {
            LOG(WARNING) << R.error();
            td::actor::send_closure(SelfId, &TpsCounter::send_query_retr, std::move(q2), std::move(promise));
            return;
          }
          auto R2 = fetch_tl_object<lite_api::liteServer_blockHeader>(R.ok(), true);
          if (R2.is_error()) {
            auto R3 = fetch_tl_object<lite_api::liteServer_error>(R.ok(), true);
            if (R3.is_ok()) {
              LOG(WARNING) << "Liteserver error: " << R3.ok()->code_ << " " << R3.ok()->message_;
            } else {
              LOG(WARNING) << "Error " << R2.error();
            }
            td::actor::send_closure(SelfId, &TpsCounter::send_query_retr, std::move(q2), std::move(promise));
            return;
          }
          promise.set_result(R.move_as_ok());
        });
  }

  void got_block(BlockIdExt id, tl_object_ptr<lite_api::liteServer_blockHeader> f) {
    --waiting_;

    Block res;
    res.id = id;

    auto proof_root = vm::std_boc_deserialize(f->header_proof_).move_as_ok();
    auto root = vm::MerkleProof::virtualize(proof_root, 1);

    ton::BlockIdExt mc_blkid;
    bool after_split;
    block::unpack_block_prev_blk_ext(root, id, res.prev, mc_blkid, after_split).ensure();

    block::gen::Block::Record blk;
    block::gen::BlockInfo::Record info;
    CHECK(tlb::unpack_cell(root, blk) && tlb::unpack_cell(blk.info, info));
    res.ts = info.gen_utime;

    res.tx_cnt = f->tx_cnt_;

    queue_.emplace(res.ts, std::move(res));
    myloop();
  }

  void myloop() {
    alarm_timestamp() = td::Timestamp::in(60.0);
    while (true) {
      if (waiting_) {
        return;
      }
      if (queue_.size() == 0) {
        std::_Exit(0);
      }
      auto it = queue_.end();
      --it;
      Block blk = std::move(it->second);
      queue_.erase(it);
      if (iter_ == 0) {
        LOG(INFO) << (int)blk.ts - (int)(interval_end_ - STEP) << " " << blk.id.id.to_str() << " " << blk.tx_cnt;
      } else {
        LOG(DEBUG) << (int)blk.ts - (int)(interval_end_ - STEP) << " " << blk.id.id.to_str() << " " << blk.tx_cnt;
      }
      for (auto x : blk.prev) {
        add_id(x);
      }

      if (blk.ts > interval_end_) {
        return;
      }
      while (blk.ts <= interval_end_ - STEP) {
        std::cout << "Minute #" << iter_ << ":\n" << cur_stat_.to_str() << "\n\n";
        std::cout.flush();
        ++iter_;
        interval_end_ -= STEP;
        cur_stat_ = {};
        if (iter_ == (td::uint32)duration) {
          std::_Exit(0);
        }
      }
      cur_stat_.add(blk);
    }
  }

  void alarm() override {
    std::cout << "Timeout";
    std::cout.flush();
    std::_Exit(0);
  }

 private:
  td::actor::ActorOwn<liteclient::ExtClient> client_;

  std::set<BlockIdExt> visited_;
  struct Block {
    BlockIdExt id;
    std::vector<BlockIdExt> prev;
    td::uint32 ts = 0;
    size_t tx_cnt = 0;
  };
  std::multimap<td::uint32, Block> queue_;
  size_t waiting_ = 0;

  td::uint32 interval_end_;
  td::uint32 iter_ = 0;

  struct OneStat {
    size_t tx = 0;
    size_t blocks = 0;
    void add(const Block& blk) {
      tx += blk.tx_cnt;
      blocks++;
    }
    std::string to_str() const {
      char buf[16];
      snprintf(buf, 16, "%06.1f %05.2f", (double)tx / (double)STEP, (double)blocks / (double)STEP * 60.0);
      return buf;
    }
  };
  struct Stat {
    std::map<ShardIdFull, OneStat> shards;
    OneStat total;
    OneStat mc;
    Stat() {
      int s = msplit;
      int rem = max_shards;
      for (int i = 0; i < (1 << s) && rem > 0; ++i, --rem) {
        shards[ShardIdFull(0, (td::uint64)(i * 2 + 1) << (64 - s - 1))];
      }
    }
    void add(const Block& blk) {
      if (blk.id.is_masterchain()) {
        mc.add(blk);
      } else {
        total.add(blk);
        shards[blk.id.shard_full()].add(blk);
      }
    }
    std::string to_str() const {
      td::StringBuilder sb;
      size_t i = 0;
      for (const auto& s : shards) {
        if (i % 8 == 0 && i != 0) {
          sb << "\n";
        }
        sb << s.first.to_str().substr(1, 6) << " " << s.second.to_str() << "    ";
        ++i;
      }
      sb << "\n";
      sb << "Total: " << total.to_str() << "\n";
      sb << "Master: " << mc.to_str();
      return sb.as_cslice().str();
    }
  } cur_stat_;
};

int main(int argc, char* argv[]) {
  SET_VERBOSITY_LEVEL(verbosity_WARNING);

  td::set_default_failure_signal_handler().ensure();

  td::actor::ActorOwn<TpsCounter> x;
  td::unique_ptr<td::LogInterface> logger_;
  SCOPE_EXIT {
    td::log_interface = td::default_log_interface;
  };

  td::OptionParser p;
  p.set_description(
      "Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor incididunt ut labore et dolore "
      "magna aliqua.\n");
  p.add_option('v', "verbosity", "set verbosity level", [&](td::Slice arg) {
    int v = VERBOSITY_NAME(FATAL) + (td::to_integer<int>(arg));
    SET_VERBOSITY_LEVEL(v);
  });
  p.add_option('d', "duration", "set duration (minutes, default=16)", [&](td::Slice arg) {
    duration = td::to_integer_safe<int>(arg).move_as_ok();
    CHECK(duration > 0);
  });
  p.add_option('\0', "delay", "starting moment is X seconds ago (default=30)", [&](td::Slice arg) {
    start_delay = td::to_integer_safe<int>(arg).move_as_ok();
    CHECK(start_delay >= 0);
  });
  p.add_option('M', "max-shards", "use only first X shards (default=unlimited)", [&](td::Slice arg) {
    max_shards = td::to_integer_safe<int>(arg).move_as_ok();
    CHECK(start_delay >= 0);
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

  td::actor::Scheduler scheduler({7});

  scheduler.run_in_context([&] { x = td::actor::create_actor<TpsCounter>("myexe"); });

  scheduler.run_in_context([&] { p.run(argc, argv).ensure(); });
  scheduler.run_in_context([&] { td::actor::send_closure(x, &TpsCounter::run); });
  while (scheduler.run(1)) {
  }

  return 0;
}