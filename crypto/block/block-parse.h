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

    Copyright 2017-2020 Telegram Systems LLP
*/
#pragma once
#include "common/refcnt.hpp"
#include "vm/cells.h"
#include "vm/cellslice.h"
#include "vm/dict.h"
#include "vm/boc.h"
#include "block/block.h"
#include <ostream>
#include "tl/tlblib.hpp"
#include "td/utils/bits.h"
#include "td/utils/StringBuilder.h"
#include "ton/ton-types.h"
#include "block-auto.h"

namespace block {

using td::Ref;

namespace tlb {
using namespace ::tlb;

struct Anycast final : TLB {
  int get_size(const vm::CellSlice& cs) const override;
  bool skip_get_depth(vm::CellSlice& cs, int& depth) const;
};

extern const Anycast t_Anycast;

struct Maybe_Anycast final : public Maybe<Anycast> {
  bool skip_get_depth(vm::CellSlice& cs, int& depth) const;
};

extern const Maybe_Anycast t_Maybe_Anycast;

struct VarUInteger final : TLB_Complex {
  int n, ln;
  VarUInteger(int _n);
  bool skip(vm::CellSlice& cs) const override;
  bool validate_skip(int* ops, vm::CellSlice& cs, bool weak = false) const override;
  td::RefInt256 as_integer_skip(vm::CellSlice& cs) const override;
  unsigned long long as_uint(const vm::CellSlice& cs) const override;
  bool null_value(vm::CellBuilder& cb) const override;
  bool store_integer_value(vm::CellBuilder& cb, const td::BigInt256& value) const override;
  unsigned precompute_integer_size(const td::BigInt256& value) const;
  unsigned precompute_integer_size(td::RefInt256 value) const;
  std::ostream& print_type(std::ostream& os) const override;
};

extern const VarUInteger t_VarUInteger_3, t_VarUInteger_7, t_VarUInteger_16, t_VarUInteger_32;

struct VarUIntegerPos final : TLB_Complex {
  int n, ln;
  bool store_pos_only;
  VarUIntegerPos(int _n, bool relaxed = false);
  bool skip(vm::CellSlice& cs) const override;
  bool validate_skip(int* ops, vm::CellSlice& cs, bool weak = false) const override;
  td::RefInt256 as_integer_skip(vm::CellSlice& cs) const override;
  unsigned long long as_uint(const vm::CellSlice& cs) const override;
  bool store_integer_value(vm::CellBuilder& cb, const td::BigInt256& value) const override;
  std::ostream& print_type(std::ostream& os) const override;
};

extern const VarUIntegerPos t_VarUIntegerPos_16, t_VarUIntegerPos_32, t_VarUIntegerPosRelaxed_32;

struct VarInteger final : TLB_Complex {
  int n, ln;
  VarInteger(int _n);
  bool skip(vm::CellSlice& cs) const override;
  bool validate_skip(int* ops, vm::CellSlice& cs, bool weak = false) const override;
  td::RefInt256 as_integer_skip(vm::CellSlice& cs) const override;
  long long as_int(const vm::CellSlice& cs) const override;
  bool null_value(vm::CellBuilder& cb) const override;
  bool store_integer_value(vm::CellBuilder& cb, const td::BigInt256& value) const override;
  std::ostream& print_type(std::ostream& os) const override;
};

struct VarIntegerNz final : TLB_Complex {
  int n, ln;
  VarIntegerNz(int _n);
  bool skip(vm::CellSlice& cs) const override;
  bool validate_skip(int* ops, vm::CellSlice& cs, bool weak = false) const override;
  td::RefInt256 as_integer_skip(vm::CellSlice& cs) const override;
  long long as_int(const vm::CellSlice& cs) const override;
  bool store_integer_value(vm::CellBuilder& cb, const td::BigInt256& value) const override;
  std::ostream& print_type(std::ostream& os) const override;
};

struct Unary final : TLB {
  int get_size(const vm::CellSlice& cs) const override;
  bool validate_skip(vm::CellSlice& cs, bool weak, int& n) const;
  bool skip(vm::CellSlice& cs, int& n) const;
  bool validate_skip(int* ops, vm::CellSlice& cs, bool weak = false) const override;
  bool skip(vm::CellSlice& cs) const override;
  bool validate(int* ops, const vm::CellSlice& cs, bool weak = false) const override;
};

extern const Unary t_Unary;

struct HmLabel final : TLB_Complex {
  enum { hml_short = 0, hml_long = 2, hml_same = 3 };
  int m;  // max size
  HmLabel(int _m);
  bool validate_skip(vm::CellSlice& cs, bool weak, int& n) const;
  bool skip(vm::CellSlice& cs, int& n) const;
  bool skip(vm::CellSlice& cs) const override;
  bool validate_skip(int* ops, vm::CellSlice& cs, bool weak = false) const override;
  int get_tag(const vm::CellSlice& cs) const override;
};

struct Hashmap final : TLB_Complex {
  const TLB& value_type;
  int n;
  Hashmap(int _n, const TLB& _val_type);
  bool skip(vm::CellSlice& cs) const override;
  bool validate_skip(int* ops, vm::CellSlice& cs, bool weak = false) const override;
};

struct HashmapNode final : TLB_Complex {
  enum { hmn_leaf = 0, hmn_fork = 1 };
  const TLB& value_type;
  int n;
  HashmapNode(int _n, const TLB& _val_type);
  int get_size(const vm::CellSlice& cs) const override;
  bool skip(vm::CellSlice& cs) const override;
  bool validate_skip(int* ops, vm::CellSlice& cs, bool weak = false) const override;
  int get_tag(const vm::CellSlice& cs) const override;
};

struct HashmapE final : TLB {
  enum { hme_empty = 0, hme_root = 1 };
  Hashmap root_type;
  HashmapE(int _n, const TLB& _val_type);
  int get_size(const vm::CellSlice& cs) const override;
  bool validate(int* ops, const vm::CellSlice& cs, bool weak = false) const override;
  int get_tag(const vm::CellSlice& cs) const override;
  bool null_value(vm::CellBuilder& cb) const override;
  bool add_values(vm::CellBuilder& cb, vm::CellSlice& cs1, vm::CellSlice& cs2) const override;
  int sub_values(vm::CellBuilder& cb, vm::CellSlice& cs1, vm::CellSlice& cs2) const override;
  bool add_values_ref(Ref<vm::Cell>& res, Ref<vm::Cell> arg1, Ref<vm::Cell> arg2) const;
  int sub_values_ref(Ref<vm::Cell>& res, Ref<vm::Cell> arg1, Ref<vm::Cell> arg2) const;
  bool store_ref(vm::CellBuilder& cb, Ref<vm::Cell> arg) const;
};

struct AugmentationCheckData : vm::dict::AugmentationData {
  const TLB& value_type;
  const TLB& extra_type;
  AugmentationCheckData(const TLB& val_type, const TLB& ex_type);
  bool skip_extra(vm::CellSlice& cs) const override;
  bool eval_fork(vm::CellBuilder& cb, vm::CellSlice& left_cs, vm::CellSlice& right_cs) const override;
  bool eval_empty(vm::CellBuilder& cb) const override;
};

struct HashmapAug final : TLB_Complex {
  const AugmentationCheckData& aug;
  int n;
  HashmapAug(int _n, const AugmentationCheckData& _aug);
  bool skip(vm::CellSlice& cs) const override;
  bool validate_skip(int* ops, vm::CellSlice& cs, bool weak = false) const override;
  bool extract_extra(vm::CellSlice& cs) const;
};

struct HashmapAugNode final : TLB_Complex {
  enum { ahmn_leaf = 0, ahmn_fork = 1 };
  const AugmentationCheckData& aug;
  int n;
  HashmapAugNode(int _n, const AugmentationCheckData& _aug);
  bool skip(vm::CellSlice& cs) const override;
  bool validate_skip(int* ops, vm::CellSlice& cs, bool weak = false) const override;
  int get_tag(const vm::CellSlice& cs) const override;
};

struct HashmapAugE final : TLB_Complex {
  enum { ahme_empty = 0, ahme_root = 1 };
  HashmapAug root_type;
  HashmapAugE(int _n, const AugmentationCheckData& _aug);
  bool skip(vm::CellSlice& cs) const override;
  bool validate_skip(int* ops, vm::CellSlice& cs, bool weak = false) const override;
  bool extract_extra(vm::CellSlice& cs) const;
  int get_tag(const vm::CellSlice& cs) const override;
};

struct Grams final : TLB_Complex {
  bool validate_skip(int* ops, vm::CellSlice& cs, bool weak = false) const override;
  td::RefInt256 as_integer_skip(vm::CellSlice& cs) const override;
  bool null_value(vm::CellBuilder& cb) const override;
  bool store_integer_value(vm::CellBuilder& cb, const td::BigInt256& value) const override;
  unsigned precompute_size(const td::BigInt256& value) const;
  unsigned precompute_size(td::RefInt256 value) const;
};

extern const Grams t_Grams;

struct MsgAddressInt final : TLB_Complex {
  enum { addr_std = 2, addr_var = 3 };
  bool validate_skip(int* ops, vm::CellSlice& cs, bool weak = false) const override;
  int get_tag(const vm::CellSlice& cs) const override;
  static ton::AccountIdPrefixFull get_prefix(vm::CellSlice&& cs);
  static ton::AccountIdPrefixFull get_prefix(const vm::CellSlice& cs);
  static ton::AccountIdPrefixFull get_prefix(Ref<vm::CellSlice> cs_ref);
  static bool get_prefix_to(vm::CellSlice&& cs, ton::AccountIdPrefixFull& pfx);
  static bool get_prefix_to(const vm::CellSlice& cs, ton::AccountIdPrefixFull& pfx);
  static bool get_prefix_to(Ref<vm::CellSlice> cs_ref, ton::AccountIdPrefixFull& pfx);
  bool skip_get_depth(vm::CellSlice& cs, int& depth) const;
  bool extract_std_address(Ref<vm::CellSlice> cs_ref, ton::WorkchainId& workchain, ton::StdSmcAddress& addr,
                           bool rewrite = true) const;
  bool extract_std_address(vm::CellSlice& cs, ton::WorkchainId& workchain, ton::StdSmcAddress& addr,
                           bool rewrite = true) const;
  bool extract_std_address(Ref<vm::CellSlice> cs_ref, block::StdAddress& addr, bool rewrite = true) const;
  bool extract_std_address(vm::CellSlice& cs, block::StdAddress& addr, bool rewrite = true) const;
  bool store_std_address(vm::CellBuilder& cb, ton::WorkchainId workchain, const ton::StdSmcAddress& addr) const;
  Ref<vm::CellSlice> pack_std_address(ton::WorkchainId workchain, const ton::StdSmcAddress& addr) const;

  bool store_std_address(vm::CellBuilder& cb, const block::StdAddress& addr) const;
  Ref<vm::CellSlice> pack_std_address(const block::StdAddress& addr) const;
};

extern const MsgAddressInt t_MsgAddressInt;

struct MsgAddressExt final : TLB {
  enum { addr_none = 0, addr_ext = 1 };
  int get_size(const vm::CellSlice& cs) const override;
  int get_tag(const vm::CellSlice& cs) const override;
};

extern const MsgAddressExt t_MsgAddressExt;

struct MsgAddress final : TLB_Complex {
  enum { addr_none = 0, addr_ext = 1, addr_std = 2, addr_var = 3 };
  bool validate_skip(int* ops, vm::CellSlice& cs, bool weak = false) const override;
  int get_tag(const vm::CellSlice& cs) const override;
};

extern const MsgAddress t_MsgAddress;

struct ExtraCurrencyCollection final : TLB {
  HashmapE dict_type, dict_type2;
  ExtraCurrencyCollection();
  int get_size(const vm::CellSlice& cs) const override;
  bool validate(int* ops, const vm::CellSlice& cs, bool weak) const override;
  bool null_value(vm::CellBuilder& cb) const override;
  bool add_values(vm::CellBuilder& cb, vm::CellSlice& cs1, vm::CellSlice& cs2) const override;
  int sub_values(vm::CellBuilder& cb, vm::CellSlice& cs1, vm::CellSlice& cs2) const override;
  bool add_values_ref(Ref<vm::Cell>& res, Ref<vm::Cell> arg1, Ref<vm::Cell> arg2) const;
  int sub_values_ref(Ref<vm::Cell>& res, Ref<vm::Cell> arg1, Ref<vm::Cell> arg2) const;
  bool store_ref(vm::CellBuilder& cb, Ref<vm::Cell> arg) const;
  unsigned precompute_size(Ref<vm::Cell> arg) const;
};

extern const ExtraCurrencyCollection t_ExtraCurrencyCollection;

struct CurrencyCollection final : TLB_Complex {
  bool skip(vm::CellSlice& cs) const override;
  bool validate_skip(int* ops, vm::CellSlice& cs, bool weak = false) const override;
  td::RefInt256 as_integer_skip(vm::CellSlice& cs) const override;
  bool null_value(vm::CellBuilder& cb) const override;
  bool add_values(vm::CellBuilder& cb, vm::CellSlice& cs1, vm::CellSlice& cs2) const override;
  bool unpack_special(vm::CellSlice& cs, td::RefInt256& balance, Ref<vm::Cell>& extra, bool inexact = false) const;
  bool unpack_special(vm::CellSlice& cs, block::CurrencyCollection& value, bool inexact = false) const;
  bool pack_special(vm::CellBuilder& cb, td::RefInt256 balance, Ref<vm::Cell> extra) const;
  bool pack_special(vm::CellBuilder& cb, const block::CurrencyCollection& value) const;
  bool pack_special(vm::CellBuilder& cb, block::CurrencyCollection&& value) const;
  bool unpack(vm::CellSlice& cs, block::CurrencyCollection& res) const;
  bool pack(vm::CellBuilder& cb, const block::CurrencyCollection& res) const;
  unsigned precompute_size(td::RefInt256 balance, Ref<vm::Cell> extra) const;
};

extern const CurrencyCollection t_CurrencyCollection;

struct CommonMsgInfo final : TLB_Complex {
  enum { int_msg_info = 0, ext_in_msg_info = 2, ext_out_msg_info = 3 };
  bool skip(vm::CellSlice& cs) const override;
  bool validate_skip(int* ops, vm::CellSlice& cs, bool weak = false) const override;
  int get_tag(const vm::CellSlice& cs) const override;
  struct Record_int_msg_info;
  bool unpack(vm::CellSlice& cs, Record_int_msg_info& data) const;
  bool get_created_lt(vm::CellSlice& cs, unsigned long long& created_lt) const;
  bool is_internal(const vm::CellSlice& cs) const;
};

struct CommonMsgInfo::Record_int_msg_info {
  bool ihr_disabled, bounce, bounced;
  Ref<vm::CellSlice> src, dest, value, ihr_fee, fwd_fee;
  unsigned long long created_lt;
  unsigned created_at;
};

extern const CommonMsgInfo t_CommonMsgInfo;

struct TickTock final : TLB {
  int get_size(const vm::CellSlice& cs) const override;
};

extern const TickTock t_TickTock;

struct StateInit final : TLB_Complex {
  bool validate_skip(int* ops, vm::CellSlice& cs, bool weak = false) const override;
  bool get_ticktock(vm::CellSlice& cs, int& ticktock) const;
};

extern const StateInit t_StateInit;

struct Message final : TLB_Complex {
  bool validate_skip(int* ops, vm::CellSlice& cs, bool weak = false) const override;
  bool extract_info(vm::CellSlice& cs) const;
  bool get_created_lt(vm::CellSlice& cs, unsigned long long& created_lt) const;
  bool is_internal(const vm::CellSlice& cs) const;
  bool is_internal(Ref<vm::Cell> ref) const;
};

extern const Message t_Message;
extern const RefTo<Message> t_Ref_Message;

struct IntermediateAddress final : TLB_Complex {
  enum { interm_addr_regular = 0, interm_addr_simple = 2, interm_addr_ext = 3 };
  int get_size(const vm::CellSlice& cs) const override;
  bool skip(vm::CellSlice& cs) const override;
  bool validate_skip(int* ops, vm::CellSlice& cs, bool weak = false) const override;
  bool fetch_regular(vm::CellSlice& cs, int& use_dst_bits) const;
  int get_tag(const vm::CellSlice& cs) const override;
};

extern const IntermediateAddress t_IntermediateAddress;

struct MsgEnvelope final : TLB_Complex {
  bool skip(vm::CellSlice& cs) const override;
  bool validate_skip(int* ops, vm::CellSlice& cs, bool weak = false) const override;
  bool extract_fwd_fees_remaining(vm::CellSlice& cs) const;
  struct Record {
    typedef MsgEnvelope type_class;
    Ref<vm::CellSlice> cur_addr, next_addr, fwd_fee_remaining;
    Ref<vm::Cell> msg;
  };
  struct Record_std {
    typedef MsgEnvelope type_class;
    int cur_addr, next_addr;
    td::RefInt256 fwd_fee_remaining;
    Ref<vm::Cell> msg;
    td::optional<ton::LogicalTime> emitted_lt;
    td::optional<MsgMetadata> metadata;
  };
  bool unpack(vm::CellSlice& cs, Record& data) const;
  bool unpack(vm::CellSlice& cs, Record_std& data) const;
  bool pack(vm::CellBuilder& cb, const Record_std& data) const;
  bool pack_cell(td::Ref<vm::Cell>& cell, const Record_std& data) const;
  bool get_emitted_lt(const vm::CellSlice& cs, unsigned long long& emitted_lt) const;
  int get_tag(const vm::CellSlice& cs) const override {
    return (int)cs.prefetch_ulong(4);
  }
};

extern const MsgEnvelope t_MsgEnvelope;
extern const RefTo<MsgEnvelope> t_Ref_MsgEnvelope;

struct StorageUsed final : TLB_Complex {
  bool skip(vm::CellSlice& cs) const override;
  bool validate_skip(int* ops, vm::CellSlice& cs, bool weak = false) const override;
};

extern const StorageUsed t_StorageUsed;

struct StorageUsedShort final : TLB_Complex {
  bool skip(vm::CellSlice& cs) const override;
  bool validate_skip(int* ops, vm::CellSlice& cs, bool weak = false) const override;
};

extern const StorageUsedShort t_StorageUsedShort;

struct StorageInfo final : TLB_Complex {
  bool skip(vm::CellSlice& cs) const override;
  bool validate_skip(int* ops, vm::CellSlice& cs, bool weak = false) const override;
};

extern const StorageInfo t_StorageInfo;

struct AccountState final : TLB_Complex {
  enum { account_uninit = 0, account_frozen = 1, account_active = 2 };
  bool validate_skip(int* ops, vm::CellSlice& cs, bool weak = false) const override;
  int get_tag(const vm::CellSlice& cs) const override;
  bool get_ticktock(vm::CellSlice& cs, int& ticktock) const;
};

extern const AccountState t_AccountState;

struct AccountStorage final : TLB_Complex {
  bool skip(vm::CellSlice& cs) const override;
  bool validate_skip(int* ops, vm::CellSlice& cs, bool weak = false) const override;
  bool skip_copy_balance(vm::CellBuilder& cb, vm::CellSlice& cs) const;
};

extern const AccountStorage t_AccountStorage;

struct Account final : TLB_Complex {
  enum { account_none = 0, account = 1 };
  bool allow_empty;
  Account(bool _allow_empty = false);
  bool skip(vm::CellSlice& cs) const override;
  bool validate_skip(int* ops, vm::CellSlice& cs, bool weak = false) const override;
  // Ref<vm::CellSlice> get_balance(const vm::CellSlice& cs) const;
  bool skip_copy_balance(vm::CellBuilder& cb, vm::CellSlice& cs) const;
  bool skip_copy_depth_balance(vm::CellBuilder& cb, vm::CellSlice& cs) const;
  int get_tag(const vm::CellSlice& cs) const override;
};

extern const Account t_Account, t_AccountE;
extern const RefTo<Account> t_Ref_AccountE;

struct AccountStatus final : TLB {
  enum { acc_state_uninit, acc_state_frozen, acc_state_active, acc_state_nonexist };
  int get_size(const vm::CellSlice& cs) const override;
  int get_tag(const vm::CellSlice& cs) const override;
};

extern const AccountStatus t_AccountStatus;

struct ShardAccount final : TLB_Complex {
  struct Record {
    using Type = ShardAccount;
    Ref<vm::Cell> account;
    ton::LogicalTime last_trans_lt;
    ton::Bits256 last_trans_hash;
    bool valid{false};
    bool is_zero{false};
    bool reset();
    bool unpack(vm::CellSlice& cs);
    bool unpack(Ref<vm::CellSlice> cs_ref);
    bool invalidate();
  };
  int get_size(const vm::CellSlice& cs) const override;
  bool skip(vm::CellSlice& cs) const override;
  bool validate_skip(int* ops, vm::CellSlice& cs, bool weak = false) const override;
  static bool unpack(vm::CellSlice& cs, Record& info);
  static bool unpack(Ref<vm::CellSlice> cs_ref, Record& info);
  static bool extract_account_state(Ref<vm::CellSlice> cs_ref, Ref<vm::Cell>& acc_state);
};

extern const ShardAccount t_ShardAccount;

struct DepthBalanceInfo final : TLB_Complex {
  bool skip(vm::CellSlice& cs) const override;
  bool validate_skip(int* ops, vm::CellSlice& cs, bool weak = false) const override;
  bool null_value(vm::CellBuilder& cb) const override;
  bool add_values(vm::CellBuilder& cb, vm::CellSlice& cs1, vm::CellSlice& cs2) const override;
};

extern const DepthBalanceInfo t_DepthBalanceInfo;

struct Aug_ShardAccounts final : AugmentationCheckData {
  Aug_ShardAccounts();
  bool eval_leaf(vm::CellBuilder& cb, vm::CellSlice& cs) const override;
};

extern const Aug_ShardAccounts aug_ShardAccounts;

struct ShardAccounts final : TLB_Complex {
  HashmapAugE dict_type;
  ShardAccounts();
  ;
  bool skip(vm::CellSlice& cs) const override;
  bool validate_skip(int* ops, vm::CellSlice& cs, bool weak = false) const override;
};

extern const ShardAccounts t_ShardAccounts;

struct AccStatusChange final : TLB {
  enum { acst_unchanged = 0, acst_frozen = 2, acst_deleted = 3 };
  int get_size(const vm::CellSlice& cs) const override;
  int get_tag(const vm::CellSlice& cs) const override;
};

extern const AccStatusChange t_AccStatusChange;

struct TrStoragePhase final : TLB_Complex {
  bool skip(vm::CellSlice& cs) const override;
  bool validate_skip(int* ops, vm::CellSlice& cs, bool weak = false) const override;
  bool get_storage_fees(vm::CellSlice& cs, td::RefInt256& storage_fees) const;
  bool maybe_get_storage_fees(vm::CellSlice& cs, td::RefInt256& storage_fees) const;
};

extern const TrStoragePhase t_TrStoragePhase;

struct TrCreditPhase final : TLB_Complex {
  bool skip(vm::CellSlice& cs) const override;
  bool validate_skip(int* ops, vm::CellSlice& cs, bool weak = false) const override;
};

extern const TrCreditPhase t_TrCreditPhase;

struct TrComputeInternal1 final : TLB_Complex {
  bool skip(vm::CellSlice& cs) const override;
  bool validate_skip(int* ops, vm::CellSlice& cs, bool weak = false) const override;
};

struct ComputeSkipReason final : TLB {
  enum { cskip_no_state = 0, cskip_bad_state = 1, cskip_no_gas = 2, cskip_suspended = 3 };
  int get_size(const vm::CellSlice& cs) const override;
  bool validate_skip(int* ops, vm::CellSlice& cs, bool weak = false) const override;
  int get_tag(const vm::CellSlice& cs) const override;
};

extern const ComputeSkipReason t_ComputeSkipReason;

struct TrComputePhase final : TLB_Complex {
  enum { tr_phase_compute_skipped = 0, tr_phase_compute_vm = 1 };
  bool skip(vm::CellSlice& cs) const override;
  bool validate_skip(int* ops, vm::CellSlice& cs, bool weak = false) const override;
  int get_tag(const vm::CellSlice& cs) const override;
};

extern const TrComputePhase t_TrComputePhase;

struct TrActionPhase final : TLB_Complex {
  bool skip(vm::CellSlice& cs) const override;
  bool validate_skip(int* ops, vm::CellSlice& cs, bool weak = false) const override;
};

extern const TrActionPhase t_TrActionPhase;

struct TrBouncePhase final : TLB_Complex {
  enum { tr_phase_bounce_negfunds = 0, tr_phase_bounce_nofunds = 1, tr_phase_bounce_ok = 2 };
  bool skip(vm::CellSlice& cs) const override;
  bool validate_skip(int* ops, vm::CellSlice& cs, bool weak = false) const override;
  int get_tag(const vm::CellSlice& cs) const override;
};

extern const TrBouncePhase t_TrBouncePhase;

struct SplitMergeInfo final : TLB_Complex {
  bool skip(vm::CellSlice& cs) const override;
  bool validate_skip(int* ops, vm::CellSlice& cs, bool weak = false) const override;
};

extern const SplitMergeInfo t_SplitMergeInfo;

struct TransactionDescr final : TLB_Complex {
  enum {
    trans_ord = 0,
    trans_storage = 1,
    trans_tick_tock = 2,
    trans_split_prepare = 4,
    trans_split_install = 5,
    trans_merge_prepare = 6,
    trans_merge_install = 7
  };
  bool skip(vm::CellSlice& cs) const override;
  bool validate_skip(int* ops, vm::CellSlice& cs, bool weak = false) const override;
  int get_tag(const vm::CellSlice& cs) const override;
  bool skip_to_storage_phase(vm::CellSlice& cs, bool& found) const;
  bool get_storage_fees(Ref<vm::Cell> cell, td::RefInt256& storage_fees) const;
};

extern const TransactionDescr t_TransactionDescr;

struct Transaction_aux final : TLB_Complex {
  bool skip(vm::CellSlice& cs) const override;
  bool validate_skip(int* ops, vm::CellSlice& cs, bool weak = false) const override;
};

extern const Transaction_aux t_Transaction_aux;

struct Transaction final : TLB_Complex {
  bool skip(vm::CellSlice& cs) const override;
  bool validate_skip(int* ops, vm::CellSlice& cs, bool weak = false) const override;
  bool get_total_fees(vm::CellSlice&& cs, block::CurrencyCollection& total_fees) const;
  bool get_descr(Ref<vm::Cell> cell, Ref<vm::Cell>& tdescr) const;
  bool get_descr(vm::CellSlice& cs, Ref<vm::Cell>& tdescr) const;
  bool get_storage_fees(Ref<vm::Cell> cell, td::RefInt256& storage_fees) const;
};

extern const Transaction t_Transaction;
extern const RefTo<Transaction> t_Ref_Transaction;

struct Aug_AccountTransactions final : AugmentationCheckData {
  Aug_AccountTransactions();
  bool eval_leaf(vm::CellBuilder& cb, vm::CellSlice& cs) const override;
};

extern const Aug_AccountTransactions aug_AccountTransactions;
extern const HashmapAug t_AccountTransactions;  // (HashmapAug 64 ^Transaction CurrencyCollection)

struct HashUpdate final : TLB_Complex {
  bool skip(vm::CellSlice& cs) const override;
  bool validate_skip(int* ops, vm::CellSlice& cs, bool weak = false) const override;
};

extern const HashUpdate t_HashUpdate;
extern const RefTo<HashUpdate> t_Ref_HashUpdate;

struct AccountBlock final : TLB_Complex {
  bool skip(vm::CellSlice& cs) const override;
  bool validate_skip(int* ops, vm::CellSlice& cs, bool weak = false) const override;
  bool get_total_fees(vm::CellSlice&& cs, block::CurrencyCollection& total_fees) const;
};

extern const AccountBlock t_AccountBlock;

struct Aug_ShardAccountBlocks final : AugmentationCheckData {
  Aug_ShardAccountBlocks();
  bool eval_leaf(vm::CellBuilder& cb, vm::CellSlice& cs) const override;
};

extern const Aug_ShardAccountBlocks aug_ShardAccountBlocks;
extern const HashmapAugE t_ShardAccountBlocks;  // (HashmapAugE 256 AccountBlock CurrencyCollection)

struct ImportFees final : TLB_Complex {
  bool skip(vm::CellSlice& cs) const override;
  bool validate_skip(int* ops, vm::CellSlice& cs, bool weak = false) const override;
  bool null_value(vm::CellBuilder& cb) const override;
  bool add_values(vm::CellBuilder& cb, vm::CellSlice& cs1, vm::CellSlice& cs2) const override;
};

extern const ImportFees t_ImportFees;

struct InMsg final : TLB_Complex {
  enum {
    msg_import_ext = 0,
    msg_import_ihr = 2,
    msg_import_imm = 3,
    msg_import_fin = 4,
    msg_import_tr = 5,
    msg_discard_fin = 6,
    msg_discard_tr = 7,
    msg_import_deferred_fin = 8,
    msg_import_deferred_tr = 9
  };
  bool skip(vm::CellSlice& cs) const override;
  bool validate_skip(int* ops, vm::CellSlice& cs, bool weak = false) const override;
  int get_tag(const vm::CellSlice& cs) const override;
  bool get_import_fees(vm::CellBuilder& cb, vm::CellSlice& cs) const;
};

extern const InMsg t_InMsg;

struct OutMsg final : TLB_Complex {
  enum {
    msg_export_ext = 0,
    msg_export_new = 1,
    msg_export_imm = 2,
    msg_export_tr = 3,
    msg_export_deq_imm = 4,
    msg_export_deq = 12,
    msg_export_deq_short = 13,
    msg_export_tr_req = 7,
    msg_export_new_defer = 20,   // 0b10100
    msg_export_deferred_tr = 21  // 0b10101
  };
  bool skip(vm::CellSlice& cs) const override;
  bool validate_skip(int* ops, vm::CellSlice& cs, bool weak = false) const override;
  int get_tag(const vm::CellSlice& cs) const override;
  bool get_export_value(vm::CellBuilder& cb, vm::CellSlice& cs) const;
  bool get_emitted_lt(vm::CellSlice& cs, unsigned long long& emitted_lt) const;
};

extern const OutMsg t_OutMsg;

// next: InMsgDescr, OutMsgDescr, OutMsgQueue, and their augmentations

struct Aug_InMsgDescr final : AugmentationCheckData {
  Aug_InMsgDescr();
  bool eval_leaf(vm::CellBuilder& cb, vm::CellSlice& cs) const override;
};

extern const Aug_InMsgDescr aug_InMsgDescr;

struct InMsgDescr final : TLB_Complex {
  HashmapAugE dict_type;
  InMsgDescr();
  ;
  bool skip(vm::CellSlice& cs) const override;
  bool validate_skip(int* ops, vm::CellSlice& cs, bool weak = false) const override;
};

extern const InMsgDescr t_InMsgDescr;

struct Aug_OutMsgDescr final : AugmentationCheckData {
  Aug_OutMsgDescr();
  bool eval_leaf(vm::CellBuilder& cb, vm::CellSlice& cs) const override;
};

extern const Aug_OutMsgDescr aug_OutMsgDescr;

struct OutMsgDescr final : TLB_Complex {
  HashmapAugE dict_type;
  OutMsgDescr();
  ;
  bool skip(vm::CellSlice& cs) const override;
  bool validate_skip(int* ops, vm::CellSlice& cs, bool weak = false) const override;
};

extern const OutMsgDescr t_OutMsgDescr;

struct EnqueuedMsg final : TLB_Complex {
  int get_size(const vm::CellSlice& cs) const override;
  bool skip(vm::CellSlice& cs) const override;
  bool validate_skip(int* ops, vm::CellSlice& cs, bool weak = false) const override;
  bool unpack(vm::CellSlice& cs, EnqueuedMsgDescr& descr) const;
};

extern const EnqueuedMsg t_EnqueuedMsg;

struct Aug_OutMsgQueue final : AugmentationCheckData {
  Aug_OutMsgQueue();
  bool eval_fork(vm::CellBuilder& cb, vm::CellSlice& left_cs, vm::CellSlice& right_cs) const override;
  bool eval_empty(vm::CellBuilder& cb) const override;
  bool eval_leaf(vm::CellBuilder& cb, vm::CellSlice& cs) const override;
};

extern const Aug_OutMsgQueue aug_OutMsgQueue;

struct Aug_DispatchQueue final : AugmentationCheckData {
  Aug_DispatchQueue();
  bool eval_fork(vm::CellBuilder& cb, vm::CellSlice& left_cs, vm::CellSlice& right_cs) const override;
  bool eval_empty(vm::CellBuilder& cb) const override;
  bool eval_leaf(vm::CellBuilder& cb, vm::CellSlice& cs) const override;
};

extern const Aug_DispatchQueue aug_DispatchQueue;

struct OutMsgQueue final : TLB_Complex {
  HashmapAugE dict_type;
  OutMsgQueue();
  ;
  bool skip(vm::CellSlice& cs) const override;
  bool validate_skip(int* ops, vm::CellSlice& cs, bool weak = false) const override;
};

extern const OutMsgQueue t_OutMsgQueue;

struct ProcessedUpto final : TLB {
  int get_size(const vm::CellSlice& cs) const override;
};

extern const ProcessedUpto t_ProcessedUpto;
extern const HashmapE t_ProcessedInfo;
extern const HashmapE t_IhrPendingInfo;

struct OutMsgQueueInfo final : TLB_Complex {
  bool skip(vm::CellSlice& cs) const override;
  bool validate_skip(int* ops, vm::CellSlice& cs, bool weak = false) const override;
};

extern const OutMsgQueueInfo t_OutMsgQueueInfo;
extern const RefTo<OutMsgQueueInfo> t_Ref_OutMsgQueueInfo;

struct ExtBlkRef final : TLB {
  enum { fixed_size = 64 + 32 + 256 * 2 };
  int get_size(const vm::CellSlice& cs) const override;
  bool unpack(vm::CellSlice& cs, ton::BlockIdExt& blkid, ton::LogicalTime* end_lt = nullptr) const;
  bool unpack(Ref<vm::CellSlice> cs_ref, ton::BlockIdExt& blkid, ton::LogicalTime* end_lt = nullptr) const;
  bool store(vm::CellBuilder& cb, const ton::BlockIdExt& blkid, ton::LogicalTime end_lt) const;
  Ref<vm::Cell> pack_cell(const ton::BlockIdExt& blkid, ton::LogicalTime end_lt) const;
  bool pack_to(Ref<vm::Cell>& cell, const ton::BlockIdExt& blkid, ton::LogicalTime end_lt) const;
};

extern const ExtBlkRef t_ExtBlkRef;

struct BlkMasterInfo final : TLB {
  int get_size(const vm::CellSlice& cs) const override;
};

extern const BlkMasterInfo t_BlkMasterInfo;

struct ShardIdent final : TLB_Complex {
  struct Record;
  int get_size(const vm::CellSlice& cs) const override;
  bool skip(vm::CellSlice& cs) const override;
  bool validate_skip(int* ops, vm::CellSlice& cs, bool weak = false) const override;
  int get_tag(const vm::CellSlice& cs) const override;
  bool unpack(vm::CellSlice& cs, Record& data) const;
  bool pack(vm::CellBuilder& cb, const Record& data) const;
  bool unpack(vm::CellSlice& cs, ton::ShardIdFull& data) const;
  bool pack(vm::CellBuilder& cb, ton::ShardIdFull data) const;
  bool unpack(vm::CellSlice& cs, ton::WorkchainId& workchain, ton::ShardId& shard) const;
  bool pack(vm::CellBuilder& cb, ton::WorkchainId workchain, ton::ShardId shard) const;
};

struct ShardIdent::Record {
  int shard_pfx_bits;
  int workchain_id;
  unsigned long long shard_prefix;
  Record();
  Record(int _pfxlen, int _wcid, unsigned long long _pfx);
  bool check() const;
  bool is_valid() const;
  void invalidate();
};

extern const ShardIdent t_ShardIdent;

struct BlockIdExt final : TLB_Complex {
  int get_size(const vm::CellSlice& cs) const override;
  bool skip(vm::CellSlice& cs) const override;
  bool validate_skip(int* ops, vm::CellSlice& cs, bool weak = false) const override;
  bool unpack(vm::CellSlice& cs, ton::BlockIdExt& data) const;
  bool pack(vm::CellBuilder& cb, const ton::BlockIdExt& data) const;
};

extern const BlockIdExt t_BlockIdExt;

struct ShardState final : TLB_Complex {
  enum { shard_state = (int)0x9023afe2, split_state = 0x5f327da5 };
  bool skip(vm::CellSlice& cs) const override;
  bool validate_skip(int* ops, vm::CellSlice& cs, bool weak = false) const override;
  int get_tag(const vm::CellSlice& cs) const override;
};

extern const ShardState t_ShardState;

struct ShardState_aux final : TLB_Complex {
  bool skip(vm::CellSlice& cs) const override;
  bool validate_skip(int* ops, vm::CellSlice& cs, bool weak = false) const override;
  int get_tag(const vm::CellSlice& cs) const override;
};

extern const ShardState_aux t_ShardState_aux;

struct LibDescr final : TLB_Complex {
  enum { shared_lib_descr = 0 };
  bool skip(vm::CellSlice& cs) const override;
  bool validate_skip(int* ops, vm::CellSlice& cs, bool weak = false) const override;
  int get_tag(const vm::CellSlice& cs) const override;
};

extern const LibDescr t_LibDescr;

struct BlkPrevInfo final : TLB_Complex {
  bool merged;
  BlkPrevInfo(bool _merged);
  bool skip(vm::CellSlice& cs) const override;
  bool validate_skip(int* ops, vm::CellSlice& cs, bool weak = false) const override;
};

extern const BlkPrevInfo t_BlkPrevInfo_0;

struct McStateExtra final : TLB_Complex {
  enum { masterchain_state_extra = 0xcc26 };
  bool skip(vm::CellSlice& cs) const override;
  bool validate_skip(int* ops, vm::CellSlice& cs, bool weak = false) const override;
};

extern const McStateExtra t_McStateExtra;

struct KeyExtBlkRef final : TLB {
  enum { fixed_size = 1 + ExtBlkRef::fixed_size };
  int get_size(const vm::CellSlice& cs) const override;
};

extern const KeyExtBlkRef t_KeyExtBlkRef;

struct KeyMaxLt final : TLB {
  enum { fixed_size = 1 + 64 };
  int get_size(const vm::CellSlice& cs) const override;
  bool null_value(vm::CellBuilder& cb) const override;
  bool add_values(vm::CellBuilder& cb, vm::CellSlice& cs1, vm::CellSlice& cs2) const override;
};

extern const KeyMaxLt t_KeyMaxLt;

struct Aug_OldMcBlocksInfo final : AugmentationCheckData {
  Aug_OldMcBlocksInfo();
  bool eval_leaf(vm::CellBuilder& cb, vm::CellSlice& cs) const override;
};

extern const Aug_OldMcBlocksInfo aug_OldMcBlocksInfo;

struct ShardFeeCreated final : TLB_Complex {
  bool skip(vm::CellSlice& cs) const override;
  bool validate_skip(int* ops, vm::CellSlice& cs, bool weak = false) const override;
  bool null_value(vm::CellBuilder& cb) const override;
  bool add_values(vm::CellBuilder& cb, vm::CellSlice& cs1, vm::CellSlice& cs2) const override;
};

extern const ShardFeeCreated t_ShardFeeCreated;

struct Aug_ShardFees final : AugmentationCheckData {
  Aug_ShardFees();
  bool eval_leaf(vm::CellBuilder& cb, vm::CellSlice& cs) const override;
};

extern const Aug_ShardFees aug_ShardFees;

// Validate dict of libraries in message: used when sending and receiving message
bool validate_message_libs(const td::Ref<vm::Cell> &cell);
bool validate_message_relaxed_libs(const td::Ref<vm::Cell> &cell);

}  // namespace tlb
}  // namespace block
