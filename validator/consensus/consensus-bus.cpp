#include "auto/tl/ton_api_json.h"
#include "tl/tl_json.h"

#include "consensus-bus.h"

namespace ton::validator {

namespace {

std::string candidate_to_string(BlockCandidate const& candidate) {
  return PSTRING() << "BlockCandidate{id=" << candidate.id.to_str() << ", block_size=" << candidate.data.size()
                   << ", collated_size=" << candidate.collated_data.size()
                   << ", collated_file_hash=" << candidate.collated_file_hash
                   << ", pubkey=" << candidate.pubkey.as_bits256() << "}";
}

std::string message_to_string(td::Slice message) {
  auto maybe_decoded = fetch_tl_object<ton_api::Object>(message, true);
  if (maybe_decoded.is_error()) {
    return PSTRING() << "<message of size " << message.size() << ">";
  }

  return td::json_encode<std::string>(td::ToJson(maybe_decoded.ok()));
}

std::string format_parents(std::vector<BlockIdExt> parents) {
  td::StringBuilder sb;
  sb << "[";
  bool first = true;
  for (const auto& parent : parents) {
    if (!first) {
      sb << ", ";
    }
    first = false;
    sb << parent.to_str();
  }
  sb << "]";
  return sb.as_cslice().str();
}

}  // namespace

std::string ConsensusBus::BlockFinalized::contents_to_string() const {
  return PSTRING() << "{id=" << id.to_str() << ", block_size=" << block.size()
                   << ", parents=" << format_parents(parents)
                   << ", leader=" << leader.compute_short_id().bits256_value() << "}";
}

std::string ConsensusBus::CandidateRequested::contents_to_string() const {
  return PSTRING() << "{parents=" << format_parents(parents) << "}";
}

std::string ConsensusBus::CandidateGenerated::contents_to_string() const {
  return PSTRING() << "{candidate=" << candidate_to_string(*candidate)
                   << ", collator_id=" << (collator_id.has_value() ? (PSTRING() << *collator_id) : " none ") << "} ";
}

std::string ConsensusBus::CandidateReceived::contents_to_string() const {
  return PSTRING() << "{candidate=" << candidate_to_string(*candidate) << "}";
}

std::string ConsensusBus::CandidateValidated::contents_to_string() const {
  return PSTRING() << "{candidate=" << candidate_to_string(*candidate) << ", verdict=" << verdict << "}";
}

std::string ConsensusBus::IncomingProtocolMessage::contents_to_string() const {
  return PSTRING() << "{source=" << source << ", message=" << message_to_string(message.data) << "}";
}

std::string ConsensusBus::OutgoingProtocolMessage::contents_to_string() const {
  return PSTRING() << "{" << (recipient.has_value() ? (PSTRING() << "recipient=" << *recipient) : "broadcast")
                   << ", message=" << message_to_string(message.data) << "}";
}

std::string ConsensusBus::MasterchainBlockFinalized::contents_to_string() const {
  return PSTRING() << "{block=" << block.to_str() << "}";
}

}  // namespace ton::validator
