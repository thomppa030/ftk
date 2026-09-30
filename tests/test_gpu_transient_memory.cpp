#include "gpu/transient_memory.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

using namespace fjell;
using namespace fjell::gpu;

namespace {

// Stands in for the device: chunks are CPU memory under made-up handles.
struct FakeChunks {
    std::vector<std::unique_ptr<std::vector<std::byte>>> memory;
    std::vector<uint64_t> sizes_asked;
    bool fail{false};

    TransientMemory::MakeChunk maker() {
        return [this](uint64_t size) -> Result<TransientChunk> {
            sizes_asked.push_back(size);
            if (fail) return make_error("out of memory");
            memory.push_back(std::make_unique<std::vector<std::byte>>(size));
            const auto index = static_cast<uint32_t>(memory.size());
            return TransientChunk{Buffer::make(index, 1), memory.back()->size(), *memory.back()};
        };
    }
};

constexpr uint64_t CHUNK = 1024;

TransientMemory::Desc slots(uint32_t count) {
    return {.frame_slots = count, .alignment = 64, .chunk_size = CHUNK};
}

} // namespace

TEST_CASE("Transient slices start aligned and follow each other in one chunk", "[gpu][transient]") {
    FakeChunks fake;
    TransientMemory memory(slots(2), fake.maker());

    auto first = memory.allocate(10);
    auto second = memory.allocate(100);
    auto third = memory.allocate(4);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    REQUIRE(third.has_value());

    CHECK(fake.sizes_asked == std::vector<uint64_t>{CHUNK});
    CHECK(first->range.buffer == second->range.buffer);
    CHECK(first->range.offset == 0);
    CHECK(second->range.offset == 64);
    CHECK(third->range.offset == 192);
    CHECK(second->range.size == 100);
    CHECK(second->bytes.size() == 100);
    CHECK(second->bytes.data() == fake.memory[0]->data() + 64);
}

TEST_CASE("Transient memory takes another chunk when one is full", "[gpu][transient]") {
    FakeChunks fake;
    TransientMemory memory(slots(2), fake.maker());

    auto first = memory.allocate(1000);
    auto second = memory.allocate(100);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(fake.sizes_asked == std::vector<uint64_t>{CHUNK, CHUNK});
    CHECK(first->range.buffer != second->range.buffer);
    CHECK(second->range.offset == 0);
    CHECK(memory.chunks().size() == 2);
}

TEST_CASE("A transient slice larger than a chunk gets a chunk of its own size", "[gpu][transient]") {
    FakeChunks fake;
    TransientMemory memory(slots(2), fake.maker());

    auto small = memory.allocate(16);
    auto large = memory.allocate(5000);
    REQUIRE(small.has_value());
    REQUIRE(large.has_value());
    CHECK(fake.sizes_asked == std::vector<uint64_t>{CHUNK, 5000});
    CHECK(large->range.offset == 0);
    CHECK(large->bytes.size() == 5000);
}

TEST_CASE("A frame slot hands out its chunks anew when it comes round again", "[gpu][transient]") {
    FakeChunks fake;
    TransientMemory memory(slots(2), fake.maker());

    memory.begin_frame(0);
    auto slot0_a = memory.allocate(1000);
    auto slot0_b = memory.allocate(1000);
    memory.begin_frame(1);
    auto slot1 = memory.allocate(16);
    REQUIRE(slot0_a.has_value());
    REQUIRE(slot0_b.has_value());
    REQUIRE(slot1.has_value());
    // Slot 1 is still in flight when slot 0 comes round: it has its own.
    CHECK(slot1->range.buffer != slot0_a->range.buffer);
    CHECK(slot1->range.buffer != slot0_b->range.buffer);
    CHECK(fake.sizes_asked.size() == 3);

    memory.begin_frame(0);
    auto again_a = memory.allocate(1000);
    auto again_b = memory.allocate(1000);
    REQUIRE(again_a.has_value());
    REQUIRE(again_b.has_value());
    CHECK(again_a->range == slot0_a->range);
    CHECK(again_b->range == slot0_b->range);
    CHECK(fake.sizes_asked.size() == 3);
    CHECK(memory.chunks().size() == 3);
}

TEST_CASE("Transient memory does not go back to a chunk it has moved past", "[gpu][transient]") {
    FakeChunks fake;
    TransientMemory memory(slots(1), fake.maker());

    // Slices are handed out in order, so the room left in the first chunk
    // after a slice too big for it stays unused this frame.
    auto first = memory.allocate(600);
    auto second = memory.allocate(600);
    auto third = memory.allocate(16);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    REQUIRE(third.has_value());
    CHECK(third->range.buffer == second->range.buffer);
    CHECK(third->range.offset == 640);
}

TEST_CASE("Transient memory refuses nothing and says when no chunk can be made", "[gpu][transient]") {
    FakeChunks fake;
    TransientMemory memory(slots(1), fake.maker());

    CHECK_FALSE(memory.allocate(0).has_value());
    CHECK(fake.sizes_asked.empty());

    fake.fail = true;
    auto refused = memory.allocate(16);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().find("16 bytes") != std::string::npos);
    CHECK(refused.error().find("out of memory") != std::string::npos);
    CHECK(memory.chunks().empty());
}

TEST_CASE("Transient memory the CPU cannot see hands out slices with no bytes", "[gpu][transient]") {
    // Scratch for acceleration structure builds: chunks the GPU alone sees.
    uint32_t made = 0;
    TransientMemory memory(slots(1), [&made](uint64_t size) -> Result<TransientChunk> {
        ++made;
        return TransientChunk{Buffer::make(made, 1), size, {}};
    });

    auto first = memory.allocate(100);
    auto second = memory.allocate(100);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(first->bytes.empty());
    CHECK(second->bytes.empty());
    CHECK(second->range.buffer == first->range.buffer);
    CHECK(second->range.offset == 128);
    CHECK(made == 1);
}
