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
#include "storage-stat-cache.hpp"

namespace ton::validator {

void StorageStatCache::get_cache(td::Promise<std::function<td::optional<CacheEntry>(const block::Account&)>> promise) {
  LOG(DEBUG) << "StorageStatCache::get_cache";
  promise.set_value([cache = cache_](const block::Account& account) mutable -> td::optional<CacheEntry> {
    if (account.storage.is_null() || account.storage_used.cells < MIN_ACCOUNT_CELLS) {
      return {};
    }
    if (!account.storage_dict_hash && account.balance.has_extra()) {
      // Cannot use cache, as we don't know if "storage used" includes extra currencies (account wasn't updated for a long time) or not (new version)
      return {};
    }
    vm::CellBuilder cb;
    CHECK(cb.store_maybe_ref(account.code));
    CHECK(cb.store_maybe_ref(account.data));
    CHECK(cb.store_maybe_ref(account.library));
    td::Bits256 key = cb.finalize()->get_hash().bits();
    td::Ref<vm::Cell> dict_root = cache.lookup_ref(key);
    if (dict_root.is_null()) {
      return {};
    }
    return CacheEntry{key, dict_root, account.storage_used.cells};
  });
}

void StorageStatCache::update(std::vector<CacheEntry> data) {
  for (auto& e : data) {
    if (e.dict_root.is_null()) {
      continue;
    }
    LOG(DEBUG) << "StorageStatCache::update " << e.dict_root->get_hash().to_hex() << " " << e.size_cells;
    cache_.set_ref(e.key, e.dict_root);
    lru_.put(e.key, Deleter{e.key, &cache_}, true, e.size_cells);
  }
}

td::optional<StorageStatCache::CacheEntry> StorageStatCache::account_state_to_update(const block::Account& account) {
  if (account.storage.is_null() || account.storage_used.cells < MIN_ACCOUNT_CELLS) {
    return {};
  }
  if (!account.storage_dict_hash && account.balance.has_extra()) {
    // Cannot update cache, as we don't know if "storage used" includes extra currencies (account wasn't updated for a long time) or not (new version)
    return {};
  }
  if (!account.account_storage_stat) {
    // account_storage_stat is empty when there were no storage stat updates in the block
    return {};
  }
  vm::CellBuilder cb;
  CHECK(cb.store_maybe_ref(account.code));
  CHECK(cb.store_maybe_ref(account.data));
  CHECK(cb.store_maybe_ref(account.library));
  td::Bits256 key = cb.finalize()->get_hash().bits();

  block::AccountStorageStat stat{&account.account_storage_stat.value()};
  td::Ref<vm::Cell> dict_root;
  td::Status S = [&]() -> td::Status {
    TRY_STATUS(stat.replace_roots({account.code, account.data, account.library}));
    TRY_RESULT_ASSIGN(dict_root, stat.get_dict_root());
    return td::Status::OK();
  }();
  if (S.is_error()) {
    LOG(WARNING) << "Failed to calculate storage stat cache entry for account " << account.workchain << ":"
                 << account.addr.to_hex() << " : " << S;
    return {};
  }
  return CacheEntry{.key = key, .dict_root = dict_root, .size_cells = account.storage_used.cells};
}

}  // namespace ton::validator
