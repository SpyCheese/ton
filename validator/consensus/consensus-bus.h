#pragma once

#include "keyring/keyring.hpp"
#include "keys/keys.hpp"
#include "overlay/overlays.h"
#include "ton/ton-types.h"

#include "consensus-types.h"
#include "manager-facade.h"
#include "runtime.h"

namespace ton::validator {

class ConsensusBus : public runtime::Bus {
 public:
  struct StopRequested {};

  struct SlotFinalized {
    CandidateRef candidate;
    std::optional<td::Ref<BlockSignatureSet>> finalization_cert;

    std::string contents_to_string() const;
  };

  struct OurLeaderWindowStarted {
    ParentId base;
    td::uint32 start_slot;
    td::uint32 end_slot;

    std::string contents_to_string() const;
  };

  struct OurLeaderWindowAborted {
    td::uint32 start_slot;

    std::string contents_to_string() const;
  };

  struct CandidateGenerated {
    RawCandidateRef candidate;
    std::optional<adnl::AdnlNodeIdShort> collator_id;

    std::string contents_to_string() const;
  };

  // The only guarantee is that the candidate has a valid signature from `candidate->leader`.
  struct CandidateReceived {
    RawCandidateRef candidate;

    std::string contents_to_string() const;
  };

  // Checks that if candidate contains a block, then BlockCandidate is a valid block built on top of
  // the parent.
  struct ValidationRequest {
    using ReturnType = td::Unit;

    CandidateRef candidate;

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

  using Events = td::TypeList<StopRequested, SlotFinalized, OurLeaderWindowStarted, OurLeaderWindowAborted,
                              CandidateGenerated, CandidateReceived, ValidationRequest, IncomingProtocolMessage,
                              OutgoingProtocolMessage, BlockFinalizedInMasterchain>;

  ConsensusBus() = default;

  std::vector<BlockIdExt> convert_id_to_blocks(ParentId parent) const;

  ValidatorSessionId session_id;

  ShardIdFull shard;
  td::actor::ActorId<ManagerFacade> manager;
  td::actor::ActorId<keyring::Keyring> keyring;
  td::Ref<ValidatorManagerOptions> validator_opts;

  std::vector<PeerValidator> validator_set;
  PeerValidator local_id;

  NewConsensusConfig config;
  BlockIdExt min_masterchain_block_id;

  td::actor::ActorId<overlay::Overlays> overlays;

  std::vector<BlockIdExt> first_block_parents;
};

struct BlockAccepter {
  static void register_in(runtime::Runtime&);
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
