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
#include "block/precompiled-smc/common.h"
#include "opcodes.h"

namespace block::precompiled::stablecoin {

using namespace vm;

static const ton::WorkchainId MY_WORKCHAIN = 0;

inline bool is_same_workchain(const CellSlice& addr) {
  auto res = util::parse_std_addr(addr);
  return res.first == MY_WORKCHAIN;
}

inline void check_same_workchain(const CellSlice& addr) {
  if (!is_same_workchain(addr)) {
    throw Result::error(ERROR_WRONG_WORKCHAIN);
  }
}

// https://github.com/ton-blockchain/stablecoin-contract/blob/main/contracts/jetton-utils.fc

const int STATUS_SIZE = 4;

inline td::Ref<Cell> pack_jetton_wallet_data(td::uint32 status, const td::RefInt256& balance,
                                             const CellSlice& owner_address, const CellSlice& master_address) {
  CellBuilder cb;
  util::store_ulong(cb, status, STATUS_SIZE);
  util::store_coins(cb, balance);
  util::store_slice(cb, owner_address);
  util::store_slice(cb, master_address);
  return cb.finalize();
}

inline td::Ref<Cell> calculate_jetton_wallet_state_init(const CellSlice& owner_address, const CellSlice& master_address,
                                                        const td::Ref<Cell> wallet_code) {
  /*
       https://github.com/ton-blockchain/ton/blob/8a9ff339927b22b72819c5125428b70c406da631/crypto/block/block.tlb#L144
       _ split_depth:(Maybe (## 5)) special:(Maybe TickTock)
       code:(Maybe ^Cell) data:(Maybe ^Cell)
       library:(Maybe ^Cell) = StateInit;
    */
  CellBuilder cb;
  util::store_ulong(cb, 0, 2);  // 0b00 - No split_depth; No special
  util::store_maybe_ref(cb, wallet_code);
  util::store_maybe_ref(
      cb, pack_jetton_wallet_data(/* status = */ 0, /* balance = */ td::zero_refint(), owner_address, master_address));
  util::store_ulong(cb, 0, 1);  // Empty libraries
  return cb.finalize();
}

inline CellSlice calculate_jetton_wallet_address(const td::Ref<Cell> state_init) {
  /*
      https://github.com/ton-blockchain/ton/blob/8a9ff339927b22b72819c5125428b70c406da631/crypto/block/block.tlb#L105
      addr_std$10 anycast:(Maybe Anycast) workchain_id:int8 address:bits256  = MsgAddressInt;
    */
  CellBuilder cb;
  util::store_ulong(cb, 4, 3);  // 0b100 = addr_std$10 tag; No anycast
  util::store_long(cb, MY_WORKCHAIN, 8);
  CHECK(cb.store_bytes_bool(state_init->get_hash().as_slice()));
  return load_cell_slice(cb.finalize());
}

inline CellSlice calculate_user_jetton_wallet_address(const CellSlice& owner_address, const CellSlice& master_address,
                                                      const td::Ref<Cell>& jetton_wallet_code) {
  return calculate_jetton_wallet_address(
      calculate_jetton_wallet_state_init(owner_address, master_address, jetton_wallet_code));
}

inline void check_either_forward_payload(const CellSlice& s) {
  util::check_have_bits(s, 1);
  if (s.prefetch_long(1)) {
    // forward_payload in ref
    unsigned bits = s.size();
    unsigned refs = s.size_refs();
    if (bits != 1 || refs != 1) {
      throw Result::error(ERROR_INVALID_MESSAGE);  // we check that there is no excess in the slice
    }
  }
  // else forward_payload in slice - arbitrary bits and refs
}

}  // namespace block::precompiled::stablecoin
