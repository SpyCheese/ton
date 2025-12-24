#include "consensus-bus.h"
#include "full-node.h"

namespace ton::validator {

namespace {

class BlockAccepterImpl : public runtime::SpawnsWith<ConsensusBus>, public runtime::ConnectsTo<ConsensusBus> {
 public:
  TON_RUNTIME_DEFINE_EVENT_HANDLER();

  template <>
  void handle(runtime::BusHandle<ConsensusBus>, std::shared_ptr<const ConsensusBus::StopRequested>) {
    stop();
  }

  template <>
  void handle(runtime::BusHandle<ConsensusBus>, std::shared_ptr<const ConsensusBus::SlotFinalized> last_slot) {
    finalize_burst.push_back(last_slot);
    if (!last_slot->finalization_cert) {
      return;
    }

    for (const auto& slot : finalize_burst) {
      auto& candidate = slot->candidate;
      if (auto* block = std::get_if<BlockCandidate>(&candidate->block)) {
        td::Ref<BlockSignatureSet> signatures = {};
        if (candidate->id.block == last_slot->candidate->id.block) {
          signatures = *last_slot->finalization_cert;
        }
        CHECK(candidate->id.block == block->id);
        auto block_data = create_block(block->id, block->data.clone()).move_as_ok();
        auto block_parents = owning_bus()->convert_id_to_blocks(candidate->parent_id);

        td::actor::ask(owning_bus()->manager, &ManagerFacade::accept_block, block->id, block_data, block_parents,
                       signatures, fullnode::FullNode::broadcast_mode_public, true)
            .detach();
      }
    }

    finalize_burst.clear();
  }

 private:
  std::vector<std::shared_ptr<const ConsensusBus::SlotFinalized>> finalize_burst;
};

}  // namespace

void BlockAccepter::register_in(runtime::Runtime& runtime) {
  runtime.register_actor<BlockAccepterImpl>("BlockAccepter");
}

}  // namespace ton::validator
