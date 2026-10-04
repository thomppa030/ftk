// Links ftk-jsonrpc alone and whole (tests/CMakeLists.txt), so it builds only
// if everything in the library finds what it needs in the library and its
// own dependencies. Running it frames a message and reads it back.

#include "ftk/jsonrpc/frames.hpp"
#include "ftk/jsonrpc/peer.hpp"

int main() {
    ftk::jsonrpc::FrameReader frames;
    frames.add(ftk::jsonrpc::framed("{}"));
    const auto body = frames.next();
    // A peer with no streams ends at once.
    { const ftk::jsonrpc::Peer peer({}); }
    return body && *body == "{}" ? 0 : 1;
}
