#pragma once

#include "consensus-bus.h"

namespace ton::validator {

class CandidateParent {
 public:
  CandidateParent(const ConsensusBus& bus, const ParentRef& parent) {
    if (parent.has_value()) {
      parent_blocks_ = {(*parent)->block.id};
      next_seqno_ = (*parent)->block.id.seqno() + 1;
      candidate_id_ = (*parent)->id;
    } else {
      parent_blocks_ = bus.first_block_parents;
      next_seqno_ = 0;
      candidate_id_ = {};
    }
  }

  CandidateParent(const CandidateRef& parent) {
    parent_blocks_ = {parent->block.id};
    next_seqno_ = parent->block.id.seqno() + 1;
    candidate_id_ = parent->id;
  }

  const std::vector<BlockIdExt>& parent_blocks() const {
    return parent_blocks_;
  }

  int next_seqno() const {
    return next_seqno_;
  }

  const std::optional<CandidateId>& candidate_id() const {
    return candidate_id_;
  }

 private:
  std::vector<BlockIdExt> parent_blocks_;
  int next_seqno_;
  std::optional<CandidateId> candidate_id_;
};

}  // namespace ton::validator
