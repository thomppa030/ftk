#include "ftk/jsonrpc/documents.hpp"
#include "ftk/jsonrpc/frames.hpp"
#include "ftk/jsonrpc/peer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
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

TEST_CASE("A path is a file URI with its odd bytes encoded, and back", "[jsonrpc]") {
    using ftk::jsonrpc::path_of;
    using ftk::jsonrpc::uri_of;
    namespace fs = std::filesystem;
#ifdef _WIN32
    const fs::path path = "C:\\games\\my game\\coin #2.cpp";
    REQUIRE(uri_of(path) == "file:///C%3A/games/my%20game/coin%20%232.cpp");
#else
    const fs::path path = "/home/kay/my game/coin #2.cpp";
    REQUIRE(uri_of(path) == "file:///home/kay/my%20game/coin%20%232.cpp");
#endif
    REQUIRE(path_of(uri_of(path)) == path);
    REQUIRE(path_of(uri_of(fs::path(u8"/games/m\u00fcnze.cpp").make_preferred())) ==
            fs::path(u8"/games/m\u00fcnze.cpp").make_preferred());
    REQUIRE_FALSE(path_of("https://example.com/a.cpp").has_value());
    REQUIRE_FALSE(path_of("file://server/share/a.cpp").has_value());
}

TEST_CASE("Positions count along a line in UTF-8 bytes or UTF-16 units", "[jsonrpc]") {
    using ftk::jsonrpc::byte_at;
    using ftk::jsonrpc::Encoding;
    using ftk::jsonrpc::units_at;
    // An e with an accent is two bytes and one unit; a coin, four bytes and
    // two units.
    const std::string line = "a\xC3\xA9\xF0\x9F\xAA\x99z";
    REQUIRE(units_at(line, 3, Encoding::Utf16) == 2);
    REQUIRE(units_at(line, 7, Encoding::Utf16) == 4);
    REQUIRE(byte_at(line, 4, Encoding::Utf16) == 7);
    REQUIRE(byte_at(line, 5, Encoding::Utf16) == 8);
    REQUIRE(byte_at(line, 99, Encoding::Utf16) == line.size());
    REQUIRE(units_at(line, 7, Encoding::Utf8) == 7);
}
