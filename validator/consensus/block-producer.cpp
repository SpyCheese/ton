#include "td/actor/coro_task.h"
#include "td/actor/coro_utils.h"
#include "td/utils/CancellationToken.h"

#include "consensus-bus.h"

namespace ton::validator {

namespace {

class CandidateParent {
 public:
  CandidateParent(const ConsensusBus& bus, const ParentId& parent) {
    parent_blocks_ = bus.convert_id_to_blocks(parent);
    seqno_ = parent_blocks_.size() == 1 ? parent_blocks_[0].seqno()
                                        : std::max(parent_blocks_[0].seqno(), parent_blocks_[1].seqno());
    parent_id_ = parent;
  }

  CandidateParent(const CandidateId& id) {
    parent_blocks_ = {id.block};
    seqno_ = id.block.seqno();
    parent_id_ = id;
  }

  const std::vector<BlockIdExt>& parent_blocks() const {
    return parent_blocks_;
  }

  int seqno() const {
    return seqno_;
  }

  int next_seqno() const {
    return seqno_ + 1;
  }

  ParentId id() const {
    return parent_id_;
  }

 private:
  std::vector<BlockIdExt> parent_blocks_;
  int seqno_;
  ParentId parent_id_;
};

class BlockProducerImpl : public runtime::SpawnsWith<ConsensusBus>, public runtime::ConnectsTo<ConsensusBus> {
 public:
  TON_RUNTIME_DEFINE_EVENT_HANDLER();

  void start_up() {
    auto& bus = *owning_bus();
    max_answer_size_ = bus.config.max_block_size + bus.config.max_collated_data_size + 1024;

    last_mc_finalized_seqno_ = last_consensus_finalized_seqno_ = CandidateParent{bus, std::nullopt}.seqno();
  }

  template <>
  void handle(runtime::BusHandle<ConsensusBus>, std::shared_ptr<const ConsensusBus::StopRequested>) {
    current_leader_window_ = std::nullopt;
    cancellation_source_.cancel();
    stop();
  }

  template <>
  void handle(runtime::BusHandle<ConsensusBus>, std::shared_ptr<const ConsensusBus::SlotFinalized> event) {
    if (event->finalization_cert.has_value()) {
      last_consensus_finalized_seqno_ = event->candidate->id.block.seqno();
    }
  }

  template <>
  void handle(runtime::BusHandle<ConsensusBus>, std::shared_ptr<const ConsensusBus::OurLeaderWindowStarted> event) {
    current_leader_window_ = event->start_slot;
    cancellation_source_ = td::CancellationTokenSource();
    generate_candidates(event).start().detach();
  }

  template <>
  void handle(runtime::BusHandle<ConsensusBus>, std::shared_ptr<const ConsensusBus::OurLeaderWindowAborted> event) {
    // Sanity check: consensus and us should agree on the start slot.
    CHECK(current_leader_window_ == event->start_slot);
    current_leader_window_ = std::nullopt;
    cancellation_source_ = td::CancellationTokenSource();
  }

  template <>
  void handle(runtime::BusHandle<ConsensusBus>,
              std::shared_ptr<const ConsensusBus::BlockFinalizedInMasterchain> event) {
    last_mc_finalized_seqno_ = event->block.seqno();
  }

 private:
  bool should_generate_empty_block(BlockSeqno new_seqno) {
    if (owning_bus()->shard.is_masterchain()) {
      return last_consensus_finalized_seqno_ + 1 < new_seqno;
    } else {
      return last_mc_finalized_seqno_ + 8 < new_seqno;
    }
  }

  td::actor::Task<> generate_candidates(std::shared_ptr<const ConsensusBus::OurLeaderWindowStarted> event) {
    auto& bus = *owning_bus();

    auto window = current_leader_window_;
    if (window == std::nullopt) {
      co_return td::Unit{};
    }

    td::Timestamp target_time = td::Timestamp::now();

    CandidateParent parent{bus, event->base};

    td::uint32 slot = event->start_slot;

    while (current_leader_window_ == window && slot < event->end_slot) {
      co_await td::actor::coro_sleep(target_time);

      BlockSeqno new_seqno = parent.next_seqno();

      RawCandidateId raw_candidate_id;
      std::optional<BlockCandidate> block;
      std::optional<adnl::AdnlNodeIdShort> collator;

      if (should_generate_empty_block(new_seqno)) {
        LOG(WARNING) << "Generating an empty block for slot " << slot << "! new_seqno=" << new_seqno
                     << ", last_consensus_finalized_seqno_=" << last_consensus_finalized_seqno_
                     << ", last_mc_finalized_seqno_=" << last_mc_finalized_seqno_;
        CHECK(parent.id().has_value());  // first generated block in an epoch cannot be empty

        raw_candidate_id = RawCandidateId::create(slot, std::nullopt, parent.id());
      } else {
        // Before doing anything substantial, check the leader window.
        if (current_leader_window_ != window) {
          break;
        }

        // FIXME: What to do if collate_block suddenly fails?
        auto block_candidate = co_await td::actor::ask(
            bus.collation_manager, &CollationManager::collate_block, bus.shard, bus.min_masterchain_block_id,
            parent.parent_blocks(), Ed25519_PublicKey{bus.local_id.key.ed25519_value().raw()}, BlockCandidatePriority{},
            bus.validator_set_for_external_code, max_answer_size_, cancellation_source_.get_cancellation_token());

        block = std::move(block_candidate.candidate);
        if (!block_candidate.collator_node_id.is_zero()) {
          collator = adnl::AdnlNodeIdShort{block_candidate.collator_node_id};
        }

        raw_candidate_id = RawCandidateId::create(slot, block, parent.id());
      }

      auto candidate_id_to_sign = create_serialize_tl_object<ton_api::consensus_ordinaryCandidateParent>(
          raw_candidate_id.slot, raw_candidate_id.hash);
      auto data_to_sign =
          create_serialize_tl_object<ton_api::consensus_dataToSign>(bus.session_id, std::move(candidate_id_to_sign));
      auto signature = co_await td::actor::ask(bus.keyring, &keyring::Keyring::sign_message, bus.local_id.short_id,
                                               std::move(data_to_sign));

      auto raw_candidate = td::make_ref<RawCandidate>(raw_candidate_id, parent.id(), bus.local_id.idx, std::move(block),
                                                      std::move(signature));

      if (current_leader_window_ != window) {
        break;
      }
      owning_bus().publish<ConsensusBus::CandidateGenerated>(raw_candidate, collator);
      owning_bus().publish<ConsensusBus::CandidateReceived>(raw_candidate);

      ++slot;
      parent = raw_candidate->resolve_id(parent.id());
      target_time = td::Timestamp::in(bus.config.target_rate_ms / 1000., target_time);
    }

    co_return td::Unit{};
  }

  std::optional<td::uint32> current_leader_window_;
  td::CancellationTokenSource cancellation_source_;

  td::uint64 max_answer_size_;

  BlockSeqno last_consensus_finalized_seqno_ = 0;
  BlockSeqno last_mc_finalized_seqno_ = 0;
};

}  // namespace

void BlockProducer::register_in(runtime::Runtime& runtime) {
  runtime.register_actor<BlockProducerImpl>("BlockProducer");
}

}  // namespace ton::validator
