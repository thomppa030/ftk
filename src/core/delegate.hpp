#pragma once

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
    Delegate(Delegate&&) = delete;
    Delegate& operator=(Delegate&&) = delete;

    // Subscribe a callable. The returned Connection must be kept alive —
    // destroying it unsubscribes the listener.
    template <typename F>
        requires std::invocable<F, Args...>
    [[nodiscard]] Connection bind(F&& callback) {
        auto id = state_->next_id++;
        state_->listeners.push_back(
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

        auto count = s->listeners.size();
        for (size_t i = 0; i < count; ++i) {
            if (s->listeners[i].callback) {
                s->listeners[i].callback(args...);
            }
        }

        s->broadcast_depth--;

        if (s->broadcast_depth == 0 && s->dirty) {
            std::erase_if(s->listeners, [](const Listener& l) { return !l.callback; });
            s->dirty = false;
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
        uint64_t next_id{1};
        uint32_t broadcast_depth{0};
        bool dirty{false};
    };

    std::shared_ptr<State> state_;
};

} // namespace fjell
