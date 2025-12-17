#include <queue>

#include "td/actor/coro_utils.h"

#include "simplex-consensus-bus.h"

namespace ton::validator {

namespace {

struct Slot {
  std::optional<Candidate> pending_block;
  bool voted = false;
  std::optional<CandidateParentInfo> voted_notar;
  std::optional<CandidateParentInfo> observed_notar_certificate;
  bool its_over = false;
  bool bad_window = false;
};

struct LeaderWindow {
  td::uint32 idx;
  td::uint32 start_slot;
  std::set<CandidateParent> available_bases;
  std::unique_ptr<Slot[]> slots;
  bool had_timeouts = false;
};

struct SlotRef {
  bool is_first_in_window = false;
  bool is_last_in_window = false;
  LeaderWindow& leader_window;
  Slot& state;
};

class SimplexConsensusImpl : public runtime::SpawnsWith<SimplexConsensusBus>,
                             public runtime::ConnectsTo<SimplexConsensusBus> {
 public:
  TON_RUNTIME_DEFINE_EVENT_HANDLER();

  void start_up() override {
    slots_per_leader_window_ = owning_bus()->slots_per_leader_window;
    target_rate_timeout_s_ = owning_bus()->config.target_rate_ms / 1000.;
    first_block_timeout_s_ = owning_bus()->first_block_timeout_s;

    auto& window = window_at(0);
    window.available_bases.insert(std::nullopt);
    set_timeouts(window);
  }

  template <>
  void handle(runtime::BusHandle<SimplexConsensusBus>, std::shared_ptr<const ConsensusBus::StopRequested>) {
    stop();
  }

  template <>
  void handle(runtime::BusHandle<SimplexConsensusBus>, std::shared_ptr<const ConsensusBus::BlockFinalized> event) {
    first_non_finalized_slot_ = std::max(first_non_finalized_slot_, event->id.slot + 1);

    while (leader_windows_.front()->start_slot + slots_per_leader_window_ <= first_non_finalized_slot_) {
      leader_windows_.pop_front();
      ++leader_window_offset_;
    }
  }

  // upon Block(s, hash, hashparent) do
  template <>
  void handle(runtime::BusHandle<SimplexConsensusBus>, std::shared_ptr<const ConsensusBus::CandidateReceived> event) {
    if (event->candidate.slot < first_non_finalized_slot_) {
      return;
    }
    auto slot = slot_state_of(event->candidate.slot);
    if (!slot.is_first_in_window && !event->candidate.parent.has_value()) {
      LOG(WARNING) << "MISBEHAVIOR: Dropping candidate " << event->candidate.id() << " for slot "
                   << event->candidate.slot
                   << " which builds upon effective genesis block but doesn't come first in leader window";
      return;
    }

    // if tryNotar(Block(s, hash, hashparent)) then
    if (try_notar(event->candidate)) {
      // checkPendingBlocks()
      check_pending_blocks();
    }
    // else if Voted ∉ state[s] then
    else if (!slot.state.voted) {
      // pendingBlocks[s] ← Block(s, hash, hashparent)
      slot.state.pending_block = event->candidate;
    }
  }

  // upon Timeout(s) do
  void alarm() override {
    if (!skip_timestamp_.is_in_past()) {
      return;
    }

    skip_timestamp_ = td::Timestamp::in(target_rate_timeout_s_, skip_timestamp_);
    ++skip_slot_;
    alarm_timestamp().relax(skip_timestamp_);

    td::uint32 slot_id = skip_slot_ - 1;
    if (slot_id < first_non_finalized_slot_) {
      return;
    }
    auto slot = slot_state_of(slot_id);

    // if Voted ∉ state[s] then
    if (!slot.state.voted) {
      slot.leader_window.had_timeouts = true;
      // trySkipWindow(s)
      try_skip_window(slot.leader_window);
    }
  }

  // upon BlockNotarized(s, hash(b)) do
  template <>
  void handle(runtime::BusHandle<SimplexConsensusBus>,
              std::shared_ptr<const SimplexConsensusBus::BlockNotarized> event) {
    if (event->block.slot < first_non_finalized_slot_) {
      return;
    }
    auto slot = slot_state_of(event->block.slot);

    // state[s] ← state[s] ∪ {BlockNotarized(hash(b))}
    slot.state.observed_notar_certificate = event->block;

    // tryFinal(s, hash(b))
    try_final(event->block);
  }

  // upon ParentReady(s, hash(b)) do
  template <>
  void handle(runtime::BusHandle<SimplexConsensusBus>, std::shared_ptr<const SimplexConsensusBus::ParentReady> event) {
    CHECK(event->window > 0);

    if (event->window * slots_per_leader_window_ < first_non_finalized_slot_) {
      return;
    }
    auto& window = window_at(event->window);

    // state[s] ← state[s] ∪ {ParentReady(hash(b))}
    // Adding block to available_bases can potentially allow try_notar to succeed for the first slot
    // of the window.
    window.available_bases.insert(event->block);
    auto slot = slot_state_of(window.start_slot);
    if (slot.state.pending_block.has_value() && slot.state.pending_block->parent == event->block) {
      pending_slots_.push(window.start_slot);
    }

    // checkPendingBlocks()
    check_pending_blocks();

    // setTimeouts(s)   ▷ start timer for all slots in this window
    // Timeouts are only set for the "current leader window" (which is the window with the maximal
    // index ParentReady has been received for).
    if (current_leader_window_ < window.idx) {
      // See section 3.4
      if (window_at(current_leader_window_).had_timeouts) {
        auto increase_factor = owning_bus()->timeout_increase_factor;
        auto max_delay = owning_bus()->max_backoff_delay_s;

        first_block_timeout_s_ = std::min(first_block_timeout_s_ * increase_factor, max_delay);
        target_rate_timeout_s_ = std::min(target_rate_timeout_s_ * increase_factor, max_delay);
      } else {
        restore_default_timeouts();
      }

      current_leader_window_ = window.idx;
      set_timeouts(window);
    }
  }

  // upon SafeToNotar(s, hash(b)) do
  template <>
  void handle(runtime::BusHandle<SimplexConsensusBus>, std::shared_ptr<const SimplexConsensusBus::SafeToNotar> event) {
    if (event->block.slot < first_non_finalized_slot_) {
      return;
    }
    auto slot = slot_state_of(event->block.slot);

    // trySkipWindow(s)
    try_skip_window(slot.leader_window);

    // if ItsOver ∉ state[s] then
    if (!slot.state.its_over) {
      // broadcast NotarFallbackVote(s, hash(b))   ▷ notar-fallback vote
      owning_bus().publish(std::make_shared<SimplexConsensusBus::BroadcastVote>(
          NotarizeFallbackVote(event->block.slot, event->block.id)));

      // state[s] ← state[s] ∪ {BadWindow}
      slot.state.bad_window = true;
    }
  }

  // upon SafeToSkip(s) do
  template <>
  void handle(runtime::BusHandle<SimplexConsensusBus>, std::shared_ptr<const SimplexConsensusBus::SafeToSkip> event) {
    if (event->slot < first_non_finalized_slot_) {
      return;
    }
    auto slot = slot_state_of(event->slot);

    // trySkipWindow(s)
    try_skip_window(slot.leader_window);

    // if ItsOver ∉ state[s] then
    if (!slot.state.its_over) {
      // broadcast SkipFallbackVote(s)   ▷ skip-fallback vote
      owning_bus().publish(std::make_shared<SimplexConsensusBus::BroadcastVote>(SkipFallbackVote(event->slot)));

      // state[s] ← state[s] ∪ {BadWindow}
      slot.state.bad_window = true;
    }
  }

 private:
  LeaderWindow& window_at(td::uint32 idx) {
    while (idx >= leader_window_offset_ + leader_windows_.size()) {
      td::uint32 window_idx = static_cast<td::uint32>(leader_window_offset_ + leader_windows_.size());
      auto start_slot = window_idx * slots_per_leader_window_;
      leader_windows_.emplace_back(std::make_unique<LeaderWindow>(window_idx, start_slot, std::set<CandidateParent>{},
                                                                  std::make_unique<Slot[]>(slots_per_leader_window_)));
    }
    return *leader_windows_[idx - leader_window_offset_];
  }

  SlotRef slot_state_of(td::uint32 slot) {
    CHECK(slot >= first_non_finalized_slot_);

    auto& leader_window = window_at(slot / slots_per_leader_window_);
    td::uint32 offset_in_window = slot % slots_per_leader_window_;
    auto& slot_state = leader_window.slots[offset_in_window];
    return {
        .is_first_in_window = offset_in_window == 0,
        .is_last_in_window = offset_in_window + 1 == slots_per_leader_window_,
        .leader_window = leader_window,
        .state = slot_state,
    };
  }

  void restore_default_timeouts() {
    target_rate_timeout_s_ = owning_bus()->config.target_rate_ms / 1000.;
    first_block_timeout_s_ = owning_bus()->first_block_timeout_s;
  }

  // function setTimeouts(s)   ▷ s is first slot of window
  void set_timeouts(LeaderWindow& window) {
    // for i ∈ windowSlots(s) do   ▷ set timeouts for all slots
    //   schedule event Timeout(i) at time clock()+∆timeout+(i−s+1)·∆block
    skip_slot_ = window.start_slot;
    skip_timestamp_ = td::Timestamp::in(first_block_timeout_s_ + target_rate_timeout_s_);
    alarm_timestamp().relax(skip_timestamp_);
  }

  // function tryNotar(Block(s, hash, hashparent))   ▷ Check if a notarization vote can be cast.
  bool try_notar(const Candidate& candidate) {
    auto slot = slot_state_of(candidate.slot);

    // if Voted ∈ state[s] then
    if (slot.state.voted) {
      // return false
      return false;
    }

    // firstSlot ← (s is the first slot in leader window)   ▷ boolean
    bool first_slot = slot.is_first_in_window;

    // if (firstSlot and ParentReady(hashparent) ∈ state[s]
    // or (not firstSlot and VotedNotar(hashparent) ∈ state[s − 1]) then
    bool can_vote_notar = false;
    if (first_slot) {
      can_vote_notar = slot.leader_window.available_bases.contains(candidate.parent);
    } else {
      CHECK(candidate.parent.has_value());
      auto previous_slot = slot_state_of(candidate.slot - 1);
      can_vote_notar = (previous_slot.state.voted_notar == *candidate.parent);
    }
    if (can_vote_notar) {
      // FIXME: validate block. Validate should ensure that everyone sees the same data for this (candidate.slot, candidate.id())

      // broadcast NotarVote(s, hash)   ▷ notarization vote
      owning_bus().publish(
          std::make_shared<SimplexConsensusBus::BroadcastVote>(NotarizeVote(candidate.slot, candidate.id())));

      // state[s] ← state[s] ∪ {Voted, VotedNotar(hash)}
      slot.state.voted = true;
      slot.state.voted_notar = candidate.as_parent();

      // pendingBlocks[s] ← ⊥   ▷ won’t vote notar a second time
      slot.state.pending_block = std::nullopt;

      // tryFinal(s, hash)   ▷ maybe vote finalize as well
      try_final(candidate.as_parent());

      // return true
      return true;
    }

    // return false
    return false;
  }

  // function tryFinal(s, hash(b))
  void try_final(CandidateParentInfo candidate) {
    auto slot = slot_state_of(candidate.slot);

    // if BlockNotarized(hash(b)) ∈ state[s] and VotedNotar(hash(b)) ∈ state[s]
    // and BadWindow ∉ state[s] then
    if (slot.state.observed_notar_certificate == candidate && slot.state.voted_notar == candidate &&
        !slot.state.bad_window) {
      // broadcast FinalVote(s)   ▷ finalization vote
      owning_bus().publish(
          std::make_shared<SimplexConsensusBus::BroadcastVote>(FinalizeVote(candidate.slot, candidate.id)));

      // state[s] ← state[s] ∪ {ItsOver}
      slot.state.its_over = true;
    }
  }

  // function trySkipWindow(s)
  void try_skip_window(LeaderWindow& window) {
    // for k ∈ windowSlots(s) do   ▷ skip unvoted slots
    for (td::uint32 i = 0; i < slots_per_leader_window_; ++i) {
      auto& state = window.slots[i];

      // if Voted ∉ state[k] then
      if (!state.voted) {
        // broadcast SkipVote(k)   ▷ skip vote
        owning_bus().publish(std::make_shared<SimplexConsensusBus::BroadcastVote>(SkipVote(window.start_slot + i)));

        // state[k] ← state[k] ∪ {Voted, BadWindow}
        state.voted = true;
        state.bad_window = true;

        // pendingBlocks[k] ← ⊥   ▷ won’t vote notar after skip
        state.pending_block = std::nullopt;
      }
    }
  }

  // function checkPendingBlocks()
  void check_pending_blocks() {
    // for s : pendingBlocks[s] ≠ ⊥ do   ▷ iterate with increasing s
    while (!pending_slots_.empty()) {
      // tryNotar(pendingBlocks[s])
      td::uint32 slot_id = pending_slots_.top();
      pending_slots_.pop();

      // Even though we add only non-finalized slots to pending_slots_, it's possible for a block
      // to be finalized by other validators before we have a chance to act upon it.
      if (slot_id < first_non_finalized_slot_) {
        continue;
      }

      auto slot = slot_state_of(slot_id);
      CHECK(slot.state.pending_block.has_value());
      try_notar(slot.state.pending_block.value());
    }
  }

  td::uint32 slots_per_leader_window_ = 0;

  std::deque<std::unique_ptr<LeaderWindow>> leader_windows_;
  td::uint32 leader_window_offset_ = 0;
  td::uint32 current_leader_window_ = 0;
  td::uint32 first_non_finalized_slot_ = 0;

  td::uint32 skip_slot_ = 0;
  td::Timestamp skip_timestamp_;
  double first_block_timeout_s_ = 0;
  double target_rate_timeout_s_ = 0;

  std::priority_queue<td::uint32, std::vector<td::uint32>, std::greater<>> pending_slots_;
};

}  // namespace

void SimplexConsensus::register_in(runtime::Runtime& runtime) {
  runtime.register_actor<SimplexConsensusImpl>("SimplexConsensus");
}

}  // namespace ton::validator
