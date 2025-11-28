#pragma once

#include "validator/consensus/consensus-bus.h"

namespace ton::validator {

class NullConsensusBus : public ConsensusBus {
 public:
  using Parent = ConsensusBus;
  using Events = td::TypeList<>;

  NullConsensusBus() = default;
};

struct NullConsensus {
  static void register_in(runtime::Runtime&);
};

}  // namespace ton::validator
