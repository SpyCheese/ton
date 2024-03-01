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
#include "adnl-garlic-manager.hpp"
#include "td/utils/Random.h"
#include "adnl/adnl-address-list.hpp"
#include "auto/tl/ton_api.hpp"

namespace ton {

namespace adnl {

AdnlGarlicManager::AdnlGarlicManager(AdnlNodeIdShort local_id, td::uint8 adnl_cat,
                                     td::actor::ActorId<Adnl> adnl,
                                     td::actor::ActorId<keyring::Keyring> keyring,
                                     td::actor::ActorId<overlay::Overlays> overlays,
                                     AdnlGarlicConfig config)
    : local_id_(local_id)
    , adnl_cat_(adnl_cat)
    , adnl_(std::move(adnl))
    , keyring_(std::move(keyring))
    , overlays_(std::move(overlays))
    , config_(std::move(config)) {
}

void AdnlGarlicManager::start_up() {
  create_connection_at_ = td::Timestamp::in(config_.start_delay);

  // Creating DHT node
  if (config_.use_secret_dht) {
    CHECK(config_.dht_config != nullptr);
    PrivateKey pk(privkeys::Ed25519::random());
    PublicKey pub = pk.compute_public_key();
    td::actor::send_closure(keyring_, &keyring::Keyring::add_key, pk, true, [](td::Unit) {});
    AdnlNodeIdFull dht_id(pub);
    create_secret_id(dht_id, [](td::Result<td::Unit>) {});
    auto R = dht::Dht::create_client(dht_id.compute_short_id(), "", config_.dht_config, keyring_, adnl_);
    if (R.is_error()) {
      LOG(ERROR) << "Failed to create secret DHT node: " << R.move_as_error();
      return;
    }
    secret_dht_node_ = R.move_as_ok();
    for (const auto& id : secret_ids_) {
      td::actor::send_closure(adnl_, &Adnl::set_custom_dht_node, id.first, secret_dht_node_.get());
    }
  }

  td::Bits256 X = create_hash_tl_object<ton_api::adnl_garlic_publicOverlayId>();
  td::BufferSlice b{32};
  b.as_slice().copy_from(as_slice(X));
  overlay_id_full_ = overlay::OverlayIdFull{std::move(b)};
  overlay_id_ = overlay_id_full_.compute_short_id();
  overlay::OverlayOptions opts;
  opts.announce_self_ = false;
  td::actor::send_closure(overlays_, &overlay::Overlays::create_public_overlay_ex, local_id_, overlay_id_full_.clone(),
                          std::make_unique<overlay::Overlays::EmptyCallback>(), overlay::OverlayPrivacyRules{},
                          R"({ "type": "garlic" })", opts);
  alarm();
}

void AdnlGarlicManager::tear_down() {
  td::actor::send_closure(overlays_, &overlay::Overlays::delete_overlay, local_id_, overlay_id_);
}

void AdnlGarlicManager::send_packet(AdnlNodeIdShort src, td::IPAddress dst_ip, td::BufferSlice data) {
  if (connection_.empty()) {
    LOG(DEBUG) << "Failed to send packet: connection is not ready";
    return;
  }
  td::actor::send_closure(connection_, &Connection::send_packet, src, dst_ip, std::move(data));
}

void AdnlGarlicManager::create_secret_id(AdnlNodeIdFull id, td::Promise<td::Unit> promise) {
  AdnlNodeIdShort id_short = id.compute_short_id();
  if (secret_ids_.count(id_short)) {
    promise.set_error(td::Status::Error("Duplicate secret id"));
    return;
  }
  secret_ids_[id_short].id_full = id;
  td::actor::send_closure(adnl_, &Adnl::add_id_ex, std::move(id), addr_list_, adnl_cat_, local_id_mode());
  if (config_.use_secret_dht && !secret_dht_node_.empty()) {
    td::actor::send_closure(adnl_, &Adnl::set_custom_dht_node, id_short, secret_dht_node_.get());
  }
  promise.set_result(td::Unit());
}

void AdnlGarlicManager::create_secret_id_short(AdnlNodeIdShort id, td::Promise<td::Unit> promise) {
  td::actor::send_closure(keyring_, &keyring::Keyring::get_public_key, id.pubkey_hash(),
                          [SelfId = actor_id(this), promise = std::move(promise)](td::Result<PublicKey> R) mutable {
                            if (R.is_error()) {
                              promise.set_error(R.move_as_error());
                            } else {
                              td::actor::send_closure(SelfId, &AdnlGarlicManager::create_secret_id,
                                                      AdnlNodeIdFull(R.move_as_ok()), std::move(promise));
                            }
                          });
}

void AdnlGarlicManager::alarm() {
  td::actor::send_closure(
      overlays_, &overlay::Overlays::get_overlay_random_peers_full, local_id_, overlay_id_, 8,
      [SelfId = actor_id(this)](td::Result<std::vector<std::pair<AdnlNodeIdFull, td::uint32>>> R) {
        if (R.is_ok()) {
          td::actor::send_closure(SelfId, &AdnlGarlicManager::got_servers_from_overlay, R.move_as_ok());
        } else {
          LOG(WARNING) << "Failed to get peers: " << R.move_as_error();
          td::actor::send_closure(SelfId, &AdnlGarlicManager::got_servers_from_overlay,
                                  std::vector<std::pair<AdnlNodeIdFull, td::uint32>>());
        }
      });
}

void AdnlGarlicManager::got_servers_from_overlay(std::vector<std::pair<AdnlNodeIdFull, td::uint32>> servers) {
  for (auto& p : servers) {
    AdnlNodeIdShort id = p.first.compute_short_id();
    auto& server = servers_[id];
    if (server.id_full.empty()) {
      LOG(DEBUG) << "Adding server " << id;
      server.id_full = std::move(p.first);
    }
    server.version = std::max(server.version, p.second);
  }
  try_create_connection();
  alarm_timestamp() = td::Timestamp::in(td::Random::fast(1.0, 2.0));
}

void AdnlGarlicManager::try_create_connection() {
  if (!connection_.empty() || !create_connection_at_.is_in_past()) {
    return;
  }
  size_t chain_size = config_.chain_length;
  std::vector<AdnlNodeIdFull> servers;
  for (const auto& p : servers_) {
    const auto& s = p.second;
    if (s.version > s.ignore_until && !s.id_full.empty()) {
      servers.push_back(s.id_full);
    }
  }
  if (servers.size() < chain_size) {
    LOG(DEBUG) << "Too few servers (" << servers.size() << ")";
    return;
  }
  for (size_t i = 0; i < chain_size; ++i) {
    std::swap(servers[i], servers[i + td::Random::secure_uint32() % (servers.size() - i)]);
  }
  servers.resize(chain_size);

  class Callback : public Connection::Callback {
   public:
    Callback(td::actor::ActorId<AdnlGarlicManager> id) : id_(std::move(id)) {
    }
    void on_ready(AdnlAddressList addr_list) override {
      td::actor::send_closure(id_, &AdnlGarlicManager::update_addr_list, std::move(addr_list));
    }
    void on_fail(AdnlNodeIdShort causer = AdnlNodeIdShort::zero()) override {
      td::actor::send_closure(id_, &AdnlGarlicManager::on_connection_fail, causer);
    }

   private:
    td::actor::ActorId<AdnlGarlicManager> id_;
  };
  connection_ = td::actor::create_actor<Connection>("adnlgarlicconn", local_id_, std::move(servers),
                                                    std::make_unique<Callback>(actor_id(this)), adnl_cat_, adnl_,
                                                    keyring_);
}

void AdnlGarlicManager::update_addr_list(AdnlAddressList addr_list) {
  for (const auto& p : secret_ids_) {
    td::actor::send_closure(adnl_, &Adnl::add_id_ex, p.second.id_full, addr_list, adnl_cat_, local_id_mode());
  }
  addr_list_ = std::move(addr_list);
}

void AdnlGarlicManager::on_connection_fail(AdnlNodeIdShort causer) {
  connection_.reset();
  servers_[causer].ignore_until = (td::uint32)td::Clocks::system() + 120;
  try_create_connection();
}

}  // namespace adnl

}  // namespace ton
