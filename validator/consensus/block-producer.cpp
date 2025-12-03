#include "td/actor/coro_task.h"
#include "td/actor/coro_utils.h"
#include "td/utils/CancellationToken.h"

#include "consensus-bus.h"

using td::actor::Task, td::actor::StartedTask;

namespace ton::validator {

namespace {

class BlockProducerImpl : public runtime::SpawnsWith<ConsensusBus>, public runtime::ConnectsTo<ConsensusBus> {
 public:
  TON_RUNTIME_DEFINE_EVENT_HANDLER();

  void start_up() {
    auto& bus = *owning_bus();
    max_answer_size_ = bus.config.max_block_size + bus.config.max_collated_data_size + 1024;

    bool found = false;
    for (auto& el : bus.validator_set->export_vector()) {
      if (PublicKey{pubkeys::Ed25519{el.key}}.compute_short_id() == bus.local_id) {
        CHECK(!found);
        found = true;
        local_id_full_ = el.key;
      }
    }
    CHECK(found);
  }

  template <>
  void handle(runtime::BusHandle<ConsensusBus>, std::shared_ptr<ConsensusBus::StopRequested const>) {
    cancellation_source.cancel();
    stop();
  }

  template <>
  void handle(runtime::BusHandle<ConsensusBus>, std::shared_ptr<ConsensusBus::CandidateRequested const> request) {
    create_block_candidate(std::move(request)).start().detach();
  }

 private:
  Task<td::Unit> create_block_candidate(std::shared_ptr<ConsensusBus::CandidateRequested const> request) {
    auto& bus = *owning_bus();
    auto candidate = co_await td::actor::ask(bus.collation_manager, &CollationManager::collate_block, bus.shard,
                                             bus.min_masterchain_block_id, request->parents, local_id_full_,
                                             BlockCandidatePriority{}, bus.validator_set, max_answer_size_,
                                             cancellation_source.get_cancellation_token());

    std::optional collator = candidate.collator_node_id;
    if (candidate.self_collated) {
      collator = std::nullopt;
    }

    owning_bus().publish(std::make_shared<ConsensusBus::CandidateGenerated>(
        std::make_shared<BlockCandidate>(std::move(candidate.candidate)), collator));
    co_return {};
  }

  td::CancellationTokenSource cancellation_source;

  td::uint64 max_answer_size_;
  Ed25519_PublicKey local_id_full_;
};

}  // namespace

void BlockProducer::register_in(runtime::Runtime& runtime) {
  runtime.register_actor<BlockProducerImpl>("BlockProducer");
}

}  // namespace ton::validator
