#include "td/actor/coro_utils.h"

#include "consensus-bus.h"
#include "fabric.h"

using td::actor::Task, td::actor::StartedTask;

namespace ton::validator {

namespace {

struct BlockState {
  BlockIdExt id;
  std::shared_ptr<BlockCandidate const> candidate;

  std::size_t signatures_collected = 0;
  std::vector<BlockSignature> approve_signatures;
  std::vector<BlockSignature> sign_signatures;

  void set_or_check_id(const BlockIdExt& new_id) {
    if (!id.is_valid()) {
      id = new_id;
    } else {
      CHECK(id == new_id);
    }
  }

  bool is_ready(size_t validator_count) {
    return candidate && signatures_collected >= validator_count;
  }
};

class NullConsensusImpl : public runtime::SpawnsWith<NullConsensusBus>, public runtime::ConnectsTo<NullConsensusBus> {
 public:
  TON_RUNTIME_DEFINE_EVENT_HANDLER();

  void start_up() {
    auto& bus = *owning_bus();

    parents_ = bus.first_block_parents;
    for (auto parent : parents_) {
      last_finalized_seqno_ = std::max(last_finalized_seqno_, parent.id.seqno);
    }
    last_mc_finalized_seqno_ = last_finalized_seqno_;
    last_finalized_ = parents_;

    auto validators = bus.validator_set->export_vector();
    validator_count_ = validators.size();

    leader_ = PublicKey{pubkeys::Ed25519{validators[0].key}}.compute_short_id();
    is_leader_ = bus.local_id == leader_;

    if (is_leader_) {
      send_message({}, create_tl_object<ton_api::nullConsensus_handshake>());
    } else {
      send_message(leader_, create_tl_object<ton_api::nullConsensus_handshake>());
    }
  }

  template <>
  void handle(runtime::BusHandle<NullConsensusBus>, std::shared_ptr<ConsensusBus::StopRequested const>) {
    stop();
  }

  template <>
  void handle(runtime::BusHandle<NullConsensusBus>, std::shared_ptr<ConsensusBus::CandidateValidated const> event) {
    event->verdict.ensure();
    auto candidate = event->candidate;

    auto& state = block_states_[candidate->id.seqno()];
    state.set_or_check_id(state.id);
    CHECK(!state.candidate);
    state.candidate = candidate;

    broadcast_signatures(candidate).start().detach();

    if (candidate_promise_) {
      candidate_promise_.set_value(std::move(candidate));
    }
  }

  template <>
  void handle(runtime::BusHandle<NullConsensusBus>,
              std::shared_ptr<ConsensusBus::IncomingProtocolMessage const> event) {
    auto message = fetch_tl_object<ton_api::Object>(event->message.data, true).move_as_ok();

    ton_api::downcast_call(
        *message,
        td::overloaded(
            [&](ton_api::nullConsensus_handshake& handshake) {
              if (is_leader_) {
                bool inserted = seen_handshakes_.insert(event->source).second;
                if (inserted && seen_handshakes_.size() == validator_count_) {
                  run().start().detach();
                }
              } else {
                send_message(leader_, create_tl_object<ton_api::nullConsensus_handshake>());
              }
            },
            [&](ton_api::nullConsensus_signature& signature) {
              auto& state = block_states_[signature.blockId_->seqno_];
              state.set_or_check_id(create_block_id(signature.blockId_));

              ++state.signatures_collected;
              state.approve_signatures.push_back(
                  {event->source.bits256_value(), std::move(signature.approve_signature_)});
              state.sign_signatures.push_back({event->source.bits256_value(), std::move(signature.sign_signature_)});

              if (state.is_ready(validator_count_)) {
                try_finalize_blocks();
              }
            },
            [](auto&) {}));
  }

  template <>
  void handle(runtime::BusHandle<NullConsensusBus>,
              std::shared_ptr<ConsensusBus::MasterchainBlockFinalized const> event) {
    last_mc_finalized_seqno_ = event->block.seqno();
    if (mc_finalized_promise_) {
      mc_finalized_promise_.set_value(td::Unit{});
    }
  }

 private:
  template <typename T>
  void send_message(std::optional<PublicKeyHash> recipient, ton::tl_object_ptr<T> message) {
    auto data = serialize_tl_object(message, true);
    owning_bus().publish(std::make_shared<ConsensusBus::OutgoingProtocolMessage>(recipient, std::move(data)));
  }

  Task<td::Unit> broadcast_signatures(std::shared_ptr<BlockCandidate const> candidate) {
    auto& bus = *owning_bus();

    auto approve_signature_data =
        create_serialize_tl_object<ton_api::ton_blockIdApprove>(candidate->id.root_hash, candidate->id.file_hash);
    auto approve_signature = co_await td::actor::ask(bus.keyring, &keyring::Keyring::sign_message, bus.local_id,
                                                     std::move(approve_signature_data));
    auto sign_signature_data =
        create_serialize_tl_object<ton_api::ton_blockId>(candidate->id.root_hash, candidate->id.file_hash);
    auto sign_signature = co_await td::actor::ask(bus.keyring, &keyring::Keyring::sign_message, bus.local_id,
                                                  std::move(sign_signature_data));

    send_message({}, create_tl_object<ton_api::nullConsensus_signature>(
                         create_tl_block_id(candidate->id), std::move(approve_signature), std::move(sign_signature)));
    co_return {};
  }

  void try_finalize_blocks() {
    while (true) {
      auto it = block_states_.find(last_finalized_seqno_ + 1);
      if (it == block_states_.end() || !it->second.is_ready(validator_count_)) {
        break;
      }

      auto& state = it->second;

      owning_bus().publish(std::make_shared<ConsensusBus::BlockFinalized>(
          state.id, state.candidate->data.clone(), last_finalized_, pubkeys::Ed25519{state.candidate->pubkey},
          FinalizationCertificate{
              .approve_signatures = create_signature_set(std::move(state.approve_signatures)),
              .sign_signatures = create_signature_set(std::move(state.sign_signatures)),
          }));
      last_finalized_ = {state.id};

      ++last_finalized_seqno_;
    }
  }

  Task<std::shared_ptr<BlockCandidate const>> generate_block() {
    auto [awaiter, promise] = StartedTask<std::shared_ptr<BlockCandidate const>>::make_bridge();
    candidate_promise_ = std::move(promise);
    owning_bus().publish(std::make_shared<ConsensusBus::CandidateRequested>(parents_));
    auto candidate = co_await std::move(awaiter);
    candidate_promise_ = {};
    co_return candidate;
  }

  Task<td::Unit> run() {
    while (true) {
      while (parents_.size() == 1 && parents_[0].seqno() >= last_mc_finalized_seqno_ + 8) {
        auto [awaiter, promise] = StartedTask<td::Unit>::make_bridge();
        mc_finalized_promise_ = std::move(promise);
        co_await std::move(awaiter);
      }

      auto candidate = co_await generate_block();
      parents_ = {candidate->id};
    }
  }

  std::vector<BlockIdExt> parents_;
  td::Promise<std::shared_ptr<BlockCandidate const>> candidate_promise_;
  td::Promise<td::Unit> mc_finalized_promise_;

  std::set<PublicKeyHash> seen_handshakes_;

  size_t validator_count_;
  PublicKeyHash leader_;
  bool is_leader_;

  BlockSeqno last_mc_finalized_seqno_ = 0;
  BlockSeqno last_finalized_seqno_ = 0;
  std::vector<BlockIdExt> last_finalized_;
  std::map<BlockSeqno, BlockState> block_states_;
};

}  // namespace

void NullConsensus::register_in(runtime::Runtime& runtime) {
  runtime.register_actor<NullConsensusImpl>("NullConsensus");
}

}  // namespace ton::validator
