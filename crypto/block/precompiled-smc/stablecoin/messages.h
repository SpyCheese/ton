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

namespace block::precompiled::stablecoin {

// https://github.com/ton-blockchain/stablecoin-contract/blob/main/contracts/stdlib.fc

using namespace vm;

inline bool is_address_none(const CellSlice& s) {
  util::check_have_bits(s, 2);
  return s.prefetch_ulong(2) == 0;
}

// MESSAGE

// The message header info is organized as follows:

// https://github.com/ton-blockchain/ton/blob/8a9ff339927b22b72819c5125428b70c406da631/crypto/block/block.tlb#L126
// int_msg_info$0 ihr_disabled:Bool bounce:Bool bounced:Bool
// src:MsgAddressInt dest:MsgAddressInt
// value:CurrencyCollection ihr_fee:Grams fwd_fee:Grams
// created_lt:uint64 created_at:uint32 = CommonMsgInfo;

// https://github.com/ton-blockchain/ton/blob/8a9ff339927b22b72819c5125428b70c406da631/crypto/block/block.tlb#L135
// int_msg_info$0 ihr_disabled:Bool bounce:Bool bounced:Bool
// src:MsgAddress dest:MsgAddressInt
// value:CurrencyCollection ihr_fee:Grams fwd_fee:Grams
// created_lt:uint64 created_at:uint32 = CommonMsgInfoRelaxed;

// https://github.com/ton-blockchain/ton/blob/8a9ff339927b22b72819c5125428b70c406da631/crypto/block/block.tlb#L123C1-L124C33
// currencies$_ grams:Grams other:ExtraCurrencyCollection = CurrencyCollection;

// MSG FLAGS

const unsigned BOUNCEABLE = 0b011000;      // tag - 0, ihr_disabled - 1, bounce - 1, bounced - 0, src = adr_none$00
const unsigned NON_BOUNCEABLE = 0b010000;  // tag - 0, ihr_disabled - 1, bounce - 0, bounced - 0, src = adr_none$00

// store msg_flags and address none
inline void store_msg_flags_and_address_none(CellBuilder& cb, unsigned msg_flags) {
  util::store_ulong(cb, msg_flags, 6);
}

// load msg flags only
inline unsigned load_msg_flags(CellSlice& cs) {
  return (int)util::load_ulong(cs, 4);
}

// @param `msg_flags` - 4-bit
inline bool is_bounced(unsigned msg_flags) {
  return msg_flags & 1;
}

inline void skip_bounced_prefix(CellSlice& cs) {
  util::skip_bits(cs, 32);
}

// after `grams:Grams` we have (1 + 4 + 4 + 64 + 32) zeroes - zeroed extracurrency, ihr_fee, fwd_fee, created_lt and created_at
const int MSG_INFO_REST_BITS = 1 + 4 + 4 + 64 + 32;

// MSG

// https://github.com/ton-blockchain/ton/blob/8a9ff339927b22b72819c5125428b70c406da631/crypto/block/block.tlb#L155
// message$_ {X:Type} info:CommonMsgInfo
//  Maybe (Either StateInit ^StateInit)
//  body:(Either X ^X) = Message X;
//
//message$_ {X:Type} info:CommonMsgInfoRelaxed
//  init:(Maybe (Either StateInit ^StateInit))
//  body:(Either X ^X) = MessageRelaxed X;
//
//_ (Message Any) = MessageAny;

// if have StateInit (always place StateInit in ref):
// 0b11 for `Maybe (Either StateInit ^StateInit)` and 0b1 or 0b0 for `body:(Either X ^X)`

const int MSG_WITH_STATE_INIT_AND_BODY_SIZE = MSG_INFO_REST_BITS + 1 + 1 + 1;
const unsigned MSG_HAVE_STATE_INIT = 4;
const unsigned MSG_STATE_INIT_IN_REF = 2;
const unsigned MSG_BODY_IN_REF = 1;

// if no StateInit:
// 0b0 for `Maybe (Either StateInit ^StateInit)` and 0b1 or 0b0 for `body:(Either X ^X)`

const int MSG_ONLY_BODY_SIZE = MSG_INFO_REST_BITS + 1 + 1;

inline void store_statinit_ref_and_body_ref(CellBuilder& cb, const td::Ref<Cell>& state_init,
                                            const td::Ref<Cell>& body) {
  util::store_ulong(cb, MSG_HAVE_STATE_INIT + MSG_STATE_INIT_IN_REF + MSG_BODY_IN_REF,
                    MSG_WITH_STATE_INIT_AND_BODY_SIZE);
  util::store_ref(cb, state_init);
  util::store_ref(cb, body);
}

inline void store_only_body_ref(CellBuilder& cb, const td::Ref<Cell>& body) {
  util::store_ulong(cb, MSG_BODY_IN_REF, MSG_ONLY_BODY_SIZE);
  util::store_ref(cb, body);
}

inline void store_prefix_only_body(CellBuilder& cb) {
  util::store_ulong(cb, 0, MSG_ONLY_BODY_SIZE);
}

// parse after sender_address
inline td::RefInt256 retrieve_fwd_fee(CellSlice& cs) {
  util::load_msg_addr(cs);      // dst
  util::load_coins(cs);         // value
  util::load_maybe_ref(cs);     // extracurrency
  util::load_coins(cs);         // ihr_fee
  return util::load_coins(cs);  // fwd_fee
}

// MSG BODY

// According to the guideline, it is recommended to start the body of the internal message with uint32 op and uint64 query_id

const int MSG_OP_SIZE = 32;
const int MSG_QUERY_ID_SIZE = 64;

inline td::uint32 load_op(CellSlice& cs) {
  return (td::uint32)util::load_ulong(cs, MSG_OP_SIZE);
}
inline void skip_op(CellSlice& cs) {
  util::skip_bits(cs, MSG_OP_SIZE);
}
inline void store_op(CellBuilder& cb, td::uint32 x) {
  util::store_ulong(cb, x, MSG_OP_SIZE);
}
inline td::uint64 load_query_id(CellSlice& cs) {
  return util::load_ulong(cs, MSG_QUERY_ID_SIZE);
}
inline void skip_query_id(CellSlice& cs) {
  util::skip_bits(cs, MSG_QUERY_ID_SIZE);
}
inline void store_query_id(CellBuilder& cb, td::uint64 x) {
  util::store_ulong(cb, x, MSG_QUERY_ID_SIZE);
}

// SEND MODES - https://docs.ton.org/tvm.pdf page 137, SENDRAWMSG
// For `send_raw_message` and `send_message`:
const int SEND_MODE_REGULAR = 0;
const int SEND_MODE_PAY_FEES_SEPARATELY = 1;
const int SEND_MODE_IGNORE_ERRORS = 2;
const int SEND_MODE_DESTROY = 32;
const int SEND_MODE_CARRY_ALL_REMAINING_MESSAGE_VALUE = 64;
const int SEND_MODE_CARRY_ALL_BALANCE = 128;
const int SEND_MODE_BOUNCE_ON_ACTION_FAIL = 16;

// RESERVE MODES - https://docs.ton.org/tvm.pdf page 137, RAWRESERVE

// Creates an output action which would reserve exactly x nanograms (if y = 0).
const int RESERVE_REGULAR = 0;
// Creates an output action which would reserve at most x nanograms (if y = 2).
// Bit +2 in y means that the external action does not fail if the specified amount cannot be reserved; instead, all remaining balance is reserved.
const int RESERVE_AT_MOST = 2;
// in the case of action fail - bounce transaction. No effect if RESERVE_AT_MOST (+2) is used. TVM UPGRADE 2023-07. https://docs.ton.org/learn/tvm-instructions/tvm-upgrade-2023-07#sending-messages
const int RESERVE_BOUNCE_ON_ACTION_FAIL = 16;

}  // namespace block::precompiled::stablecoin
