#include "td/actor/coro_utils.h"
#include "validator/fabric.h"

#include "candidate-parent.h"
#include "consensus-bus.h"

namespace ton::validator {

namespace {

class BlockValidatorImpl : public runtime::SpawnsWith<ConsensusBus>, public runtime::ConnectsTo<ConsensusBus> {
 public:
  TON_RUNTIME_DEFINE_EVENT_HANDLER();

  template <>
  void handle(runtime::BusHandle<ConsensusBus>, std::shared_ptr<const ConsensusBus::StopRequested>) {
    stop();
  }

  template <>
  td::actor::Task<> handle(runtime::BusHandle<ConsensusBus>, std::shared_ptr<ConsensusBus::ValidationRequest> event) {
    auto& bus = *owning_bus();

    ValidateParams validate_params{
        .shard = bus.shard,
        .min_masterchain_block_id = bus.min_masterchain_block_id,
        .prev = CandidateParent{bus, event->parent}.parent_blocks(),
        .validator_set = bus.validator_set_for_external_code,
        .local_validator_id = bus.local_id.short_id,
    };
    auto [awaiter, promise] = td::actor::StartedTask<ValidateCandidateResult>::make_bridge();
    run_validate_query(event->candidate->block.clone(), validate_params, bus.real_manager_for_external_code,
                       td::Timestamp::in(60), std::move(promise));
    auto maybe_candidate_reject = co_await std::move(awaiter);

    if (maybe_candidate_reject.has<CandidateReject>()) {
      auto error = td::Status::Error(0, maybe_candidate_reject.get<CandidateReject>().reason);

      if (event->candidate->leader == bus.local_id.idx) {
        LOG(ERROR) << "BUG! Candidate " << event->candidate->id << " (block: " << event->candidate->block.id.to_str()
                   << ") is self-rejected: " << error;
      }

      co_return error;
    }

    co_return td::Unit{};
  }
};

}  // namespace

void BlockValidator::register_in(runtime::Runtime& runtime) {
  runtime.register_actor<BlockValidatorImpl>("BlockValidator");
}

}  // namespace ton::validator
