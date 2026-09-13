#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace fjell {

struct PlayServices;

/// Editor state captured before play starts and put back when it stops.
///
/// The scene goes through the same serializer that writes `.fjell` files, so
/// whatever a scene file can hold survives a play round trip by construction,
/// and whatever it cannot hold is not editor state. Beside the scene, the
/// snapshot remembers engine-side state that play is allowed to change and
/// that has no other owner to put it back: the input context stack and the
/// set of script-created scene viewports.
struct PlaySnapshot {
    nlohmann::json scene;
    bool scene_dirty{false};
    std::vector<std::string> input_contexts;
    std::vector<std::string> viewports;

    [[nodiscard]] static PlaySnapshot capture(const PlayServices& s);

    /// Replaces the live scene with the captured one and re-creates the
    /// editor-side script instances. Every node in the snapshot comes back;
    /// a node that fails to deserialize is reported as an error, never skipped
    /// silently, since its assets were loaded a moment ago.
    void restore(const PlayServices& s) const;
};

} // namespace fjell
