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
#include "StablecoinWallet.h"
#include "opcodes.h"
#include "jetton-utils.h"

namespace block::precompiled::stablecoin {

// https://github.com/ton-blockchain/stablecoin-contract/blob/main/contracts/gas.fc

using namespace vm;

const td::uint64 ONE_TON = 1000000000;

const td::uint64 MIN_STORAGE_DURATION = 5 * 365 * 24 * 3600;  // 5 years

//# Precompiled constants
//
//All of the contents are result of contract emulation tests
//

//## Minimal fees
//
//- Transfer [/sandbox_tests/JettonWallet.spec.ts#L935](L935) `0.028627415` TON
//- Burn [/sandbox_tests/JettonWallet.spec.ts#L1185](L1185) `0.016492002` TON

//## Storage
//
//Get calculated in a separate test file [/sandbox_tests/StateInit.spec.ts](StateInit.spec.ts)

//- `JETTON_WALLET_BITS` [/sandbox_tests/StateInit.spec.ts#L92](L92)
const td::uint64 JETTON_WALLET_BITS = 1033;

//- `JETTON_WALLET_CELLS`: [/sandbox_tests/StateInit.spec.ts#L92](L92)
const td::uint64 JETTON_WALLET_CELLS = 3;

// difference in JETTON_WALLET_BITS/JETTON_WALLET_INITSTATE_BITS is difference in
// StateInit and AccountStorage (https://github.com/ton-blockchain/ton/blob/master/crypto/block/block.tlb)
// we count bits as if balances are max possible
//- `JETTON_WALLET_INITSTATE_BITS` [/sandbox_tests/StateInit.spec.ts#L95](L95)
const td::uint64 JETTON_WALLET_INITSTATE_BITS = 931;
//- `JETTON_WALLET_INITSTATE_CELLS` [/sandbox_tests/StateInit.spec.ts#L95](L95)
const td::uint64 JETTON_WALLET_INITSTATE_CELLS = 3;

// jetton-wallet.fc#L163 - maunal bits counting
const td::uint64 BURN_NOTIFICATION_BITS = 754;  // body = 32+64+124+(3+8+256)+(3+8+256)
const td::uint64 BURN_NOTIFICATION_CELLS = 1;   // body always in ref

//## Gas
//
//Gas constants are calculated in the main test suite.
//First the related transaction is found, and then it's
//resulting gas consumption is printed to the console.

//- `SEND_TRANSFER_GAS_CONSUMPTION` [/sandbox_tests/JettonWallet.spec.ts#L853](L853)
const td::uint64 SEND_TRANSFER_GAS_CONSUMPTION = 9255;

//- `RECEIVE_TRANSFER_GAS_CONSUMPTION` [/sandbox_tests/JettonWallet.spec.ts#L862](L862)
const td::uint64 RECEIVE_TRANSFER_GAS_CONSUMPTION = 10355;

//- `SEND_BURN_GAS_CONSUMPTION` [/sandbox_tests/JettonWallet.spec.ts#L1154](L1154)
const td::uint64 SEND_BURN_GAS_CONSUMPTION = 5791;

//- `RECEIVE_BURN_GAS_CONSUMPTION` [/sandbox_tests/JettonWallet.spec.ts#L1155](L1155)
const td::uint64 RECEIVE_BURN_GAS_CONSUMPTION = 6775;

td::RefInt256 StablecoinWallet::calculate_jetton_wallet_min_storage_fee() {
  return get_storage_fee(MY_WORKCHAIN, MIN_STORAGE_DURATION, JETTON_WALLET_BITS, JETTON_WALLET_CELLS);
}

td::RefInt256 StablecoinWallet::forward_init_state_overhead() {
  return get_simple_forward_fee(MY_WORKCHAIN, JETTON_WALLET_INITSTATE_BITS, JETTON_WALLET_INITSTATE_CELLS);
}

void StablecoinWallet::check_amount_is_enough_to_transfer(const td::RefInt256& forward_ton_amount,
                                                          const td::RefInt256& fwd_fee) {
  int fwd_count = forward_ton_amount->sgn() ? 2 : 1;  // second sending (forward) will be cheaper that first

  td::uint64 jetton_wallet_gas_consumption = precompiled_gas_usage_;
  td::uint64 send_transfer_gas_consumption = jetton_wallet_gas_consumption;
  td::uint64 receive_transfer_gas_consumption = jetton_wallet_gas_consumption;

  td::RefInt256 fee = forward_ton_amount;
  // 3 messages: wal1->wal2,  wal2->owner, wal2->response
  // but last one is optional (it is ok if it fails)
  fee += fwd_fee * fwd_count;
  fee += forward_init_state_overhead();  // additional fwd fees related to initstate in iternal_transfer
  fee += get_compute_fee(MY_WORKCHAIN, send_transfer_gas_consumption);
  fee += get_compute_fee(MY_WORKCHAIN, receive_transfer_gas_consumption);
  fee += calculate_jetton_wallet_min_storage_fee();
  util::check_finite(fee);

  if (in_msg_balance_.grams <= fee) {
    throw Result::error(ERROR_NOT_ENOUGH_GAS);
  }
}

void StablecoinWallet::check_amount_is_enough_to_burn() {
  td::uint64 jetton_wallet_gas_consumption = precompiled_gas_usage_;
  td::uint64 send_burn_gas_consumption = jetton_wallet_gas_consumption;

  td::RefInt256 fee = get_forward_fee(MY_WORKCHAIN, BURN_NOTIFICATION_BITS, BURN_NOTIFICATION_CELLS);
  fee += get_compute_fee(MY_WORKCHAIN, send_burn_gas_consumption);
  fee += get_compute_fee(MY_WORKCHAIN, RECEIVE_BURN_GAS_CONSUMPTION);
  util::check_finite(fee);

  if (in_msg_balance_.grams <= fee) {
    throw Result::error(ERROR_NOT_ENOUGH_GAS);
  }
}

}  // namespace block::precompiled::stablecoin
