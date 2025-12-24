#include "auto/tl/ton_api.hpp"
#include "keys/encryptor.h"
#include "td/utils/overloaded.h"
#include "validator-session/candidate-serializer.h"

#include "checksum.h"
#include "consensus-bus.h"

namespace ton::validator {

bool PeerValidator::check_signature(ValidatorSessionId session, td::Slice data, td::Slice signature) const {
  auto signed_data = create_serialize_tl_object<ton_api::consensus_dataToSign>(session, td::BufferSlice(data));
  return key.create_encryptor().move_as_ok()->check_signature(signed_data, signature).is_ok();
}

RawCandidateId RawCandidateId::from_tl(const tl_object_ptr<ton_api::consensus_candidateId>& tl_parent) {
  return RawCandidateId{static_cast<td::uint32>(tl_parent->slot_), tl_parent->hash_};
}

tl_object_ptr<ton_api::consensus_candidateId> RawCandidateId::to_tl() const {
  return create_tl_object<ton_api::consensus_candidateId>(slot, hash);
}

namespace {

ton::tl_object_ptr<ton_api::consensus_CandidateParent> parent_id_to_tl(RawParentId parent) {
  if (!parent) {
    return create_tl_object<ton_api::consensus_candidateWithoutParents>();
  } else {
    return create_tl_object<ton_api::consensus_candidateParent>(parent->to_tl());
  }
}

RawParentId tl_to_parent_id(const tl_object_ptr<ton_api::consensus_CandidateParent>& tl_parent) {
  RawParentId id;
  auto without_parents_fn = [&](const ton_api::consensus_candidateWithoutParents&) {};
  auto parent_fn = [&](const ton_api::consensus_candidateParent& parent) { id = RawCandidateId::from_tl(parent.id_); };
  ton_api::downcast_call(*tl_parent, td::overloaded(without_parents_fn, parent_fn));
  return id;
}

}  // namespace

CandidateId CandidateId::create(td::uint32 slot, const std::variant<BlockIdExt, BlockCandidate>& candidate,
                                std::optional<RawCandidateId> parent) {
  BlockIdExt block;

  auto empty_fn = [&](const BlockIdExt& id) {
    block = id;
    return create_hash_tl_object<ton_api::consensus_candidateHashDataEmpty>(create_tl_block_id(id), parent->to_tl());
  };
  auto block_fn = [&](const BlockCandidate& candidate) {
    block = candidate.id;
    return create_hash_tl_object<ton_api::consensus_candidateHashDataOrdinary>(
        create_tl_block_id(candidate.id), candidate.collated_file_hash, parent_id_to_tl(parent));
  };
  auto hash = std::visit(td::overloaded(empty_fn, block_fn), candidate);
  return CandidateId{RawCandidateId{slot, hash}, block};
}

td::Result<RawCandidateRef> RawCandidate::deserialize(td::Slice data, const PeerValidator& leader,
                                                      const ConsensusBus& bus) {
  TRY_RESULT(broadcast, fetch_tl_object<ton_api::consensus_CandidateData>(data, true));

  struct ExtractedData {
    td::uint32 slot;
    RawParentId parent_id;
    std::variant<BlockIdExt, BlockCandidate> block;
    td::BufferSlice signature;
  };
  td::Result<ExtractedData> maybe_data;

  auto empty_fn = [](ton_api::consensus_empty& empty_broadcast) -> td::Result<ExtractedData> {
    return ExtractedData{
        .slot = static_cast<td::uint32>(empty_broadcast.slot_),
        .parent_id = RawCandidateId::from_tl(empty_broadcast.parent_),
        .block = create_block_id(empty_broadcast.block_),
        .signature = std::move(empty_broadcast.signature_),
    };
  };

  auto ordinary_fn = [&](ton_api::consensus_block& block_broadcast) -> td::Result<ExtractedData> {
    TRY_RESULT(candidate, validatorsession::deserialize_candidate(
                              block_broadcast.candidate_, true,
                              bus.config.max_block_size + bus.config.max_collated_data_size + 1024));

    if (!candidate->src_.is_zero()) {
      return td::Status::Error("src field of the candidate broadcast must be null");
    }

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

    BlockCandidate block{
        creator, block_id, collated_file_hash, std::move(candidate->data_), std::move(candidate->collated_data_),
    };

    return ExtractedData{
        .slot = static_cast<td::uint32>(block_broadcast.slot_),
        .parent_id = tl_to_parent_id(block_broadcast.parent_),
        .block = std::move(block),
        .signature = std::move(block_broadcast.signature_),
    };
  };

  ton_api::downcast_call(*broadcast,
                         [&](auto& broadcast) { maybe_data = td::overloaded(empty_fn, ordinary_fn)(broadcast); });
  TRY_RESULT(parsed, std::move(maybe_data));

  auto id = CandidateId::create(parsed.slot, parsed.block, parsed.parent_id);

  auto signed_data = serialize_tl_object(id.as_raw().to_tl(), true);
  if (!leader.check_signature(bus.session_id, signed_data, parsed.signature)) {
    return td::Status::Error("Candidate broadcast signature is not valid");
  }

  return td::make_ref<RawCandidate>(id, parsed.parent_id, leader.idx, std::move(parsed.block),
                                    std::move(parsed.signature));
}

td::BufferSlice RawCandidate::serialize() const {
  auto empty_fn = [&](const BlockIdExt& referenced_block) {
    return create_serialize_tl_object<ton_api::consensus_empty>(
        id.slot, parent_id->to_tl(), create_tl_block_id(referenced_block), signature.clone());
  };
  auto block_fn = [&](const BlockCandidate& candidate) {
    auto candidate_tl = create_tl_object<ton_api::validatorSession_candidate>(
        td::Bits256{}, candidate.id.seqno(), candidate.id.root_hash, candidate.data.clone(),
        candidate.collated_data.clone());

    return create_serialize_tl_object<ton_api::consensus_block>(
        id.slot, parent_id_to_tl(parent_id), validatorsession::serialize_candidate(candidate_tl, true).move_as_ok(),
        signature.clone());
  };
  return std::visit(td::overloaded(empty_fn, block_fn), block);
}

}  // namespace ton::validator
