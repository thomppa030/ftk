#include "renderer/resources/fjsl_compiler.hpp"

#include "core/log.hpp"
#include "core/profiler.hpp"
#include "platform/platform.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <optional>
#include <sstream>
#include <stdexcept>

namespace fjell {

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

// A shader's text as the parser and the templates see it: read as text, so
// line endings are the platform's own.
std::optional<std::string> read_text(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return std::nullopt;
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// What a compiled shader is a function of, beyond its own generated source:
// the includes it pulls in, the defines it was built with, the stage it was
// built as, and the compiler itself.
//
// FJELL_SHADER_CODEGEN_VERSION stands in for the generator. The generated
// GLSL is an output of fjsl_parser.cpp, so changing how that emits must
// invalidate everything — the .fjsl did not move but what it produces did.
// Bump it whenever generation changes shape.
constexpr uint32_t FJELL_SHADER_CODEGEN_VERSION = 1;

uint64_t key_of(uint64_t include_hash, std::string_view source, const FjslCompiler::Job& job) {
    uint64_t key = include_hash;
    key = hash_bytes(source, key);
    for (const auto& define : job.defines) key = hash_bytes(define, key);
    key = hash_bytes(job.stage, key);
    key = hash_bytes(std::string_view{reinterpret_cast<const char*>(&FJELL_SHADER_CODEGEN_VERSION),
                                      sizeof(FJELL_SHADER_CODEGEN_VERSION)}, key);
    return key;
}

// Whether a reference and a path from the file watcher name the same file.
// The reference may be relative ("shaders/toon.fjsl") while the watcher
// always reports absolute, so the test is whether one ends with the other,
// anchored to a separator, so "water.fjsl" does not match "deep_water.fjsl".
bool same_shader_file(std::string_view reference, std::string_view changed) {
    if (reference == changed) return true;
    if (reference.size() >= changed.size()) return false;
    if (!changed.ends_with(reference)) return false;
    return changed[changed.size() - reference.size() - 1] == '/';
}

// Every error among `diagnostics`, one per line as file:line:column: message.
std::string error_text(const std::vector<FjslDiagnostic>& diagnostics, const std::string& file) {
    std::string errors;
    for (const auto& d : diagnostics) {
        if (d.severity != FjslSeverity::error) continue;
        if (!errors.empty()) errors += "\n";
        errors += (d.file.empty() ? file : d.file) + ":" + std::to_string(d.line) + ":"
                + std::to_string(d.column) + ": " + d.message;
    }
    return errors;
}

} // namespace

bool FjslProgram::depends_on(std::string_view changed) const {
    if (same_shader_file(path, changed)) return true;
    return std::ranges::any_of(used_paths, [&](const std::string& used) {
        return same_shader_file(used, changed);
    });
}

std::string generate_fragment(FjslProgram& program, const std::string& tmpl) {
    const FjslComposition* composed = program.metadata.uses.empty() ? nullptr : &program.composition;
    std::string source = generate_fragment_shader(program.metadata, tmpl, composed);
    if (composed == nullptr) return source;

    // A composed function's uniforms are part of the program: they take a
    // run of the same parameter block, so they are folded in under the names
    // the generated GLSL just gave them. Without this a composed sky reads
    // past what it allocated and shows none of the dials it reads.
    ShaderMetadata& meta = program.metadata;
    const uint32_t own_textures = meta.custom_texture_count;
    for (const auto& file : composed->files) {
        if (file.use_name.empty()) continue;
        for (const auto& u : file.meta.uniforms) {
            FjslUniform folded = u;
            folded.name = file.use_name + "_" + u.name;
            // A sampler's slot is an absolute index into texture_indices,
            // where a scalar's is an offset into the params block, so the two
            // shift by different bases, and a sampler's keeps the PBR set at
            // the front.
            folded.slot = u.type == UniformType::Sampler2D
                ? meta.custom_texture_count + u.slot
                : meta.custom_param_count + u.slot;
            // Grouped under the wiring, so the inspector shows whose dials
            // these are rather than one flat wall.
            folded.group = folded.group.empty()
                ? file.use_name
                : file.use_name + " / " + folded.group;
            meta.uniforms.push_back(std::move(folded));
        }
        meta.custom_param_count += file.meta.custom_param_count;
        meta.custom_texture_count += file.meta.custom_texture_count;
    }

    // The parser caps one file's own samplers; only here is the
    // composition's total known. Name every file that asked for one, because
    // the author of the shader that went over may not have written any of
    // them.
    if (meta.custom_texture_count > CUSTOM_TEXTURE_SLOTS) {
        std::string who;
        if (own_textures > 0) {
            who += "\n  " + program.path + ": " + std::to_string(own_textures);
        }
        for (const auto& file : composed->files) {
            if (file.use_name.empty() || file.meta.custom_texture_count == 0) continue;
            who += "\n  " + file.path + " (as '" + file.use_name + "'): "
                 + std::to_string(file.meta.custom_texture_count);
        }
        throw std::runtime_error(
            "this composition wants " + std::to_string(meta.custom_texture_count)
            + " textures and a material has room for " + std::to_string(CUSTOM_TEXTURE_SLOTS)
            + ":" + who);
    }
    return source;
}

FjslCompiler::FjslCompiler(std::string shader_dir, std::string generated_dir, FjslLocator locator)
    : shader_dir_{std::move(shader_dir)}
    , generated_dir_{std::move(generated_dir)}
    , cache_dir_{generated_dir_ + "/.fjcache"}
    , locator_{std::move(locator)} {
    std::filesystem::create_directories(generated_dir_);
    std::filesystem::create_directories(cache_dir_);
    rehash_includes();
}

void FjslCompiler::rehash_includes() {
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

std::string FjslCompiler::read_include(const std::string& name) const {
    return read_text(shader_dir_ + "/include/" + name).value_or(std::string{});
}

FjslProgram FjslCompiler::load(const std::string& reference) const {
    FjslProgram program;
    program.path = reference;

    const std::string full_path = locator_ ? locator_(reference) : std::string{};
    if (full_path.empty()) {
        throw std::runtime_error("FJSL file not found: " + reference);
    }
    const std::optional<std::string> source = read_text(full_path);
    if (!source) {
        throw std::runtime_error("Failed to open FJSL file: " + full_path);
    }

    program.metadata = parse_fjsl(*source);
    // The name a compiler error should carry. The parser works on text and
    // does not know where it came from.
    program.metadata.source_path = reference;

    // The parser collects problems rather than throwing on the first, so a
    // shader with two mistakes reports both. Warnings are logged and the
    // build continues; an error means the metadata cannot be built from.
    log_fjsl_diagnostics(program.metadata, reference);
    if (std::string errors = error_text(program.metadata.diagnostics, reference); !errors.empty()) {
        throw std::runtime_error(errors);
    }

    // A `use` names a file the same way a material names a shader, so it is
    // found the same way: through the locator, which lets a host put its own
    // function at a path the engine's already has.
    if (!program.metadata.uses.empty()) {
        program.composition = resolve_composition(
            program.metadata, reference,
            [this](const std::string& path) -> std::optional<std::string> {
                const std::string full = locator_ ? locator_(path) : std::string{};
                if (full.empty()) return std::nullopt;
                return read_text(full);
            });
        log_fjsl_diagnostics(program.composition, reference);
        if (program.composition.has_errors()) {
            throw std::runtime_error(error_text(program.composition.diagnostics, reference));
        }
        for (const auto& file : program.composition.files) {
            if (file.use_name.empty()) continue;
            program.used_paths.push_back(file.path);
        }
    }
    return program;
}

std::string FjslCompiler::write_generated(const std::string& name, const std::string& source) const {
    const std::string path = generated_dir_ + "/" + name;
    std::ofstream out(path);
    if (!out.is_open()) {
        throw std::runtime_error("Failed to write generated shader: " + path);
    }
    out << source;
    return path;
}

uint64_t FjslCompiler::cache_key(const Job& job) const {
    return key_of(include_hash_, read_file(job.source_path), job);
}

std::string FjslCompiler::compile(const Job& job) const {
    const std::string spv_path = generated_dir_ + "/" + job.output_name + ".spv";
    const std::string include_dir = shader_dir_ + "/include";

    // Everything the compiled result is a function of. glslc is
    // deterministic, the same input twice gives byte-identical SPIR-V, so
    // this is an exact key rather than a guess, with none of the staleness a
    // timestamp comparison invites.
    const std::string source = read_file(job.source_path);
    const uint64_t key = key_of(include_hash_, source, job);

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
            FJELL_PROFILE_SCOPE_N("glslc_cache_hit");
            return spv_path;
        }
        // A cache we cannot read is not a reason to fail; fall through and
        // compile as if it had never been there.
    }

    // The largest startup cost in the renderer: ~64 ms per invocation, once
    // per shader, every launch the cache does not cover.
    FJELL_PROFILE_SCOPE_N("glslc");
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
        FJELL_GFX_ERROR("FJSL shader compilation failed:\n{}", output);
        // Carry what the compiler said, not just which file it was compiling:
        // this is what the editor's shader panel shows. #line makes the text
        // name the .fjsl, so it is worth showing.
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

    FJELL_GFX_DEBUG("Compiled FJSL shader: {} -> {}", job.source_path, spv_path);
    return spv_path;
}

std::vector<std::string> FjslCompiler::compile_all(std::span<const Job> jobs) const {
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

} // namespace fjell
