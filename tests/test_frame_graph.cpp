// The frame graph's derivation of transitions, run without a GPU: a host
// that answers textures' shapes from a table and keeps every batch the graph
// would record, beside markers for the passes as they run.

#include "core/log.hpp"
#include "renderer/frame_graph.hpp"
#include "renderer/pass_builder.hpp"

#include <catch2/catch_test_macros.hpp>
#include <spdlog/sinks/callback_sink.h>

#include <memory>
#include <utility>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

using namespace fjell;
using gpu::Access;

namespace {

// One run of a graph after another, against textures that exist only as a
// table of shapes.
class GraphRun {
public:
    struct Batch {
        std::vector<gpu::Transition> transitions;
        gpu::Queue queue{gpu::Queue::graphics};
    };

    GraphRun() {
        host_.shape = [this](gpu::Texture texture) { return shapes_.at(texture.id); };
        host_.record = [this](auto, gpu::Queue queue, std::span<const gpu::Transition> batch,
                              std::string*) {
            log_.push_back({.ran = {},
                            .batch = {std::vector<gpu::Transition>(batch.begin(), batch.end()),
                                      queue}});
        };
        host_.state_name = [](gpu::AccessSet, bool) { return std::string(); };
    }

    GraphRun(const GraphRun&) = delete;
    GraphRun& operator=(const GraphRun&) = delete;

    gpu::Texture texture(TextureShape shape = {}) {
        const auto made = gpu::Texture::make(next_++, 1);
        shapes_[made.id] = shape;
        return made;
    }

    gpu::Buffer buffer() { return gpu::Buffer::make(next_++, 1); }

    // Starts a run on the next frame; the graph keeps what earlier runs left.
    void begin() {
        graph_.new_frame();
        graph_.begin_frame(host_);
        log_.clear();
    }

    // Declares a pass named `name` through `declare(PassBuilder&)`.
    template <typename Declare>
    void pass(const std::string& name, Declare declare) {
        PassBuilder builder;
        declare(builder);
        graph_.submit_declared_pass(name, builder,
                                    [this, name](auto) {
                                        log_.push_back({.ran = name, .batch = {}});
                                    });
    }

    // Backs the transient declared as `name` with `texture`.
    void back(std::string_view name, gpu::Texture texture) {
        const auto& images = graph_.images();
        for (uint32_t id = 0; id < images.size(); ++id) {
            if (images[id].virtual_resource && images[id].name == name) {
                graph_.bind_virtual_image(id, texture);
            }
        }
    }

    void execute() { (void)graph_.execute(nullptr, nullptr, nullptr, nullptr, nullptr); }

    // The batches recorded after the pass `after` ran and before `before`
    // did; an empty name is the run's start or end.
    [[nodiscard]] std::vector<Batch> batches(std::string_view after, std::string_view before) const {
        std::vector<Batch> out;
        bool open = after.empty();
        for (const Entry& entry : log_) {
            if (!entry.ran.empty()) {
                if (entry.ran == before) break;
                if (entry.ran == after) open = true;
                continue;
            }
            if (open) out.push_back(entry.batch);
        }
        return out;
    }

    // Every transition in those batches.
    [[nodiscard]] std::vector<gpu::Transition> between(std::string_view after,
                                                       std::string_view before) const {
        std::vector<gpu::Transition> out;
        for (const Batch& batch : batches(after, before)) {
            out.insert(out.end(), batch.transitions.begin(), batch.transitions.end());
        }
        return out;
    }

private:
    struct Entry {
        std::string ran;
        Batch batch;
    };

    FrameGraph graph_;
    GraphHost host_;
    std::unordered_map<uint32_t, TextureShape> shapes_;
    std::vector<Entry> log_;
    uint32_t next_{1};
};

// The renderer's warnings while it lives, kept here instead of the terminal
// so a test that provokes them passes quietly.
class Warnings {
public:
    Warnings() {
        auto& sinks = log::renderer()->sinks();
        kept_ = std::move(sinks);
        sinks = {std::make_shared<spdlog::sinks::callback_sink_mt>(
            [this](const spdlog::details::log_msg& message) {
                if (message.level == spdlog::level::warn) {
                    lines_.emplace_back(message.payload.data(), message.payload.size());
                }
            })};
    }
    ~Warnings() { log::renderer()->sinks() = std::move(kept_); }

    Warnings(const Warnings&) = delete;
    Warnings& operator=(const Warnings&) = delete;

    [[nodiscard]] const std::vector<std::string>& lines() const { return lines_; }

private:
    std::vector<spdlog::sink_ptr> kept_;
    std::vector<std::string> lines_;
};

} // namespace

TEST_CASE("A read after a write waits for it and moves the texture", "[framegraph]") {
    GraphRun run;
    const gpu::Texture t = run.texture();
    run.begin();
    run.pass("write", [&](PassBuilder& b) { b.write(b.import("t", t), Access::storage_write_compute); });
    run.pass("read", [&](PassBuilder& b) { b.read(b.import("t", t), Access::sampled_fragment); });
    run.execute();

    // Nothing is known of what it holds before its first write.
    const auto first = run.between("", "write");
    REQUIRE(first.size() == 1);
    CHECK(first[0].texture.texture == t);
    CHECK(first[0].from.empty());
    CHECK(first[0].to == Access::storage_write_compute);
    CHECK(first[0].wait_for.empty());

    const auto read = run.between("write", "read");
    REQUIRE(read.size() == 1);
    CHECK(read[0].wait_for == Access::storage_write_compute);
    CHECK(read[0].flush == Access::storage_write_compute);
    CHECK(read[0].visible_to == Access::sampled_fragment);
    CHECK(read[0].from == Access::storage_write_compute);
    CHECK(read[0].to == Access::sampled_fragment);
    CHECK(read[0].texture.base_mip == 0);
    CHECK(read[0].texture.mip_count == 1);
}

TEST_CASE("A reader the last transition covers needs none; another is widened", "[framegraph]") {
    GraphRun run;
    const gpu::Texture t = run.texture();
    run.begin();
    run.pass("write", [&](PassBuilder& b) { b.write(b.import("t", t), Access::storage_write_compute); });
    run.pass("fragment", [&](PassBuilder& b) { b.read(b.import("t", t), Access::sampled_fragment); });
    run.pass("again", [&](PassBuilder& b) { b.read(b.import("t", t), Access::sampled_fragment); });
    run.pass("compute", [&](PassBuilder& b) { b.read(b.import("t", t), Access::sampled_compute); });
    run.execute();

    CHECK(run.between("fragment", "again").empty());

    // Same state, but the write is not yet visible to compute. The transition
    // that moved it for the fragment reads is waited for as the write is.
    const auto compute = run.between("again", "compute");
    REQUIRE(compute.size() == 1);
    CHECK(compute[0].wait_for == (Access::storage_write_compute | Access::sampled_fragment));
    CHECK(compute[0].flush == Access::storage_write_compute);
    CHECK(compute[0].visible_to == (Access::sampled_compute | Access::sampled_fragment));
    CHECK(compute[0].from == Access::sampled_fragment);
    CHECK(compute[0].to == Access::sampled_compute);
}

TEST_CASE("A write waits for every reader since the last write", "[framegraph]") {
    GraphRun run;
    const gpu::Texture t = run.texture();
    run.begin();
    run.pass("write", [&](PassBuilder& b) { b.write(b.import("t", t), Access::storage_write_compute); });
    run.pass("read", [&](PassBuilder& b) { b.read(b.import("t", t), Access::sampled_compute); });
    run.pass("rewrite", [&](PassBuilder& b) { b.write(b.import("t", t), Access::storage_write_compute); });
    run.execute();

    const auto rewrite = run.between("read", "rewrite");
    REQUIRE(rewrite.size() == 1);
    CHECK(rewrite[0].wait_for == (Access::storage_write_compute | Access::sampled_compute));
    CHECK(rewrite[0].flush == Access::storage_write_compute);
    CHECK(rewrite[0].from == Access::sampled_compute);
    CHECK(rewrite[0].to == Access::storage_write_compute);
}

TEST_CASE("Buffers wait for writes and, to be written, for readers", "[framegraph]") {
    GraphRun run;
    const gpu::Buffer buffer = run.buffer();
    run.begin();
    run.pass("fill", [&](PassBuilder& b) {
        b.write(b.import("b", buffer), Access::storage_buffer_write_compute);
    });
    run.pass("draw", [&](PassBuilder& b) { b.read(b.import("b", buffer), Access::indirect_read); });
    run.pass("refill", [&](PassBuilder& b) {
        b.write(b.import("b", buffer), Access::storage_buffer_write_compute);
    });
    run.execute();

    // A buffer nothing has touched has nothing to wait for.
    CHECK(run.between("", "fill").empty());

    const auto draw = run.between("fill", "draw");
    REQUIRE(draw.size() == 1);
    CHECK(draw[0].buffer == buffer);
    CHECK_FALSE(draw[0].texture.texture.valid());
    CHECK(draw[0].wait_for == Access::storage_buffer_write_compute);
    CHECK(draw[0].flush == Access::storage_buffer_write_compute);
    CHECK(draw[0].visible_to == Access::indirect_read);

    const auto refill = run.between("draw", "refill");
    REQUIRE(refill.size() == 1);
    CHECK(refill[0].wait_for == (Access::storage_buffer_write_compute | Access::indirect_read));
    CHECK(refill[0].flush == Access::storage_buffer_write_compute);
}

TEST_CASE("A resting image starts at rest and goes back after a pass moves it", "[framegraph]") {
    GraphRun run;
    const gpu::Texture t = run.texture();
    ImportCatalog catalog;
    catalog.imports.emplace(fg_name_hash("t"),
                        ImportedImage{.view = t, .resting = Access::sampled_fragment});
    run.begin();
    run.pass("write", [&](PassBuilder& b) {
        b.write(b.import_named(catalog, "t"), Access::storage_write_compute);
    });
    run.execute();

    const auto first = run.between("", "write");
    REQUIRE(first.size() == 1);
    CHECK(first[0].from == Access::sampled_fragment);
    CHECK(first[0].to == Access::storage_write_compute);

    const auto rest = run.between("write", "");
    REQUIRE(rest.size() == 1);
    CHECK(rest[0].from == Access::storage_write_compute);
    CHECK(rest[0].to == Access::sampled_fragment);
    CHECK(rest[0].wait_for == Access::storage_write_compute);
    CHECK(rest[0].flush == Access::storage_write_compute);
    CHECK(rest[0].visible_to == Access::sampled_fragment);
}

TEST_CASE("A resting image nothing has written yet starts undefined", "[framegraph]") {
    GraphRun run;
    const gpu::Texture t = run.texture();
    ImportCatalog catalog;
    catalog.imports.emplace(fg_name_hash("t"), ImportedImage{.view = t,
                                                         .resting = Access::sampled_fragment,
                                                         .unwritten = true});
    run.begin();
    run.pass("write", [&](PassBuilder& b) {
        b.write(b.import_named(catalog, "t"), Access::storage_write_compute);
    });
    run.execute();

    const auto first = run.between("", "write");
    REQUIRE(first.size() == 1);
    CHECK(first[0].from.empty());
}

TEST_CASE("A run starts each texture where the run before left it", "[framegraph]") {
    GraphRun run;
    const gpu::Texture history = run.texture();
    run.begin();
    run.pass("write", [&](PassBuilder& b) {
        b.write(b.import("history", history), Access::storage_write_compute);
    });
    run.execute();

    run.begin();
    run.pass("read", [&](PassBuilder& b) {
        b.read(b.import("history", history), Access::sampled_fragment);
    });
    run.execute();

    const auto read = run.between("", "read");
    REQUIRE(read.size() == 1);
    CHECK(read[0].from == Access::storage_write_compute);
    CHECK(read[0].wait_for == Access::storage_write_compute);
    CHECK(read[0].flush == Access::storage_write_compute);
}

TEST_CASE("A pass that says what it leaves a texture in takes no transition to it",
          "[framegraph]") {
    GraphRun run;
    const gpu::Texture chain = run.texture({.mips = 4});
    run.begin();
    run.pass("mips", [&](PassBuilder& b) {
        const FgTexture t = b.write(b.import("chain", chain), Access::copy_dst);
        b.leaves(t, Access::copy_dst, Access::sampled_fragment);
    });
    run.pass("read", [&](PassBuilder& b) { b.read(b.import("chain", chain), Access::sampled_fragment); });
    run.execute();

    const auto mips = run.between("", "mips");
    REQUIRE(mips.size() == 1);
    CHECK(mips[0].texture.mip_count == 4);
    CHECK(run.between("mips", "read").empty());
}

TEST_CASE("A compute-queue pass's transitions are recorded for the compute queue",
          "[framegraph]") {
    GraphRun run;
    const gpu::Texture t = run.texture();
    run.begin();
    run.pass("async", [&](PassBuilder& b) {
        b.queue(QueueType::async_compute);
        b.write(b.import("t", t), Access::storage_write_compute);
    });
    run.execute();

    const auto batches = run.batches("", "async");
    REQUIRE(batches.size() == 1);
    CHECK(batches[0].queue == gpu::Queue::compute);
}

TEST_CASE("A transient starts undefined in the texture that backs it", "[framegraph]") {
    GraphRun run;
    const gpu::Texture backing = run.texture();
    run.begin();
    run.pass("make", [&](PassBuilder& b) {
        b.write(b.create("scratch", TextureDesc{.format = gpu::Format::r8_unorm}),
                Access::storage_write_compute);
    });
    run.pass("use", [&](PassBuilder& b) {
        b.read(b.create("scratch", TextureDesc{.format = gpu::Format::r8_unorm}),
               Access::sampled_compute);
    });
    run.back("scratch", backing);
    run.execute();

    const auto make = run.between("", "make");
    REQUIRE(make.size() == 1);
    CHECK(make[0].texture.texture == backing);
    CHECK(make[0].from.empty());
    const auto use = run.between("make", "use");
    REQUIRE(use.size() == 1);
    CHECK(use[0].wait_for == Access::storage_write_compute);
}

TEST_CASE("An acceleration structure is traced after its build, apart from buffers",
          "[framegraph]") {
    GraphRun run;
    const auto tlas = gpu::AccelerationStructure::make(90, 1);
    // A buffer whose id is the structure's: tracked apart all the same.
    const auto records = gpu::Buffer::make(90, 1);
    run.begin();
    run.pass("build", [&](PassBuilder& b) {
        b.write(b.import("tlas", tlas), Access::acceleration_build);
        b.write(b.import("records", records), Access::storage_buffer_write_compute);
    });
    run.pass("trace", [&](PassBuilder& b) {
        b.read(b.import("tlas", tlas), Access::acceleration_trace_compute);
        b.read(b.import("records", records), Access::storage_buffer_read_compute);
    });
    run.execute();

    CHECK(run.between("", "build").empty());
    const auto trace = run.between("build", "trace");
    REQUIRE(trace.size() == 2);
    const gpu::Transition& structure = trace[0].structure.valid() ? trace[0] : trace[1];
    const gpu::Transition& buffer = trace[0].structure.valid() ? trace[1] : trace[0];
    CHECK(structure.structure == tlas);
    CHECK_FALSE(structure.buffer.valid());
    CHECK(structure.wait_for == Access::acceleration_build);
    CHECK(structure.flush == Access::acceleration_build);
    CHECK(structure.visible_to == Access::acceleration_trace_compute);
    CHECK(buffer.buffer == records);
    CHECK_FALSE(buffer.structure.valid());

    // The next run's build waits for the trace the run before left.
    run.begin();
    run.pass("rebuild", [&](PassBuilder& b) {
        b.write(b.import("tlas", tlas), Access::acceleration_build);
    });
    run.execute();
    const auto rebuild = run.between("", "rebuild");
    REQUIRE(rebuild.size() == 1);
    CHECK(rebuild[0].structure == tlas);
    CHECK(rebuild[0].wait_for == (Access::acceleration_build | Access::acceleration_trace_compute));
}

TEST_CASE("A use declared with an access its resource lacks is left out and warned of",
          "[framegraph]") {
    GraphRun run;
    const gpu::Texture t = run.texture();
    const gpu::Buffer b = run.buffer();
    Warnings warnings;
    run.begin();
    run.pass("wrong", [&](PassBuilder& p) {
        p.read(p.import("t", t), Access::uniform_read);
        p.read(p.import("b", b), Access::sampled_fragment);
    });
    run.execute();

    CHECK(run.between("", "").empty());
    REQUIRE(warnings.lines().size() == 2);
    CHECK(warnings.lines()[0] ==
          "FrameGraph: pass 'wrong' declares sampled_fragment on a buffer, which has no such "
          "access; the use is left out.");
    CHECK(warnings.lines()[1] ==
          "FrameGraph: pass 'wrong' declares uniform_read on a texture, which has no such "
          "access; the use is left out.");
}
