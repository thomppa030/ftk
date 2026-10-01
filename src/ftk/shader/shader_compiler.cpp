#include "ftk/shader/shader_compiler.hpp"

#include "ftk/base/log.hpp"
#include "ftk/base/profiler.hpp"
#include "ftk/platform/platform.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace ftk {

namespace {

// FNV-1a. Not std::hash: that is unspecified and free to differ between
// runs and standard libraries, which is fine for a lookup table and fatal
// for a key written to disk and read back by a later build.
constexpr uint64_t FNV_OFFSET = 1469598103934665603ULL;
constexpr uint64_t FNV_PRIME = 1099511628211ULL;

uint64_t hash_bytes(std::string_view bytes, uint64_t seed = FNV_OFFSET) {
    uint64_t h = seed;
    for (unsigned char c : bytes) {
        h ^= c;
        h *= FNV_PRIME;
    }
    return h;
}

std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// A template's text as a generator sees it: read as text, so line endings
// are the platform's own.
std::optional<std::string> read_text(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return std::nullopt;
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// What a compiled shader is a function of, beyond its own source: the
// includes it pulls in, the defines it was built with, the stage it was built
// as, and the generator that wrote it.
uint64_t key_of(uint64_t include_hash, std::string_view source, const ShaderCompiler::Job& job,
                uint32_t generator_version) {
    uint64_t key = include_hash;
    key = hash_bytes(source, key);
    for (const auto& define : job.defines) key = hash_bytes(define, key);
    key = hash_bytes(job.stage, key);
    key = hash_bytes(std::string_view{reinterpret_cast<const char*>(&generator_version),
                                      sizeof(generator_version)}, key);
    return key;
}

} // namespace

ShaderCompiler::ShaderCompiler(ShaderCompilerDesc desc)
    : shader_dir_{std::move(desc.shader_dir)}
    , generated_dir_{std::move(desc.generated_dir)}
    , cache_dir_{std::move(desc.cache_dir)}
    , generator_version_{desc.generator_version} {
    std::filesystem::create_directories(generated_dir_);
    std::filesystem::create_directories(cache_dir_);
    rehash_includes();
}

void ShaderCompiler::rehash_includes() {
    // One hash over every include, in a fixed order so two runs over the same
    // files agree. What a shader pulls in with #include is not in its
    // generated source, so without this an edited include would find the old
    // binary under an unchanged key.
    //
    // Subdirectories count: include/tonemap/ holds the tone curves, and an
    // edit there has to rebuild a shader that includes them like any other.
    const std::filesystem::path root = shader_dir_ + "/include";
    std::vector<std::filesystem::path> includes;
    std::error_code ec;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root, ec)) {
        if (entry.is_regular_file(ec)) includes.push_back(entry.path());
    }
    std::sort(includes.begin(), includes.end());
    uint64_t h = FNV_OFFSET;
    for (const auto& path : includes) {
        h = hash_bytes(path.lexically_relative(root).generic_string(), h);
        h = hash_bytes(read_file(path.string()), h);
    }
    include_hash_ = h;
}

std::string ShaderCompiler::read_include(const std::string& name) const {
    return read_text(shader_dir_ + "/include/" + name).value_or(std::string{});
}

std::string ShaderCompiler::write_generated(const std::string& name, const std::string& source) const {
    const std::string path = generated_dir_ + "/" + name;
    std::ofstream out(path);
    if (!out.is_open()) {
        throw std::runtime_error("Failed to write generated shader: " + path);
    }
    out << source;
    return path;
}

uint64_t ShaderCompiler::cache_key(const Job& job) const {
    return key_of(include_hash_, read_file(job.source_path), job, generator_version_);
}

std::string ShaderCompiler::compile(const Job& job) const {
    const std::string spv_path = generated_dir_ + "/" + job.output_name + ".spv";
    const std::string include_dir = shader_dir_ + "/include";

    // Everything the compiled result is a function of. glslc is
    // deterministic, the same input twice gives byte-identical SPIR-V, so
    // this is an exact key rather than a guess, with none of the staleness a
    // timestamp comparison invites.
    const std::string source = read_file(job.source_path);
    const uint64_t key = key_of(include_hash_, source, job, generator_version_);

    char key_hex[17];
    std::snprintf(key_hex, sizeof(key_hex), "%016llx", static_cast<unsigned long long>(key));
    const std::string cached = cache_dir_ + "/" + key_hex + ".spv";

    // A hit skips glslc entirely: hashing costs about a millisecond against
    // the 64 ms a compile takes.
    if (!source.empty() && std::filesystem::exists(cached)) {
        std::error_code ec;
        std::filesystem::copy_file(cached, spv_path,
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (!ec) {
            FTK_PROFILE_SCOPE_N("glslc_cache_hit");
            return spv_path;
        }
        // A cache we cannot read is not a reason to fail; fall through and
        // compile as if it had never been there.
    }

    // The largest startup cost in the renderer: ~64 ms per invocation, once
    // per shader, every launch the cache does not cover.
    FTK_PROFILE_SCOPE_N("glslc");
    std::string cmd = "glslc --target-env=vulkan1.3 -I " + include_dir;
    if (!job.stage.empty()) {
        cmd += " -fshader-stage=" + job.stage;
    }
    for (const auto& define : job.defines) {
        cmd += " -D" + define;
    }
    cmd += " " + job.source_path + " -o " + spv_path + " 2>&1";

    std::string output;
    const int status = platform::run_command(cmd, output);
    if (status != 0) {
        FTK_GFX_ERROR("Shader compilation failed:\n{}", output);
        // Carry what the compiler said, not just which file it was compiling:
        // this is what an editor's shader panel shows. A generator's #line
        // makes the text name the file its author wrote, so it is worth
        // showing.
        throw std::runtime_error(output.empty()
                                     ? "shader compilation failed for: " + job.source_path
                                     : output);
    }

    // Keep the result for the next run. Written to a temporary and renamed,
    // so a process that dies mid-write leaves no half-file for a later run to
    // read as a valid cached shader. A failure to cache is not a failure to
    // compile: the SPIR-V is already where the caller wants it.
    if (!source.empty()) {
        std::error_code ec;
        const std::string temp = cached + ".tmp";
        std::filesystem::copy_file(spv_path, temp,
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (!ec) {
            std::filesystem::rename(temp, cached, ec);
        }
        if (ec) std::filesystem::remove(temp, ec);
    }

    FTK_GFX_DEBUG("Compiled shader: {} -> {}", job.source_path, spv_path);
    return spv_path;
}

std::vector<std::string> ShaderCompiler::compile_all(std::span<const Job> jobs) const {
    std::vector<std::future<std::string>> futures;
    futures.reserve(jobs.size());
    for (const Job& job : jobs) {
        futures.push_back(std::async(std::launch::async, [this, job] { return compile(job); }));
    }
    std::vector<std::string> spv_paths;
    spv_paths.reserve(futures.size());
    for (auto& future : futures) {
        spv_paths.push_back(future.get());
    }
    return spv_paths;
}

} // namespace ftk
