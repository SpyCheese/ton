/*
 * Copyright (c) 2025-2026, TON CORE TECHNOLOGIES CO. L.L.C
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */
#include "adnl/adnl-local-id.h"
#include "adnl/adnl.h"
#include "td/utils/Random.h"

#include "block-auto.h"
#include "block-parse.h"
#include "shard.hpp"
#include "transaction.h"
#include "validator-registry-watcher.hpp"

namespace ton::validator {

namespace {
bool cell_equal(const Ref<vm::Cell>& a, const Ref<vm::Cell>& b) {
  if (a.is_null()) {
    return b.is_null();
  }
  if (b.is_null()) {
    return false;
  }
  return a->get_hash() == b->get_hash();
}

td::uint32 get_elections_end_before(Ref<vm::Cell> cell) {
  if (cell.is_null()) {
    return 0;
  }
  block::gen::ElectionTimings::Record rec;
  if (!block::gen::unpack_cell(cell, rec)) {
    return 0;
  }
  return rec.elections_end_before;
}

struct Entry {
  std::vector<adnl::AdnlNodeIdShort> collators;
};

struct ValidatorInfo {
  UnixTime allow_update_at = 0;
  Ref<vm::Cell> entry = {};
};

struct RegistryState {
  vm::Dictionary validators{256};
  BlockSeqno last_cleanup_key_block_seqno = 0;
  vm::Dictionary adnl_ids{256};
  bool contract_updated = false;
  block::ValidatorRegistryConfig config;

  td::Result<ValidatorInfo> get_validator_info(td::Bits256 public_key) {
    try {
      auto cs = validators.lookup(public_key);
      if (cs.is_null()) {
        return ValidatorInfo{};
      }
      block::gen::ValRegistryValidator::Record val_rec;
      if (!block::gen::csr_unpack(cs, val_rec)) {
        return td::Status::Error("failed to parse contract data");
      }
      return ValidatorInfo{.allow_update_at = val_rec.allow_update_at, .entry = val_rec.entry->prefetch_ref()};
    } catch (vm::VmError& e) {
      return e.as_status("failed to parse contract data: ");
    }
  }

  td::Result<Entry> get_validator_entry(td::Bits256 public_key) {
    try {
      TRY_RESULT(info, get_validator_info(public_key));
      if (info.entry.is_null()) {
        return Entry{};
      }
      Ref<vm::Cell> collators_root;
      if (block::gen::ValRegistryEntry::Record rec; block::gen::unpack_cell(info.entry, rec)) {
        collators_root = rec.collators->prefetch_ref();
      } else if (block::gen::ValRegistryEntryOld::Record rec; block::gen::unpack_cell(info.entry, rec)) {
        collators_root = rec.collators->prefetch_ref();
      } else {
        return td::Status::Error("failed to parse entry");
      }
      Entry entry;
      td::uint32 cnt = 0;
      vm::Dictionary{collators_root, 256}.check_for_each([&](Ref<vm::CellSlice>, td::ConstBitPtr key, int) -> bool {
        if (cnt >= config.max_collators_per_validator) {
          return false;
        }
        ++cnt;
        entry.collators.emplace_back(td::Bits256{key});
        return true;
      });
      return entry;
    } catch (vm::VmError& e) {
      return e.as_status("failed to parse entry: ");
    }
  }

  static td::Result<RegistryState> fetch(Ref<MasterchainState> mc_state) {
    TRY_RESULT(registry_config, mc_state->get_validator_registry_config());
    auto mc_config = Ref<MasterchainStateQ>{mc_state}->get_config();
    if (!mc_config->is_special_smartcontract(registry_config.contract_address)) {
      return td::Status::Error(PSTRING() << "registry contract -1:" << registry_config.contract_address.to_hex()
                                         << " should be special");
    }
    auto accounts_dict = mc_config->get_accounts_dict();
    const StdSmcAddress& addr = registry_config.contract_address;
    auto account_cs = accounts_dict.lookup(addr);
    if (account_cs.is_null()) {
      return td::Status::Error(PSTRING() << "registry contract -1:" << addr.to_hex() << " not found");
    }
    block::Account account;
    if (!account.unpack(std::move(account_cs), mc_state->get_unix_time(), true)) {
      return td::Status::Error(PSTRING() << "failed to unpack registry contract -1:" << addr.to_hex());
    }
    if (account.data.is_null()) {
      return td::Status::Error(PSTRING() << "registry contract -1:" << addr.to_hex() << " has no data");
    }

    RegistryState result;
    result.config = std::move(registry_config);
    if (block::gen::ValRegistryStorage::Record_val_registry_storage_old rec;
        block::gen::unpack_cell(account.data, rec)) {
      result.validators = vm::Dictionary{rec.registry, 256, false};
      result.last_cleanup_key_block_seqno = rec.last_cleanup_key_block_seqno;
      return std::move(result);
    }
    if (block::gen::ValRegistryStorage::Record_val_registry_storage rec; block::gen::unpack_cell(account.data, rec)) {
      result.validators = vm::Dictionary{rec.registry, 256, false};
      result.last_cleanup_key_block_seqno = rec.last_cleanup_key_block_seqno;
      result.adnl_ids = vm::Dictionary{rec.adnl_ids, 256, false};
      result.contract_updated = true;
      return std::move(result);
    }
    return td::Status::Error(PSTRING() << "failed to unpack registry contract -1:" << addr.to_hex() << " data");
  }
};

class ValidatorRegistryWatcherImpl : public ValidatorRegistryWatcher {
 public:
  explicit ValidatorRegistryWatcherImpl(td::actor::ActorId<ValidatorManager> manager,
                                        td::actor::ActorId<keyring::Keyring> keyring,
                                        td::actor::ActorId<adnl::Adnl> adnl)
      : manager_(manager), keyring_(keyring), adnl_(adnl) {
  }

  void start_up() override {
    class Cb : public adnl::Adnl::LocalIdCallback {
     public:
      explicit Cb(td::actor::ActorId<ValidatorRegistryWatcherImpl> actor) : actor_(actor) {
      }
      void local_id_added(adnl::AdnlNodeIdFull id_full, adnl::AdnlAddressList address_list) override {
        td::actor::send_closure(actor_, &ValidatorRegistryWatcherImpl::local_id_added, id_full,
                                std::move(address_list));
      }
      void local_id_updated(adnl::AdnlNodeIdShort id, adnl::AdnlAddressList address_list) override {
        td::actor::send_closure(actor_, &ValidatorRegistryWatcherImpl::local_id_updated, id, std::move(address_list));
      }
      void local_id_deleted(adnl::AdnlNodeIdShort id) override {
        td::actor::send_closure(actor_, &ValidatorRegistryWatcherImpl::local_id_deleted, id);
      }

     private:
      td::actor::ActorId<ValidatorRegistryWatcherImpl> actor_;
    };
    td::actor::send_closure(adnl_, &adnl::Adnl::add_local_id_callback, std::make_unique<Cb>(actor_id(this)));
  }

  void add_validator_key(PublicKeyHash key_hash) override {
    local_validators_.emplace(key_hash, LocalValidator{});
    if (inited_) {
      local_validators_[key_hash].new_entry_cell = make_entry_cell(key_hash);
    }
    update_is_current_validator();
  }

  void del_validator_key(PublicKeyHash key_hash) override {
    local_validators_.erase(key_hash);
    update_is_current_validator();
  }

  void update(Ref<MasterchainState> mc_state, Ref<ValidatorManagerOptions> opts) override;

 private:
  td::actor::ActorId<ValidatorManager> manager_;
  td::actor::ActorId<keyring::Keyring> keyring_;
  td::actor::ActorId<adnl::Adnl> adnl_;

  struct LocalValidator {
    td::Timestamp update_at = td::Timestamp::now();
    Ref<vm::Cell> new_entry_cell = {};
  };
  std::map<PublicKeyHash, LocalValidator> local_validators_;
  td::Timestamp try_cleanup_at_ = td::Timestamp::now();
  bool is_current_validator_ = false;
  bool inited_ = false;

  Ref<MasterchainState> mc_state_;
  RegistryState registry_state_;
  Ref<CollatorsList> collators_list_;

  struct LocalAdnlId {
    td::Timestamp update_at = td::Timestamp::now();
    adnl::AdnlNodeIdFull id_full;
    adnl::AdnlAddressList address_list;
  };
  std::map<adnl::AdnlNodeIdShort, LocalAdnlId> local_adnl_ids_;
  struct StoredAdnlId {
    std::optional<adnl::AdnlNode> node = std::nullopt;
    UnixTime allow_update_at = 0;
  };
  std::map<adnl::AdnlNodeIdShort, StoredAdnlId> stored_adnl_ids_;
  std::map<adnl::AdnlNodeIdShort, UnixTime> adnl_ids_to_gc_;

  void update_is_current_validator() {
    is_current_validator_ = false;
    if (mc_state_.is_null()) {
      return;
    }
    auto val_set = mc_state_->get_total_validator_set(0);
    if (val_set.is_null()) {
      return;
    }
    for (auto& [id, _] : local_validators_) {
      if (val_set->get_validator(id.bits256_value())) {
        is_current_validator_ = true;
        return;
      }
    }
  }

  void update_local_validator(PublicKeyHash key_hash, LocalValidator& validator);
  Ref<vm::Cell> make_entry_cell(PublicKeyHash key_hash);

  void read_adnl_id_diff(vm::Dictionary& old_dict, vm::Dictionary& new_dict);
  void update_local_adnl_id(adnl::AdnlNodeIdShort id, LocalAdnlId& info);

  void local_id_added(adnl::AdnlNodeIdFull id_full, adnl::AdnlAddressList address_list) {
    local_adnl_ids_.emplace(id_full.compute_short_id(),
                            LocalAdnlId{.id_full = id_full, .address_list = std::move(address_list)});
  }

  void local_id_updated(adnl::AdnlNodeIdShort id, adnl::AdnlAddressList address_list) {
    auto it = local_adnl_ids_.find(id);
    if (it == local_adnl_ids_.end()) {
      return;
    }
    it->second.address_list = std::move(address_list);
    it->second.update_at = td::Timestamp::now();
  }

  void local_id_deleted(adnl::AdnlNodeIdShort id) {
    local_adnl_ids_.erase(id);
  }

  td::actor::Task<> sign_and_send_request(StdSmcAddress addr, PublicKeyHash key_hash, Ref<vm::Cell> request_cell);
  td::actor::Task<> send_external_message(StdSmcAddress addr, vm::CellSlice body);
};

void ValidatorRegistryWatcherImpl::update(Ref<MasterchainState> mc_state, Ref<ValidatorManagerOptions> opts) {
  mc_state_ = mc_state;
  auto r_registry_state = RegistryState::fetch(mc_state_);
  RegistryState new_registry_state;
  if (r_registry_state.is_error()) {
    VLOG(validator, WARNING) << "Validator registry: " << r_registry_state.move_as_error();
    new_registry_state = RegistryState{};
  } else {
    new_registry_state = r_registry_state.move_as_ok();
  }
  if (mc_state_->is_key_state() || !inited_) {
    update_is_current_validator();
  }
  read_adnl_id_diff(registry_state_.adnl_ids, new_registry_state.adnl_ids);
  registry_state_ = std::move(new_registry_state);
  inited_ = true;
  if (registry_state_.contract_updated) {
    for (auto& [id, info] : local_adnl_ids_) {
      update_local_adnl_id(id, info);
    }
  }
  if (opts->get_collators_list() != collators_list_ || mc_state_->is_key_state()) {
    collators_list_ = opts->get_collators_list();
    for (auto& [key_hash, validator] : local_validators_) {
      validator.new_entry_cell = make_entry_cell(key_hash);
      validator.update_at = td::Timestamp::now();
    }
  }
  if (local_validators_.empty() || mc_state_->get_unix_time() < (UnixTime)td::Clocks::system() - 60) {
    return;
  }
  for (auto& [id, validator] : local_validators_) {
    update_local_validator(id, validator);
  }

  auto vset = mc_state_->get_total_validator_set(0);
  if (is_current_validator_ && try_cleanup_at_.is_in_past() && vset.not_null()) {
    if (registry_state_.last_cleanup_key_block_seqno < mc_state_->last_key_block_id().seqno() &&
        td::Random::fast(1, (int)vset->size()) == 1) {
      VLOG(validator, INFO) << "Update registry: cleanup";
      vm::CellBuilder cb;
      cb.store_long(block::gen::ValRegistryMessageCleanup::cons_tag[0], 32);
      if (registry_state_.contract_updated) {
        cb.store_long(td::Random::fast_uint64(), 64);
      }
      send_external_message(registry_state_.config.contract_address, cb.as_cellslice())
          .start()
          .detach("send registry cleanup");
    }
    for (auto& [id, remove_at] : adnl_ids_to_gc_) {
      if ((double)remove_at < td::Clocks::system() && td::Random::fast(1, (int)vset->size()) == 1) {
        VLOG(validator, DEBUG) << "Update registry: remove adnl id " << id;
        vm::CellBuilder cb;
        CHECK(block::gen::t_ValRegistryMessageCleanupAdnlId.pack_val_registry_message_cleanup_adnl_id(
            cb, id.bits256_value(), td::Random::fast_uint64()));
        send_external_message(registry_state_.config.contract_address, cb.as_cellslice())
            .start()
            .detach("send adnl id cleanup");
      }
    }
    try_cleanup_at_ = td::Timestamp::in(60.0);
  }
}

void ValidatorRegistryWatcherImpl::update_local_validator(PublicKeyHash key_hash, LocalValidator& validator) {
  if (!validator.update_at || !validator.update_at.is_in_past()) {
    return;
  }
  validator.update_at = td::Timestamp::never();
  bool found = false;
  td::uint32 val_set_idx = 0, val_idx = 0;
  size_t val_set_size = 0;
  td::Bits256 public_key;
  for (int next : {0, 1, -1}) {
    auto val_set = mc_state_->get_total_validator_set(next);
    if (val_set.not_null()) {
      auto [val, idx] = val_set->get_validator_with_idx(key_hash.bits256_value());
      if (val) {
        val_set_idx = 34 + next * 2;
        val_idx = idx;
        val_set_size = val_set->size();
        public_key = val->key.as_bits256();
        found = true;
        break;
      }
    }
  }
  if (!found) {
    return;
  }
  auto r_info = registry_state_.get_validator_info(public_key);
  if (r_info.is_error()) {
    VLOG(validator, ERROR) << "Update registry: failed to get validator info from registry: " << r_info.move_as_error();
    validator.update_at.relax(td::Timestamp::in(3600.0));
    return;
  }
  ValidatorInfo info = r_info.move_as_ok();
  if (cell_equal(validator.new_entry_cell, info.entry)) {
    validator.update_at.relax(td::Timestamp::in(60.0));
    return;
  }
  if (info.allow_update_at > (UnixTime)td::Clocks::system()) {
    auto t = info.allow_update_at - (UnixTime)td::Clocks::system() + 1;
    VLOG(validator, INFO) << "Update registry: need to update entry, wait for allow_update_at (" << t << " s)";
    validator.update_at.relax(td::Timestamp::in(t));
    return;
  }
  if (val_set_idx == 36) {
    CHECK(val_idx < val_set_size);
    Ref<MasterchainStateQ> mc_state{mc_state_};
    auto val_set_start_at = (double)mc_state->get_config()->get_validator_set_start_stop(1).first;
    td::uint32 elections_end_before = get_elections_end_before(mc_state->get_config()->get_config_param(15));
    if (elections_end_before != 0) {
      double window_start = val_set_start_at - (double)elections_end_before * 0.9;
      double window_length = (double)elections_end_before * 0.5;
      td::Timestamp send_at = td::Timestamp::at_unix(window_start + window_length * val_idx / (double)val_set_size);
      if (!send_at.is_in_past()) {
        VLOG(validator, INFO) << "Update registry: need to update entry for the next validator set (" << val_idx << "/"
                              << val_set_size << "), waiting (" << send_at.in() << " s)";
        validator.update_at.relax(send_at);
        return;
      }
    }
  }

  VLOG(validator, INFO) << "Update registry: updating entry";
  vm::CellBuilder cb;
  cb.store_maybe_ref(validator.new_entry_cell);
  block::gen::ValRegistryRequest::Record_val_registry_request_update request{
      .last_key_block_seqno = mc_state_->last_key_block_id().seqno(),
      .val_set = (int)val_set_idx,
      .val_idx = (int)val_idx,
      .valid_until = (UnixTime)td::Clocks::system() + 60,
      .new_entry = cb.as_cellslice_ref()};
  Ref<vm::Cell> request_cell;
  CHECK(block::gen::pack_cell(request_cell, request));
  sign_and_send_request(registry_state_.config.contract_address, key_hash, std::move(request_cell))
      .start()
      .detach("send registry validator request");
  validator.update_at.relax(td::Timestamp::in(60.0));
}

Ref<vm::Cell> ValidatorRegistryWatcherImpl::make_entry_cell(PublicKeyHash key_hash) {
  std::set<adnl::AdnlNodeIdShort> collators;
  collators.insert(collators_list_->register_collators.begin(), collators_list_->register_collators.end());
  if (collators.size() > registry_state_.config.max_collators_per_validator) {
    VLOG(validator, WARNING) << "Make entry cell: too many collators, pruning (max. "
                             << registry_state_.config.max_collators_per_validator << ", found " << collators.size()
                             << ")";
    while (collators.size() > registry_state_.config.max_collators_per_validator) {
      collators.erase(collators.begin());
    }
  }
  VLOG(validator, INFO) << "Make entry cell: " << collators.size() << " collators";
  if (collators.empty() && !registry_state_.contract_updated) {
    return {};
  }
  auto monitoring_shards_all = vm::CellBuilder{}.store_long(0, 4).store_ones(1).as_cellslice_ref();
  vm::Dictionary collators_dict{256};
  for (const adnl::AdnlNodeIdShort& id : collators) {
    vm::CellBuilder cb;
    CHECK(block::gen::t_ValRegistryCollator.pack_val_registry_collator(cb, monitoring_shards_all));
    collators_dict.set_builder(id.bits256_value(), std::move(cb));
  }
  Ref<vm::Cell> result;
  if (registry_state_.contract_updated) {
    td::Bits256 self_adnl_id = td::Bits256::zero();
    for (int next : {0, 1, -1}) {
      auto val_set = mc_state_->get_total_validator_set(next);
      if (val_set.not_null()) {
        auto val = val_set->get_validator(key_hash.bits256_value());
        if (val) {
          self_adnl_id = val->addr.is_zero() ? key_hash.bits256_value() : val->addr;
          break;
        }
      }
    }
    CHECK(block::gen::t_ValRegistryEntry.cell_pack_val_registry_entry(result, collators_dict.get_root(),
                                                                      monitoring_shards_all, self_adnl_id));
  } else {
    CHECK(block::gen::t_ValRegistryEntryOld.cell_pack_val_registry_entry_old(result, collators_dict.get_root(),
                                                                             monitoring_shards_all));
  }
  return result;
}

td::Result<adnl::AdnlNode> parse_adnl_node(adnl::AdnlNodeIdShort id, Ref<vm::Cell> cell) {
  vm::CellSlice cs{vm::NoVm{}, cell};
  TRY_BOOL(cs.size_refs() == 0 && cs.size() % 8 == 0);
  td::BufferSlice buf{cs.size() / 8};
  cs.fetch_bytes(buf.as_slice());
  TRY_RESULT(tl, fetch_tl_object<ton_api::adnl_node>(buf, true));
  TRY_RESULT(node, adnl::AdnlNode::create(std::move(tl)));
  if (node.compute_short_id() != id) {
    return td::Status::Error("wrong node adnl id");
  }
  return std::move(node);
}

void ValidatorRegistryWatcherImpl::read_adnl_id_diff(vm::Dictionary& old_dict, vm::Dictionary& new_dict) {
  try {
    bool ok = old_dict.scan_diff(
        new_dict,
        [&](td::ConstBitPtr key, int key_len, Ref<vm::CellSlice> old_val, Ref<vm::CellSlice> new_val) -> bool {
          CHECK(key_len == 256);
          adnl::AdnlNodeIdShort adnl_id{key};
          if (auto it = local_adnl_ids_.find(adnl_id); it != local_adnl_ids_.end()) {
            it->second.update_at = td::Timestamp::now();
          }
          Ref<vm::Cell> old_cell, new_cell;
          bool old_authorized = false, new_authorized = false;
          if (old_val.not_null()) {
            block::gen::ValRegistryAdnlIdInfo::Record rec;
            if (!block::gen::csr_unpack(old_val, rec)) {
              VLOG(validator, WARNING) << "read_adnl_id_diff error: failed to parse old_val for " << adnl_id;
              return true;
            }
            if (rec.refcnt > 0) {
              old_cell = rec.address_list->prefetch_ref();
              old_authorized = true;
            }
          }
          UnixTime allow_update_at = 0;
          if (new_val.not_null()) {
            block::gen::ValRegistryAdnlIdInfo::Record rec;
            if (!block::gen::csr_unpack(new_val, rec)) {
              VLOG(validator, WARNING) << "read_adnl_id_diff error: failed to parse new_val for " << adnl_id;
              return true;
            }
            if (rec.refcnt > 0) {
              new_cell = rec.address_list->prefetch_ref();
              new_authorized = true;
              allow_update_at = rec.allow_update_at;
              adnl_ids_to_gc_.erase(adnl_id);
            } else {
              adnl_ids_to_gc_[adnl_id] = rec.allow_update_at;
            }
          } else {
            adnl_ids_to_gc_.erase(adnl_id);
          }
          if (!new_authorized) {
            if (stored_adnl_ids_.erase(adnl_id)) {
              VLOG(validator, DEBUG) << "adnl node " << adnl_id << " removed from registry";
              td::actor::send_closure(adnl_, &adnl::Adnl::del_static_peer, adnl_id);
            }
            return true;
          }
          stored_adnl_ids_[adnl_id].allow_update_at = allow_update_at;
          if (old_authorized && cell_equal(old_cell, new_cell)) {
            return true;
          }
          if (new_cell.is_null()) {
            VLOG(validator, DEBUG) << "adnl node " << adnl_id << " has empty entry in registry";
            stored_adnl_ids_[adnl_id].node = std::nullopt;
            td::actor::send_closure(adnl_, &adnl::Adnl::del_static_peer, adnl_id);
            return true;
          }
          auto r_node = parse_adnl_node(adnl_id, new_cell);
          if (r_node.is_error()) {
            VLOG(validator, DEBUG) << "adnl node " << adnl_id << " in registry is invalid: " << r_node.move_as_error();
            stored_adnl_ids_[adnl_id].node = std::nullopt;
            td::actor::send_closure(adnl_, &adnl::Adnl::del_static_peer, adnl_id);
            return true;
          }
          VLOG(validator, DEBUG) << "updated adnl node " << adnl_id << " in registry";
          auto node = r_node.move_as_ok();
          stored_adnl_ids_[adnl_id].node.emplace(node);
          td::actor::send_closure(adnl_, &adnl::Adnl::add_static_peer, std::move(node));
          return true;
        });
    if (!ok) {
      VLOG(validator, WARNING) << "read_adnl_id_diff dict error";
    }
  } catch (vm::VmError& e) {
    VLOG(validator, WARNING) << "read_adnl_id_diff dict error: " << e.get_msg();
  }
}

void ValidatorRegistryWatcherImpl::update_local_adnl_id(adnl::AdnlNodeIdShort id, LocalAdnlId& info) {
  if (!info.update_at || !info.update_at.is_in_past()) {
    return;
  }
  info.update_at = td::Timestamp::never();
  if (!stored_adnl_ids_.contains(id)) {
    return;
  }
  auto& stored_node = stored_adnl_ids_[id];
  bool need_update = false;
  if (stored_node.node.has_value()) {
    auto& stored_address_list = stored_node.node->addr_list();
    if (stored_address_list.size() != info.address_list.size()) {
      need_update = true;
    } else if (stored_address_list.quic_addrs() != info.address_list.quic_addrs()) {
      need_update = true;
    } else if (stored_address_list.has_reverse() != info.address_list.has_reverse()) {
      need_update = true;
    } else {
      for (td::uint32 i = 0; i < stored_address_list.size(); i++) {
        auto addr1 = stored_address_list.adnl_addrs()[i]->to_ip_address();
        auto addr2 = info.address_list.adnl_addrs()[i]->to_ip_address();
        if (addr1.is_error() || addr2.is_error()) {
          // Unexpected situation - nodes only use valid ipv4 addresses
          need_update = (addr1.is_error() != addr2.is_error());
          continue;
        }
        if (addr1.ok() != addr2.ok()) {
          need_update = true;
        }
      }
    }
  } else {
    need_update = true;
  }
  if (!need_update) {
    info.update_at = td::Timestamp::in(600.0);
    return;
  }
  if (stored_node.allow_update_at > (UnixTime)td::Clocks::system()) {
    auto t = stored_node.allow_update_at - (UnixTime)td::Clocks::system() + 1;
    VLOG(validator, INFO) << "Update adnl id " << id << " : need to update, wait for allow_update_at (" << t << " s)";
    info.update_at = td::Timestamp::in(t);
    return;
  }
  auto tl = serialize_tl_object(adnl::AdnlNode{info.id_full, info.address_list}.tl(), true);
  if (tl.size() > 127) {
    VLOG(validator, INFO) << "Update adnl id " << id << " : cannot store adnl node - serialized size too big ("
                          << tl.size() << ")";
    return;
  }
  VLOG(validator, INFO) << "Update adnl id " << id << " : updating";
  Ref<vm::Cell> node_cell = vm::CellBuilder{}.store_bytes(tl).finalize_novm();
  block::gen::ValRegistryRequest::Record_val_registry_request_update_adnl_id request{
      .adnl_id_full = info.id_full.pubkey().ed25519_value().raw(),
      .valid_until = (UnixTime)td::Clocks::system() + 60,
      .new_adnl_node = vm::CellBuilder{}.store_ref(node_cell).store_ones(1).as_cellslice_ref()};
  Ref<vm::Cell> request_cell;
  CHECK(block::gen::pack_cell(request_cell, request));
  sign_and_send_request(registry_state_.config.contract_address, id.pubkey_hash(), std::move(request_cell))
      .start()
      .detach("send registry adnl id request");
  info.update_at.relax(td::Timestamp::in(60.0));
}

td::actor::Task<> ValidatorRegistryWatcherImpl::sign_and_send_request(StdSmcAddress addr, PublicKeyHash key_hash,
                                                                      Ref<vm::Cell> request_cell) {
  td::BufferSlice to_sign{request_cell->get_hash().as_slice()};
  td::BufferSlice signature =
      co_await td::actor::ask(keyring_, &keyring::Keyring::sign_message, key_hash, std::move(to_sign));
  auto body = vm::CellBuilder{}
                  .store_long(block::gen::ValRegistryMessageRequest::cons_tag[0], 32)
                  .store_bytes(signature)
                  .append_cellslice(vm::load_cell_slice(request_cell))
                  .as_cellslice();
  co_await send_external_message(addr, std::move(body));
  co_return {};
}

td::actor::Task<> ValidatorRegistryWatcherImpl::send_external_message(StdSmcAddress addr, vm::CellSlice body) {
  block::gen::CommonMsgInfo::Record_ext_in_msg_info info;
  info.src = vm::CellBuilder{}.store_zeroes(2).as_cellslice_ref();  // addr_none$00
  info.dest = block::tlb::t_MsgAddressInt.pack_std_address(masterchainId, addr);
  CHECK(block::tlb::t_Grams.pack_integer(info.import_fee, td::zero_refint()));
  vm::CellBuilder cb;
  CHECK(block::gen::pack(cb, info));
  cb.store_zeroes(1);  // State init
  if (cb.can_extend_by(body.size() + 1, body.size_refs())) {
    cb.store_zeroes(1);
    cb.append_cellslice(body);
  } else {
    cb.store_ones(1);
    cb.store_ref(vm::CellBuilder{}.append_cellslice(body).finalize_novm());
  }
  td::BufferSlice data = vm::std_boc_serialize(cb.finalize_novm()).ensure().move_as_ok();
  co_await td::actor::ask(manager_, &ValidatorManager::new_external_message_query, std::move(data));
  co_return {};
}

}  // namespace

std::set<adnl::AdnlNodeIdShort> ValidatorRegistryWatcher::get_all_collators(Ref<MasterchainState> mc_state) {
  auto r_registry_state = RegistryState::fetch(mc_state);
  if (r_registry_state.is_error()) {
    VLOG(validator, DEBUG) << "Get collators from validator registry: " << r_registry_state.move_as_error();
    return {};
  }
  auto registry_state = r_registry_state.move_as_ok();

  std::set<PublicKeyHash> all_validator_keys;
  for (int next : {-1, 0, 1}) {
    auto val_set = mc_state->get_total_validator_set(next);
    if (val_set.not_null()) {
      for (auto& val : val_set->export_vector()) {
        PublicKeyHash key = PublicKey{pubkeys::Ed25519{val.key}}.compute_short_id();
        all_validator_keys.insert(key);
        if (!val.addr.is_zero()) {
          all_validator_keys.emplace(val.addr);
        }
      }
    }
  }

  std::set<adnl::AdnlNodeIdShort> all_collators;
  std::set<PublicKeyHash> visited;
  for (int next : {-1, 0, 1}) {
    auto val_set = mc_state->get_total_validator_set(next);
    if (val_set.not_null()) {
      for (auto& val : val_set->export_vector()) {
        PublicKeyHash key = PublicKey{pubkeys::Ed25519{val.key}}.compute_short_id();
        if (!visited.insert(key).second) {
          continue;
        }
        auto r_entry = registry_state.get_validator_entry(val.key.as_bits256());
        if (r_entry.is_error()) {
          VLOG(validator, WARNING) << "Get collators from validator registry: failed to get entry for pubkey "
                                   << val.key.as_bits256().to_hex() << ": " << r_entry.move_as_error();
          continue;
        }
        auto entry = r_entry.move_as_ok();
        for (const adnl::AdnlNodeIdShort& collator_id : entry.collators) {
          if (!all_validator_keys.contains(collator_id.pubkey_hash())) {
            all_collators.insert(collator_id);
          }
        }
      }
    }
  }
  return all_collators;
}

td::actor::ActorOwn<ValidatorRegistryWatcher> ValidatorRegistryWatcher::create(
    td::actor::ActorId<ValidatorManager> manager, td::actor::ActorId<keyring::Keyring> keyring,
    td::actor::ActorId<adnl::Adnl> adnl) {
  return td::actor::create_actor<ValidatorRegistryWatcherImpl>("ValidatorRegistry", std::move(manager),
                                                               std::move(keyring), std::move(adnl));
}

}  // namespace ton::validator
