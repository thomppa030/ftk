#include "gpu/binding.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace fjell::gpu;

namespace {

// The test shaders in tests/shaders/, compiled by the build.
ShaderLayout reflected(const std::string& name) {
    const std::filesystem::path path = std::filesystem::path(FJELL_TEST_SHADER_DIR) / (name + ".spv");
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    REQUIRE(file.good());
    std::vector<uint32_t> words(static_cast<size_t>(file.tellg()) / sizeof(uint32_t));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(words.data()), static_cast<std::streamsize>(words.size() * 4));
    auto layout = reflect(words);
    REQUIRE(layout.has_value());
    return *layout;
}

const Texture TEX = Texture::make(1, 1);
const Sampler SMP = Sampler::make(1, 1);
const Buffer BUF = Buffer::make(1, 1);

ShaderBinding declared(uint32_t set, uint32_t binding, BindingKind kind, uint32_t count = 1) {
    ShaderBinding b;
    b.name = "b" + std::to_string(set) + "_" + std::to_string(binding);
    b.set = set;
    b.binding = binding;
    b.kind = kind;
    b.count = count;
    b.array = count != 1;
    return b;
}

SharedLayoutDesc shared(std::string name, uint32_t usual_set, std::vector<ShaderBinding> bindings) {
    return {std::move(name), usual_set, std::move(bindings)};
}

const SharedLayoutDesc GLOBALS = shared("globals", 0, {declared(0, 0, BindingKind::uniform_buffer),
                                                       declared(0, 1, BindingKind::sampled_texture),
                                                       declared(0, 4, BindingKind::sampled_texture)});
const SharedLayoutDesc BINDLESS = shared("bindless", 1, {declared(1, 0, BindingKind::sampled_texture, 1024),
                                                         declared(1, 1, BindingKind::storage_buffer),
                                                         declared(1, 2, BindingKind::storage_buffer)});
const SharedLayoutDesc DRAW_DATA = shared("draw data", 2, {declared(2, 0, BindingKind::storage_buffer),
                                                           declared(2, 1, BindingKind::storage_buffer)});

ShaderLayout with(std::vector<ShaderBinding> bindings) {
    ShaderLayout layout;
    layout.bindings = std::move(bindings);
    return layout;
}

} // namespace

TEST_CASE("entries are placed in the set their names give, in binding order", "[gpu][binding]") {
    const ShaderLayout layout = reflected("reflect.comp");
    const BindEntry entries[] = {{"target", storage(TEX)}, {"source", sampled(TEX, SMP)}};
    auto placed = place(layout, entries);
    REQUIRE(placed.has_value());
    CHECK(placed->set == 1);
    REQUIRE(placed->entries.size() == 2);
    CHECK(placed->entries[0].binding == 0);
    CHECK(placed->entries[0].resource == sampled(TEX, SMP));
    CHECK(placed->entries[1].binding == 1);
}

TEST_CASE("a texture and its sampler bound apart are placed", "[gpu][binding]") {
    const BindEntry entries[] = {{"plain", texture(TEX)}, {"pointy", sampler(SMP)}};
    auto placed = place(reflected("reflect.comp"), entries);
    REQUIRE(placed.has_value());
    CHECK(placed->set == 3);
}

TEST_CASE("buffers are placed by their block's name", "[gpu][binding]") {
    const BindEntry entries[] = {{"Items", storage(BufferRange(BUF))},
                                 {"counts_out", storage(BufferRange(BUF, 256, 64))}};
    auto placed = place(reflected("reflect.comp"), entries);
    REQUIRE(placed.has_value());
    CHECK(placed->set == 2);
    CHECK(placed->entries[1].resource.buffer.offset == 256);
}

TEST_CASE("a set left short names what is missing", "[gpu][binding]") {
    const BindEntry entries[] = {{"source", sampled(TEX, SMP)}};
    auto placed = place(reflected("reflect.comp"), entries);
    REQUIRE_FALSE(placed.has_value());
    CHECK(placed.error().find("'target'") != std::string::npos);
}

TEST_CASE("a resource of the wrong kind is refused by name", "[gpu][binding]") {
    const BindEntry entries[] = {{"source", storage(TEX)}, {"target", storage(TEX)}};
    auto placed = place(reflected("reflect.comp"), entries);
    REQUIRE_FALSE(placed.has_value());
    CHECK(placed.error().find("'source' is a sampled texture") != std::string::npos);
}

TEST_CASE("an acceleration structure is placed where a shader traces one", "[gpu][binding]") {
    const AccelerationStructure scene = AccelerationStructure::make(1, 1);
    const Buffer hits = Buffer::make(2, 1);
    const BindEntry entries[] = {{"hits_out", storage(hits)}, {"scene", acceleration(scene)}};
    auto placed = place(reflected("ray_query.comp"), entries);
    REQUIRE(placed.has_value());
    CHECK(placed->set == 0);
    REQUIRE(placed->entries.size() == 2);
    CHECK(placed->entries[0].binding == 0);
    CHECK(placed->entries[0].resource == acceleration(scene));

    const BindEntry wrong[] = {{"hits_out", storage(hits)}, {"scene", storage(hits)}};
    auto refused = place(reflected("ray_query.comp"), wrong);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().find("'scene' is an acceleration structure") != std::string::npos);
}

TEST_CASE("a name the shaders do not declare is refused", "[gpu][binding]") {
    const BindEntry entries[] = {{"sauce", sampled(TEX, SMP)}};
    auto placed = place(reflected("reflect.comp"), entries);
    REQUIRE_FALSE(placed.has_value());
    CHECK(placed.error().find("'sauce'") != std::string::npos);
}

TEST_CASE("one bind fills one set", "[gpu][binding]") {
    const BindEntry entries[] = {{"source", sampled(TEX, SMP)}, {"plain", texture(TEX)}};
    auto placed = place(reflected("reflect.comp"), entries);
    REQUIRE_FALSE(placed.has_value());
    CHECK(placed.error().find("one bind fills one set") != std::string::npos);
}

TEST_CASE("a set holding a runtime-sized array is left to a shared group", "[gpu][binding]") {
    const BindEntry entries[] = {{"pair", sampled(TEX, SMP), 0}, {"pair", sampled(TEX, SMP), 1}};
    auto placed = place(reflected("reflect.comp"), entries);
    REQUIRE_FALSE(placed.has_value());
    CHECK(placed.error().find("'textures'") != std::string::npos);
}

TEST_CASE("an array's elements are placed and counted", "[gpu][binding]") {
    const ShaderLayout layout = with({declared(0, 0, BindingKind::sampled_texture, 2)});
    const std::string name = layout.bindings[0].name;

    const BindEntry both[] = {{name, sampled(TEX, SMP), 1}, {name, sampled(TEX, SMP), 0}};
    auto placed = place(layout, both);
    REQUIRE(placed.has_value());
    CHECK(placed->entries[0].element == 0);
    CHECK(placed->entries[1].element == 1);

    const BindEntry one[] = {{name, sampled(TEX, SMP), 0}};
    CHECK_FALSE(place(layout, one).has_value());

    const BindEntry twice[] = {{name, sampled(TEX, SMP), 0}, {name, sampled(TEX, SMP), 0}};
    CHECK_FALSE(place(layout, twice).has_value());

    const BindEntry past[] = {{name, sampled(TEX, SMP), 2}};
    CHECK_FALSE(place(layout, past).has_value());
}

TEST_CASE("a shared layout a pipeline names takes the set that fits it", "[gpu][binding]") {
    const ShaderLayout layout = with({declared(0, 0, BindingKind::uniform_buffer),
                                      declared(0, 4, BindingKind::sampled_texture),
                                      declared(1, 0, BindingKind::sampled_texture)});
    const SharedLayoutDesc named[] = {GLOBALS};
    auto sets = place_shared(layout, named);
    REQUIRE(sets.has_value());
    CHECK(*sets == std::vector<uint32_t>{0});
}

TEST_CASE("names take no part in fitting a shared layout", "[gpu][binding]") {
    ShaderBinding renamed = declared(0, 4, BindingKind::sampled_texture);
    renamed.name = "depth_tex";
    const SharedLayoutDesc named[] = {GLOBALS};
    auto sets = place_shared(with({renamed}), named);
    REQUIRE(sets.has_value());
    CHECK(sets->front() == 0);
}

TEST_CASE("a single texture never reads as the bindless table", "[gpu][binding]") {
    // bloom, Hi-Z and the terrain's height map: one texture at binding 0.
    const SharedLayoutDesc named[] = {BINDLESS};
    auto sets = place_shared(with({declared(1, 0, BindingKind::sampled_texture)}), named);
    REQUIRE_FALSE(sets.has_value());
    CHECK(sets.error().find("no set that fits the shared 'bindless'") != std::string::npos);
}

TEST_CASE("an array compiled to one element still reads as the bindless table", "[gpu][binding]") {
    // A sky or fog shader indexes the table at constants only, and glslang
    // sizes `textures[]` to one element.
    ShaderBinding table = declared(1, 0, BindingKind::sampled_texture);
    table.array = true;
    const SharedLayoutDesc named[] = {BINDLESS};
    auto sets = place_shared(with({table, declared(1, 1, BindingKind::storage_buffer),
                                   declared(1, 2, BindingKind::storage_buffer)}),
                             named);
    REQUIRE(sets.has_value());
    CHECK(sets->front() == 1);
}

TEST_CASE("a shared layout declared away from its usual set is found there", "[gpu][binding]") {
    // The particle shaders read the bindless table at set 2.
    const ShaderLayout layout = with({declared(2, 0, BindingKind::sampled_texture, 0),
                                      declared(2, 1, BindingKind::storage_buffer)});
    const SharedLayoutDesc named[] = {BINDLESS};
    auto sets = place_shared(layout, named);
    REQUIRE(sets.has_value());
    CHECK(sets->front() == 2);
}

TEST_CASE("the usual set wins over another that fits", "[gpu][binding]") {
    const ShaderLayout layout = with({declared(2, 0, BindingKind::storage_buffer),
                                      declared(5, 0, BindingKind::storage_buffer)});
    const SharedLayoutDesc named[] = {DRAW_DATA};
    auto sets = place_shared(layout, named);
    REQUIRE(sets.has_value());
    CHECK(sets->front() == 2);
}

TEST_CASE("a shared layout fitting several sets, none usual, is refused", "[gpu][binding]") {
    const ShaderLayout layout = with({declared(4, 0, BindingKind::storage_buffer),
                                      declared(5, 0, BindingKind::storage_buffer)});
    const SharedLayoutDesc named[] = {DRAW_DATA};
    auto sets = place_shared(layout, named);
    REQUIRE_FALSE(sets.has_value());
    CHECK(sets.error().find("fits sets 4, 5") != std::string::npos);
}

TEST_CASE("two shared layouts cannot take one set", "[gpu][binding]") {
    const SharedLayoutDesc also_draw = shared("more draw data", 2, DRAW_DATA.bindings);
    const SharedLayoutDesc named[] = {DRAW_DATA, also_draw};
    auto sets = place_shared(with({declared(2, 0, BindingKind::storage_buffer)}), named);
    REQUIRE_FALSE(sets.has_value());
    CHECK(sets.error().find("only one can") != std::string::npos);
}

TEST_CASE("sets a pipeline does not name as shared stay its own", "[gpu][binding]") {
    // A private storage-buffer set that would fit draw data takes nothing
    // unless the pipeline names draw data.
    const ShaderLayout layout = with({declared(0, 0, BindingKind::uniform_buffer),
                                      declared(2, 0, BindingKind::storage_buffer)});
    const SharedLayoutDesc named[] = {GLOBALS};
    auto sets = place_shared(layout, named);
    REQUIRE(sets.has_value());
    CHECK(*sets == std::vector<uint32_t>{0});
}

TEST_CASE("an array longer than the shared one does not fit", "[gpu][binding]") {
    const SharedLayoutDesc named[] = {BINDLESS};
    CHECK_FALSE(place_shared(with({declared(1, 0, BindingKind::sampled_texture, 2048)}), named).has_value());
}

TEST_CASE("a shared group's entries are placed by name, element and binding order", "[gpu][binding]") {
    const std::string table = BINDLESS.bindings[0].name;
    const std::string params = BINDLESS.bindings[2].name;
    const BindEntry entries[] = {{params, storage(BufferRange{BUF})},
                                 {table, sampled(TEX, SMP), 9},
                                 {table, sampled(TEX, SMP), 3}};
    auto placed = place_some(BINDLESS, entries);
    REQUIRE(placed.has_value());
    CHECK(placed->set == 1);
    REQUIRE(placed->entries.size() == 3);
    CHECK(placed->entries[0].binding == 0);
    CHECK(placed->entries[0].element == 3);
    CHECK(placed->entries[1].element == 9);
    CHECK(placed->entries[2].binding == 2);
}

TEST_CASE("a shared group's update need not fill the set", "[gpu][binding]") {
    const BindEntry one[] = {{BINDLESS.bindings[1].name, storage(BufferRange{BUF})}};
    auto placed = place_some(BINDLESS, one);
    REQUIRE(placed.has_value());
    CHECK(placed->entries.size() == 1);
}

TEST_CASE("a shared group refuses what its layout does not hold", "[gpu][binding]") {
    const std::string table = BINDLESS.bindings[0].name;

    const BindEntry unknown[] = {{"materials", storage(BufferRange{BUF})}};
    auto missing = place_some(BINDLESS, unknown);
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().find("no binding 'materials'") != std::string::npos);

    const BindEntry wrong_kind[] = {{table, storage(BufferRange{BUF}), 0}};
    auto kind = place_some(BINDLESS, wrong_kind);
    REQUIRE_FALSE(kind.has_value());
    CHECK(kind.error().find("is a sampled texture, given a storage buffer") != std::string::npos);

    const BindEntry past[] = {{table, sampled(TEX, SMP), 1024}};
    CHECK_FALSE(place_some(BINDLESS, past).has_value());

    const BindEntry twice[] = {{table, sampled(TEX, SMP), 5}, {table, sampled(TEX, SMP), 5}};
    auto repeated = place_some(BINDLESS, twice);
    REQUIRE_FALSE(repeated.has_value());
    CHECK(repeated.error().find("given twice") != std::string::npos);

    CHECK_FALSE(place_some(BINDLESS, {}).has_value());
}
