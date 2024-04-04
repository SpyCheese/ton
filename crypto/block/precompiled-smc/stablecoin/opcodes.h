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
#include "tdutils/td/utils/common.h"

namespace block::precompiled::stablecoin {

// https://github.com/ton-blockchain/stablecoin-contract/blob/main/contracts/op-codes.fc

static const td::uint32 OP_TRANSFER = 0xf8a7ea5;
static const td::uint32 OP_TRANSFER_NOTIFICATION = 0x7362d09c;
static const td::uint32 OP_INTERNAL_TRANSFER = 0x178d4519;
static const td::uint32 OP_EXCESSES = 0xd53276db;
static const td::uint32 OP_BURN = 0x595f07bc;
static const td::uint32 OP_BURN_NOTIFICATION = 0x7bdd97de;

static const td::uint32 OP_PROVIDE_WALLET_ADDRESS = 0x2c76b973;
static const td::uint32 OP_TAKE_WALLET_ADDRESS = 0xd1735400;

static const td::uint32 OP_TOP_UP = 0xd372158c;

static const td::uint32 ERROR_INVALID_OP = 72;
static const td::uint32 ERROR_WRONG_OP = 0xffff;
static const td::uint32 ERROR_NOT_OWNER = 73;
static const td::uint32 ERROR_NOT_VALID_WALLET = 74;
static const td::uint32 ERROR_WRONG_WORKCHAIN = 333;

// jetton-minter

static const td::uint32 OP_MINT = 0x642b7d07;
static const td::uint32 OP_CHANGE_ADMIN = 0x6501f354;
static const td::uint32 OP_CLAIM_ADMIN = 0xfb88e119;
static const td::uint32 OP_UPGRADE = 0x2508d66a;
static const td::uint32 OP_CALL_TO = 0x235caf52;
static const td::uint32 OP_CHANGE_METADATA_URI = 0xcb862902;

// jetton-wallet

static const td::uint32 OP_SET_STATUS = 0xeed236d3;

static const td::uint32 ERROR_CONTRACT_LOCKED = 45;
static const td::uint32 ERROR_BALANCE_ERROR = 47;
static const td::uint32 ERROR_NOT_ENOUGH_GAS = 48;
static const td::uint32 ERROR_INVALID_MESSAGE = 49;

}  // namespace block::precompiled::stablecoin
