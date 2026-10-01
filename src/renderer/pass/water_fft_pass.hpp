#pragma once

#include "ftk/gpu/buffer.hpp"
#include "ftk/gpu/command_list.hpp"
#include "ftk/gpu/format.hpp"
#include "ftk/gpu/owned.hpp"
#include "ftk/gpu/pipeline.hpp"
#include "ftk/gpu/sampler.hpp"
#include "ftk/gpu/texture.hpp"
#include "renderer/pass/render_pass.hpp"
#include "renderer/renderer_constants.hpp"
#include "renderer/resources/fjwater.hpp"
#include "renderer/resources/water_spectrum.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace fjell {

class ThreadPool;

// Simulates a sea from a spectrum: the patch of water an `FftContributor`
// describes, carried forward to now each frame and transformed into a tiling
// texture of where its surface has moved to.
//
// The spectrum is drawn on the CPU (water_spectrum.cpp) whenever the sea it
// describes changes, and everything per frame is here: every wavevector is
// turned to the current time, an inverse FFT takes the three displacement
// spectra to the surface, and the result is gathered with its derivatives and
// mipped so that a far-off vertex reads a sea averaged over the ground it
// stands for rather than one texel of it.
//
// The three images the water samples — this frame's displacement, the one
// before it and the derivatives — are handed to the graph as
// "water_displacement", "water_displacement_prev" and "water_derivatives";
// the graph moves them into the step's layouts and its readers declare their
// reads. The spectra the transform works in, and the mip chain inside a step,
// stay the pass's own. `stepped_this_frame_` because the sea is one thing in
// the world and record() runs once per viewport.
class WaterFftPass : public RenderPass {
public:
    /// What the transform works in. Two complex numbers a texel; 32-bit
    /// because the sum across a 256-wide grid loses a 16-bit float's precision
    /// long before the last stage.
    static constexpr gpu::Format SPECTRUM_FORMAT = gpu::Format::rgba32_float;
    /// What the surface is sampled from. Every device filters it linearly,
    /// which 32-bit float does not promise, and a millimetre at the height of
    /// a storm wave is inside what it holds.
    static constexpr gpu::Format SURFACE_FORMAT = gpu::Format::rgba16_float;
    static constexpr std::uint32_t MIN_RESOLUTION = 16;
    static constexpr std::uint32_t MAX_RESOLUTION = 1024;

    Result<> create(const PassResources& resources) override;
    void destroy() override;
    void prepare_frame(const FrameContext& ctx, GpuContext& gpu) override;
    void record(const FrameContext& ctx) override;
    [[nodiscard]] PassSetup setup() const override;
    void declare(PassBuilder& builder, const DeclareContext& ctx) override;
    void collect_exports(DeclareContext& ctx) override;

    /// The sea to simulate, as it has been drawn, or null for none. Compared
    /// against the one already running, so handing over the same sea every
    /// frame costs nothing; a new spectrum is uploaded, and one of a different
    /// resolution rebuilds the images.
    ///
    /// The pass animates a sea and does not decide which: drawing the spectrum
    /// belongs to whoever derives the field the world runs on, so that physics
    /// floats things on the same draw and a machine with no renderer can make
    /// it too.
    void set_sea(const FjWaterData::SimulatedSea* sea);

    /// Whether there is a simulated surface to sample this frame.
    [[nodiscard]] bool active() const { return spectrum_ != nullptr && images_ready_; }
    /// Metres the textures tile over.
    [[nodiscard]] float patch_size() const { return patch_size_; }
    /// xyz displacement in world axes, w d(displacement x)/dz; this frame's
    /// and the one before it, for the surface's own motion.
    ///
    /// Always something valid to bind. With no sea running these are a texel
    /// of still water, so a water shader's descriptor set never has a hole in
    /// it for want of a simulation.
    [[nodiscard]] gpu::Texture displacement() const {
        return active() ? displacement_[cur_].get() : still_.get();
    }
    [[nodiscard]] gpu::Texture previous_displacement() const {
        return active() ? displacement_[1 - cur_].get() : still_.get();
    }
    /// x dh/dx, y dh/dz, z d(displacement x)/dx, w d(displacement z)/dz.
    [[nodiscard]] gpu::Texture derivatives() const {
        return active() ? derivatives_.get() : still_.get();
    }
    /// Linear, repeating, across the whole mip chain.
    [[nodiscard]] gpu::Sampler sampler() const { return sampler_; }

private:
    struct EvolvePush {
        std::uint32_t resolution;
        std::uint32_t log2_resolution;
        float k_spacing;
        float time;
        float choppiness;
    };
    struct ButterflyPush {
        std::uint32_t resolution;
        std::uint32_t stage;
        std::uint32_t vertical;
    };
    struct AssemblePush {
        std::uint32_t resolution;
        float texel_size;
    };

    /// Makes the textures and buffers for `resolution_`. Nothing is made
    /// and the reason is given when any of them cannot be.
    [[nodiscard]] Result<> build_images();
    /// Releases them; the frames still reading them keep them until done.
    void retire_images();
    /// Which of the two displacement images holds this frame's surface once
    /// the step has run: the one it writes next if it has yet to run this
    /// frame, else the one it wrote. What the exports and the views agree on.
    [[nodiscard]] std::uint32_t written_index() const {
        return active() && !stepped_this_frame_ ? 1 - cur_ : cur_;
    }

    /// A surface's top mip has been written as storage, and after it `from`
    /// if not storage: fills the rest of the chain from it, leaving it all
    /// as a copy target.
    void generate_mips(gpu::CommandList& cmd, gpu::Texture surface, gpu::AccessSet from) const;

    /// Asked for through FJELL_VERIFY_WATER_FFT: simulate a fixed sea at a
    /// fixed time, read the surface back, and hold it to the CPU's plain
    /// Fourier sum of the same spectrum.
    void begin_verification();
    /// Copies this frame's surface and derivatives, which the assemble step
    /// has just written, for finish_verification() to read, and leaves both
    /// as copy sources. False, with nothing recorded, when there is nowhere
    /// to copy them.
    [[nodiscard]] bool record_verification_copy(gpu::CommandList& cmd);
    void finish_verification();

    gpu::Device* gpu_device_{nullptr};

    gpu::Owned<gpu::ComputePipeline> evolve_;
    gpu::Owned<gpu::ComputePipeline> butterfly_;
    gpu::Owned<gpu::ComputePipeline> assemble_;
    gpu::Sampler sampler_{};

    /// The sea being animated: its spectrum, the patch it tiles over and how
    /// hard its crests are pinched. Null for none.
    std::shared_ptr<const water::SpectrumField> spectrum_;
    float patch_size_{1.0f};
    float choppiness_{1.0f};
    std::uint32_t resolution_{0};
    std::uint32_t mips_{1};
    float k_spacing_{0.0f};
    bool images_ready_{false};
    /// Set when the images are new, so their first use starts from nothing
    /// rather than what they are otherwise left as.
    bool images_fresh_{true};
    float time_{0.0f};
    bool stepped_this_frame_{false};

    /// Which frame slots still hold an older spectrum than `spectrum_`. A
    /// buffer per slot because the GPU may still be reading last frame's while
    /// this frame's is written.
    std::array<gpu::Owned<gpu::Buffer>, MAX_FRAMES_IN_FLIGHT> amplitude_buffers_;
    std::array<bool, MAX_FRAMES_IN_FLIGHT> amplitudes_stale_{};

    // The transform ping-pongs between two pairs of images: every stage reads
    // two texels of the stage before to write one, so it cannot work in place.
    std::array<gpu::Owned<gpu::Texture>, 2> spectra_a_;
    std::array<gpu::Owned<gpu::Texture>, 2> spectra_b_;
    // This frame's surface and last frame's, swapped each step.
    std::array<gpu::Owned<gpu::Texture>, 2> displacement_;
    gpu::Owned<gpu::Texture> derivatives_;
    std::uint32_t cur_{0};
    /// One texel of zeros: what there is to sample when no sea is running.
    /// Cleared when made, and never written again.
    gpu::Owned<gpu::Texture> still_;

    bool verifying_{false};
    /// The fixed sea the verification runs, kept alive for the pass to animate.
    std::optional<FjWaterData::SimulatedSea> verify_sea_;
    int verify_frames_left_{-1};
    gpu::Owned<gpu::Buffer> verify_readback_;
};

} // namespace fjell
