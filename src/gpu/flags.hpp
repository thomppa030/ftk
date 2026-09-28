#pragma once

#include <bit>
#include <cstdint>

namespace fjell::gpu {

/// Opts an enum into `Flags`: its values are bit positions (0, 1, 2, …) under
/// 64, and `a | b` on two of them makes a set. Specialise it to `true` beside
/// the enum.
template <typename E>
inline constexpr bool is_flag_enum = false;

/// A set of an enum's values, one bit per value. The enum lists plain
/// positions and never spells a mask, so a new value is one line.
///
/// @code
/// gpu::TextureUses use = gpu::TextureUse::storage | gpu::TextureUse::sampled;
/// if (use.has(gpu::TextureUse::storage)) { ... }
/// @endcode
template <typename E>
    requires is_flag_enum<E>
class Flags {
public:
    constexpr Flags() noexcept = default;

    /// A set of one: a single value goes wherever a set is taken.
    constexpr Flags(E value) noexcept : bits_(bit(value)) {}

    /// Whether `value` is in the set.
    [[nodiscard]] constexpr bool has(E value) const noexcept {
        return (bits_ & bit(value)) != 0;
    }

    /// Whether the set holds nothing.
    [[nodiscard]] constexpr bool empty() const noexcept { return bits_ == 0; }

    /// Calls `fn(value)` for each value in the set, lowest position first.
    template <typename Fn>
    constexpr void for_each(Fn fn) const {
        for (uint64_t rest = bits_; rest != 0; rest &= rest - 1) {
            fn(static_cast<E>(std::countr_zero(rest)));
        }
    }

    [[nodiscard]] constexpr Flags operator|(Flags other) const noexcept {
        Flags out;
        out.bits_ = bits_ | other.bits_;
        return out;
    }

    constexpr Flags& operator|=(Flags other) noexcept {
        bits_ |= other.bits_;
        return *this;
    }

    friend constexpr bool operator==(Flags, Flags) noexcept = default;

private:
    [[nodiscard]] static constexpr uint64_t bit(E value) noexcept {
        return uint64_t{1} << static_cast<uint64_t>(value);
    }

    uint64_t bits_{0};
};

/// Two values of a flag enum make a set of both.
template <typename E>
    requires is_flag_enum<E>
[[nodiscard]] constexpr Flags<E> operator|(E a, E b) noexcept {
    return Flags<E>(a) | b;
}

} // namespace fjell::gpu
