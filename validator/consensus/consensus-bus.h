#pragma once

#include "keyring/keyring.hpp"
#include "keys/keys.hpp"
#include "overlay/overlays.h"
#include "ton/ton-types.h"
#include "validator/collation-manager.hpp"

#include "consensus-types.h"
#include "manager-facade.h"
#include "runtime.h"

namespace ton::validator {

class ConsensusBus : public runtime::Bus {
 public:
  struct StopRequested {};

  struct BlockFinalized {
    CandidateRef candidate;
    ParentRef parent;
    td::Ref<BlockSignatureSet> signatures;

    std::string contents_to_string() const;
  };

  struct OurLeaderWindowStarted {
    ParentRef base;
    td::uint32 start_slot;
    td::uint32 end_slot;

    std::string contents_to_string() const;
  };

  struct OurLeaderWindowAborted {
    td::uint32 start_slot;

    std::string contents_to_string() const;
  };

  struct CandidateGenerated {
    CandidateRef candidate;
    std::optional<td::Bits256> collator_id;

    std::string contents_to_string() const;
  };

  struct CandidateReceived {
    CandidateRef candidate;

    std::string contents_to_string() const;
  };

  struct ValidationRequest {
    using ReturnType = td::Unit;

    CandidateRef candidate;
    ParentRef parent;

    std::string contents_to_string() const;
  };

  struct IncomingProtocolMessage {
    PeerValidatorId source;
    ProtocolMessage message;

    std::string contents_to_string() const;
  };

  struct OutgoingProtocolMessage {
    std::optional<PeerValidatorId> recipient;
    ProtocolMessage message;

    std::string contents_to_string() const;
  };

  struct BlockFinalizedInMasterchain {
    BlockIdExt block;

    std::string contents_to_string() const;
  };

  using Events = td::TypeList<StopRequested, BlockFinalized, OurLeaderWindowStarted, OurLeaderWindowAborted,
                              CandidateGenerated, CandidateReceived, ValidationRequest, IncomingProtocolMessage,
                              OutgoingProtocolMessage, BlockFinalizedInMasterchain>;

  ConsensusBus() = default;

  ValidatorSessionId session_id;

  ShardIdFull shard;
  td::actor::ActorId<ManagerFacade> manager;
  td::actor::ActorId<ValidatorManager> real_manager_for_external_code;
  td::actor::ActorId<keyring::Keyring> keyring;
  td::Ref<ValidatorManagerOptions> validator_opts;

  std::vector<PeerValidator> validator_set;
  PeerValidator local_id;
  td::Ref<ValidatorSet> validator_set_for_external_code;

  td::actor::ActorId<CollationManager> collation_manager;
  NewConsensusConfig config;
  BlockIdExt min_masterchain_block_id;

  td::actor::ActorId<overlay::Overlays> overlays;

  std::vector<BlockIdExt> first_block_parents;
};

struct BlockProducer {
  static void register_in(runtime::Runtime&);
};

struct BlockValidator {
  static void register_in(runtime::Runtime&);
};

struct PrivateOverlay {
  static void register_in(runtime::Runtime&);
};

// struct StatsCollector {
//   static void register_in(runtime::Runtime&);
// };

}  // namespace ton::validator
