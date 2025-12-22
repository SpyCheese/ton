#include "auto/tl/ton_api.hpp"
#include "keys/encryptor.h"
#include "td/utils/overloaded.h"
#include "validator-session/candidate-serializer.h"

#include "checksum.h"
#include "consensus-bus.h"

namespace ton::validator {

namespace {

ton::tl_object_ptr<ton_api::consensus_CandidateParent> parent_id_to_tl(RawParentId parent) {
  if (!parent) {
    return create_tl_object<ton_api::consensus_noCandidateParents>();
  } else {
    return create_tl_object<ton_api::consensus_ordinaryCandidateParent>(parent->slot, parent->hash);
  }
}

}  // namespace

bool PeerValidator::check_signature(ValidatorSessionId session, td::Slice data, td::Slice signature) const {
  auto signed_data = create_serialize_tl_object<ton_api::consensus_dataToSign>(session, td::BufferSlice(data));
  return key.create_encryptor().move_as_ok()->check_signature(signed_data, signature).is_ok();
}

RawCandidateId RawCandidateId::create(td::uint32 slot, const std::optional<BlockCandidate>& candidate,
                                      std::optional<RawCandidateId> parent) {
  if (!candidate.has_value()) {
    auto hash = create_hash_tl_object<ton_api::consensus_candidateIdData>(slot, create_tl_block_id(BlockIdExt{}),
                                                                          td::Bits256::zero(), parent_id_to_tl(parent));
    return RawCandidateId{slot, hash};
  } else {
    auto hash = create_hash_tl_object<ton_api::consensus_candidateIdData>(
        slot, create_tl_block_id(candidate->id), candidate->collated_file_hash, parent_id_to_tl(parent));
    return RawCandidateId{slot, hash};
  }
}

td::Result<RawCandidateRef> RawCandidate::deserialize(td::Slice data, const PeerValidator& leader,
                                                      const ConsensusBus& bus) {
  TRY_RESULT(broadcast, fetch_tl_object<ton_api::consensus_candidate>(data, true));

  std::optional<RawCandidateId> parent;
  ton_api::downcast_call(*broadcast->parent_,
                         td::overloaded([&](ton_api::consensus_noCandidateParents&) {},
                                        [&](ton_api::consensus_ordinaryCandidateParent& ordinary_parent) {
                                          parent = RawCandidateId{
                                              .slot = static_cast<td::uint32>(ordinary_parent.slot_),
                                              .hash = ordinary_parent.hash_,
                                          };
                                        }));

  std::optional<BlockCandidate> block;

  if (!broadcast->candidate_.empty()) {
    TRY_RESULT(candidate,
               validatorsession::deserialize_candidate(
                   broadcast->candidate_, true, bus.config.max_block_size + bus.config.max_collated_data_size + 1024));

    if (candidate->data_.size() > bus.config.max_block_size ||
        candidate->collated_data_.size() > bus.config.max_collated_data_size) {
      return td::Status::Error(PSTRING() << "Too big candidate broadcast with data_size=" << candidate->data_.size()
                                         << ", collated_data_size=" << candidate->collated_data_.size());
    }

    BlockIdExt block_id{
        BlockId{bus.shard, static_cast<BlockSeqno>(candidate->round_)},
        candidate->root_hash_,
        td::sha256_bits256(candidate->data_.as_slice()),
    };

    auto collated_file_hash = td::sha256_bits256(candidate->collated_data_.as_slice());

    Ed25519_PublicKey creator{leader.key.ed25519_value().raw()};

    block = BlockCandidate{
        creator, block_id, collated_file_hash, std::move(candidate->data_), std::move(candidate->collated_data_),
    };
  } else {
    if (!parent.has_value()) {
      return td::Status::Error("Empty candidate must have a parent");
    }
  }

  auto id = RawCandidateId::create(static_cast<td::uint32>(broadcast->slot_), block, parent);
  auto id_to_sign = create_serialize_tl_object<ton_api::consensus_ordinaryCandidateParent>(id.slot, id.hash);

  if (!leader.check_signature(bus.session_id, id_to_sign, broadcast->signature_)) {
    return td::Status::Error("Candidate broadcast signature is not valid");
  }

  return td::make_ref<RawCandidate>(id, parent, leader.idx, std::move(block), std::move(broadcast->signature_));
}

td::BufferSlice RawCandidate::serialize() const {
  td::BufferSlice serialized_candidate;
  if (block) {
    auto candidate_tl = create_tl_object<ton_api::validatorSession_candidate>(
        td::Bits256{}, block->id.seqno(), block->id.root_hash, block->data.clone(), block->collated_data.clone());
    serialized_candidate = validatorsession::serialize_candidate(candidate_tl, true).move_as_ok();
  }

  return create_serialize_tl_object<ton_api::consensus_candidate>(id.slot, std::move(serialized_candidate),
                                                                  parent_id_to_tl(parent_id), signature.clone());
}

}  // namespace ton::validator
