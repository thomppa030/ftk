#include "net/spawn_replicator.hpp"

#include "net/packet_codec.hpp"
#include "net/net_scene.hpp"
#include "net/transport.hpp"
#include "ftk/base/log.hpp"
#include "scene/scene.hpp"
#include "scene/preset_asset.hpp"
#include "renderer/resources/resource_manager.hpp"

namespace fjell {

using netcodec::put;
using netcodec::put_string;
using netcodec::take;
using netcodec::take_string;

std::vector<uint8_t> SpawnReplicator::build_spawn_packet(NetId net_id) const {
    std::vector<uint8_t> packet;
    auto it = spawn_info_.find(net_id);
    if (it == spawn_info_.end()) return packet;
    const SpawnInfo& info = it->second;
    put(packet, static_cast<uint8_t>(PacketType::spawn));
    put(packet, net_id);
    put(packet, info.owner);
    put_string(packet, info.preset_path);
    put(packet, info.position);
    return packet;
}

NetId SpawnReplicator::spawn(Scene& scene, ResourceManager& resources,
                             NetScene& net_scene, Transport* transport,
                             const std::string& preset_path,
                             const glm::vec3& position, PeerId owner,
                             const Project* project) {
    auto handle = resources.load_preset_asset(preset_path, project);
    if (!handle) {
        FTK_CORE_ERROR("NetManager: spawn failed, can't load preset '{}'", preset_path);
        return NET_ID_NONE;
    }
    PresetAsset* asset = resources.get(*handle);
    if (!asset) return NET_ID_NONE;

    Transform offset;
    offset.position = position;
    uint32_t node_id = asset->preset.instantiate(scene, -1, offset);
    if (node_id == 0) return NET_ID_NONE;

    NetId net_id = net_scene.assign_net_id();
    net_scene.register_object(net_id, node_id);
    net_scene.set_owner(net_id, owner);
    spawn_info_[net_id] = {preset_path, owner, position};

    // Tell every connected client to instantiate the same preset.
    if (transport) {
        auto packet = build_spawn_packet(net_id);
        transport->broadcast(packet, Channel::reliable);
    }

    FTK_CORE_INFO("NetManager: spawned '{}' as net {} (owner {})",
                    preset_path, net_id, owner);
    return net_id;
}

NetId SpawnReplicator::destroy(Scene& scene, NetScene& net_scene,
                               Transport* transport, NetId net_id) {
    uint32_t node_id = net_scene.node_id_for(net_id);
    if (node_id != 0) scene.queue_destroy(node_id);

    std::vector<uint8_t> packet;
    put(packet, static_cast<uint8_t>(PacketType::despawn));
    put(packet, net_id);
    if (transport) transport->broadcast(packet, Channel::reliable);

    FTK_CORE_INFO("NetManager: despawned net {}", net_id);
    return net_id;
}

void SpawnReplicator::apply_spawn(Scene& scene, ResourceManager& resources,
                                  NetScene& net_scene, const Project* project,
                                  std::span<const uint8_t> data) {
    NetId net_id = NET_ID_NONE;
    PeerId owner = SERVER_PEER;
    std::string preset_path;
    glm::vec3 position{};
    if (!take(data, net_id)) return;
    if (!take(data, owner)) return;
    if (!take_string(data, preset_path)) return;
    if (!take(data, position)) return;

    // Already have it (duplicate/reordered reliable packet)? Ignore.
    if (net_scene.node_id_for(net_id) != 0) return;

    auto handle = resources.load_preset_asset(preset_path, project);
    if (!handle) {
        FTK_CORE_ERROR("NetManager: spawn of net {} failed, can't load '{}'",
                         net_id, preset_path);
        return;
    }
    PresetAsset* asset = resources.get(*handle);
    if (!asset) return;

    Transform offset;
    offset.position = position;
    uint32_t node_id = asset->preset.instantiate(scene, -1, offset);
    if (node_id == 0) return;

    net_scene.register_object(net_id, node_id);
    net_scene.set_owner(net_id, owner);
    FTK_CORE_INFO("NetManager: spawned net {} from '{}' (owner {})", net_id, preset_path, owner);
}

NetId SpawnReplicator::apply_despawn(Scene& scene, NetScene& net_scene,
                                     std::span<const uint8_t> data) {
    NetId net_id = NET_ID_NONE;
    if (!take(data, net_id)) return NET_ID_NONE;

    uint32_t node_id = net_scene.node_id_for(net_id);
    if (node_id != 0) scene.queue_destroy(node_id);
    FTK_CORE_INFO("NetManager: despawned net {}", net_id);
    return net_id;
}

void SpawnReplicator::replay_to(Transport& transport, PeerId peer) const {
    // Replay every already-spawned networked object so the late-joiner catches up
    // on objects created before it connected (e.g. the host's own player).
    for (const auto& [net_id, info] : spawn_info_) {
        (void)info;
        auto packet = build_spawn_packet(net_id);
        if (!packet.empty()) transport.send(peer, packet, Channel::reliable);
    }
}

} // namespace fjell
