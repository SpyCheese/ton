#include "simplex-consensus-bus.h"

namespace ton::validator {

namespace {

std::string format_vote(const NotarizeVote& vote) {
  return PSTRING() << "NotarizeVote{slot=" << vote.slot << ", block=" << vote.block.to_str() << "}";
}

std::string format_vote(const FinalizeVote& vote) {
  return PSTRING() << "FinalizeVote{slot=" << vote.slot << ", block=" << vote.block.to_str() << "}";
}

std::string format_vote(const SkipVote& vote) {
  return PSTRING() << "SkipVote{slot=" << vote.slot << "}";
}

std::string format_vote(const NotarizeFallbackVote& vote) {
  return PSTRING() << "NotarizeFallbackVote{slot=" << vote.slot << ", block=" << vote.block.to_str() << "}";
}

std::string format_vote(const SkipFallbackVote& vote) {
  return PSTRING() << "SkipFallbackVote{slot=" << vote.vote << "}";
}

// FIXME: deduplicate with consensus-bus.cpp
std::string parent_info_to_string(const CandidateParentInfo& parent_info) {
  return PSTRING() << "CandidateParentInfo{slot=" << parent_info.slot << ", id=" << parent_info.id.to_str() << "}";
}

}  // namespace

std::string SimplexConsensusBus::BroadcastVote::contents_to_string() const {
  auto vote_contents = std::visit([](const auto& vote) { return format_vote(vote); }, vote);
  return PSTRING() << "{vote=" << vote_contents << "}";
}

std::string SimplexConsensusBus::BlockNotarized::contents_to_string() const {
  return PSTRING() << "{block=" << parent_info_to_string(block) << "}";
}

std::string SimplexConsensusBus::ParentReady::contents_to_string() const {
  return PSTRING() << "{window=" << window << ", block=" << parent_info_to_string(block) << "}";
}

std::string SimplexConsensusBus::SafeToNotar::contents_to_string() const {
  return PSTRING() << "{block=" << parent_info_to_string(block) << "}";
}

std::string SimplexConsensusBus::SafeToSkip::contents_to_string() const {
  return PSTRING() << "{slot=" << slot << "}";
}

}  // namespace ton::validator
