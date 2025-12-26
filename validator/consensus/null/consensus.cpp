#include "td/actor/coro_utils.h"
#include "validator/fabric.h"

#include "consensus-bus.h"

namespace ton::validator {

namespace {

struct SlotState {
  std::optional<RawCandidateRef> raw_candidate;
  std::optional<CandidateRef> candidate;

  std::vector<BlockSignature> signatures;
  ValidatorWeight total_signed_weight = 0;
  std::vector<bool> signed_by;

  bool validated = false, finalized = false;

  void add_signature(const PeerValidator& validator, td::BufferSlice signature) {
    if (finalized) {
      return;
    }
    auto idx = (size_t)validator.idx.value();
    if (idx >= signed_by.size()) {
      signed_by.resize(idx + 1);
    }
    if (signed_by[idx]) {
      return;
    }
    signed_by[idx] = true;
    signatures.emplace_back(validator.short_id.bits256_value(), std::move(signature));
    total_signed_weight += validator.weight;
  }
};

class NullConsensusImpl : public runtime::SpawnsWith<NullConsensusBus>, public runtime::ConnectsTo<NullConsensusBus> {
 public:
  TON_RUNTIME_DEFINE_EVENT_HANDLER();

  void start_up() {
    auto& bus = *owning_bus();

    validator_count_ = bus.validator_set.size();
    ValidatorWeight total_weight = 0;
    for (const auto& validator : bus.validator_set) {
      total_weight += validator.weight;
    }
    weight_threshold_ = (total_weight * 2) / 3 + 1;

    leader_ = bus.validator_set[0].idx;
    is_leader_ = bus.local_id.idx == leader_;

    if (bus.validator_set.size() == 1) {
      try_start_generation();
    } else if (is_leader_) {
      send_message({}, create_tl_object<ton_api::consensus_null_handshake>());
    } else {
      send_message(leader_, create_tl_object<ton_api::consensus_null_handshake>());
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
    auto message = fetch_tl_object<ton_api::consensus_null_Message>(event->message.data, true).move_as_ok();

    ton_api::downcast_call(*message, [&](auto& message) { handle_message(event->source, message); });
  }

 private:
  void send_message(std::optional<PeerValidatorId> recipient, ProtocolMessage message) {
    owning_bus().publish<ConsensusBus::OutgoingProtocolMessage>(recipient, std::move(message));
  }

  void handle_message(PeerValidatorId source, ton_api::consensus_null_handshake& handshake) {
    if (is_leader_) {
      if (seen_handshakes_.insert(source).second) {
        try_start_generation();
      }
    } else {
      send_message(leader_, create_tl_object<ton_api::consensus_null_handshake>());
    }
  }

  void handle_message(PeerValidatorId source, ton_api::consensus_null_signature& signature) {
    auto slot = static_cast<td::uint32>(signature.slot_);

    if (next_slot_to_finalize_ > slot) {
      return;
    }

    SlotState& state = block_states_[slot];
    state.add_signature(owning_bus()->validator_set[source.value()], std::move(signature.signature_));
    try_finalize_blocks();
  }

  void try_start_generation() {
    if (seen_handshakes_.size() == validator_count_ - 1) {
      owning_bus().publish<ConsensusBus::OurLeaderWindowStarted>(std::nullopt, 0,
                                                                 std::numeric_limits<td::uint32>::max());
    }
  }

  td::actor::Task<> on_new_candidate(RawCandidateRef candidate) {
    SlotState& state = block_states_[candidate->id.slot];
    CHECK(!state.raw_candidate.has_value());
    state.raw_candidate = candidate;

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
      SlotState& state = it->second;
      CHECK(!state.validated);
      if (!state.raw_candidate.has_value()) {
        break;
      }
      const auto& raw_candidate = *state.raw_candidate;

      CHECK(raw_candidate->parent_id == parent_for_validation_);

      CandidateRef candidate = td::make_ref<Candidate>(parent_for_validation_, raw_candidate);
      state.candidate = candidate;

      auto validation_result = co_await owning_bus().publish<ConsensusBus::ValidationRequest>(candidate).wrap();
      validation_result.ensure();

      auto signature_data = create_serialize_tl_object<ton_api::ton_blockId>(candidate->id.block.root_hash,
                                                                             candidate->id.block.file_hash);
      auto signature = co_await td::actor::ask(bus.keyring, &keyring::Keyring::sign_message, bus.local_id.short_id,
                                               std::move(signature_data));

      state.add_signature(bus.local_id, signature.clone());
      state.validated = true;

      send_message({}, create_tl_object<ton_api::consensus_null_signature>(candidate->id.slot, std::move(signature)));

      ++next_slot_to_validate_;
      parent_for_validation_ = raw_candidate->id;
      if (state.finalized) {
        block_states_.erase(it);
      }
    }

    co_return td::Unit{};
  }

  void try_finalize_blocks() {
    while (true) {
      auto it = block_states_.find(next_slot_to_finalize_);
      if (it == block_states_.end()) {
        break;
      }
      SlotState& state = it->second;
      CHECK(!state.finalized);
      if (state.total_signed_weight < weight_threshold_ || !state.candidate.has_value()) {
        break;
      }
      auto& bus = owning_bus();
      bus.publish<ConsensusBus::SlotFinalized>(
          *state.candidate, block::BlockSignatureSet::create_ordinary(std::move(state.signatures), bus->cc_seqno,
                                                                      bus->validator_set_hash));
      ++next_slot_to_finalize_;
      state.finalized = true;
      if (state.validated) {
        block_states_.erase(it);
      }
    }
  }

  std::set<PeerValidatorId> seen_handshakes_;

  size_t validator_count_ = 0;
  ValidatorWeight weight_threshold_ = 0;
  PeerValidatorId leader_;
  bool is_leader_ = false;

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
