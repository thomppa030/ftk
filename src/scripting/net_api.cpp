// net_api.cpp — Engine-side implementation of the fjell_net_* script exports.
#include "fjell/api.h"
#include "scripting/script_globals.hpp"
#include "net/net_manager.hpp"
#include "renderer/resources/resource_manager.hpp"

#include <glm/glm.hpp>

extern "C" FJELL_SDK_EXPORT int fjell_net_role() {
    auto* net = fjell::script_services().net;
    return net ? static_cast<int>(net->role()) : 0;
}

extern "C" FJELL_SDK_EXPORT uint32_t fjell_net_local_peer() {
    auto* net = fjell::script_services().net;
    return net ? net->local_peer() : 0;
}

extern "C" FJELL_SDK_EXPORT int fjell_net_is_local_authority(uint32_t node_id) {
    auto* net = fjell::script_services().net;
    if (!net) return 1;  // single-player: always authoritative
    fjell::NetId net_id = net->net_id_for(node_id);
    return net->is_local_authority(net_id) ? 1 : 0;
}

extern "C" FJELL_SDK_EXPORT void fjell_net_host(uint16_t port, uint32_t max_clients) {
    auto* net = fjell::script_services().net;
    if (net) net->host(port, max_clients);
}

extern "C" FJELL_SDK_EXPORT void fjell_net_join(const char* address, uint16_t port) {
    auto* net = fjell::script_services().net;
    if (net && address) net->join(address, port);
}

extern "C" FJELL_SDK_EXPORT void fjell_net_disconnect() {
    auto* net = fjell::script_services().net;
    if (net) net->shutdown();
}

extern "C" FJELL_SDK_EXPORT uint32_t fjell_net_spawn(uint32_t preset_handle_id,
                                                     float x, float y, float z,
                                                     uint32_t owner_peer) {
    auto& s = fjell::script_services();
    if (!s.net || !s.scene || !s.resources) return 0;
    std::string path = s.resources->asset_path_of(fjell::PresetAssetHandle{preset_handle_id});
    if (path.empty()) return 0;
    return s.net->spawn(*s.scene, *s.resources, path, glm::vec3{x, y, z}, owner_peer);
}

extern "C" FJELL_SDK_EXPORT void fjell_net_destroy(uint32_t net_id) {
    auto& s = fjell::script_services();
    if (s.net && s.scene) s.net->destroy(*s.scene, net_id);
}

extern "C" FJELL_SDK_EXPORT void fjell_net_set_owner(uint32_t net_id, uint32_t peer) {
    auto* net = fjell::script_services().net;
    if (net) net->set_owner(net_id, peer);
}
