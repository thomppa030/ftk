#pragma once

// One side of a JSON-RPC 2.0 conversation, as the language server protocol
// carries it: each message framed by its length (frames.hpp), over a pair
// of byte streams, a client's pipes to a server it runs or a server's own
// standard streams. Writing and reading each have a thread of their own, so
// nothing the owner does waits on the other side. What comes is handed out
// on the owner's thread by poll(): each answer to the callback of the
// request it answers, and the other side's own notifications and requests
// to a handler.
//
//     jsonrpc::Peer peer({.read = read_from_server, .write = write_to_server}, {.arrived = wake});
//     peer.request("initialize", params, [](jsonrpc::Answer answer) { ... });
//     // each frame, or as a server's loop:
//     peer.poll([](const jsonrpc::Incoming& message) { ...; return true; });

#include "ftk/base/result.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <expected>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace ftk::jsonrpc {

using Json = nlohmann::json;

/// The error the other side answered a request with.
struct ResponseError {
    int code{0};
    std::string message{};
};

/// JSON-RPC's own error codes.
inline constexpr int PARSE_ERROR = -32700;
inline constexpr int INVALID_REQUEST = -32600;
inline constexpr int METHOD_NOT_FOUND = -32601;
inline constexpr int INVALID_PARAMS = -32602;
inline constexpr int INTERNAL_ERROR = -32603;
/// The language server protocol's: a request given up because it was
/// cancelled, and one whose answer the text has since changed under.
inline constexpr int REQUEST_CANCELLED = -32800;
inline constexpr int CONTENT_MODIFIED = -32801;

/// What a request comes back with: its result, or the other side's error.
using Answer = std::expected<Json, ResponseError>;
using Answered = std::function<void(Answer answer)>;

/// What the other side says of its own accord: a notification, or a
/// request it waits on an answer to.
struct Incoming {
    std::string method{};
    Json params{};
    /// Set on a request: what its answer names.
    std::optional<Json> id{};
};

/// The two streams a peer talks over.
struct Channel {
    /// Waits for bytes from the other side and reads them into the span:
    /// how many, none once its stream has ended. Called on the peer's
    /// reading thread alone.
    std::function<Result<std::size_t>(std::span<char> into)> read{};
    /// Writes all of the bytes to the other side, waiting while its pipe
    /// is full. Called on the peer's writing thread alone.
    std::function<Result<>(std::string_view bytes)> write{};
};

struct PeerOptions {
    /// Called on the reading thread whenever something has come for poll()
    /// to hand out, and when the other side stops: to wake the loop that
    /// calls poll().
    std::function<void()> arrived{};
};

class Peer {
public:
    /// Talks over `channel`, its threads started.
    explicit Peer(Channel channel, PeerOptions options = {});

    /// Stops sending once what is queued is written, and waits for its
    /// threads: the channel's read must end first, its stream closed by the
    /// other side or by the owner (a child process ended).
    ~Peer();

    Peer(const Peer&) = delete;
    Peer& operator=(const Peer&) = delete;
    Peer(Peer&&) = delete;
    Peer& operator=(Peer&&) = delete;

    /// Sends the request `method` with `params` (none when null);
    /// `answered`, when given, is called by poll() with what it comes back
    /// with. @return its id, to cancel it by
    int request(std::string_view method, const Json& params, Answered answered);

    /// Sends the notification `method` with `params` (none when null).
    void notify(std::string_view method, const Json& params);

    /// Asks the other side to give up on the request `id`, as the language
    /// server protocol has it ($/cancelRequest); whatever it answers is
    /// never handed out.
    void cancel(int id);

    /// Answers the other side's request `id` with `result`, or with an
    /// error.
    void answer(const Json& id, const Json& result);
    void answer_error(const Json& id, int code, std::string_view message);

    /// Hands out what has come since the last call, in the order it came:
    /// answers to their requests' callbacks, and the other side's
    /// notifications and requests to `heard`. A request `heard` doesn't
    /// answer (returning false) is answered as a method this side doesn't
    /// know.
    void poll(const std::function<bool(const Incoming&)>& heard);

    /// Waits until something has come for poll() to hand out, the other
    /// side has stopped, or `patience` has passed. @return whether anything
    /// waits to be handed out or the other side has stopped
    bool wait(std::chrono::milliseconds patience);

    /// Whether the other side still talks: false once its stream has ended.
    [[nodiscard]] bool open() const;

    /// Sends nothing more: what is queued is still written. For the end of
    /// a conversation, its last words queued first.
    void stop();

private:
    void send(const Json& message);
    void write_all();
    void read_all();

    Channel channel_;
    PeerOptions options_;

    // Between the owner's thread and the peer's own.
    mutable std::mutex lock_;
    std::condition_variable more_to_write_;
    std::condition_variable came_;
    std::deque<std::string> outgoing_;
    bool stopping_{false};
    std::vector<Json> incoming_;
    bool ended_{false};

    // The owner's alone.
    int next_id_{1};
    std::map<int, Answered> waiting_;
    bool stopped_{false};

    std::thread writer_;
    std::thread reader_;
};

} // namespace ftk::jsonrpc
