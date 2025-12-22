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

struct RawCandidateId {
  static RawCandidateId create(td::uint32 slot, const std::optional<BlockCandidate>& candidate,
                               std::optional<RawCandidateId> parent);

  bool operator==(const RawCandidateId& other) const = default;

  td::uint32 slot{0};
  Bits256 hash{};
};

inline td::StringBuilder& operator<<(td::StringBuilder& stream, const RawCandidateId& id) {
  return stream << "{" << id.slot << ", " << id.hash << ", ?}";
}

using RawParentId = std::optional<RawCandidateId>;

struct CandidateId {
  CandidateId() = default;

  CandidateId(RawCandidateId id, BlockIdExt block) : slot(id.slot), hash(id.hash), block(block) {
  }

  operator RawCandidateId() const {
    return RawCandidateId{slot, hash};
  }

  td::uint32 slot{0};
  Bits256 hash{};
  BlockIdExt block;
};

inline td::StringBuilder& operator<<(td::StringBuilder& stream, const CandidateId& id) {
  return stream << "{" << id.slot << ", " << id.hash << ", " << id.block.to_str() << "}";
}

using ParentId = std::optional<CandidateId>;

struct RawCandidate : td::CntObject {
  static td::Result<td::Ref<RawCandidate>> deserialize(td::Slice data, const PeerValidator& leader,
                                                       const ConsensusBus& bus);

  RawCandidate(RawCandidateId id, RawParentId parent_id, PeerValidatorId leader, std::optional<BlockCandidate> block,
               td::BufferSlice signature)
      : id(id)
      , parent_id(std::move(parent_id))
      , leader(leader)
      , block(std::move(block))
      , signature(std::move(signature)) {
    CHECK(this->block.has_value() || this->parent_id.has_value());
  }

  CandidateId resolve_id(ParentId parent) const {
    if (block.has_value()) {
      return CandidateId{id, block->id};
    } else {
      CHECK(parent.has_value());
      return CandidateId{id, parent->block};
    }
  }

  td::BufferSlice serialize() const;

  RawCandidateId id;
  RawParentId parent_id;
  PeerValidatorId leader;
  std::optional<BlockCandidate> block;
  td::BufferSlice signature;
};

using RawCandidateRef = td::Ref<RawCandidate>;

struct Candidate : td::CntObject {
  Candidate(CandidateId id, ParentId parent_id, RawCandidateRef raw)
      : id(id)
      , parent_id(parent_id)
      , leader(raw->leader)
      , block(raw->block)
      , signature(raw->signature)
      , raw(std::move(raw)) {
    CHECK(this->raw->id == id);
    CHECK(parent_id == this->raw->parent_id);
    if (block.has_value()) {
      CHECK(block->id == id.block);
    } else {
      CHECK(parent_id->block == id.block);
    }
  }

  CandidateId id;
  ParentId parent_id;
  PeerValidatorId leader;
  const std::optional<BlockCandidate>& block;
  const td::BufferSlice& signature;

  RawCandidateRef raw;
};

using CandidateRef = td::Ref<Candidate>;

}  // namespace ton::validator
