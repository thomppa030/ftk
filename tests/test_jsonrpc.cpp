#include "ftk/jsonrpc/frames.hpp"
#include "ftk/jsonrpc/peer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

using namespace ftk::jsonrpc;
using namespace std::chrono_literals;

namespace {

// Bytes one way between two peers in this program: what is written waits
// to be read, and a read waits for something written or the pipe closed.
struct Pipe {
    std::mutex lock;
    std::condition_variable more;
    std::deque<char> bytes;
    bool closed{false};

    ftk::Result<std::size_t> read(std::span<char> into) {
        std::unique_lock held(lock);
        more.wait(held, [&] { return !bytes.empty() || closed; });
        std::size_t got = 0;
        while (got < into.size() && !bytes.empty()) {
            into[got++] = bytes.front();
            bytes.pop_front();
        }
        return got;
    }
    ftk::Result<> write(std::string_view text) {
        const std::lock_guard held(lock);
        if (closed) return ftk::make_error("closed");
        bytes.insert(bytes.end(), text.begin(), text.end());
        more.notify_all();
        return {};
    }
    void close() {
        const std::lock_guard held(lock);
        closed = true;
        more.notify_all();
    }
};

// Two peers talking: a client and a server, each over its own pipe of the
// pair. The pipes close before the peers go, so their reads end.
struct Talk {
    Pipe to_server;
    Pipe to_client;
    std::unique_ptr<Peer> client;
    std::unique_ptr<Peer> server;

    Talk() {
        client = std::make_unique<Peer>(Channel{.read = [this](std::span<char> into) { return to_client.read(into); },
                                                .write = [this](std::string_view b) { return to_server.write(b); }});
        server = std::make_unique<Peer>(Channel{.read = [this](std::span<char> into) { return to_server.read(into); },
                                                .write = [this](std::string_view b) { return to_client.write(b); }});
    }
    ~Talk() {
        to_server.close();
        to_client.close();
        client.reset();
        server.reset();
    }
};

} // namespace

TEST_CASE("A message is framed by its length", "[jsonrpc]") {
    REQUIRE(framed("{}") == "Content-Length: 2\r\n\r\n{}");
}

TEST_CASE("Frames are read back whole, however the bytes come", "[jsonrpc]") {
    const std::string bytes = framed(R"({"a":1})") + framed(R"({"b":"two"})");
    SECTION("a byte at a time") {
        FrameReader frames;
        std::vector<std::string> read;
        for (const char c : bytes) {
            frames.add(std::string_view(&c, 1));
            while (auto body = frames.next()) read.push_back(*body);
        }
        REQUIRE(read == std::vector<std::string>{R"({"a":1})", R"({"b":"two"})"});
    }
    SECTION("all at once") {
        FrameReader frames;
        frames.add(bytes);
        REQUIRE(frames.next() == R"({"a":1})");
        REQUIRE(frames.next() == R"({"b":"two"})");
        REQUIRE_FALSE(frames.next().has_value());
    }
}

TEST_CASE("A header's other lines, its case and one with no length are read as the protocol has them", "[jsonrpc]") {
    FrameReader frames;
    frames.add("content-length: 2\r\nContent-Type: application/vscode-jsonrpc; charset=utf-8\r\n\r\n{}");
    frames.add("X-Nothing: 1\r\n\r\n");
    frames.add(framed("[]"));
    REQUIRE(frames.next() == "{}");
    REQUIRE(frames.next() == "[]");
}

TEST_CASE("A request is heard on the other side and its answer comes back to its callback", "[jsonrpc]") {
    Talk talk;
    std::optional<Answer> answer;
    talk.client->request("echo", {{"x", 3}}, [&](Answer got) { answer = std::move(got); });

    REQUIRE(talk.server->wait(5s));
    std::vector<std::string> heard;
    talk.server->poll([&](const Incoming& message) {
        heard.push_back(message.method);
        talk.server->answer(*message.id, message.params);
        return true;
    });
    REQUIRE(heard == std::vector<std::string>{"echo"});

    REQUIRE(talk.client->wait(5s));
    talk.client->poll({});
    REQUIRE(answer.has_value());
    REQUIRE(answer->has_value());
    REQUIRE((**answer)["x"] == 3);
}

TEST_CASE("An error comes back with its code, and a request nobody answers is answered as unknown", "[jsonrpc]") {
    Talk talk;
    std::optional<Answer> refused;
    std::optional<Answer> unknown;
    talk.client->request("refuse", nullptr, [&](Answer got) { refused = std::move(got); });
    talk.client->request("mystery", nullptr, [&](Answer got) { unknown = std::move(got); });
    for (int i = 0; i < 2 && talk.server->wait(5s); ++i) {
        talk.server->poll([&](const Incoming& message) {
            if (message.method != "refuse") return false;
            talk.server->answer_error(*message.id, 7, "no");
            return true;
        });
    }
    for (int i = 0; i < 50 && !(refused && unknown); ++i) {
        (void)talk.client->wait(100ms);
        talk.client->poll({});
    }
    REQUIRE(refused.has_value());
    REQUIRE(refused->error().code == 7);
    REQUIRE(refused->error().message == "no");
    REQUIRE(unknown.has_value());
    REQUIRE(unknown->error().code == METHOD_NOT_FOUND);
}

TEST_CASE("A request cancelled tells the other side, and its answer is never handed out", "[jsonrpc]") {
    Talk talk;
    bool answered = false;
    const int id = talk.client->request("slow", nullptr, [&](Answer) { answered = true; });
    talk.client->cancel(id);
    std::vector<Incoming> heard;
    while (heard.size() < 2 && talk.server->wait(5s)) {
        talk.server->poll([&](const Incoming& message) {
            heard.push_back(message);
            if (message.id) talk.server->answer_error(*message.id, REQUEST_CANCELLED, "cancelled");
            return true;
        });
    }
    REQUIRE(heard.size() == 2);
    REQUIRE(heard[1].method == "$/cancelRequest");
    REQUIRE(heard[1].params["id"] == id);
    (void)talk.client->wait(200ms);
    talk.client->poll({});
    REQUIRE_FALSE(answered);
}

TEST_CASE("A peer whose other side has stopped is no longer open, and a wait for it ends", "[jsonrpc]") {
    Talk talk;
    REQUIRE(talk.client->open());
    talk.to_client.close();
    REQUIRE(talk.client->wait(5s));
    REQUIRE_FALSE(talk.client->open());
}

TEST_CASE("A peer stopped writes what it queued, then sends nothing more", "[jsonrpc]") {
    Talk talk;
    talk.client->notify("last", nullptr);
    talk.client->stop();
    talk.client->notify("never", nullptr);
    std::vector<std::string> heard;
    while (talk.server->wait(300ms)) {
        talk.server->poll([&](const Incoming& message) {
            heard.push_back(message.method);
            return true;
        });
    }
    REQUIRE(heard == std::vector<std::string>{"last"});
}
