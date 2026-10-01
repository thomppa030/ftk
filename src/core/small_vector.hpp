#pragma once

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <span>
#include <type_traits>
#include <vector>

namespace fjell {

/// A vector that keeps its first `N` elements inside itself and goes to the
/// heap only past them: for short lists made often on hot paths, such as the
/// entries of one bind. Elements are trivially copyable and trivially
/// destructible, so moving between the two places is a copy of bytes and
/// unused places are left unset.
template <typename T, size_t N>
    requires(std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T> && N > 0)
class SmallVector {
public:
    SmallVector() = default;

    SmallVector(std::initializer_list<T> values) {
        for (const T& value : values) push_back(value);
    }

    SmallVector(const SmallVector& other) { assign(other); }
    SmallVector& operator=(const SmallVector& other) {
        if (this != &other) assign(other);
        return *this;
    }
    SmallVector(SmallVector&& other) noexcept { take(other); }
    SmallVector& operator=(SmallVector&& other) noexcept {
        if (this != &other) take(other);
        return *this;
    }
    ~SmallVector() = default;

    void push_back(const T& value) {
        if (size_ == N && heap_.empty()) {
            // Past the inline places: everything moves to the heap.
            heap_.reserve(N * 2);
            heap_.assign(inline_data(), inline_data() + N);
        }
        if (!heap_.empty()) {
            heap_.push_back(value);
        } else {
            std::construct_at(inline_data() + size_, value);
        }
        ++size_;
    }

    void clear() noexcept {
        heap_.clear();
        size_ = 0;
    }

    [[nodiscard]] size_t size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

    [[nodiscard]] T* data() noexcept { return heap_.empty() ? inline_data() : heap_.data(); }
    [[nodiscard]] const T* data() const noexcept {
        return heap_.empty() ? inline_data() : heap_.data();
    }

    [[nodiscard]] T& operator[](size_t index) noexcept { return data()[index]; }
    [[nodiscard]] const T& operator[](size_t index) const noexcept { return data()[index]; }
    [[nodiscard]] T& back() noexcept { return data()[size_ - 1]; }
    [[nodiscard]] const T& back() const noexcept { return data()[size_ - 1]; }

    [[nodiscard]] T* begin() noexcept { return data(); }
    [[nodiscard]] T* end() noexcept { return data() + size_; }
    [[nodiscard]] const T* begin() const noexcept { return data(); }
    [[nodiscard]] const T* end() const noexcept { return data() + size_; }

    operator std::span<T>() noexcept { return {data(), size_}; }
    operator std::span<const T>() const noexcept { return {data(), size_}; }

    /// Whether the elements are on the heap.
    [[nodiscard]] bool spilled() const noexcept { return !heap_.empty(); }

    /// The same elements in the same order, wherever each keeps them.
    friend bool operator==(const SmallVector& a, const SmallVector& b)
        requires std::equality_comparable<T>
    {
        return std::ranges::equal(std::span<const T>(a), std::span<const T>(b));
    }

private:
    // The inline places, left unset until an element is put there.
    union Places {
        Places() noexcept {}
        T items[N];
    };

    [[nodiscard]] T* inline_data() noexcept { return places_.items; }
    [[nodiscard]] const T* inline_data() const noexcept { return places_.items; }

    void assign(const SmallVector& other) {
        size_ = other.size_;
        if (other.spilled()) {
            heap_ = other.heap_;
        } else {
            heap_.clear();
            std::memcpy(places_.items, other.places_.items, other.size_ * sizeof(T));
        }
    }

    void take(SmallVector& other) noexcept {
        size_ = other.size_;
        heap_ = std::move(other.heap_);
        if (heap_.empty()) {
            std::memcpy(places_.items, other.places_.items, other.size_ * sizeof(T));
        }
        other.heap_.clear();
        other.size_ = 0;
    }

    Places places_;
    /// Holds every element once there are more than `N`; empty before.
    std::vector<T> heap_;
    size_t size_{0};
};

} // namespace fjell
