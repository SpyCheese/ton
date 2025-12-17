#include "td/actor/coro_utils.h"
#include "tl/generate/auto/tl/ton_api.hpp"

#include "simplex-consensus-bus.h"

namespace ton::validator {

namespace {

// produces:
// BlockFinalized
// BlockNotarized
// ParentReady
// SafeToNotar
// SafeToSkip

using SerializedUnsignedVote = tl_object_ptr<ton_api::simplexConsensus_UnsignedVote>;
using SerializedVote = tl_object_ptr<ton_api::simplexConsensus_vote>;

struct TrueFinalizeVote : FinalizeVote {
  td::BufferSlice mc_signature;
};

template <typename T>
concept Vote = td::OneOf<T, NotarizeVote, SkipVote, TrueFinalizeVote, SkipFallbackVote, NotarizeFallbackVote>;

template <Vote T>
struct Unsigned {
  static SerializedUnsignedVote serialize(const T &vote);

  static td::BufferSlice data_to_sign(const T &vote) {
    return serialize_tl_object(serialize(vote), true);
  }
};

template <>
SerializedUnsignedVote Unsigned<NotarizeVote>::serialize(const NotarizeVote &vote) {
  return create_tl_object<ton_api::simplexConsensus_notarizeVote>(vote.slot, create_tl_block_id(vote.block));
}

template <>
SerializedUnsignedVote Unsigned<SkipVote>::serialize(const SkipVote &vote) {
  return create_tl_object<ton_api::simplexConsensus_skipVote>(vote.begin, vote.end);
}

template <>
SerializedUnsignedVote Unsigned<TrueFinalizeVote>::serialize(const TrueFinalizeVote &vote) {
  return create_tl_object<ton_api::simplexConsensus_finalizeVote>(vote.slot, create_tl_block_id(vote.block),
                                                                  vote.mc_signature.clone());
}

template <>
SerializedUnsignedVote Unsigned<SkipFallbackVote>::serialize(const SkipFallbackVote &vote) {
  return create_tl_object<ton_api::simplexConsensus_skipFallbackVote>(vote.begin, vote.end);
}

template <>
SerializedUnsignedVote Unsigned<NotarizeFallbackVote>::serialize(const NotarizeFallbackVote &vote) {
  return create_tl_object<ton_api::simplexConsensus_notarizeFallbackVote>(vote.slot, create_tl_block_id(vote.block));
}

template <Vote T>
struct Signed : T {
  td::BufferSlice signature;

  SerializedVote to_tl() const {
    return create_tl_object<ton_api::simplexConsensus_vote>(Unsigned<T>::serialize(*this), signature.clone());
  }
};

// FIXME: WTF Why does clangd (but not clang) think these functions are unused
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
NotarizeVote deserialize_vote(ton_api::simplexConsensus_notarizeVote vote) {
  return {static_cast<td::uint32>(vote.slot_), create_block_id(vote.block_)};
}

SkipVote deserialize_vote(ton_api::simplexConsensus_skipVote vote) {
  return {static_cast<td::uint32>(vote.slotBegin_), static_cast<td::uint32>(vote.slotEnd_)};
}

TrueFinalizeVote deserialize_vote(ton_api::simplexConsensus_finalizeVote vote) {
  return {{static_cast<td::uint32>(vote.slot_), create_block_id(vote.block_)}, std::move(vote.signature_for_mc_)};
}

SkipFallbackVote deserialize_vote(ton_api::simplexConsensus_skipFallbackVote vote) {
  return {static_cast<td::uint32>(vote.slotBegin_), static_cast<td::uint32>(vote.slotEnd_)};
}

NotarizeFallbackVote deserialize_vote(ton_api::simplexConsensus_notarizeFallbackVote vote) {
  return {static_cast<td::uint32>(vote.slot_), create_block_id(vote.block_)};
}
#pragma GCC diagnostic pop

auto deserialize_vote(auto vote, auto signature) {
  return Signed{deserialize_vote(std::move(vote)), std::move(signature)};
}

struct ConflictingVotesMisbehaviorProof {
  SerializedVote vote1;
  SerializedVote vote2;

  static ConflictingVotesMisbehaviorProof create(td::IsSpecializationOf<Signed> const auto &vote1,
                                                 td::IsSpecializationOf<Signed> const auto &vote2) {
    return {vote1.to_tl(), vote2.to_tl()};
  }
};

struct TooManyFallbackVotesMisbehaviorProof {
  std::vector<SerializedVote> votes;

  static TooManyFallbackVotesMisbehaviorProof create(const std::set<Signed<NotarizeFallbackVote>> &votes) {
    CHECK(votes.size() > 3);
    std::vector<SerializedVote> result;
    for (const auto &vote : votes) {
      result.push_back(vote.to_tl());
      if (result.size() > 3) {
        break;
      }
    }
    return TooManyFallbackVotesMisbehaviorProof{std::move(result)};
  }
};

using MisbehaviorProof = std::variant<ConflictingVotesMisbehaviorProof, TooManyFallbackVotesMisbehaviorProof>;

struct Votes {
  std::optional<MisbehaviorProof> count_at_tsentrizbirkom(Signed<NotarizeVote> vote, ValidatorWeight weight) {
    if (notarize.has_value() && notarize->block != vote.block) {
      return ConflictingVotesMisbehaviorProof::create(vote, *notarize);
    }

    notarize = std::move(vote);

    if (auto misbehavior = check_invariants()) {
      notarize = std::nullopt;
      return misbehavior;
    }
    return std::nullopt;
  }

  std::optional<MisbehaviorProof> count_at_tsentrizbirkom(Signed<SkipVote> vote, ValidatorWeight weight) {
    if (skip.has_value()) {
      return std::nullopt;
    }

    skip = std::move(vote);

    if (auto misbehavior = check_invariants()) {
      skip = std::nullopt;
      return misbehavior;
    }
    return std::nullopt;
  }

  std::optional<MisbehaviorProof> count_at_tsentrizbirkom(Signed<TrueFinalizeVote> vote, ValidatorWeight weight) {
    if (finalize.has_value() && finalize->block != vote.block) {
      return ConflictingVotesMisbehaviorProof::create(vote, *finalize);
    }

    finalize = std::move(vote);

    if (auto misbehavior = check_invariants()) {
      finalize = std::nullopt;
      return misbehavior;
    }
    return std::nullopt;
  }

  std::optional<MisbehaviorProof> count_at_tsentrizbirkom(Signed<SkipFallbackVote> vote, ValidatorWeight weight) {
    if (fallback_skip.has_value()) {
      return std::nullopt;
    }

    fallback_skip = std::move(vote);

    if (auto misbehavior = check_invariants()) {
      fallback_skip = std::nullopt;
      return misbehavior;
    }
    return std::nullopt;
  }

  std::optional<MisbehaviorProof> count_at_tsentrizbirkom(Signed<NotarizeFallbackVote> vote, ValidatorWeight weight) {
    fallback_notarize.insert(std::move(vote));

    if (auto misbehavior = check_invariants()) {
      fallback_notarize.erase(vote);
      return misbehavior;
    }
    return std::nullopt;
  }

  std::optional<MisbehaviorProof> check_invariants() const {
    if (notarize.has_value() && finalize.has_value() && notarize->block != finalize->block) {
      return ConflictingVotesMisbehaviorProof::create(*notarize, *finalize);
    }
    if (notarize.has_value() && skip.has_value()) {
      return ConflictingVotesMisbehaviorProof::create(*notarize, *skip);
    }
    if (finalize.has_value() && skip.has_value()) {
      return ConflictingVotesMisbehaviorProof::create(*finalize, *skip);
    }
    if (finalize.has_value() && !fallback_notarize.empty()) {
      return ConflictingVotesMisbehaviorProof::create(*finalize, *fallback_notarize.begin());
    }
    if (finalize.has_value() && fallback_skip.has_value()) {
      return ConflictingVotesMisbehaviorProof::create(*finalize, *fallback_skip);
    }
    if (fallback_notarize.size() > 3) {
      return TooManyFallbackVotesMisbehaviorProof::create(fallback_notarize);
    }

    return std::nullopt;
  }

  std::optional<Signed<NotarizeVote>> notarize;
  std::optional<Signed<SkipVote>> skip;
  std::optional<Signed<TrueFinalizeVote>> finalize;
  std::optional<Signed<SkipFallbackVote>> fallback_skip;
  std::set<Signed<NotarizeFallbackVote>> fallback_notarize;
};

struct Slot {
  std::vector<Votes> votes;

  std::map<BlockIdExt, ValidatorWeight> notarize_weight_by_block;
  std::map<BlockIdExt, ValidatorWeight> finalize_weight_by_block;

  ValidatorWeight notarize_or_skip_weight = 0;
  ValidatorWeight skip_or_skip_fallback_weight = 0;

  bool block_notarized_published = false;
  bool safe_to_skip_published = false;
  bool block_finalized_published = false;

  std::set<BlockIdExt> safe_to_notar_blocks;
};

class SimplexPoolImpl : public runtime::SpawnsWith<SimplexConsensusBus>,
                        public runtime::ConnectsTo<SimplexConsensusBus> {
 public:
  TON_RUNTIME_DEFINE_EVENT_HANDLER();

  void start_up() {
    ValidatorWeight total = 0;
    for (const auto &validator : owning_bus()->validator_set) {
      total += validator.weight;
    }
    threshold_66_ = (total * 2 + 2) / 3;
    threshold_33_ = (total + 2) / 3;

    // FIXME: Load our existing votes from disk
  }

  template <>
  void handle(runtime::BusHandle<SimplexConsensusBus>, std::shared_ptr<const ConsensusBus::StopRequested>) {
    stop();
  }

  template <>
  void handle(runtime::BusHandle<SimplexConsensusBus>,
              std::shared_ptr<const ConsensusBus::IncomingProtocolMessage> message) {
    auto maybe_vote = fetch_tl_object<ton_api::simplexConsensus_vote>(message->message.data, true);
    if (maybe_vote.is_error()) {
      return;
    }
    auto vote = maybe_vote.move_as_ok();

    auto serialized_vote = serialize_tl_object(vote->unsignedVote_, true);
    auto signature_valid =
        message->source.key.create_encryptor().move_as_ok()->check_signature(serialized_vote, vote->signature_);
    if (signature_valid.is_error()) {
      LOG(WARNING) << "MISBEHAVIOR: Dropping invalid vote from validator " << message->source << ": "
                   << signature_valid;
      return;
    }

    ton_api::downcast_call(*vote->unsignedVote_, [&](auto &unsigned_vote) {
      handle_vote(deserialize_vote(std::move(unsigned_vote), std::move(vote->signature_)));
    });
  }

  template <>
  void handle(runtime::BusHandle<SimplexConsensusBus>,
              std::shared_ptr<const SimplexConsensusBus::BroadcastVote> event) {
    std::visit(td::overloaded([&](const auto &vote) { handle_our_vote(vote).start().detach(); }), event->vote);
  }

 private:
  Slot &slot_state_at(td::uint32 slot) {
    // FIXME
    __builtin_unreachable();
  }

  void check_and_publish_events(td::uint32 slot_id, Slot &slot) {
    for (const auto &[block, weight] : slot.notarize_weight_by_block) {
      if (!slot.block_notarized_published && weight >= threshold_66_) {
        owning_bus().publish(
            std::make_shared<SimplexConsensusBus::BlockNotarized>(CandidateParentInfo{slot_id, block}));
        slot.block_notarized_published = true;
      }

      if (slot.safe_to_notar_blocks.find(block) == slot.safe_to_notar_blocks.end() && weight >= threshold_33_) {
        owning_bus().publish(std::make_shared<SimplexConsensusBus::SafeToNotar>(CandidateParentInfo{slot_id, block}));
        slot.safe_to_notar_blocks.insert(block);
      }
    }

    if (!slot.safe_to_skip_published) {
      ValidatorWeight max_notarize_weight = 0;
      for (const auto &[block, weight] : slot.notarize_weight_by_block) {
        max_notarize_weight = std::max(max_notarize_weight, weight);
      }

      if (slot.notarize_or_skip_weight - max_notarize_weight >= threshold_33_) {
        owning_bus().publish(std::make_shared<SimplexConsensusBus::SafeToSkip>(slot_id));
        slot.safe_to_skip_published = true;
      }
    }
  }

  template <typename T>
  void handle_vote(PeerValidatorId validator, Signed<T> vote) {
    auto weight = owning_bus()->validator_set[validator.value()].weight;
    auto serialized_vote = serialize_tl_object(vote.to_tl(), true);

    if constexpr (std::is_same_v<T, NotarizeVote>) {
      auto &slot = slot_state_at(vote.slot);
      auto &votes = slot.votes[validator.value()];

      bool had_notarize_or_skip = votes.notarize.has_value() || votes.skip.has_value();

      if (auto misbehavior = votes.count_at_tsentrizbirkom(std::move(vote), weight)) {
        LOG(WARNING) << "MISBEHAVIOR: Conflicting votes";
        return;
      }

      slot.notarize_weight_by_block[vote.block] += weight;
      if (!had_notarize_or_skip) {
        slot.notarize_or_skip_weight += weight;
      }

      check_and_publish_events(vote.slot, slot);
    } else if constexpr (std::is_same_v<T, NotarizeFallbackVote>) {
      auto &slot = slot_state_at(vote.slot);
      auto &votes = slot.votes[validator.value()];

      if (auto misbehavior = votes.count_at_tsentrizbirkom(std::move(vote), weight)) {
        LOG(WARNING) << "MISBEHAVIOR: Conflicting votes";
        return;
      }
    } else if constexpr (std::is_same_v<T, TrueFinalizeVote>) {
      auto &slot = slot_state_at(vote.slot);
      auto &votes = slot.votes[validator.value()];

      if (auto misbehavior = votes.count_at_tsentrizbirkom(std::move(vote), weight)) {
        LOG(WARNING) << "MISBEHAVIOR: Conflicting votes";
        return;
      }

      slot.finalize_weight_by_block[vote.block] += weight;
    } else if constexpr (td::OneOf<T, SkipVote, SkipFallbackVote>) {
      // FIXME: Support begin != end
      CHECK(vote.begin == vote.end);

      auto &slot = slot_state_at(vote.begin);
      auto &votes = slot.votes[validator.value()];

      bool had_notarize_or_skip = votes.notarize.has_value() || votes.skip.has_value();
      bool had_skip_or_skip_fallback = votes.skip.has_value() || votes.fallback_skip.has_value();

      if (auto misbehavior = votes.count_at_tsentrizbirkom(std::move(vote), weight)) {
        LOG(WARNING) << "MISBEHAVIOR: Conflicting votes";
        return;
      }

      if (!had_notarize_or_skip) {
        slot.notarize_or_skip_weight += weight;
      }
      if (!had_skip_or_skip_fallback) {
        slot.skip_or_skip_fallback_weight += weight;
      }

      check_and_publish_events(vote.begin, slot);
    }
  }

  template <typename Vote>
  td::actor::Task<td::Unit> handle_our_vote(Vote vote) {
    auto &bus = *owning_bus();

    auto signature_data = Unsigned<Vote>::data_to_sign(vote);
    auto signature = co_await td::actor::ask(bus.keyring, &keyring::Keyring::sign_message, bus.local_id.short_id,
                                             std::move(signature_data));

    Signed<Vote> signed_vote{std::move(vote), std::move(signature)};
    auto serialized_vote = serialize_tl_object(signed_vote.to_tl(), true);

    // FIXME: Store in db

    owning_bus().publish(
        std::make_shared<ConsensusBus::OutgoingProtocolMessage>(std::nullopt, std::move(serialized_vote)));
    handle_vote(owning_bus()->local_id.idx, std::move(signed_vote));
    co_return {};
  }

  template <>
  td::actor::Task<td::Unit> handle_our_vote<FinalizeVote>(FinalizeVote vote) {
    auto &bus = *owning_bus();

    auto signature_data = create_serialize_tl_object<ton_api::ton_blockId>(vote.block.root_hash, vote.block.file_hash);
    auto signature = co_await td::actor::ask(bus.keyring, &keyring::Keyring::sign_message, bus.local_id.short_id,
                                             std::move(signature_data));
    co_return co_await handle_our_vote(TrueFinalizeVote{{vote.slot, vote.block}, std::move(signature)});
  }

  td::uint32 first_non_finalized_slot_ = 0;
  ValidatorWeight threshold_66_ = 0;
  ValidatorWeight threshold_33_ = 0;
};

}  // namespace

void SimplexPool::register_in(runtime::Runtime &runtime) {
  runtime.register_actor<SimplexPoolImpl>("SimplexPool");
}

}  // namespace ton::validator
