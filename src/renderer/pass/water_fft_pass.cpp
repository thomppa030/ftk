#include "renderer/pass/water_fft_pass.hpp"

#include "ftk/base/log.hpp"
#include "ftk/base/profiler.hpp"
#include "ftk/framegraph/pass_builder.hpp"
#include "ftk/gpu/device.hpp"
#include "renderer/resources/water_spectrum.hpp"
#include "renderer/resources/water_surface.hpp"

#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace fjell {

namespace {

// The time the verification sea is held at: late enough that every wavevector
// has turned a different amount, which a check at zero would not exercise.
constexpr float VERIFY_TIME = 3.7f;
// Frames between the copy being recorded and its result being read: long
// enough for every frame in flight when it was recorded to have finished.
constexpr int VERIFY_WAIT_FRAMES = static_cast<int>(ftk::MAX_FRAMES_IN_FLIGHT) + 2;
// Where the surfaces rest: sampled by the water's task and mesh stages, and
// by the waterline and fog in compute. A reader anywhere else declares its
// read and gets its own barrier from the graph.
constexpr ftk::gpu::AccessSet SURFACE_READS = ftk::gpu::Access::sampled_mesh | ftk::gpu::Access::sampled_compute;

} // namespace

ftk::Result<> WaterFftPass::create(const PassResources& resources) {
    gpu_device_ = resources.device;
    ftk::gpu::Device& device = *gpu_device_;

    FTK_TRY(evolve_, device.create(ftk::gpu::ComputePipelineDesc{
                           .shader = "shaders/water_fft_evolve.comp",
                           .name = "water_fft_evolve",
                       }));
    FTK_TRY(butterfly_, device.create(ftk::gpu::ComputePipelineDesc{
                              .shader = "shaders/water_fft_butterfly.comp",
                              .name = "water_fft_butterfly",
                          }));
    FTK_TRY(assemble_, device.create(ftk::gpu::ComputePipelineDesc{
                             .shader = "shaders/water_fft_assemble.comp",
                             .name = "water_fft_assemble",
                         }));
    // The patch tiles: the sea past its edge is the sea at its other edge.
    sampler_ = device.sampler({.filter = ftk::gpu::Filter::linear,
                               .mip_filter = ftk::gpu::Filter::linear,
                               .address = ftk::gpu::Address::repeat});
    FTK_TRY(still_, device.create(ftk::gpu::TextureDesc{
                          .format = SURFACE_FORMAT,
                          .width = 1,
                          .height = 1,
                          .use = ftk::gpu::TextureUse::sampled,
                          .initial = ftk::gpu::Clear{0.0f, 0.0f, 0.0f, 0.0f},
                          .name = "water_fft_still",
                      }));

    const char* verify = std::getenv("FJELL_VERIFY_WATER_FFT");
    if (verify != nullptr && verify[0] != '\0' && verify[0] != '0') {
        begin_verification();
    }
    return {};
}

void WaterFftPass::destroy() {
    verify_readback_.reset();
    retire_images();
    still_.reset();
    evolve_.reset();
    butterfly_.reset();
    assemble_.reset();
}

void WaterFftPass::retire_images() {
    for (auto* set : {&spectra_a_, &spectra_b_, &displacement_}) {
        for (auto& texture : *set) texture.reset();
    }
    derivatives_.reset();
    for (auto& buffer : amplitude_buffers_) buffer.reset();
    images_ready_ = false;
}

ftk::Result<> WaterFftPass::build_images() {
    mips_ = static_cast<std::uint32_t>(std::countr_zero(resolution_)) + 1;
    ftk::gpu::Device& device = *gpu_device_;

    const auto texture = [&](ftk::gpu::Format format, std::uint32_t mips, ftk::gpu::TextureUses use,
                             std::string_view name) {
        return device.create(ftk::gpu::TextureDesc{
            .format = format,
            .width = resolution_,
            .height = resolution_,
            .mips = mips,
            .use = use,
            .name = name,
        });
    };
    constexpr ftk::gpu::TextureUses SURFACE_USE = ftk::gpu::TextureUse::storage | ftk::gpu::TextureUse::sampled;
    for (std::uint32_t i = 0; i < 2; ++i) {
        FTK_TRY(spectra_a_[i], texture(SPECTRUM_FORMAT, 1, ftk::gpu::TextureUse::storage, "water_fft_spectra_a"));
        FTK_TRY(spectra_b_[i], texture(SPECTRUM_FORMAT, 1, ftk::gpu::TextureUse::storage, "water_fft_spectra_b"));
        FTK_TRY(displacement_[i], texture(SURFACE_FORMAT, mips_, SURFACE_USE, "water_fft_displacement"));
    }
    FTK_TRY(derivatives_, texture(SURFACE_FORMAT, mips_, SURFACE_USE, "water_fft_derivatives"));

    const std::uint64_t bytes = std::uint64_t{resolution_} * resolution_ * sizeof(glm::vec2);
    for (auto& buffer : amplitude_buffers_) {
        FTK_TRY(buffer, device.create(ftk::gpu::BufferDesc{
                              .size = bytes,
                              .use = ftk::gpu::BufferUse::storage,
                              .memory = ftk::gpu::Memory::upload,
                              .name = "water_fft_amplitudes",
                          }));
    }
    images_fresh_ = true;
    images_ready_ = true;
    return {};
}

void WaterFftPass::set_sea(const FjWaterData::SimulatedSea* sea) {
    if (verifying_ && (sea == nullptr || sea != &*verify_sea_)) return;   // not replaced under it

    if (sea == nullptr || sea->spectrum == nullptr) {
        spectrum_.reset();
        return;
    }
    patch_size_ = sea->drawn_from.patch_size;
    choppiness_ = sea->choppiness;
    if (sea->spectrum == spectrum_) return;

    const std::uint32_t resolution = sea->spectrum->resolution;
    if (resolution < MIN_RESOLUTION || resolution > MAX_RESOLUTION || patch_size_ <= 0.0f) {
        if (spectrum_ != nullptr || resolution_ != resolution) {
            FTK_CORE_WARN("WaterFftPass: a simulated sea needs a patch with a size and a "
                            "resolution from {} to {}, got {} m at {}", MIN_RESOLUTION,
                            MAX_RESOLUTION, patch_size_, resolution);
        }
        resolution_ = resolution;   // so the warning is said once per offending sea
        images_ready_ = false;
        spectrum_.reset();
        return;
    }

    if (resolution != resolution_ || !images_ready_) {
        retire_images();
        resolution_ = resolution;
        if (auto built = build_images(); !built) {
            FTK_CORE_ERROR("{}", built.error());
            retire_images();
            spectrum_.reset();
            return;
        }
    }
    spectrum_ = sea->spectrum;
    k_spacing_ = spectrum_->k_spacing;
    amplitudes_stale_.fill(true);
}

void WaterFftPass::prepare_frame(const FrameContext& ctx, GpuContext& /*gpu*/) {
    stepped_this_frame_ = false;
    time_ = verifying_ ? VERIFY_TIME : ctx.elapsed_time;

    if (verifying_ && verify_frames_left_ > 0 && --verify_frames_left_ == 0) {
        finish_verification();
    }
}

PassSetup WaterFftPass::setup() const {
    return {
        .name = "water_fft",
        .phase = RenderPhase::Environment,
        .sub_order = 65,   // a slot of its own, after the cloud shadow; nothing here reads it
    };
}

void WaterFftPass::declare(ftk::PassBuilder& builder, const DeclareContext& ctx) {
    // Steps once a frame, in the first run that keeps it; with no sea running
    // the still texel is all there is and nothing is written.
    if (stepped_this_frame_ || !active()) { return; }
    // The graph brings both surfaces to storage for the transform's write;
    // the step's own mip chain leaves them as copy targets, and the graph
    // returns them to rest.
    for (const char* name : {"water_displacement", "water_derivatives"}) {
        auto image = builder.import_named(ctx, name);
        builder.write(image, ftk::gpu::Access::storage_write_compute);
        builder.leaves(image, ftk::gpu::Access::copy_dst, ftk::gpu::Access::copy_dst);
    }
    // New images: last frame's surface is cleared to still water, and the
    // graph returns it to rest.
    if (images_fresh_) {
        builder.write(builder.import_named(ctx, "water_displacement_prev"), ftk::gpu::Access::copy_dst);
    }
}

void WaterFftPass::collect_exports(DeclareContext& ctx) {
    // Every surface rests where its readers sample it. Fresh images hold
    // nothing until the first step; the still texel is cleared when made.
    const auto add = [&](std::string_view name, ftk::gpu::Texture texture, bool unwritten) {
        // This frame's surface is the next frame's previous one.
        ctx.export_texture(name, texture,
                           {.persistent = true, .resting = SURFACE_READS, .unwritten = unwritten});
    };
    if (!active()) {
        if (!still_) { return; }
        for (const char* name : {"water_displacement", "water_displacement_prev",
                                 "water_derivatives"}) {
            add(name, still_, false);
        }
        return;
    }
    const std::uint32_t written = written_index();
    add("water_displacement", displacement_[written], images_fresh_);
    add("water_displacement_prev", displacement_[1 - written], images_fresh_);
    add("water_derivatives", derivatives_, images_fresh_);
}

void WaterFftPass::generate_mips(ftk::gpu::CommandList& cmd, ftk::gpu::Texture surface, ftk::gpu::AccessSet from) const {
    // The top mip as written, the rest to be filled from it: all as copy
    // targets, which is where the chain starts and ends.
    cmd.barrier(surface, from, ftk::gpu::Access::copy_dst);
    cmd.generate_mipmaps(surface);
}

void WaterFftPass::record(const FrameContext& ctx) {
    if (stepped_this_frame_) return;
    stepped_this_frame_ = true;
    if (!active()) return;

    FTK_PROFILE_SCOPE_N("record_water_fft");
    auto& cmd = ctx.commands();
    auto zone = cmd.zone("WaterFft");

    // This step writes the surface that was the previous one. Swapped here,
    // with the work, rather than when the frame is prepared: a frame that is
    // prepared and never recorded would otherwise leave "this frame's" surface
    // naming the older of the two.
    cur_ = 1 - cur_;
    const std::uint32_t slot = ctx.frame_index % ftk::MAX_FRAMES_IN_FLIGHT;
    // Written here rather than when the sea was set: a sea can be set after
    // this pass has prepared its frame, and the transform must never run from
    // a buffer that has not been filled.
    if (amplitudes_stale_[slot]) {
        const auto amplitudes = std::as_bytes(std::span(spectrum_->amplitudes));
        std::memcpy(gpu_device_->mapped(amplitude_buffers_[slot]).data(), amplitudes.data(),
                    amplitudes.size());
        amplitudes_stale_[slot] = false;
    }
    const std::uint32_t groups = (resolution_ + 7) / 8;
    const auto log2_resolution = static_cast<std::uint32_t>(std::countr_zero(resolution_));

    // The spectra are written whole, so what they held before is of no
    // interest and they start from nothing. The graph has the surface and
    // its derivatives as storage already.
    const std::array<ftk::gpu::TextureView, 4> spectra{spectra_a_[0], spectra_b_[0], spectra_a_[1], spectra_b_[1]};
    cmd.barrier(spectra, {}, ftk::gpu::Access::storage_read_write_compute);

    if (images_fresh_) {
        // Nothing has been simulated into last frame's surface yet. Still
        // water is the honest thing for it to hold: the surface then appears
        // to have risen out of a flat sea over one frame, not out of whatever
        // was in the memory. The graph has it as a copy target.
        cmd.clear(displacement_[1 - cur_], ftk::gpu::Clear{0.0f, 0.0f, 0.0f, 0.0f});
        images_fresh_ = false;
    }

    // Turn the drawn sea to now.
    {
        FTK_PROFILE_SCOPE_N("water_fft_evolve");
        auto evolve_zone = cmd.zone("WaterFft Evolve");
        cmd.set_pipeline(evolve_);
        cmd.bind({{"sea", ftk::gpu::storage(amplitude_buffers_[slot])},
                  {"spectra_a", ftk::gpu::storage(spectra_a_[0])},
                  {"spectra_b", ftk::gpu::storage(spectra_b_[0])}});
        cmd.push(EvolvePush{resolution_, log2_resolution, k_spacing_, time_, choppiness_});
        cmd.dispatch(groups, groups, 1);
    }

    // Transform: every stage along x, then every stage along z. Each stage
    // reads what the one before wrote and writes what it read.
    std::uint32_t source = 0;
    {
        FTK_PROFILE_SCOPE_N("water_fft_butterflies");
        auto butterfly_zone = cmd.zone("WaterFft Butterflies");
        cmd.set_pipeline(butterfly_);
        for (std::uint32_t vertical = 0; vertical < 2; ++vertical) {
            for (std::uint32_t stage = 1; stage <= log2_resolution; ++stage) {
                cmd.barrier(spectra, ftk::gpu::Access::storage_read_write_compute,
                            ftk::gpu::Access::storage_read_write_compute);
                cmd.bind({{"a_in", ftk::gpu::storage(spectra_a_[source])},
                          {"b_in", ftk::gpu::storage(spectra_b_[source])},
                          {"a_out", ftk::gpu::storage(spectra_a_[1 - source])},
                          {"b_out", ftk::gpu::storage(spectra_b_[1 - source])}});
                cmd.push(ButterflyPush{resolution_, stage, vertical});
                cmd.dispatch(groups, groups, 1);
                source = 1 - source;
            }
        }
    }

    // Gather the surface and how it changes across the patch.
    {
        FTK_PROFILE_SCOPE_N("water_fft_assemble");
        auto assemble_zone = cmd.zone("WaterFft Assemble");
        cmd.barrier(std::array<ftk::gpu::TextureView, 2>{spectra_a_[source], spectra_b_[source]},
                    ftk::gpu::Access::storage_read_write_compute, ftk::gpu::Access::storage_read_compute);
        cmd.set_pipeline(assemble_);
        cmd.bind({{"surface_a", ftk::gpu::storage(spectra_a_[source])},
                  {"surface_b", ftk::gpu::storage(spectra_b_[source])},
                  {"displacement", ftk::gpu::storage(ftk::gpu::mip(displacement_[cur_], 0))},
                  {"derivatives", ftk::gpu::storage(ftk::gpu::mip(derivatives_, 0))}});
        cmd.push(AssemblePush{resolution_, patch_size_ / static_cast<float>(resolution_)});
        cmd.dispatch(groups, groups, 1);
    }

    ftk::gpu::AccessSet written = ftk::gpu::Access::storage_write_compute;
    if (verifying_ && verify_frames_left_ < 0 && record_verification_copy(cmd)) {
        written = ftk::gpu::Access::copy_src;
    }

    {
        FTK_PROFILE_SCOPE_N("water_fft_mips");
        auto mips_zone = cmd.zone("WaterFft Mips");
        generate_mips(cmd, displacement_[cur_], written);
        generate_mips(cmd, derivatives_, written);
    }
}

void WaterFftPass::begin_verification() {
    // Every setting spelled out: this is a fixed sea to hold the transform to,
    // and it should not move when a contributor's defaults do.
    FftContributor sea;
    sea.wind_direction = {0.8f, 0.6f};
    sea.wind_speed = 9.0f;
    sea.fetch = 100000.0f;
    sea.spread_degrees = 35.0f;
    sea.high_cut = 0.0f;
    sea.amplitude_scale = 1.0f;
    sea.patch_size = 96.0f;
    // Small enough that the plain Fourier sum it is held to takes a moment
    // rather than minutes, large enough to run every kind of butterfly stage.
    sea.resolution = 64;
    sea.choppiness = 1.2f;
    sea.seed = 7;
    verify_sea_ = water::draw_simulated_sea(sea);
    verifying_ = true;
    set_sea(&*verify_sea_);
    FTK_CORE_INFO("WaterFftPass: verifying the GPU transform against the CPU reference");
}

bool WaterFftPass::record_verification_copy(ftk::gpu::CommandList& cmd) {
    const std::uint64_t texels = std::uint64_t{resolution_} * resolution_;
    const std::uint64_t image_bytes = texels * 4 * sizeof(std::uint16_t);
    auto readback = gpu_device_->create(ftk::gpu::BufferDesc{
        .size = image_bytes * 2,
        .use = ftk::gpu::BufferUse::storage,
        .memory = ftk::gpu::Memory::readback,
        .name = "water_fft_verify",
    });
    if (!readback) {
        FTK_CORE_ERROR("WaterFftPass: {}", readback.error());
        return false;
    }
    verify_readback_ = std::move(*readback);

    // What the assemble step wrote, copied as it stands.
    for (const auto& [surface, offset] : {std::pair{displacement_[cur_].get(), std::uint64_t{0}},
                                         std::pair{derivatives_.get(), image_bytes}}) {
        cmd.barrier(surface, ftk::gpu::Access::storage_write_compute, ftk::gpu::Access::copy_src);
        cmd.copy(ftk::gpu::mip(surface, 0), ftk::gpu::BufferRange(verify_readback_, offset, image_bytes));
    }
    verify_frames_left_ = VERIFY_WAIT_FRAMES;
    return true;
}

void WaterFftPass::finish_verification() {
    const std::uint32_t n = resolution_;
    const water::DisplacementSpectra spectra =
        water::evolve_spectrum(*spectrum_, choppiness_, VERIFY_TIME);
    const std::vector<float> height = water::inverse_transform_reference(spectra.height, n);
    const std::vector<float> along_x = water::inverse_transform_reference(spectra.along_x, n);
    const std::vector<float> along_z = water::inverse_transform_reference(spectra.along_z, n);

    const auto* read = reinterpret_cast<const std::uint16_t*>(gpu_device_->mapped(verify_readback_).data());
    const std::size_t texels = static_cast<std::size_t>(n) * n;
    const float across = 1.0f / (2.0f * patch_size_ / static_cast<float>(n));
    auto at = [n](const std::vector<float>& v, std::uint32_t x, std::uint32_t z) {
        return v[static_cast<std::size_t>(z % n) * n + x % n];
    };

    float worst_surface = 0.0f;
    float worst_slope = 0.0f;
    float tallest = 0.0f;
    for (std::uint32_t z = 0; z < n; ++z) {
        for (std::uint32_t x = 0; x < n; ++x) {
            const std::size_t i = (static_cast<std::size_t>(z) * n + x) * 4;
            const std::size_t d = i + texels * 4;
            auto half = [&](std::size_t index) { return glm::unpackHalf1x16(read[index]); };

            tallest = std::max(tallest, std::abs(at(height, x, z)));
            worst_surface = std::max({worst_surface,
                                      std::abs(half(i + 0) - at(along_x, x, z)),
                                      std::abs(half(i + 1) - at(height, x, z)),
                                      std::abs(half(i + 2) - at(along_z, x, z))});

            // The same differences the shader takes, of the CPU's surface.
            const float dh_dx = (at(height, x + 1, z) - at(height, x + n - 1, z)) * across;
            const float dh_dz = (at(height, x, z + 1) - at(height, x, z + n - 1)) * across;
            const float dx_dx = (at(along_x, x + 1, z) - at(along_x, x + n - 1, z)) * across;
            const float dz_dz = (at(along_z, x, z + 1) - at(along_z, x, z + n - 1)) * across;
            const float dx_dz = (at(along_x, x, z + 1) - at(along_x, x, z + n - 1)) * across;
            worst_slope = std::max({worst_slope,
                                    std::abs(half(d + 0) - dh_dx), std::abs(half(d + 1) - dh_dz),
                                    std::abs(half(d + 2) - dx_dx), std::abs(half(d + 3) - dz_dz),
                                    std::abs(half(i + 3) - dx_dz)});
        }
    }

    // A 16-bit float holds about three and a half digits, so the surface can
    // be off by a thousandth of its height and no closer; a wrong transform
    // is off by the height itself.
    const float allowed = std::max(tallest, 0.1f) * 2e-3f;
    if (worst_surface <= allowed && worst_slope <= allowed * 4.0f) {
        FTK_CORE_INFO("WaterFftPass: VERIFIED. {}x{} sea up to {:.3f} m: surface within "
                        "{:.5f} m, derivatives within {:.5f}", n, n, tallest, worst_surface,
                        worst_slope);
    } else {
        FTK_CORE_ERROR("WaterFftPass: VERIFICATION FAILED. {}x{} sea up to {:.3f} m: surface "
                         "off by {:.5f} m, derivatives by {:.5f} (allowed {:.5f})", n, n, tallest,
                         worst_surface, worst_slope, allowed);
    }
    verify_readback_.reset();
}

} // namespace fjell
