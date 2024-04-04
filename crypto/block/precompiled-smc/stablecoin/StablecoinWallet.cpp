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
#include "StablecoinWallet.h"
#include "opcodes.h"
#include "messages.h"
#include "jetton-utils.h"

namespace block::precompiled::stablecoin {

// FunC code: https://github.com/ton-blockchain/stablecoin-contract/blob//contracts/jetton-wallet.fc
// commit c107ec4e7ba036d9d77ce05e59df85d1af880a82

using namespace vm;

Result StablecoinWallet::do_run() {
  if (is_external_) {
    return Result::not_accepted();
  }
  load_cell_slice(my_code_);  // my_code_ can be a library cell. Validate that the library is available.

  CellSlice in_msg_full_slice = load_cell_slice(in_msg_);
  unsigned msg_flags = load_msg_flags(in_msg_full_slice);
  if (is_bounced(msg_flags)) {
    return on_bounce();
  }
  sender_address_ = util::load_msg_addr(in_msg_full_slice);
  td::RefInt256 fwd_fee_from_in_msg = retrieve_fwd_fee(in_msg_full_slice);
  td::RefInt256 fwd_fee = get_original_fwd_fee(
      MY_WORKCHAIN, fwd_fee_from_in_msg);  // we use message fwd_fee for estimation of forward_payload costs

  td::uint32 op = load_op(in_msg_body_);
  switch (op) {
    case OP_TRANSFER:  // outgoing transfer
      return send_jettons(std::move(fwd_fee));
    case OP_INTERNAL_TRANSFER:  // incoming transfer
      return receive_jettons();
    case OP_BURN:  // burn
      return burn();
    case OP_SET_STATUS: {
      skip_query_id(in_msg_body_);
      auto new_status = (td::uint32)util::load_ulong(in_msg_body_, STATUS_SIZE);
      util::end_parse(in_msg_body_);
      load_data();
      if (sender_address_.lex_cmp(jetton_master_address_) != 0) {
        return Result::error(ERROR_NOT_VALID_WALLET);
      }
      jetton_status_ = new_status;
      save_data();
      return Result::success();
    }
    case OP_TOP_UP:
      return Result::success();  // just accept tons
    default:
      return Result::error(ERROR_WRONG_OP);
  }
}

Result StablecoinWallet::on_bounce() {
  skip_bounced_prefix(in_msg_body_);
  load_data();
  td::uint32 op = load_op(in_msg_body_);
  if (op != OP_INTERNAL_TRANSFER && op != OP_BURN_NOTIFICATION) {
    return Result::error(ERROR_WRONG_OP);
  }
  skip_query_id(in_msg_body_);
  td::RefInt256 jetton_amount = util::load_coins(in_msg_body_);
  jetton_balance_ += jetton_amount;
  util::check_finite(jetton_balance_);  // throw on overflow
  save_data();
  return Result::success();
}

Result StablecoinWallet::send_jettons(td::RefInt256 fwd_fee) {
  // see transfer TL-B layout in jetton.tlb
  td::uint64 query_id = load_query_id(in_msg_body_);
  td::RefInt256 jetton_amount = util::load_coins(in_msg_body_);
  CellSlice to_owner_address = util::load_msg_addr(in_msg_body_);
  check_same_workchain(to_owner_address);
  load_data();
  bool is_from_master = jetton_master_address_.lex_cmp(sender_address_) == 0;
  bool outgoing_transfers_unlocked = (jetton_status_ & 1) == 0;
  if (!outgoing_transfers_unlocked && !is_from_master) {
    return Result::error(ERROR_CONTRACT_LOCKED);
  }
  if (jetton_owner_address_.lex_cmp(sender_address_) != 0 && !is_from_master) {
    return Result::error(ERROR_NOT_OWNER);
  }

  jetton_balance_ -= jetton_amount;
  util::check_finite(jetton_balance_);
  if (jetton_balance_->sgn() < 0) {
    return Result::error(ERROR_BALANCE_ERROR);
  }

  td::Ref<Cell> state_init = calculate_jetton_wallet_state_init(to_owner_address, jetton_master_address_, my_code_);
  CellSlice to_wallet_address = calculate_jetton_wallet_address(state_init);
  CellSlice response_address = util::load_msg_addr(in_msg_body_);
  util::load_maybe_ref(in_msg_body_);  // custom payload
  td::RefInt256 forward_ton_amount = util::load_coins(in_msg_body_);
  check_either_forward_payload(in_msg_body_);
  CellSlice either_forward_payload = in_msg_body_;

  // see internal TL-B layout in jetton.tlb
  CellBuilder cb;
  store_op(cb, OP_INTERNAL_TRANSFER);
  store_query_id(cb, query_id);
  util::store_coins(cb, jetton_amount);
  util::store_slice(cb, jetton_owner_address_);
  util::store_slice(cb, response_address);
  util::store_coins(cb, forward_ton_amount);
  util::store_slice(cb, either_forward_payload);
  td::Ref<Cell> msg_body = cb.finalize();

  // build MessageRelaxed, see TL-B layout in stdlib.fc#L733
  cb.reset();
  store_msg_flags_and_address_none(cb, BOUNCEABLE);
  util::store_slice(cb, to_wallet_address);
  util::store_coins(cb, td::zero_refint());
  store_statinit_ref_and_body_ref(cb, state_init, msg_body);
  td::Ref<Cell> msg = cb.finalize();

  check_amount_is_enough_to_transfer(forward_ton_amount, fwd_fee);
  send_raw_message(msg, SEND_MODE_CARRY_ALL_REMAINING_MESSAGE_VALUE | SEND_MODE_BOUNCE_ON_ACTION_FAIL);
  save_data();
  return Result::success();
}

Result StablecoinWallet::receive_jettons() {
  load_data();
  bool incoming_transfers_locked = (jetton_status_ & 2) == 2;
  if (incoming_transfers_locked) {
    return Result::error(ERROR_CONTRACT_LOCKED);
  }
  // see internal TL-B layout in jetton.tlb
  td::uint64 query_id = load_query_id(in_msg_body_);
  td::RefInt256 jetton_amount = util::load_coins(in_msg_body_);
  jetton_balance_ += jetton_amount;
  util::check_finite(jetton_balance_);
  CellSlice from_address = util::load_msg_addr(in_msg_body_);
  CellSlice response_address = util::load_msg_addr(in_msg_body_);
  if (jetton_master_address_.lex_cmp(sender_address_) != 0 &&
      calculate_user_jetton_wallet_address(from_address, jetton_master_address_, my_code_)
              .lex_cmp(sender_address_) != 0) {
    return Result::error(ERROR_NOT_VALID_WALLET);
  }
  td::RefInt256 forward_ton_amount = util::load_coins(in_msg_body_);

  if (forward_ton_amount->sgn()) {
    CellSlice either_forward_payload = in_msg_body_;

    // see transfer_notification TL-B layout in jetton.tlb
    CellBuilder cb;
    store_op(cb, OP_TRANSFER_NOTIFICATION);
    store_query_id(cb, query_id);
    util::store_coins(cb, jetton_amount);
    util::store_slice(cb, from_address);
    util::store_slice(cb, either_forward_payload);
    td::Ref<Cell> msg_body = cb.finalize();

    // build MessageRelaxed, see TL-B layout in stdlib.fc#L733
    cb.reset();
    store_msg_flags_and_address_none(cb, NON_BOUNCEABLE);
    util::store_slice(cb, jetton_owner_address_);
    util::store_coins(cb, forward_ton_amount);
    store_only_body_ref(cb, msg_body);
    td::Ref<Cell> msg = cb.finalize();

    send_raw_message(msg, SEND_MODE_PAY_FEES_SEPARATELY | SEND_MODE_BOUNCE_ON_ACTION_FAIL);
  }

  if (!is_address_none(response_address)) {
    td::RefInt256 to_leave_on_balance = balance_.grams - in_msg_balance_.grams + due_payment_;
    util::check_finite(to_leave_on_balance);
    raw_reserve(std::max(to_leave_on_balance, calculate_jetton_wallet_min_storage_fee()), RESERVE_AT_MOST);

    // build MessageRelaxed, see TL-B layout in stdlib.fc#L733
    CellBuilder cb;
    store_msg_flags_and_address_none(cb, NON_BOUNCEABLE);
    util::store_slice(cb, response_address);
    util::store_coins(cb, td::zero_refint());
    store_prefix_only_body(cb);
    store_op(cb, OP_EXCESSES);
    store_query_id(cb, query_id);
    td::Ref<Cell> msg = cb.finalize();
    send_raw_message(msg, SEND_MODE_CARRY_ALL_BALANCE | SEND_MODE_IGNORE_ERRORS);
  }

  save_data();
  return Result::success();
}

Result StablecoinWallet::burn() {
  load_data();
  td::uint64 query_id = load_query_id(in_msg_body_);
  td::RefInt256 jetton_amount = util::load_coins(in_msg_body_);
  CellSlice response_address = util::load_msg_addr(in_msg_body_);
  util::load_maybe_ref(in_msg_body_); // custom payload
  util::end_parse(in_msg_body_);

  jetton_balance_ -= jetton_amount;
  util::check_finite(jetton_balance_);
  bool is_from_master = jetton_master_address_.lex_cmp(sender_address_) == 0;
  if (!is_from_master) {
    return Result::error(ERROR_NOT_OWNER);
  }
  if (jetton_balance_->sgn() < 0) {
    return Result::error(ERROR_BALANCE_ERROR);
  }

  // see burn_notification TL-B layout in jetton.tlb
  CellBuilder cb;
  store_op(cb, OP_BURN_NOTIFICATION);
  store_query_id(cb, query_id);
  util::store_coins(cb, jetton_amount);
  util::store_slice(cb, jetton_owner_address_);
  util::store_slice(cb, response_address);
  td::Ref<Cell> msg_body = cb.finalize();

  // build MessageRelaxed, see TL-B layout in stdlib.fc#L733
  cb.reset();
  store_msg_flags_and_address_none(cb, BOUNCEABLE);
  util::store_slice(cb, jetton_master_address_);
  util::store_coins(cb, td::zero_refint());
  store_only_body_ref(cb, msg_body);
  td::Ref<Cell> msg = cb.finalize();

  check_amount_is_enough_to_burn();
  send_raw_message(msg, SEND_MODE_CARRY_ALL_REMAINING_MESSAGE_VALUE | SEND_MODE_BOUNCE_ON_ACTION_FAIL);
  save_data();
  return Result::success();
}

void StablecoinWallet::load_data() {
  CellSlice ds = load_cell_slice(c4_);
  jetton_status_ = (td::uint32)util::load_ulong(ds, STATUS_SIZE);
  jetton_balance_ = util::load_coins(ds);
  jetton_owner_address_ = util::load_msg_addr(ds);
  jetton_master_address_ = util::load_msg_addr(ds);
  util::end_parse(ds);
}

void StablecoinWallet::save_data() {
  c4_ = pack_jetton_wallet_data(jetton_status_, jetton_balance_, jetton_owner_address_, jetton_master_address_);
}

}  // namespace block::precompiled::stablecoin
