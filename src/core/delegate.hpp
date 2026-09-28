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

// RAII connection handle. Destroying it auto-unsubscribes the listener.
// Move-only — store it as a member variable or in a vector for lifetime management.
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

    void disconnect() {
        if (disconnect_fn_) {
            disconnect_fn_();
            disconnect_fn_ = nullptr;
        }
    }

    [[nodiscard]] bool connected() const { return disconnect_fn_ != nullptr; }

private:
    template <typename S>
    friend class Delegate;

    explicit Connection(std::move_only_function<void()> fn)
        : disconnect_fn_{std::move(fn)} {}

    std::move_only_function<void()> disconnect_fn_;
};

// Multicast delegate. Multiple listeners can subscribe; all are called on broadcast().
// Partial specialization for void(Args...) signatures.
template <typename... Args>
class Delegate<void(Args...)> {
public:
    Delegate() : state_{std::make_shared<State>()} {}
    ~Delegate() = default;

    Delegate(const Delegate&) = delete;
    Delegate& operator=(const Delegate&) = delete;
    Delegate(Delegate&&) noexcept = default;
    Delegate& operator=(Delegate&&) noexcept = default;

    // Subscribe a callable. The returned Connection must be kept alive —
    // destroying it unsubscribes the listener.
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

    // Fire all listeners with the given arguments.
    void broadcast(Args... args) {
        auto s = state_; // local copy keeps state alive if Delegate is destroyed mid-broadcast
        s->broadcast_depth++;
        // Settles the broadcast however it ends: one a listener threw out of
        // is as finished as one that ran through, or the delegate would stay
        // mid-broadcast and hold every later bind back.
        struct Settle {
            State& state;
            explicit Settle(State& settled) : state{settled} {}
            Settle(const Settle&) = delete;
            Settle& operator=(const Settle&) = delete;
            ~Settle() {
                state.broadcast_depth--;
                if (state.broadcast_depth != 0) return;
                if (state.dirty) {
                    std::erase_if(state.listeners, [](const Listener& l) { return !l.callback; });
                    state.dirty = false;
                }
                // Anything bound while broadcasting joins now, after the cleanup
                // above so a clear() during the broadcast cannot take it with it.
                if (!state.pending.empty()) {
                    state.listeners.insert(state.listeners.end(),
                                           std::make_move_iterator(state.pending.begin()),
                                           std::make_move_iterator(state.pending.end()));
                    state.pending.clear();
                }
            }
        } const settle{*s};

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
    }

    // Remove all listeners.
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

    [[nodiscard]] bool empty() const { return state_->listeners.empty(); }
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
