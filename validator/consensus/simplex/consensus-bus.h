#include "consensus-bus.h"

namespace ton::validator {

struct NotarizeVote {
  td::uint32 slot;
  BlockIdExt block;
};

struct FinalizeVote {
  td::uint32 slot;
  BlockIdExt block;
};

struct SkipVote {
  td::uint32 begin;
  td::uint32 end;
};

struct NotarizeFallbackVote {
  td::uint32 slot;
  BlockIdExt block;
};

struct SkipFallbackVote {
  td::uint32 begin;
  td::uint32 end;
};

using Vote = std::variant<NotarizeVote, FinalizeVote, SkipVote, NotarizeFallbackVote, SkipFallbackVote>;

class SimplexConsensusBus : public ConsensusBus {
 public:
  struct BroadcastVote {
    Vote vote;

    std::string contents_to_string() const;
  };

  struct BlockNotarized {
    CandidateParentInfo block;

    std::string contents_to_string() const;
  };

  struct ParentReady {
    td::uint32 window;
    CandidateParentInfo block;

    std::string contents_to_string() const;
  };

  struct SafeToNotar {
    CandidateParentInfo block;

    std::string contents_to_string() const;
  };

  struct SafeToSkip {
    td::uint32 slot;

    std::string contents_to_string() const;
  };

  using Parent = ConsensusBus;
  using Events = td::TypeList<BroadcastVote, BlockNotarized, ParentReady, SafeToNotar, SafeToSkip>;

  SimplexConsensusBus() = default;

  // FIXME: These belong to config
  td::uint32 slots_per_leader_window = 0;
  double first_block_timeout_s = 0;

  double max_backoff_delay_s = 100;
  double timeout_increase_factor = 1.05;
};

struct SimplexPool {
  static void register_in(runtime::Runtime&);
};

struct SimplexConsensus {
  static void register_in(runtime::Runtime&);
};

}  // namespace ton::validator
