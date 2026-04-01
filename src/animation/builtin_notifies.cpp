#include "animation/anim_notify.hpp"
#include "animation/anim_notify_registry.hpp"
#include "core/log.hpp"
#include "scene/node.hpp"

#include <cstdint>
#include <string>

namespace fjell {

// Compute field offset for types with virtual functions (offsetof is UB on
// non-standard-layout). Safe because we control the struct layout.
template <typename T, typename Field>
static uint32_t field_offset(Field T::*member) {
    alignas(T) char buf[sizeof(T)]{};
    auto* fake = reinterpret_cast<T*>(buf);
    return static_cast<uint32_t>(
        reinterpret_cast<const char*>(&(fake->*member)) - reinterpret_cast<const char*>(fake));
}

// ── LogNotify ──────────────────────────────────────────────────────────
// Debug notify: logs a message when fired. Useful for verifying the
// notify pipeline works end-to-end.

struct LogNotify : public AnimNotify {
    char message[128]{"notify fired"};

    void notify(Node3D& owner) override {
        FJELL_CORE_INFO("[AnimNotify] {} on '{}'", message, owner.name);
    }
    [[nodiscard]] const char* notify_type_name() const override { return "LogNotify"; }
};

static const bool log_registered = [] {
    NotifyTypeInfo info;
    info.name = "LogNotify";
    info.create = []() -> std::unique_ptr<AnimNotify> { return std::make_unique<LogNotify>(); };
    info.is_state = false;
    info.fields = {{
        .name = "message",
        .type = FJELL_FIELD_STRING,
        .offset = field_offset(&LogNotify::message),
        .size = sizeof(LogNotify::message),
        .enum_labels = {},
        .min = 0.0f, .max = 0.0f, .speed = 0.0f,
        .ref_type_name = {},
    }};
    AnimNotifyRegistry::instance().register_notify(std::move(info));
    return true;
}();

// ── LogNotifyState ─────────────────────────────────────────────────────
// Debug state notify: logs begin/tick/end for verifying state spans.

struct LogNotifyState : public AnimNotifyState {
    char message[128]{"state notify"};

    void notify_begin(Node3D& owner) override {
        FJELL_CORE_INFO("[AnimNotify] BEGIN '{}' on '{}'", message, owner.name);
    }
    void notify_tick(Node3D& owner, float dt) override {
        (void)owner; (void)dt;
    }
    void notify_end(Node3D& owner) override {
        FJELL_CORE_INFO("[AnimNotify] END '{}' on '{}'", message, owner.name);
    }
    [[nodiscard]] const char* notify_type_name() const override { return "LogNotifyState"; }
};

static const bool log_state_registered = [] {
    NotifyTypeInfo info;
    info.name = "LogNotifyState";
    info.create = []() -> std::unique_ptr<AnimNotify> { return std::make_unique<LogNotifyState>(); };
    info.is_state = true;
    info.fields = {{
        .name = "message",
        .type = FJELL_FIELD_STRING,
        .offset = field_offset(&LogNotifyState::message),
        .size = sizeof(LogNotifyState::message),
        .enum_labels = {},
        .min = 0.0f, .max = 0.0f, .speed = 0.0f,
        .ref_type_name = {},
    }};
    AnimNotifyRegistry::instance().register_notify(std::move(info));
    return true;
}();

} // namespace fjell
