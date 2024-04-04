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
#include "crypto/block/precompiled-smc/common.h"

namespace block::precompiled::stablecoin {

// FunC code: https://github.com/ton-blockchain/stablecoin-contract/blob/main/contracts/jetton-wallet.fc
// commit de08b905214eb253d27009db6a124fd1feadbf72

using namespace vm;

class StablecoinWallet : public PrecompiledSmartContract {
 public:
  std::string get_name() const override {
    return "stablecoin-wallet";
  }

 protected:
  Result do_run() override;
  Result on_bounce();
  Result send_jettons(td::RefInt256 fwd_fee);
  Result receive_jettons();
  Result burn();

  /*
    Storage

    Note, status==0 means unlocked - user can freely transfer and recieve jettons (only admin can burn).
          (status & 1) bit means user can not send jettons
          (status & 2) bit means user can not receive jettons.
    Master (minter) smart-contract able to make outgoing actions (transfer, burn jettons) with any status.

    storage#_ status:uint4
              balance:Coins owner_address:MsgAddressInt
              jetton_master_address:MsgAddressInt = Storage;
  */
  td::uint32 jetton_status_;
  td::RefInt256 jetton_balance_;
  CellSlice jetton_owner_address_;
  CellSlice jetton_master_address_;

  CellSlice sender_address_;

  void load_data();
  void save_data();

  td::RefInt256 calculate_jetton_wallet_min_storage_fee();
  td::RefInt256 forward_init_state_overhead();
  void check_amount_is_enough_to_transfer(const td::RefInt256& forward_ton_amount, const td::RefInt256& fwd_fee);
  void check_amount_is_enough_to_burn();
};

}  // namespace block::precompiled::stablecoin
