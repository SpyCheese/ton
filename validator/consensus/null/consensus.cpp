#include "td/actor/coro_utils.h"
#include "validator/fabric.h"

#include "consensus-bus.h"

namespace ton::validator {

namespace {

struct SlotState {
  std::optional<RawCandidateRef> raw_candidate;
  std::optional<CandidateRef> candidate;

  std::vector<BlockSignature> signatures;

  bool is_ready(size_t validator_count) {
    return candidate.has_value() && signatures.size() == validator_count;
  }
};

class NullConsensusImpl : public runtime::SpawnsWith<NullConsensusBus>, public runtime::ConnectsTo<NullConsensusBus> {
 public:
  TON_RUNTIME_DEFINE_EVENT_HANDLER();

  void start_up() {
    auto& bus = *owning_bus();

    validator_count_ = bus.validator_set.size();

    leader_ = bus.validator_set[0].idx;
    is_leader_ = bus.local_id.idx == leader_;

    if (bus.validator_set.size() == 1) {
      try_start_generation();
    } else if (is_leader_) {
      send_message({}, create_tl_object<ton_api::nullConsensus_handshake>());
    } else {
      send_message(leader_, create_tl_object<ton_api::nullConsensus_handshake>());
    }
  }

  template <>
  void handle(runtime::BusHandle<NullConsensusBus>, std::shared_ptr<const ConsensusBus::StopRequested>) {
    stop();
  }

  template <>
  void handle(runtime::BusHandle<NullConsensusBus>, std::shared_ptr<const ConsensusBus::CandidateReceived> event) {
    on_new_candidate(event->candidate).start().detach();
  }

  template <>
  void handle(runtime::BusHandle<NullConsensusBus>,
              std::shared_ptr<const ConsensusBus::IncomingProtocolMessage> event) {
    auto message = fetch_tl_object<ton_api::nullConsensus_Message>(event->message.data, true).move_as_ok();

    ton_api::downcast_call(*message, [&](auto& message) { handle_message(event->source, message); });
  }

 private:
  void send_message(std::optional<PeerValidatorId> recipient, ProtocolMessage message) {
    owning_bus().publish<ConsensusBus::OutgoingProtocolMessage>(recipient, std::move(message));
  }

  void handle_message(PeerValidatorId source, ton_api::nullConsensus_handshake& handshake) {
    if (is_leader_) {
      if (seen_handshakes_.insert(source).second) {
        try_start_generation();
      }
    } else {
      send_message(leader_, create_tl_object<ton_api::nullConsensus_handshake>());
    }
  }

  void handle_message(PeerValidatorId source, ton_api::nullConsensus_signature& signature) {
    auto& state = block_states_[signature.slot_];

    auto node_id = owning_bus()->validator_set[source.value()].short_id;
    state.signatures.push_back({node_id.bits256_value(), std::move(signature.signature_)});
    try_finalize_blocks();
  }

  void try_start_generation() {
    if (seen_handshakes_.size() == validator_count_ - 1) {
      owning_bus().publish<ConsensusBus::OurLeaderWindowStarted>(std::nullopt, 0,
                                                                 std::numeric_limits<td::uint32>::max());
    }
  }

  td::actor::Task<> on_new_candidate(RawCandidateRef candidate) {
    auto& slot = block_states_[candidate->id.slot];
    CHECK(!slot.raw_candidate.has_value());
    slot.raw_candidate = candidate;

    co_await try_validate_blocks();
    try_finalize_blocks();

    co_return td::Unit{};
  }

  td::actor::Task<> try_validate_blocks() {
    if (try_validate_blocks_running_) {
      co_return td::Unit{};
    }
    try_validate_blocks_running_ = true;
    SCOPE_EXIT {
      try_validate_blocks_running_ = false;
    };

    auto& bus = *owning_bus();

    while (true) {
      auto it = block_states_.find(next_slot_to_validate_);
      if (it == block_states_.end()) {
        break;
      }

      auto& state = it->second;
      if (!state.raw_candidate.has_value()) {
        break;
      }
      const auto& raw_candidate = *state.raw_candidate;

      CHECK(raw_candidate->parent_id == parent_for_validation_);

      auto id = raw_candidate->resolve_id(parent_for_validation_);
      state.candidate = td::make_ref<Candidate>(id, parent_for_validation_, raw_candidate);
      const auto& candidate = *state.candidate;

      auto validation_result = co_await owning_bus().publish<ConsensusBus::ValidationRequest>(candidate).wrap();
      validation_result.ensure();

      auto signature_data = create_serialize_tl_object<ton_api::ton_blockId>(candidate->id.block.root_hash,
                                                                             candidate->id.block.file_hash);
      auto signature = co_await td::actor::ask(bus.keyring, &keyring::Keyring::sign_message, bus.local_id.short_id,
                                               std::move(signature_data));
      state.signatures.push_back({bus.local_id.short_id.bits256_value(), signature.clone()});

      send_message({}, create_tl_object<ton_api::nullConsensus_signature>(candidate->id.slot, std::move(signature)));

      ++next_slot_to_validate_;
      parent_for_validation_ = id;
    }

    co_return td::Unit{};
  }

  void try_finalize_blocks() {
    while (true) {
      auto it = block_states_.find(next_slot_to_finalize_);
      if (it == block_states_.end() || !it->second.is_ready(validator_count_)) {
        break;
      }

      auto& state = it->second;

      owning_bus().publish<ConsensusBus::SlotFinalized>(*state.candidate,
                                                        create_signature_set(std::move(state.signatures)));

      ++next_slot_to_finalize_;
      block_states_.erase(it);
    }
  }

  std::set<PeerValidatorId> seen_handshakes_;

  size_t validator_count_;
  PeerValidatorId leader_;
  bool is_leader_;

  std::map<td::uint32, SlotState> block_states_;

  bool try_validate_blocks_running_ = false;
  ParentId parent_for_validation_;
  td::uint32 next_slot_to_validate_ = 0;

  td::uint32 next_slot_to_finalize_ = 0;
};

}  // namespace

void NullConsensus::register_in(runtime::Runtime& runtime) {
  runtime.register_actor<NullConsensusImpl>("NullConsensus");
}

}  // namespace ton::validator
