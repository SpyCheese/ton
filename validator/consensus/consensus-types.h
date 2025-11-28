#pragma once

#include "adnl/adnl-node-id.hpp"
#include "keys/keys.hpp"
#include "ton/ton-types.h"

namespace ton::validator {

class ConsensusBus;

class PeerValidatorId {
 public:
  PeerValidatorId() : idx_(-1) {
  }

  explicit PeerValidatorId(int idx) : idx_(idx) {
  }

  int value() const {
    return idx_;
  }

  std::strong_ordering operator<=>(const PeerValidatorId& other) const = default;

 private:
  int idx_;
};

struct PeerValidator {
  [[nodiscard]] bool check_signature(ValidatorSessionId session, td::Slice data, td::Slice signature) const;

  bool operator==(const PeerValidator& other) const = default;

  PeerValidatorId idx;
  PublicKey key;
  PublicKeyHash short_id;
  adnl::AdnlNodeIdShort adnl_id;
  ValidatorWeight weight;
};

inline td::StringBuilder& operator<<(td::StringBuilder& stream, const PeerValidator& peer_validator) {
  return stream << "validator " << peer_validator.idx.value() << " at " << peer_validator.short_id;
}

struct ProtocolMessage {
  static constexpr size_t max_length = 1024;

  ProtocolMessage(td::BufferSlice data) : data(std::move(data)) {
    CHECK(this->data.size() <= max_length);
  }

  template <typename T>
  ProtocolMessage(const tl_object_ptr<T>& object) : data(serialize_tl_object(object, true)) {
    CHECK(data.size() <= max_length);
  }

  td::BufferSlice data;
};

struct CandidateId {
  static CandidateId create(td::uint32 slot, const BlockCandidate& candidate, std::optional<CandidateId> parent);

  bool operator==(const CandidateId& other) const = default;

  td::uint32 slot;
  Bits256 hash;
};

inline td::StringBuilder& operator<<(td::StringBuilder& stream, const CandidateId& id) {
  return stream << "{" << id.slot << ", " << id.hash << "}";
}

struct Candidate : td::CntObject {
  static td::Result<td::Ref<Candidate>> deserialize(td::Slice data, const PeerValidator& leader,
                                                    const ConsensusBus& bus);

  Candidate(CandidateId id, std::optional<CandidateId> parent_id, PeerValidatorId leader, BlockCandidate&& block,
            td::BufferSlice&& signature)
      : id(id)
      , parent_id(std::move(parent_id))
      , leader(leader)
      , block(std::move(block))
      , signature(std::move(signature)) {
  }

  td::BufferSlice serialize() const;

  CandidateId id;
  std::optional<CandidateId> parent_id;
  PeerValidatorId leader;
  BlockCandidate block;
  td::BufferSlice signature;
};

using CandidateRef = td::Ref<Candidate>;
using ParentRef = std::optional<td::Ref<Candidate>>;

}  // namespace ton::validator
