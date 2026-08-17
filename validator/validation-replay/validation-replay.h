/*
    This file is part of TON Blockchain Library.

    TON Blockchain Library is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    TON Blockchain Library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with TON Blockchain Library.  If not, see <http://www.gnu.org/licenses/>.
*/
#pragma once
#include "interfaces/validator-manager.h"

namespace ton::validator {

class ValidationReplayer : public td::actor::Actor {
 public:
  virtual td::actor::Task<std::string> run_command(std::string command) = 0;
  virtual void update_options(Ref<ValidatorManagerOptions> opts) = 0;

  static td::actor::ActorOwn<ValidationReplayer> create(td::actor::ActorId<ValidatorManager> manager,
                                                        Ref<ValidatorManagerOptions> opts);
};

}  // namespace ton::validator
