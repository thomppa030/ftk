// net_api.cpp — Engine-side implementation of the fjell_net_* script exports.
#include "fjell/api.h"
#include "scripting/script_globals.hpp"
#include "net/net_manager.hpp"

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
