#include "ftk/jsonrpc/peer.hpp"

#include "ftk/jsonrpc/frames.hpp"

#include <array>
#include <utility>

namespace ftk::jsonrpc {

Peer::Peer(Channel channel, PeerOptions options) : channel_(std::move(channel)), options_(std::move(options)) {
    writer_ = std::thread([this] { write_all(); });
    reader_ = std::thread([this] { read_all(); });
}

Peer::~Peer() {
    stop();
    for (std::thread* thread : {&writer_, &reader_}) {
        if (thread->joinable()) thread->join();
    }
}

void Peer::send(const Json& message) {
    if (stopped_) return;
    // Text that isn't valid UTF-8 goes as the replacement character rather
    // than stopping the message.
    std::string body = message.dump(-1, ' ', false, Json::error_handler_t::replace);
    {
        const std::lock_guard held(lock_);
        outgoing_.push_back(framed(body));
    }
    more_to_write_.notify_one();
}

int Peer::request(std::string_view method, const Json& params, Answered answered) {
    const int id = next_id_++;
    Json message{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}};
    if (!params.is_null()) message["params"] = params;
    waiting_[id] = std::move(answered);
    send(message);
    return id;
}

void Peer::notify(std::string_view method, const Json& params) {
    Json message{{"jsonrpc", "2.0"}, {"method", method}};
    if (!params.is_null()) message["params"] = params;
    send(message);
}

void Peer::cancel(int id) {
    if (waiting_.erase(id) == 0) return;
    notify("$/cancelRequest", {{"id", id}});
}

void Peer::answer(const Json& id, const Json& result) {
    send({{"jsonrpc", "2.0"}, {"id", id}, {"result", result}});
}

void Peer::answer_error(const Json& id, int code, std::string_view message) {
    send({{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}});
}

void Peer::poll(const std::function<bool(const Incoming&)>& heard) {
    std::vector<Json> came;
    {
        const std::lock_guard held(lock_);
        came.swap(incoming_);
    }
    for (Json& message : came) {
        const auto method = message.find("method");
        const auto id = message.find("id");
        if (method == message.end()) {
            // An answer, to a request still waited on.
            if (id == message.end() || !id->is_number_integer()) continue;
            const auto waiting = waiting_.find(id->get<int>());
            if (waiting == waiting_.end()) continue;
            const Answered answered = std::move(waiting->second);
            waiting_.erase(waiting);
            if (!answered) continue;
            if (const auto error = message.find("error"); error != message.end() && error->is_object()) {
                answered(std::unexpected(ResponseError{.code = error->value("code", 0),
                                                       .message = error->value("message", std::string())}));
            } else {
                answered(message.value("result", Json()));
            }
            continue;
        }
        if (!method->is_string()) continue;
        const Incoming said{.method = method->get<std::string>(),
                            .params = message.value("params", Json()),
                            .id = id != message.end() ? std::optional<Json>(*id) : std::nullopt};
        const bool handled = heard && heard(said);
        if (said.id && !handled) answer_error(*said.id, METHOD_NOT_FOUND, "nothing here answers " + said.method);
    }
}

bool Peer::wait(std::chrono::milliseconds patience) {
    std::unique_lock held(lock_);
    return came_.wait_for(held, patience, [this] { return !incoming_.empty() || ended_; });
}

bool Peer::open() const {
    const std::lock_guard held(lock_);
    return !ended_;
}

void Peer::stop() {
    stopped_ = true;
    {
        const std::lock_guard held(lock_);
        stopping_ = true;
    }
    more_to_write_.notify_one();
}

void Peer::write_all() {
    std::unique_lock held(lock_);
    for (;;) {
        more_to_write_.wait(held, [this] { return stopping_ || !outgoing_.empty(); });
        if (outgoing_.empty()) return;
        const std::string message = std::move(outgoing_.front());
        outgoing_.pop_front();
        held.unlock();
        const bool written = channel_.write && channel_.write(message).has_value();
        held.lock();
        // The other side is gone: nothing more can reach it.
        if (!written) {
            outgoing_.clear();
            return;
        }
    }
}

void Peer::read_all() {
    FrameReader frames;
    std::array<char, 16 * 1024> buffer{};
    while (channel_.read) {
        const Result<std::size_t> got = channel_.read(buffer);
        if (!got || *got == 0) break;
        frames.add(std::string_view(buffer.data(), *got));
        bool came = false;
        while (const std::optional<std::string> body = frames.next()) {
            Json message = Json::parse(*body, nullptr, false);
            if (message.is_discarded() || !message.is_object()) continue;
            const std::lock_guard held(lock_);
            incoming_.push_back(std::move(message));
            came = true;
        }
        if (came) {
            came_.notify_all();
            if (options_.arrived) options_.arrived();
        }
    }
    {
        const std::lock_guard held(lock_);
        ended_ = true;
    }
    came_.notify_all();
    if (options_.arrived) options_.arrived();
}

} // namespace ftk::jsonrpc
