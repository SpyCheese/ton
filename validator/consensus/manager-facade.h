#pragma once

#include "interfaces/validator-manager.h"
#include "td/actor/actor.h"

namespace ton::validator {

class ManagerFacade : public td::actor::Actor {
 public:
  ManagerFacade(td::actor::ActorId<ValidatorManager> manager) : manager_(manager) {
  }

  void set_block_candidate(BlockIdExt id, BlockCandidate candidate, CatchainSeqno cc_seqno,
                           td::uint32 validator_set_hash, td::Promise<td::Unit> promise) {
    td::actor::send_closure(manager_, &ValidatorManager::set_block_candidate, id, std::move(candidate), cc_seqno,
                            validator_set_hash, std::move(promise));
  }

  void log_validator_session_stats(validatorsession::ValidatorSessionStats stats) {
    td::actor::send_closure(manager_, &ValidatorManager::log_validator_session_stats, std::move(stats));
  }

 private:
  td::actor::ActorId<ValidatorManager> manager_;
};

}  // namespace ton::validator
