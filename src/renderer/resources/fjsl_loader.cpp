#include "renderer/resources/fjsl_loader.hpp"

#include <algorithm>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>

namespace fjell {

namespace {

// A shader's text as the parser sees it: read as text, so line endings are
// the platform's own.
std::optional<std::string> read_text(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return std::nullopt;
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
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

FjslLoader::FjslLoader(FjslLocator locator)
    : locator_{std::move(locator)} {}

FjslProgram FjslLoader::load(const std::string& reference) const {
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

} // namespace fjell
