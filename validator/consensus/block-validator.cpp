#include "td/actor/coro_utils.h"

#include "consensus-bus.h"

using td::actor::Task;

namespace ton::validator {

namespace {

class BlockValidatorImpl : public runtime::SpawnsWith<ConsensusBus>, public runtime::ConnectsTo<ConsensusBus> {
 public:
  TON_RUNTIME_DEFINE_EVENT_HANDLER();

  template <>
  void handle(runtime::BusHandle<ConsensusBus>, std::shared_ptr<ConsensusBus::StopRequested const>) {
    stop();
  }

  template <>
  void handle(runtime::BusHandle<ConsensusBus>, std::shared_ptr<ConsensusBus::CandidateGenerated const> event) {
    validate_candidate(event->candidate).start().detach();
  }

  template <>
  void handle(runtime::BusHandle<ConsensusBus>, std::shared_ptr<ConsensusBus::CandidateReceived const> event) {
    validate_candidate(event->candidate).start().detach();
  }

 private:
  Task<td::Unit> validate_candidate(std::shared_ptr<BlockCandidate const> candidate) {
    auto& bus = *owning_bus();
    co_await td::actor::ask(owning_bus()->manager, &ManagerFacade::set_block_candidate, candidate->id,
                            candidate->clone(), 0, bus.validator_set->get_validator_set_hash());

    owning_bus().publish(std::make_shared<ConsensusBus::CandidateValidated>(candidate, td::Status::OK()));

    co_return {};
  }
};

}  // namespace

void BlockValidator::register_in(runtime::Runtime& runtime) {
  runtime.register_actor<BlockValidatorImpl>("BlockValidator");
}

}  // namespace ton::validator
