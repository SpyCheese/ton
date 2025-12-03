#pragma once

#include "keyring/keyring.hpp"
#include "keys/keys.hpp"
#include "overlay/overlays.h"
#include "ton/ton-types.h"

#include "collation-manager.hpp"
#include "manager-facade.h"
#include "runtime.h"

namespace ton::validator {

struct FinalizationCertificate {
  td::Ref<BlockSignatureSet> approve_signatures;
  td::Ref<BlockSignatureSet> sign_signatures;
};

struct ProtocolMessage {
  static constexpr size_t max_length = 1024;

  ProtocolMessage(td::BufferSlice data) : data(std::move(data)) {
    CHECK(this->data.size() <= max_length);
  }

  td::BufferSlice data;
};

class ConsensusBus : public runtime::Bus {
 public:
  struct StopRequested {};

  struct BlockFinalized {
    BlockIdExt id;
    td::BufferSlice block;
    std::vector<BlockIdExt> parents;

    PublicKey leader;
    FinalizationCertificate finalization_cert;

    std::string contents_to_string() const;
  };

  struct CandidateRequested {
    std::vector<BlockIdExt> parents;

    std::string contents_to_string() const;
  };

  struct CandidateGenerated {
    std::shared_ptr<BlockCandidate const> candidate;
    std::optional<td::Bits256> collator_id;

    std::string contents_to_string() const;
  };

  struct CandidateReceived {
    std::shared_ptr<BlockCandidate const> candidate;

    std::string contents_to_string() const;
  };

  struct CandidateValidated {
    std::shared_ptr<BlockCandidate const> candidate;
    td::Status verdict;

    std::string contents_to_string() const;
  };

  struct IncomingProtocolMessage {
    PublicKeyHash source;
    ProtocolMessage message;

    std::string contents_to_string() const;
  };

  struct OutgoingProtocolMessage {
    std::optional<PublicKeyHash> recipient;
    ProtocolMessage message;

    std::string contents_to_string() const;
  };

  struct MasterchainBlockFinalized {
    BlockIdExt block;

    std::string contents_to_string() const;
  };

  using Events =
      td::TypeList<StopRequested, BlockFinalized, CandidateRequested, CandidateGenerated, CandidateReceived,
                   CandidateValidated, IncomingProtocolMessage, OutgoingProtocolMessage, MasterchainBlockFinalized>;

  ConsensusBus() = default;

  ShardIdFull shard;
  td::actor::ActorId<ManagerFacade> manager;
  td::actor::ActorId<ValidatorManager> real_manager_for_external_code;
  td::actor::ActorId<keyring::Keyring> keyring;
  td::Ref<ValidatorManagerOptions> validator_opts;

  // Validator set
  td::Ref<ValidatorSet> validator_set;
  PublicKeyHash local_id;

  // Collation / validation config
  td::actor::ActorId<CollationManager> collation_manager;
  NewConsensusConfig config;
  BlockIdExt min_masterchain_block_id;

  // Transport stack
  ValidatorSessionId session_id;
  // PrivateOverlay
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

struct StatsCollector {
  static void register_in(runtime::Runtime&);
};

class NullConsensusBus : public ConsensusBus {
 public:
  using Parent = ConsensusBus;
  using Events = td::TypeList<>;

  NullConsensusBus() = default;
};

struct NullConsensus {
  static void register_in(runtime::Runtime&);
};

}  // namespace ton::validator
