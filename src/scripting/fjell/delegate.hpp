// delegate.hpp — Multicast delegate + RAII Connection for Fjell game code.
// Copy of core/delegate.hpp, shipped to game projects as part of the SDK.
#pragma once
#ifndef FJELL_DELEGATE_HPP
#define FJELL_DELEGATE_HPP

#include <algorithm>
#include <concepts>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace fjell {

// Forward declaration
template <typename Signature>
class Delegate;

/// Keeps a delegate subscription alive. Destroying it unsubscribes, so the
/// listener can never outlive the object that registered it.
///
/// Move-only, and the return value must be stored — a discarded `Connection`
/// is destroyed immediately and the subscription ends with it. Hold one as a
/// member, or several in a `std::vector<Connection>`.
class Connection {
public:
    Connection() = default;
    ~Connection() { disconnect(); }

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    Connection(Connection&& other) noexcept
        : disconnect_fn_{std::move(other.disconnect_fn_)} {}

    Connection& operator=(Connection&& other) noexcept {
        if (this != &other) {
            disconnect();
            disconnect_fn_ = std::move(other.disconnect_fn_);
        }
        return *this;
    }

    /// Unsubscribe now, before the `Connection` goes out of scope. Safe to
    /// call more than once.
    void disconnect() {
        if (disconnect_fn_) {
            disconnect_fn_();
            disconnect_fn_ = nullptr;
        }
    }

    /// Whether this handle still holds a live subscription.
    [[nodiscard]] bool connected() const { return disconnect_fn_ != nullptr; }

private:
    template <typename S>
    friend class Delegate;

    explicit Connection(std::move_only_function<void()> fn)
        : disconnect_fn_{std::move(fn)} {}

    std::move_only_function<void()> disconnect_fn_;
};

/// Multicast event: any number of listeners subscribe, and `broadcast()`
/// calls them all. Use it to let other systems react to something without the
/// sender knowing who is listening.
///
/// Safe to subscribe or unsubscribe from inside a handler — changes apply
/// after the current broadcast finishes rather than invalidating it.
///
/// @code
/// // Publisher
/// fjell::Delegate<void(int)> on_damaged;
/// on_damaged.broadcast(25);
///
/// // Subscriber — conn must be stored, or the subscription ends immediately
/// fjell::Connection conn = health.on_damaged.bind(
///     [this](int amount) { flash_red(amount); });
/// @endcode
template <typename... Args>
class Delegate<void(Args...)> {
public:
    Delegate() : state_{std::make_shared<State>()} {}
    ~Delegate() = default;

    Delegate(const Delegate&) = delete;
    Delegate& operator=(const Delegate&) = delete;
    Delegate(Delegate&&) noexcept = default;
    Delegate& operator=(Delegate&&) noexcept = default;

    /// Subscribe a callable — a lambda, a functor, or a bound member function.
    /// @param callback Invoked with the broadcast arguments. Capturing `this`
    ///                 is safe as long as the `Connection` does not outlive it.
    /// @return Handle that must be stored; discarding it unsubscribes at once.
    template <typename F>
        requires std::invocable<F, Args...>
    [[nodiscard]] Connection bind(F&& callback) {
        auto id = state_->next_id++;
        auto& target = state_->broadcast_depth > 0 ? state_->pending : state_->listeners;
        target.push_back(
            {id, std::move_only_function<void(Args...)>{std::forward<F>(callback)}});

        std::weak_ptr<State> weak = state_;
        return Connection{[weak, id]() {
            auto s = weak.lock();
            if (!s) return;

            if (s->broadcast_depth > 0) {
                for (auto& l : s->listeners) {
                    if (l.id == id) {
                        l.callback = nullptr;
                        s->dirty = true;
                        return;
                    }
                }
                // Bound during this same broadcast and disconnected before it
                // ended — drop it rather than merging it in.
                std::erase_if(s->pending, [id](const Listener& l) { return l.id == id; });
            } else {
                auto& v = s->listeners;
                for (auto it = v.begin(); it != v.end(); ++it) {
                    if (it->id == id) {
                        *it = std::move(v.back());
                        v.pop_back();
                        return;
                    }
                }
            }
        }};
    }

    /// Call every subscribed listener, in subscription order.
    /// @param args Forwarded to each listener. With no listeners this is a
    ///             cheap no-op.
    void broadcast(Args... args) {
        auto s = state_; // local copy keeps state alive if Delegate is destroyed mid-broadcast
        s->broadcast_depth++;

        // The size is snapshotted so a listener bound during the broadcast
        // waits for the next one. Binding also reallocates the vector, so the
        // callback is fetched through a fresh subscript each iteration rather
        // than through a reference taken before the call — the old buffer is
        // freed the moment a listener binds another.
        auto count = s->listeners.size();
        for (size_t i = 0; i < count; ++i) {
            auto& callback = s->listeners[i].callback;
            if (!callback) continue;
            callback(args...);
        }

        s->broadcast_depth--;

        if (s->broadcast_depth == 0) {
            if (s->dirty) {
                std::erase_if(s->listeners, [](const Listener& l) { return !l.callback; });
                s->dirty = false;
            }
            // Anything bound while broadcasting joins now, after the cleanup
            // above so a clear() during the broadcast cannot take it with it.
            if (!s->pending.empty()) {
                s->listeners.insert(s->listeners.end(),
                                    std::make_move_iterator(s->pending.begin()),
                                    std::make_move_iterator(s->pending.end()));
                s->pending.clear();
            }
        }
    }

    /// Unsubscribe every listener at once. Outstanding `Connection` objects
    /// become disconnected and are safe to destroy afterwards.
    void clear() {
        if (state_->broadcast_depth > 0) {
            for (auto& l : state_->listeners) {
                l.callback = nullptr;
            }
            state_->dirty = true;
        } else {
            state_->listeners.clear();
        }
    }

    /// Whether no listeners are subscribed.
    [[nodiscard]] bool empty() const { return state_->listeners.empty(); }

    /// Number of subscribed listeners.
    [[nodiscard]] uint32_t size() const { return static_cast<uint32_t>(state_->listeners.size()); }

private:
    struct Listener {
        uint64_t id;
        std::move_only_function<void(Args...)> callback;
    };

    struct State {
        std::vector<Listener> listeners;
        // Listeners bound while a broadcast is running. They must not go into
        // `listeners` yet: growing it reallocates the storage the in-flight
        // callback is executing from. Merged in once the outermost broadcast
        // finishes, which is also when they first become eligible to fire.
        std::vector<Listener> pending;
        uint64_t next_id{1};
        uint32_t broadcast_depth{0};
        bool dirty{false};
    };

    std::shared_ptr<State> state_;
};

} // namespace fjell

#endif // FJELL_DELEGATE_HPP
