#include "ui/texture_preview.hpp"

#include "ftk/app/icon_cache.hpp"
#include "ftk/app/imgui_layer.hpp"
#include "ftk/base/log.hpp"
#include "ftk/ui/kit/button.hpp"
#include "ftk/ui/kit/feedback.hpp"
#include "ui/icons.hpp"
#include "ftk/ui/kit/panel.hpp"
#include "ftk/ui/kit/viewport_toolbar.hpp"
#include "ftk/ui/theme.hpp"

#include <stb_image.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <system_error>

namespace fjell {

namespace {

constexpr float CHECKER = 12.0f;  // screen pixels a square

std::string file_size_text(std::uintmax_t bytes) {
    if (bytes < 1024) return std::format("{} B", bytes);
    if (bytes < 1024 * 1024) return std::format("{:.1f} KB", static_cast<double>(bytes) / 1024.0);
    return std::format("{:.1f} MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
}

// The squares behind the picture, fixed to the screen so they don't swim
// as the view moves, and only where the picture is.
void checkerboard(ImDrawList* dl, ImVec2 min, ImVec2 max) {
    const ImU32 light = ImGui::GetColorU32(ftk::theme::surface_active());
    dl->AddRectFilled(min, max, ImGui::GetColorU32(ftk::theme::surface_raised()));
    const float x0 = std::floor(min.x / CHECKER) * CHECKER;
    const float y0 = std::floor(min.y / CHECKER) * CHECKER;
    for (float y = y0; y < max.y; y += CHECKER) {
        const bool odd_row = static_cast<long>(std::floor(y / CHECKER)) % 2 != 0;
        for (float x = x0; x < max.x; x += CHECKER) {
            const bool odd = (static_cast<long>(std::floor(x / CHECKER)) % 2 != 0) != odd_row;
            if (!odd) continue;
            dl->AddRectFilled({std::max(x, min.x), std::max(y, min.y)},
                              {std::min(x + CHECKER, max.x), std::min(y + CHECKER, max.y)}, light);
        }
    }
}

} // namespace

void TexturePreview::open(std::string path) {
    path_ = std::move(path);
    open_ = true;
}

void TexturePreview::update() {
    if (!open_) {
        if (!loaded_path_.empty()) release();
        return;
    }
    if (loaded_path_ != path_) {
        load();
    } else if (mask_ != uploaded_mask_ && !pixels_.empty()) {
        upload();
    }
}

void TexturePreview::shutdown() {
    release();
    open_ = false;
}

void TexturePreview::load() {
    release();
    loaded_path_ = path_;
    failed_ = false;
    framed_ = false;
    mask_ = {};

    int w = 0;
    int h = 0;
    int channels = 0;
    stbi_uc* data = stbi_load(path_.c_str(), &w, &h, &channels, 4);
    if (data == nullptr) {
        FTK_CORE_WARN("Texture preview couldn't read {}: {}", path_, stbi_failure_reason());
        failed_ = true;
        return;
    }
    pixels_.assign(data, data + static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4);
    stbi_image_free(data);
    width_ = w;
    height_ = h;
    file_channels_ = channels;
    std::error_code ec;
    file_size_ = std::filesystem::file_size(path_, ec);
    if (ec) file_size_ = 0;
    upload();
}

void TexturePreview::upload() {
    if (icons_ == nullptr) return;
    // The frames still drawing the old picture keep it until they are done.
    const std::vector<uint8_t> shown = show_channels(pixels_, mask_);
    picture_ = icons_->upload_rgba(shown, static_cast<uint32_t>(width_), static_cast<uint32_t>(height_),
                                   "texture preview " + std::filesystem::path(path_).filename().string());
    uploaded_mask_ = mask_;
}

void TexturePreview::release() {
    picture_ = {};
    pixels_.clear();
    pixels_.shrink_to_fit();
    loaded_path_.clear();
    width_ = height_ = file_channels_ = 0;
}

void TexturePreview::fit() {
    view_.frame({0.0f, 0.0f}, {static_cast<float>(width_), static_cast<float>(height_)});
}

void TexturePreview::pixel_for_pixel() {
    // A window the canvas's size in pixels frames at one pixel a pixel,
    // about what is in the middle now.
    const ImVec2 size = view_.size();
    const glm::vec2 centre = view_.to_world({view_.origin().x + size.x * 0.5f, view_.origin().y + size.y * 0.5f});
    view_.frame(centre - glm::vec2{size.x, size.y} * 0.5f, centre + glm::vec2{size.x, size.y} * 0.5f);
}

void TexturePreview::draw() {
    if (!open_) return;
    ImGui::SetNextWindowSize({520.0f, 560.0f}, ImGuiCond_FirstUseEver);
    if (auto panel = ftk::ui::Panel(icon::texture, "Texture Preview", &open_)) {
        if (failed_) {
            ftk::ui::status(ftk::ui::Severity::Error, "The file couldn't be read as an image; the console says why.");
            return;
        }
        if (!picture_) return;  // read at the next update()

        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const float info_h = ImGui::GetFrameHeight() + ftk::theme::GAP_S;
        const ImVec2 size{std::max(avail.x, 1.0f), std::max(avail.y - info_h, 1.0f)};
        draw_canvas(origin, size);
        ImGui::SetCursorScreenPos({origin.x, origin.y + size.y + ftk::theme::GAP_S});
        draw_info();
    }
}

void TexturePreview::draw_canvas(ImVec2 origin, ImVec2 size) {
    view_.place(origin, size);
    // The pills are drawn over the canvas after it, and take the mouse there.
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##canvas", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    view_.input(hovered);
    if (!framed_) {
        if (!view_.recall("texture:" + path_)) fit();
        framed_ = true;
    }
    if (ftk::ui::canvas_has_keys()) {
        if (ImGui::IsKeyPressed(ImGuiKey_F, false)) fit();
        if (ImGui::IsKeyPressed(ImGuiKey_1, false)) pixel_for_pixel();
    }

    const ImVec2 max{origin.x + size.x, origin.y + size.y};
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(origin, max, true);
    dl->AddRectFilled(origin, max, ImGui::GetColorU32(ftk::theme::surface_sunken()));
    const ImVec2 a = view_.to_screen({0.0f, 0.0f});
    const ImVec2 b = view_.to_screen({static_cast<float>(width_), static_cast<float>(height_)});
    checkerboard(dl, {std::max(a.x, origin.x), std::max(a.y, origin.y)}, {std::min(b.x, max.x), std::min(b.y, max.y)});
    // Enlarged, each pixel is a square rather than a blur between them.
    const ImTextureID picture = view_.scale_x() >= 1.0f && icons_ != nullptr
                                    ? ftk::imgui_texture(picture_, icons_->pixel_sampler())
                                    : ftk::imgui_texture(picture_);
    dl->AddImage(picture, a, b);

    // The pixel under the mouse, and what it holds as read from the file.
    if (hovered && !view_.panning()) {
        const glm::vec2 at = view_.to_world(ImGui::GetMousePos());
        const int x = static_cast<int>(std::floor(at.x));
        const int y = static_cast<int>(std::floor(at.y));
        if (x >= 0 && y >= 0 && x < width_ && y < height_) {
            const std::size_t i = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) +
                                   static_cast<std::size_t>(x)) * 4;
            const std::string text = std::format("{}, {} \xc2\xb7 {} {} {} {}", x, y, pixels_[i], pixels_[i + 1],
                                                 pixels_[i + 2], pixels_[i + 3]);
            ftk::ui::canvas_readout(dl, ImGui::GetMousePos(), text.c_str(), origin, max);
        }
    }
    dl->PopClipRect();

    // Left: the channels. One alone shows in grey; the last one shown
    // stays on, since a picture of nothing tells nothing.
    if (auto pill = ftk::ui::ViewportPill("##channels", origin, max, ftk::ui::PillPlace::TopLeft)) {
        const auto channel = [&](const char* id, const char* letter, bool& on, const char* tooltip) {
            if (ftk::ui::toggle_button(id, letter, on, tooltip) && (!on || mask_.count() > 1)) on = !on;
        };
        channel("##r", "R", mask_.r, "Red");
        ImGui::SameLine();
        channel("##g", "G", mask_.g, "Green");
        ImGui::SameLine();
        channel("##b", "B", mask_.b, "Blue");
        ImGui::SameLine();
        channel("##a", "A", mask_.a, "Alpha");
    }
    // Right: how large it is shown, fitting it, and one pixel a pixel.
    if (auto pill = ftk::ui::ViewportPill("##zoom", origin, max, ftk::ui::PillPlace::TopRight)) {
        ImGui::AlignTextToFramePadding();
        ImGui::Text(" %.0f", view_.scale_x() * 100.0f);
        ImGui::SameLine(0.0f, 2.0f);
        ImGui::TextColored(ftk::theme::text_secondary(), "%% ");
        ImGui::SameLine();
        if (ftk::ui::icon_button("##fit", icon::frame_view, "Fit (F)")) fit();
        ImGui::SameLine();
        if (ftk::ui::icon_button("##one", "1:1", "Pixel for pixel (1)")) pixel_for_pixel();
    }
}

void TexturePreview::draw_info() {
    const std::filesystem::path path{path_};
    std::error_code ec;
    std::filesystem::path folder = std::filesystem::proximate(path.parent_path(), ec);
    if (ec) folder = path.parent_path();
    const std::string facts = std::format("{} \xc3\x97 {}  \xc2\xb7  {}  \xc2\xb7  {}  \xc2\xb7  {}", width_, height_,
                                          channels_name(file_channels_), file_size_text(file_size_),
                                          folder.generic_string());
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(path.filename().string().c_str());
    ImGui::SameLine(0.0f, ftk::theme::GAP_M * 1.5f);
    ImGui::TextColored(ftk::theme::text_secondary(), "%s", facts.c_str());
}

} // namespace fjell
