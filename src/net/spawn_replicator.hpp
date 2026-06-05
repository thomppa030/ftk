// spawn_replicator.hpp — Server-authoritative spawn/despawn replication.
//
// The server instantiates a preset locally, assigns a NetId + owner via NetScene,
// records how it was spawned (for late-join replay), and broadcasts a spawn packet.
// Clients apply spawn/despawn packets to mirror the server's object set. A
// late-joining peer is caught up by replaying the recorded spawns.
#pragma once

#include "net/net_types.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace fjell {

class Scene;
class ResourceManager;
class Project;
class Transport;
class NetScene;

class SpawnReplicator {
public:
    // Server: instantiate the preset, register it in NetScene, record spawn info,
    // and broadcast the spawn. Returns the new NetId (NET_ID_NONE on failure).
    NetId spawn(Scene& scene, ResourceManager& resources, NetScene& net_scene,
                Transport* transport, const std::string& preset_path,
                const glm::vec3& position, PeerId owner, const Project* project);

    // Server: destroy locally + tell clients. Returns the net_id so the caller can
    // run cross-component cleanup (NetScene/prediction/shadow); NET_ID_NONE if off
    // the server.
    NetId destroy(Scene& scene, NetScene& net_scene, Transport* transport,
                  NetId net_id);

    // Client: apply a received spawn / despawn packet. apply_despawn returns the
    // despawned net_id (or NET_ID_NONE) so the caller can run cross-component cleanup.
    void  apply_spawn(Scene& scene, ResourceManager& resources, NetScene& net_scene,
                      const Project* project, std::span<const uint8_t> data);
    NetId apply_despawn(Scene& scene, NetScene& net_scene, std::span<const uint8_t> data);

    // Server: send every recorded spawn to a freshly-connected peer (late-join).
    void replay_to(Transport& transport, PeerId peer) const;

    void erase(NetId net_id) { spawn_info_.erase(net_id); }
    void clear() { spawn_info_.clear(); }

private:
    // Server: remembers how each networked object was spawned, so a late-joining
    // client can be brought up to date (replay spawns it missed). Spawn position
    // is the initial one; the subsequent state_update stream carries live motion.
    struct SpawnInfo {
        std::string preset_path;
        PeerId      owner{SERVER_PEER};
        glm::vec3   position{0.0f};
    };
    std::unordered_map<NetId, SpawnInfo> spawn_info_;

    [[nodiscard]] std::vector<uint8_t> build_spawn_packet(NetId net_id) const;
};

} // namespace fjell
