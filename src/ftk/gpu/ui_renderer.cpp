#include "ftk/gpu/ui_renderer.hpp"

#include "ftk/base/log.hpp"
#include "ftk/gpu/binding.hpp"
#include "ftk/gpu/command_list.hpp"
#include "ftk/gpu/pipeline.hpp"
#include "ftk/gpu/render_encoder.hpp"
#include "ftk/gpu/sampler.hpp"
#include "ftk/gpu/transition.hpp"
#include "ftk/gpu/upload.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <span>
#include <vector>

namespace ftk::gpu {

namespace {

// The shaders, compiled into the library.
constexpr uint32_t DRAW_VERTEX[] = {
#include "ui.vert.spv.inc"
};
constexpr uint32_t DRAW_FRAGMENT[] = {
#include "ui.frag.spv.inc"
};
constexpr uint32_t COPY_VERTEX[] = {
#include "ui_copy.vert.spv.inc"
};
constexpr uint32_t COPY_FRAGMENT[] = {
#include "ui_copy.frag.spv.inc"
};

// What one draw reads: the atlas's pages and a batch of pictures, as many as
// ui.frag declares.
constexpr std::size_t ATLAS_SLOTS = 4;
constexpr std::size_t PICTURE_SLOTS = 8;

// The canvas holds sRGB values as they are, so they blend as such.
constexpr Format CANVAS_FORMAT = Format::rgba8_unorm;

// ui.vert's and ui.frag's push data.
struct View {
    glm::vec2 size;
    float scale;
    float unused;
};

// Shapes from `first` up to `end` that read the same pictures.
struct Batch {
    std::size_t first{0};
    std::size_t end{0};
    std::array<TextureView, PICTURE_SLOTS> views{};
};

} // namespace

Result<std::unique_ptr<UiRenderer>> UiRenderer::create(Device& device, Format target_format) {
    std::unique_ptr<UiRenderer> renderer(new UiRenderer(device));
    auto draw = device.create(GraphicsPipelineDesc{
        .vertex = std::span<const uint32_t>(DRAW_VERTEX),
        .fragment = std::span<const uint32_t>(DRAW_FRAGMENT),
        .color = {{CANVAS_FORMAT, Blend::alpha}},
        .name = "ui",
    });
    if (!draw) return make_error(draw.error());
    renderer->draw_ = std::move(*draw);
    auto copy = device.create(GraphicsPipelineDesc{
        .vertex = std::span<const uint32_t>(COPY_VERTEX),
        .fragment = std::span<const uint32_t>(COPY_FRAGMENT),
        .color = {{target_format, Blend::none}},
        .name = "ui_copy",
    });
    if (!copy) return make_error(copy.error());
    renderer->copy_ = std::move(*copy);
    renderer->linear_ = device.sampler({.filter = Filter::linear, .mip_filter = Filter::nearest, .address = Address::clamp});
    renderer->nearest_ = device.sampler({.filter = Filter::nearest, .mip_filter = Filter::nearest, .address = Address::clamp});
    auto blank = device.create(TextureDesc{
        .format = Format::rgba8_unorm,
        .use = TextureUse::sampled,
        .initial = Clear{1.0f, 1.0f, 1.0f, 1.0f},
        .name = "ui_blank",
    });
    if (!blank) return make_error(blank.error());
    renderer->blank_ = std::move(*blank);
    return renderer;
}

UiRenderer::~UiRenderer() = default;

ui::Texture UiRenderer::texture(const TextureView& view) {
    const uint64_t id = next_picture_++;
    pictures_[id] = view;
    return ui::Texture{id};
}

void UiRenderer::forget(ui::Texture texture) {
    pictures_.erase(texture.id);
}

Result<> UiRenderer::upload_atlas(text::GlyphAtlas& atlas) {
    while (pages_.size() < atlas.page_count()) {
        auto page = device_->create(TextureDesc{
            .format = Format::r8_unorm,
            .width = atlas.page_size(),
            .height = atlas.page_size(),
            .use = TextureUse::sampled,
            .initial = Clear{0.0f},
            .name = "ui_atlas",
        });
        if (!page) return make_error(page.error());
        pages_.push_back(std::move(*page));
    }
    // Each region the atlas wrote, its rows packed together.
    for (const text::AtlasWrite& write : atlas.take_writes()) {
        const std::span<const uint8_t> page = atlas.page(write.page);
        const std::size_t stride = atlas.page_size();
        region_.resize(static_cast<std::size_t>(write.rect.width) * write.rect.height);
        for (uint16_t row = 0; row < write.rect.height; ++row) {
            std::memcpy(region_.data() + static_cast<std::size_t>(row) * write.rect.width,
                        page.data() + (write.rect.y + row) * stride + write.rect.x, write.rect.width);
        }
        device_->upload().to_texture(
            pages_[write.page].get(),
            {.x = write.rect.x, .y = write.rect.y, .width = write.rect.width, .height = write.rect.height},
            std::as_bytes(std::span(region_)),
            {.before = Access::sampled_fragment, .after = Access::sampled_fragment});
    }
    return {};
}

Result<> UiRenderer::fit_canvas(glm::uvec2 size) {
    if (canvas_ && canvas_size_ == size) return {};
    auto canvas = device_->create(TextureDesc{
        .format = CANVAS_FORMAT,
        .width = size.x,
        .height = size.y,
        .use = TextureUse::color_target | TextureUse::sampled,
        .name = "ui_canvas",
    });
    if (!canvas) return make_error(canvas.error());
    // The one it replaces goes once the frames drawing with it are done.
    canvas_ = std::move(*canvas);
    canvas_size_ = size;
    canvas_used_ = false;
    return {};
}

void UiRenderer::render(CommandList& cmd, const TextureView& target, glm::uvec2 size, const ui::core::RootOutput& root,
                        text::GlyphAtlas& atlas, ui::Color background) {
    if (size.x == 0 || size.y == 0 || root.draw == nullptr) return;
    if (auto uploaded = upload_atlas(atlas); !uploaded) FTK_GFX_ERROR("UI: the glyph atlas: {}", uploaded.error());
    if (auto fitted = fit_canvas(size); !fitted) {
        FTK_GFX_ERROR("UI: the canvas: {}", fitted.error());
        return;
    }
    const ui::core::DrawData& data = *root.draw;

    // The shapes, each picture renumbered to its slot in its draw's batch:
    // a new batch begins where a picture finds every slot taken.
    primitives_.assign(data.primitives.begin(), data.primitives.end());
    std::vector<Batch> batches(1);
    std::array<uint32_t, PICTURE_SLOTS> slot_of{};  // DrawData::textures index per slot
    std::size_t used = 0;
    for (std::size_t i = 0; i < primitives_.size(); ++i) {
        ui::core::Primitive& p = primitives_[i];
        if (p.kind != ui::core::PrimitiveKind::Rect || p.texture == 0) continue;
        const auto taken = std::find(slot_of.begin(), slot_of.begin() + static_cast<std::ptrdiff_t>(used), p.texture);
        std::size_t slot = static_cast<std::size_t>(taken - slot_of.begin());
        if (slot == used) {
            if (used == PICTURE_SLOTS) {
                batches.back().end = i;
                batches.push_back({.first = i});
                used = 0;
            }
            slot = used++;
            slot_of[slot] = p.texture;
            const auto found = pictures_.find(data.textures[p.texture].id);
            batches.back().views[slot] = found != pictures_.end() ? found->second : TextureView(blank_.get());
        }
        p.texture = static_cast<uint32_t>(slot + 1);
    }
    batches.back().end = primitives_.size();

    // The canvas, read by the last frame's copy, cleared and drawn into.
    const Transition to_draw{.texture = canvas_.get(),
                             .wait_for = canvas_used_ ? AccessSet{Access::sampled_fragment} : AccessSet{},
                             .visible_to = Access::color_attachment,
                             .to = Access::color_attachment,
                             .name = "ui_canvas"};
    cmd.transition({&to_draw, 1});
    {
        auto pass = cmd.render({.color = {{.view = canvas_.get(),
                                           .load = Load::clear,
                                           .clear = Clear{background.r, background.g, background.b, 1.0f}}}});
        if (!primitives_.empty()) {
            pass.set_pipeline(draw_.get());
            pass.push(View{.size = root.size, .scale = root.scale, .unused = 0.0f});
            const BufferRange shapes = pass.transient(primitives_);
            const BufferRange clips = pass.transient(data.clips);
            for (const Batch& batch : batches) {
                if (batch.end == batch.first) continue;
                std::array<BindEntry, 2 + ATLAS_SLOTS + PICTURE_SLOTS> entries{};
                std::size_t n = 0;
                entries[n++] = {.name = "UiPrimitives", .resource = storage(shapes)};
                entries[n++] = {.name = "UiClips", .resource = storage(clips)};
                for (uint32_t page = 0; page < ATLAS_SLOTS; ++page) {
                    const TextureView view = page < pages_.size() ? TextureView(pages_[page].get()) : TextureView(blank_.get());
                    entries[n++] = {.name = "atlas", .resource = sampled(view, linear_), .element = page};
                }
                for (uint32_t slot = 0; slot < PICTURE_SLOTS; ++slot) {
                    const TextureView view = batch.views[slot].texture.valid() ? batch.views[slot] : TextureView(blank_.get());
                    entries[n++] = {.name = "pictures", .resource = sampled(view, linear_), .element = slot};
                }
                pass.bind(std::span<const BindEntry>(entries.data(), n));
                pass.draw(static_cast<uint32_t>(batch.end - batch.first) * 6, 1, static_cast<uint32_t>(batch.first) * 6, 0);
            }
        }
    }

    // Copied into the target, decoded so the sRGB target stores the values as
    // they were.
    const Transition to_read{.texture = canvas_.get(),
                             .wait_for = Access::color_attachment,
                             .flush = Access::color_attachment,
                             .visible_to = Access::sampled_fragment,
                             .from = Access::color_attachment,
                             .to = Access::sampled_fragment,
                             .name = "ui_canvas"};
    cmd.transition({&to_read, 1});
    canvas_used_ = true;
    {
        auto pass = cmd.render({.color = {{.view = target, .load = Load::discard}}, .width = size.x, .height = size.y});
        pass.set_pipeline(copy_.get());
        pass.bind({{"canvas", sampled(canvas_.get(), nearest_)}});
        pass.draw(3);
    }
}

} // namespace ftk::gpu
