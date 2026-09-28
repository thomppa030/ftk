#pragma once

#include <utility>

namespace fjell::gpu {

class Device;

/// Owns a GPU object by its handle. When the `Owned` goes, the handle goes
/// back to its owner, the device, which destroys the object once the GPU has
/// finished every submission that may use it: nothing waits for the GPU and
/// nothing is retired by hand. Moving hands the object on; it is released
/// exactly once. Converts to its handle, so it goes wherever a handle is
/// taken.
///
/// The owner is told through `release(Owner&, H)`, found beside the owner's
/// type. The engine's owner is the device; tests put a stand-in there.
///
/// @code
/// gpu::Owned<gpu::Texture> map_;          // a pass's member
/// map_ = std::move(*device.create(desc)); // the old texture, if any, is released
/// cmd.bind({{"shadow_out", gpu::storage(map_)}});
/// @endcode
template <typename H, typename Owner = Device>
class Owned {
public:
    Owned() = default;
    Owned(Owner& owner, H handle) noexcept : owner_(&owner), handle_(handle) {}
    ~Owned() { reset(); }

    Owned(const Owned&) = delete;
    Owned& operator=(const Owned&) = delete;

    Owned(Owned&& other) noexcept
        : owner_(std::exchange(other.owner_, nullptr)), handle_(std::exchange(other.handle_, H{})) {}

    Owned& operator=(Owned&& other) noexcept {
        if (this != &other) {
            reset();
            owner_ = std::exchange(other.owner_, nullptr);
            handle_ = std::exchange(other.handle_, H{});
        }
        return *this;
    }

    /// The handle; invalid when nothing is owned.
    [[nodiscard]] H get() const noexcept { return handle_; }
    operator H() const noexcept { return handle_; }

    /// Whether something is owned.
    [[nodiscard]] explicit operator bool() const noexcept { return handle_.valid(); }

    /// Releases what is owned now; owns nothing afterwards.
    void reset() noexcept {
        if (owner_ != nullptr && handle_.valid()) release(*owner_, handle_);
        owner_ = nullptr;
        handle_ = H{};
    }

private:
    Owner* owner_{nullptr};
    H handle_{};
};

} // namespace fjell::gpu
