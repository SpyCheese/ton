#include "auto/tl/ton_api.hpp"
#include "keys/encryptor.h"
#include "td/utils/overloaded.h"
#include "validator-session/candidate-serializer.h"

#include "checksum.h"
#include "consensus-bus.h"

namespace ton::validator {

namespace {

ton::tl_object_ptr<ton_api::consensus_CandidateParent> parent_id_to_tl(std::optional<CandidateId> parent) {
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

CandidateId CandidateId::create(td::uint32 slot, const BlockCandidate& candidate, std::optional<CandidateId> parent) {
  auto hash = create_hash_tl_object<ton_api::consensus_candidateIdData>(
      slot, create_tl_block_id(candidate.id), candidate.collated_file_hash, parent_id_to_tl(parent));
  return CandidateId{slot, hash};
}

td::Result<CandidateRef> Candidate::deserialize(td::Slice data, const PeerValidator& leader, const ConsensusBus& bus) {
  TRY_RESULT(broadcast, fetch_tl_object<ton_api::consensus_candidate>(data, true));

  std::optional<CandidateId> parent;
  ton_api::downcast_call(*broadcast->parent_,
                         td::overloaded([&](ton_api::consensus_noCandidateParents&) {},
                                        [&](ton_api::consensus_ordinaryCandidateParent& ordinary_parent) {
                                          parent = CandidateId{
                                              .slot = static_cast<td::uint32>(ordinary_parent.slot_),
                                              .hash = ordinary_parent.hash_,
                                          };
                                        }));

  if (parent.has_value() && static_cast<td::uint32>(broadcast->slot_) <= parent->slot) {
    return td::Status::Error(PSTRING() << "Invalid candidate broadcast with slot=" << broadcast->slot_
                                       << " and parent slot=" << parent->slot);
  }

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

  BlockCandidate block_candidate(creator, block_id, collated_file_hash, std::move(candidate->data_),
                                 std::move(candidate->collated_data_));

  auto id = CandidateId::create(static_cast<td::uint32>(broadcast->slot_), block_candidate, parent);
  auto id_to_sign = create_serialize_tl_object<ton_api::consensus_ordinaryCandidateParent>(id.slot, id.hash);

  if (!leader.check_signature(bus.session_id, id_to_sign, broadcast->signature_)) {
    return td::Status::Error(PSTRING() << "Candidate broadcast signature is not valid");
  }

  return td::make_ref<Candidate>(id, parent, leader.idx, std::move(block_candidate), std::move(broadcast->signature_));
}

td::BufferSlice Candidate::serialize() const {
  auto candidate_tl = create_tl_object<ton_api::validatorSession_candidate>(
      td::Bits256{}, block.id.seqno(), block.id.root_hash, block.data.clone(), block.collated_data.clone());
  auto serialized_candidate = validatorsession::serialize_candidate(candidate_tl, true).move_as_ok();

  return create_serialize_tl_object<ton_api::consensus_candidate>(id.slot, std::move(serialized_candidate),
                                                                  parent_id_to_tl(parent_id), signature.clone());
}

}  // namespace ton::validator
