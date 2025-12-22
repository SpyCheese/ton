#include "null/consensus-bus.h"
#include "validator-session/validator-session-types.h"
#include "validator/fabric.h"
#include "validator/full-node.h"
#include "validator/validator-group.hpp"

#include "runtime.h"

namespace ton::validator {

namespace {

class ConsensusBridgeBus : public NullConsensusBus {
 public:
  using Parent = NullConsensusBus;
  using Events = td::TypeList<>;

  ConsensusBridgeBus() = default;
};

class BlockAccepter : public runtime::SpawnsWith<ConsensusBridgeBus>, public runtime::ConnectsTo<ConsensusBridgeBus> {
 public:
  TON_RUNTIME_DEFINE_EVENT_HANDLER();

  template <>
  void handle(runtime::BusHandle<ConsensusBridgeBus>, std::shared_ptr<const ConsensusBus::StopRequested>) {
    stop();
  }

  template <>
  void handle(runtime::BusHandle<ConsensusBridgeBus>, std::shared_ptr<const ConsensusBus::SlotFinalized> last_slot) {
    finalize_burst.push_back(last_slot);
    if (!last_slot->finalization_cert) {
      return;
    }

    for (const auto& slot : finalize_burst) {
      auto& candidate = slot->candidate;
      if (candidate->block.has_value()) {
        auto& block = *candidate->block;

        td::Ref<BlockSignatureSet> signatures = {};
        if (candidate->id.block == last_slot->candidate->id.block) {
          signatures = *last_slot->finalization_cert;
        }
        auto block_data = create_block(block.id, block.data.clone()).move_as_ok();
        auto block_parents = owning_bus()->convert_id_to_blocks(candidate->parent_id);

        run_accept_block_query(
            block.id, block_data, block_parents, owning_bus()->validator_set_for_external_code, signatures,
            fullnode::FullNode::broadcast_mode_public, true, owning_bus()->real_manager_for_external_code,
            td::lambda_promise([](td::Result<td::Unit> result) {
              LOG_CHECK(!result.is_error()) << "Failed to accept finalized block " << result.move_as_error();
            }));
      }
    }

    finalize_burst.clear();
  }

 private:
  std::vector<std::shared_ptr<const ConsensusBus::SlotFinalized>> finalize_burst;
};

struct BridgeCreationParams {
  std::string name;
  bool is_create_session_called;

  ShardIdFull shard;
  td::actor::ActorId<ValidatorManager> manager;
  td::actor::ActorId<keyring::Keyring> keyring;
  td::Ref<ValidatorManagerOptions> validator_opts;

  td::Ref<ValidatorSet> validator_set;
  PublicKeyHash local_id;

  td::actor::ActorId<CollationManager> collation_manager;
  NewConsensusConfig config;
  BlockIdExt min_masterchain_block_id = {};

  ValidatorSessionId session_id;
  td::actor::ActorId<overlay::Overlays> overlays;

  std::vector<BlockIdExt> first_block_parents = {};
};

class BridgeImpl final : public IValidatorGroup {
 public:
  BridgeImpl(BridgeCreationParams&& params)
      : is_create_session_called_(params.is_create_session_called), params_(std::move(params)) {
  }

  virtual void start(std::vector<BlockIdExt> prev, BlockIdExt min_masterchain_block_id) override {
    CHECK(!is_start_called_);
    is_start_called_ = true;
    params_.min_masterchain_block_id = min_masterchain_block_id;
    params_.first_block_parents = prev;
    try_start();
  }

  virtual void create_session() override {
    CHECK(!is_create_session_called_);
    is_create_session_called_ = true;
    try_start();
  }

  virtual void update_options(td::Ref<ValidatorManagerOptions> opts, bool apply_blocks) override {
    // TODO
  }

  virtual void get_validator_group_info_for_litequery(
      td::Promise<tl_object_ptr<lite_api::liteServer_nonfinal_validatorGroupInfo>> promise) override {
    // TODO
    promise.set_error(td::Status::Error("Not implemented"));
  }

  virtual void notify_mc_finalized(BlockIdExt block) override {
    if (bus_) {
      bus_.publish<ConsensusBus::BlockFinalizedInMasterchain>(block);
    }
  }

  virtual void destroy() override {
    if (is_started_) {
      bus_.publish<ConsensusBus::StopRequested>();
    }
    stop();
  }

 private:
  void try_start() {
    if (!is_start_called_ || !is_create_session_called_ || is_started_) {
      return;
    }

    runtime::Runtime runtime;
    runtime.register_actor<BlockAccepter>("BlockAccepter");
    BlockProducer::register_in(runtime);
    BlockValidator::register_in(runtime);
    PrivateOverlay::register_in(runtime);
    NullConsensus::register_in(runtime);
    // StatsCollector::register_in(runtime);

    manager_facade_ = td::actor::create_actor<ManagerFacade>(params_.name + ".ManagerFacade", params_.manager);

    auto bus = std::make_shared<ConsensusBridgeBus>();

    bus->shard = params_.shard;
    bus->manager = manager_facade_.get();
    bus->real_manager_for_external_code = params_.manager;
    bus->keyring = params_.keyring;
    bus->validator_opts = params_.validator_opts;

    bus->validator_set_for_external_code = params_.validator_set;
    bool found = false;
    int idx = 0;
    for (const auto& el : params_.validator_set->export_vector()) {
      PublicKey key{pubkeys::Ed25519{el.key}};
      PublicKeyHash short_id = key.compute_short_id();

      bus->validator_set.push_back(PeerValidator{
          .idx = PeerValidatorId{idx},
          .key = key,
          .short_id = short_id,
          .adnl_id = adnl::AdnlNodeIdShort{el.addr.is_zero() ? short_id.bits256_value() : el.addr},
          .weight = el.weight,
      });

      if (short_id == params_.local_id) {
        found = true;
        bus->local_id = bus->validator_set.back();
      }

      ++idx;
    }
    CHECK(found);

    bus->collation_manager = params_.collation_manager;
    bus->config = std::move(params_.config);
    bus->min_masterchain_block_id = params_.min_masterchain_block_id;

    bus->session_id = params_.session_id;
    bus->overlays = params_.overlays;

    bus->first_block_parents = std::move(params_.first_block_parents);

    bus_ = runtime.start(std::move(bus), params_.name);

    is_started_ = true;
  }

  bool is_start_called_ = false;
  bool is_create_session_called_ = false;
  bool is_started_ = false;

  BridgeCreationParams params_;
  td::actor::ActorOwn<ManagerFacade> manager_facade_;

  runtime::BusHandle<ConsensusBridgeBus> bus_;
};

}  // namespace

td::actor::ActorOwn<IValidatorGroup> IValidatorGroup::create_bridge(
    td::Slice name, ShardIdFull shard, PublicKeyHash local_id, ValidatorSessionId session_id,
    td::Ref<ValidatorSet> validator_set, BlockSeqno last_key_block_seqno, NewConsensusConfig config,
    td::actor::ActorId<keyring::Keyring> keyring, td::actor::ActorId<adnl::Adnl> adnl,
    td::actor::ActorId<rldp::Rldp> rldp, td::actor::ActorId<rldp2::Rldp> rldp2,
    td::actor::ActorId<overlay::Overlays> overlays, std::string db_root,
    td::actor::ActorId<ValidatorManager> validator_manager, td::actor::ActorId<CollationManager> collation_manager,
    bool create_session, bool allow_unsafe_self_blocks_resync, td::Ref<ValidatorManagerOptions> opts,
    bool monitoring_shard) {
  BridgeCreationParams params{
      .name = std::string(name.begin(), name.end()),
      .is_create_session_called = create_session,
      .shard = shard,
      .manager = validator_manager,
      .keyring = keyring,
      .validator_opts = opts,
      .validator_set = std::move(validator_set),
      .local_id = std::move(local_id),
      .collation_manager = collation_manager,
      .config = std::move(config),
      .session_id = std::move(session_id),
      .overlays = overlays,
  };
  return td::actor::create_actor<BridgeImpl>(name, std::move(params));
}

}  // namespace ton::validator
