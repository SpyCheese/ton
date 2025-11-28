#include "td/actor/coro_task.h"
#include "td/actor/coro_utils.h"
#include "td/utils/CancellationToken.h"

#include "candidate-parent.h"
#include "consensus-bus.h"

namespace ton::validator {

namespace {

class BlockProducerImpl : public runtime::SpawnsWith<ConsensusBus>, public runtime::ConnectsTo<ConsensusBus> {
 public:
  TON_RUNTIME_DEFINE_EVENT_HANDLER();

  void start_up() {
    auto& bus = *owning_bus();
    max_answer_size_ = bus.config.max_block_size + bus.config.max_collated_data_size + 1024;
  }

  template <>
  void handle(runtime::BusHandle<ConsensusBus>, std::shared_ptr<const ConsensusBus::StopRequested>) {
    current_leader_window_ = std::nullopt;
    cancellation_source_.cancel();
    stop();
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
    if (mc_finalized_promise_) {
      mc_finalized_promise_.set_value(td::Unit{});
    }
  }

 private:
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

      // FIXME: Generate an empty block instead to move consensus forward.
      while (current_leader_window_ == window && last_mc_finalized_seqno_ + 8 < new_seqno) {
        auto [awaiter, promise] = td::actor::StartedTask<td::Unit>::make_bridge();
        mc_finalized_promise_ = std::move(promise);
        LOG(WARNING) << "Masterchain is too slow! Waiting for it to finalize block with seqno at least "
                     << new_seqno - 8 << " (current finalized: " << last_mc_finalized_seqno_ << ")";
        co_await std::move(awaiter);
      }
      if (current_leader_window_ != window) {
        break;
      }

      // FIXME: What to do if collate_block fails? Now the consensus will just eventually switch to
      //        the next leader.
      auto block_candidate = co_await td::actor::ask(
          bus.collation_manager, &CollationManager::collate_block, bus.shard, bus.min_masterchain_block_id,
          parent.parent_blocks(), Ed25519_PublicKey{bus.local_id.key.ed25519_value().raw()}, BlockCandidatePriority{},
          bus.validator_set_for_external_code, max_answer_size_, cancellation_source_.get_cancellation_token());

      std::optional collator = block_candidate.collator_node_id;
      if (block_candidate.self_collated) {
        collator = std::nullopt;
      }

      auto candidate_id = CandidateId::create(slot, block_candidate.candidate, parent.candidate_id());

      auto candidate_id_to_sign =
          create_serialize_tl_object<ton_api::consensus_ordinaryCandidateParent>(candidate_id.slot, candidate_id.hash);
      auto data_to_sign =
          create_serialize_tl_object<ton_api::consensus_dataToSign>(bus.session_id, std::move(candidate_id_to_sign));
      auto signature = co_await td::actor::ask(bus.keyring, &keyring::Keyring::sign_message, bus.local_id.short_id,
                                               std::move(data_to_sign));

      auto candidate = td::make_ref<Candidate>(candidate_id, parent.candidate_id(), bus.local_id.idx,
                                               std::move(block_candidate.candidate), std::move(signature));

      owning_bus().publish<ConsensusBus::CandidateGenerated>(candidate, collator);
      owning_bus().publish<ConsensusBus::CandidateReceived>(candidate);

      parent = candidate;
      ++slot;
      target_time = td::Timestamp::in(bus.config.target_rate_ms / 1000., target_time);
    }

    co_return td::Unit{};
  }

  std::optional<td::uint32> current_leader_window_;
  td::CancellationTokenSource cancellation_source_;

  td::uint64 max_answer_size_;

  BlockSeqno last_mc_finalized_seqno_ = 0;
  td::Promise<td::Unit> mc_finalized_promise_;
};

}  // namespace

void BlockProducer::register_in(runtime::Runtime& runtime) {
  runtime.register_actor<BlockProducerImpl>("BlockProducer");
}

}  // namespace ton::validator
