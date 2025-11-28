#include <map>
#include <vector>

#include "adnl/adnl-node-id.hpp"
#include "auto/tl/ton_api.h"
#include "overlay/overlays.h"
#include "td/utils/Status.h"
#include "td/utils/logging.h"
#include "validator-session/candidate-serializer.h"

#include "checksum.h"
#include "consensus-bus.h"

namespace ton::validator {

namespace {

class PrivateOverlayImpl : public runtime::SpawnsWith<ConsensusBus>, public runtime::ConnectsTo<ConsensusBus> {
 public:
  TON_RUNTIME_DEFINE_EVENT_HANDLER();

  void start_up() {
    auto& bus = *owning_bus();
    overlays_ = bus.overlays;

    std::vector<adnl::AdnlNodeIdShort> overlay_nodes;
    std::vector<td::Bits256> overlay_nodes_tl;
    std::map<PublicKeyHash, td::uint32> authorized_keys;

    for (auto const& el : bus.validator_set->export_vector()) {
      PublicKey key{pubkeys::Ed25519{el.key}};
      PublicKeyHash key_hash = key.compute_short_id();
      adnl::AdnlNodeIdShort adnl_id{key_hash};

      if (!el.addr.is_zero()) {
        LOG(WARNING) << "Ignoring non-zero ADNL address override " << el.addr << " for validator " << key_hash;
      }

      adnl_id_to_key_[adnl_id] = el.key;
      overlay_nodes.push_back(adnl_id);
      overlay_nodes_tl.push_back(key_hash.bits256_value());
      authorized_keys.emplace(key_hash, overlay::Overlays::max_fec_broadcast_size());
    }

    auto overlay_seed = create_tl_object<ton_api::catchain_firstblock>(bus.session_id, std::move(overlay_nodes_tl));
    auto overlay_full_id = overlay::OverlayIdFull{serialize_tl_object(overlay_seed, true)};
    overlay_id_ = overlay_full_id.compute_short_id();

    overlay::OverlayOptions options;
    options.broadcast_speed_multiplier_ = bus.config.catchain_opts.broadcast_speed_multiplier;
    options.private_ping_peers_ = true;

    local_id_ = adnl::AdnlNodeIdShort{bus.local_id};

    td::actor::send_closure(overlays_, &overlay::Overlays::create_private_overlay_ex, local_id_,
                            std::move(overlay_full_id), std::move(overlay_nodes), make_callback(),
                            overlay::OverlayPrivacyRules{0, 0, std::move(authorized_keys)}, R"({ "type": "catchain" })",
                            std::move(options));
  }

  template <>
  void handle(runtime::BusHandle<ConsensusBus>, std::shared_ptr<ConsensusBus::StopRequested const>) {
    td::actor::send_closure(overlays_, &overlay::Overlays::delete_overlay, local_id_, overlay_id_);
    stop();
  }

  template <>
  void handle(runtime::BusHandle<ConsensusBus>, std::shared_ptr<ConsensusBus::OutgoingProtocolMessage const> message) {
    auto send_to_peer = [&](adnl::AdnlNodeIdShort recipient) {
      if (recipient == local_id_) {
        return;
      }
      td::actor::send_closure(overlays_, &overlay::Overlays::send_message, recipient, local_id_, overlay_id_,
                              message->message.data.clone());
    };

    if (message->recipient) {
      send_to_peer(adnl::AdnlNodeIdShort{*message->recipient});
      return;
    } else {
      owning_bus().publish(std::make_shared<ConsensusBus::IncomingProtocolMessage>(
          local_id_.pubkey_hash(), ProtocolMessage{message->message.data.clone()}));

      for (auto [node, _] : adnl_id_to_key_) {
        send_to_peer(node);
      }
    }
  }

  template <>
  void handle(runtime::BusHandle<ConsensusBus>, std::shared_ptr<ConsensusBus::CandidateGenerated const> event) {
    auto& bus = *owning_bus();

    auto& candidate = *event->candidate;
    auto candidate_tl = create_tl_object<ton_api::validatorSession_candidate>(
        td::Bits256{}, candidate.id.id.seqno, candidate.id.root_hash, candidate.data.clone(),
        candidate.collated_data.clone());
    auto serialized_candidate = validatorsession::serialize_candidate(candidate_tl, true).move_as_ok();

    td::actor::send_closure(overlays_, &overlay::Overlays::send_broadcast_fec_ex, local_id_, overlay_id_, bus.local_id,
                            0, std::move(serialized_candidate));
  }

 private:
  std::unique_ptr<overlay::Overlays::Callback> make_callback() {
    class Callback final : public overlay::Overlays::Callback {
     public:
      explicit Callback(td::actor::ActorId<PrivateOverlayImpl> owner) : owner_(owner) {
      }

      void receive_message(adnl::AdnlNodeIdShort src, overlay::OverlayIdShort, td::BufferSlice data) override {
        td::actor::send_closure(owner_, &PrivateOverlayImpl::on_overlay_message, src, std::move(data));
      }

      void receive_query(adnl::AdnlNodeIdShort, overlay::OverlayIdShort, td::BufferSlice,
                         td::Promise<td::BufferSlice> promise) override {
        promise.set_error(td::Status::Error("Queries are not supported"));
      }

      void receive_broadcast(PublicKeyHash src, overlay::OverlayIdShort, td::BufferSlice data) override {
        td::actor::send_closure(owner_, &PrivateOverlayImpl::on_overlay_broadcast, src, std::move(data));
      }

      void check_broadcast(PublicKeyHash, overlay::OverlayIdShort, td::BufferSlice,
                           td::Promise<td::Unit> promise) override {
        promise.set_value(td::Unit());
      }

     private:
      td::actor::ActorId<PrivateOverlayImpl> owner_;
    };

    return std::make_unique<Callback>(actor_id(this));
  }

  void on_overlay_message(adnl::AdnlNodeIdShort src, td::BufferSlice data) {
    if (data.size() > ProtocolMessage::max_length) {
      LOG(WARNING) << "Dropping oversized protocol message of size " << data.size() << " from peer " << src;
      return;
    }

    owning_bus().publish(
        std::make_shared<ConsensusBus::IncomingProtocolMessage>(src.pubkey_hash(), ProtocolMessage(std::move(data))));
  }

  void on_overlay_broadcast(PublicKeyHash src, td::BufferSlice data) {
    if (src == local_id_.pubkey_hash()) {
      return;
    }

    auto validator_pubkey = adnl_id_to_key_[adnl::AdnlNodeIdShort{src}];

    auto& bus = *owning_bus();

    auto maybe_candidate = validatorsession::deserialize_candidate(
        data, true, bus.config.max_block_size + bus.config.max_collated_data_size + 1024, bus.config.proto_version);
    if (maybe_candidate.is_error()) {
      LOG(WARNING) << "Failed to deserialize block candidate broadcast: " << maybe_candidate.move_as_error();
      return;
    }

    auto candidate = maybe_candidate.move_as_ok();

    if (candidate->data_.size() > bus.config.max_block_size ||
        candidate->collated_data_.size() > bus.config.max_collated_data_size) {
      LOG(WARNING) << "Dropping too big candidate broadcast size=" << candidate->data_.size() << " "
                   << candidate->collated_data_.size();
      return;
    }

    BlockIdExt block_id{BlockId{bus.shard, static_cast<BlockSeqno>(candidate->round_)}, candidate->root_hash_,
                        td::sha256_bits256(candidate->data_.as_slice())};

    auto parsed_candidate = std::make_shared<BlockCandidate>(
        validator_pubkey, block_id, td::sha256_bits256(candidate->collated_data_.as_slice()),
        std::move(candidate->data_), std::move(candidate->collated_data_));

    auto event = std::make_shared<ConsensusBus::CandidateReceived>(std::move(parsed_candidate));
    owning_bus().publish(std::move(event));
  }

  td::actor::ActorId<overlay::Overlays> overlays_;
  overlay::OverlayIdShort overlay_id_;
  adnl::AdnlNodeIdShort local_id_;
  std::map<adnl::AdnlNodeIdShort, Ed25519_PublicKey> adnl_id_to_key_;
};

}  // namespace

void PrivateOverlay::register_in(runtime::Runtime& runtime) {
  runtime.register_actor<PrivateOverlayImpl>("PrivateOverlay");
}

}  // namespace ton::validator
