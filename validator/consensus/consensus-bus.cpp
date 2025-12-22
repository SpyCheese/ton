#include "auto/tl/ton_api_json.h"
#include "tl/tl_json.h"

#include "consensus-bus.h"

namespace ton::validator {

namespace {

std::string parent_id_to_string(const td::OneOf<ParentId, RawParentId> auto& id) {
  if (!id.has_value()) {
    return "no parents";
  }
  return PSTRING() << *id;
}

std::string block_candidate_to_string(const BlockCandidate& candidate) {
  return PSTRING() << "BlockCandidate{id=" << candidate.id.to_str() << ", block_size=" << candidate.data.size()
                   << ", collated_size=" << candidate.collated_data.size()
                   << ", collated_file_hash=" << candidate.collated_file_hash
                   << ", pubkey=" << candidate.pubkey.as_bits256() << "}";
}

std::string peer_validator_id_to_string(PeerValidatorId id) {
  return PSTRING() << "validator " << id.value();
}

std::string candidate_to_string(const RawCandidateRef& candidate) {
  return PSTRING() << "RawCandidate{id=" << candidate->id << ", parent=" << parent_id_to_string(candidate->parent_id)
                   << ", leader=" << peer_validator_id_to_string(candidate->leader)
                   << ", block=" << (candidate->block ? block_candidate_to_string(*candidate->block) : "empty block")
                   << "}";
}

std::string candidate_to_string(const CandidateRef& candidate) {
  return PSTRING() << "Candidate{id=" << candidate->id << ", parent=" << parent_id_to_string(candidate->parent_id)
                   << ", leader=" << peer_validator_id_to_string(candidate->leader)
                   << ", block=" << (candidate->block ? block_candidate_to_string(*candidate->block) : "empty block")
                   << "}";
}

std::string finalization_cert_to_string(const std::optional<td::Ref<BlockSignatureSet>>& cert) {
  if (!cert) {
    return "finalized by ancestor";
  }
  return PSTRING() << "<BlockSignatureSet of size " << (*cert)->size() << ">";
}

std::string message_to_string(td::Slice message) {
  auto maybe_decoded = fetch_tl_object<ton_api::Object>(message, true);
  if (maybe_decoded.is_error()) {
    return PSTRING() << "<message of size " << message.size() << ">";
  }

  return td::json_encode<std::string>(td::ToJson(maybe_decoded.ok()));
}

}  // namespace

std::string ConsensusBus::SlotFinalized::contents_to_string() const {
  return PSTRING() << "{candidate=" << candidate_to_string(candidate)
                   << ", finalization_cert=" << finalization_cert_to_string(finalization_cert) << "}";
}

std::string ConsensusBus::OurLeaderWindowStarted::contents_to_string() const {
  return PSTRING() << "{base=" << parent_id_to_string(base) << ", start_slot=" << start_slot
                   << ", end_slot=" << end_slot << "}";
}

std::string ConsensusBus::OurLeaderWindowAborted::contents_to_string() const {
  return PSTRING() << "{start_slot=" << start_slot << "}";
}

std::string ConsensusBus::CandidateGenerated::contents_to_string() const {
  return PSTRING() << "{candidate=" << candidate_to_string(candidate)
                   << ", collator_id=" << (collator_id.has_value() ? (PSTRING() << *collator_id) : "none") << "}";
}

std::string ConsensusBus::CandidateReceived::contents_to_string() const {
  return PSTRING() << "{candidate=" << candidate_to_string(candidate) << "}";
}

std::string ConsensusBus::ValidationRequest::contents_to_string() const {
  return PSTRING() << "{candidate=" << candidate_to_string(candidate) << "}";
}

std::string ConsensusBus::IncomingProtocolMessage::contents_to_string() const {
  return PSTRING() << "{source=" << peer_validator_id_to_string(source)
                   << ", message=" << message_to_string(message.data) << "}";
}

std::string ConsensusBus::OutgoingProtocolMessage::contents_to_string() const {
  return PSTRING() << "{recipient=" << (recipient.has_value() ? peer_validator_id_to_string(*recipient) : "broadcast")
                   << ", message=" << message_to_string(message.data) << "}";
}

std::string ConsensusBus::BlockFinalizedInMasterchain::contents_to_string() const {
  return PSTRING() << "{block=" << block.to_str() << "}";
}

std::vector<BlockIdExt> ConsensusBus::convert_id_to_blocks(ParentId parent) const {
  if (parent.has_value()) {
    return {parent->block};
  } else {
    return first_block_parents;
  }
}

}  // namespace ton::validator
