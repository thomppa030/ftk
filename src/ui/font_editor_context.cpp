#include "ui/font_editor_context.hpp"
#include "ui/asset_thumbnail_renderer.hpp"
#include "ui/dockspace_layout.hpp"
#include "ui/game_ui/font_atlas.hpp"
#include "ui/game_ui/font_library.hpp"
#include "ui/game_ui/font_sample.hpp"
#include "ui/game_ui/game_ui.hpp"
#include "core/commands.hpp"
#include "core/log.hpp"
#include "core/project_manager.hpp"
#include "renderer/gpu/deferred_deleter.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/vulkan_context.hpp"

#include <imgui.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <sstream>

namespace fjell {

namespace {

/// One recipe edit, in both directions: the whole recipe before and after,
/// since a recipe is a dozen fields and diffing them buys nothing.
class FontRecipeCommand final : public Command {
public:
    FontRecipeCommand(FontEditorContext& editor, FjFontRecipe before, FjFontRecipe after,
                      std::string description)
        : editor_{editor}, before_{std::move(before)}, after_{std::move(after)}
        , description_{std::move(description)} {}

    void execute() override { editor_.apply_recipe(after_); }
    void undo() override { editor_.apply_recipe(before_); }
    [[nodiscard]] std::string description() const override { return description_; }

private:
    FontEditorContext& editor_;
    FjFontRecipe before_;
    FjFontRecipe after_;
    std::string description_;
};

bool has_range(const FjFontRecipe& recipe, CharsetRange range) {
    return std::any_of(recipe.charset.begin(), recipe.charset.end(), [&](const CharsetRange& r) {
        return r.first == range.first && r.last == range.last;
    });
}

// The ranges that are not a named set, as the recipe would spell them.
std::string custom_ranges_text(const FjFontRecipe& recipe) {
    std::string out;
    for (const auto& range : recipe.charset) {
        bool named = false;
        for (const auto& set : named_charsets()) {
            if (set.range.first == range.first && set.range.last == range.last) named = true;
        }
        if (named) continue;
        if (!out.empty()) out += ' ';
        out += charset_entry_text(range);
    }
    return out;
}

void copy_to(char* buf, size_t size, const std::string& text) {
    std::strncpy(buf, text.c_str(), size - 1);
    buf[size - 1] = '\0';
}

} // namespace

FontEditorContext::FontEditorContext(FontEditorDeps deps) : deps_{deps} {
    copy_to(sample_buf_, sizeof(sample_buf_), "The quick brown fox jumps over 13 lazy dogs");
}

void FontEditorContext::init() {
    init_shared_panels(deps_.vulkan_context, deps_.project_mgr);
    history_panel().init(&command_history());
}

void FontEditorContext::shutdown() {
    if (preview_ui_) {
        preview_ui_->destroy();
        preview_ui_.reset();
    }
    preview_target_.reset();
}

std::vector<std::string> FontEditorContext::docked_window_names() const {
    return {ctx_title("Font Preview"), ctx_title("Font"), ctx_title("Stats"), ctx_title("Console")};
}

void FontEditorContext::setup_dockspace(ImGuiID main_area) {
    enum class EPanel { Preview, Font, Stats, Console };
    using N = DockNode;
    static const auto layout = N::horizontal_split(0.62f,
        N::vertical_split(0.7f,
            N::leaf({EPanel::Preview}),
            N::leaf({EPanel::Stats, EPanel::Console})),
        N::leaf({EPanel::Font}));
    build_dockspace(main_area, layout, docked_window_names());
}

void FontEditorContext::open_font(const std::string& recipe_path) {
    recipe_path_ = recipe_path;
    recipe_abs_path_ = deps_.project_mgr && deps_.project_mgr->active()
        ? deps_.project_mgr->active()->resolve(recipe_path)
        : recipe_path;
    name_ = std::filesystem::path(recipe_path).stem().string();

    std::vector<std::string> warnings;
    auto recipe = load_font_recipe(recipe_abs_path_, &warnings);
    for (const auto& warning : warnings) FJELL_CORE_WARN("Font editor: {}", warning);
    if (!recipe) {
        FJELL_CORE_ERROR("Font editor: {}", recipe.error());
        loaded_ = false;
        return;
    }
    recipe_ = *recipe;
    loaded_ = true;
    copy_to(source_buf_, sizeof(source_buf_), recipe_.source);
    copy_to(ranges_buf_, sizeof(ranges_buf_), custom_ranges_text(recipe_));
    cell_size_edit_ = static_cast<int>(recipe_.cell_size);
    px_range_edit_ = recipe_.px_range;
    sample_dirty_ = true;

    // Asking is what starts the load; the panels show its progress.
    (void)deps_.vulkan_context->font_library().get(recipe_abs_path_);
}

bool FontEditorContext::save() {
    if (!loaded_) return false;
    auto written = save_font_recipe(recipe_abs_path_, recipe_);
    if (!written) {
        FJELL_CORE_ERROR("Font editor: {}", written.error());
        return false;
    }
    return true;
}

void FontEditorContext::apply_recipe(const FjFontRecipe& recipe) {
    recipe_ = recipe;
    copy_to(source_buf_, sizeof(source_buf_), recipe_.source);
    copy_to(ranges_buf_, sizeof(ranges_buf_), custom_ranges_text(recipe_));
    cell_size_edit_ = static_cast<int>(recipe_.cell_size);
    px_range_edit_ = recipe_.px_range;
    if (!save()) return;
    // The library rebuilds the atlas from the file just written; the
    // thumbnail follows the recipe's timestamp on its own.
    deps_.vulkan_context->font_library().reload(recipe_abs_path_);
    if (deps_.asset_thumbnails) deps_.asset_thumbnails->invalidate(recipe_abs_path_);
    sample_dirty_ = true;
}

void FontEditorContext::commit(FjFontRecipe next, const char* what) {
    command_history().execute(std::make_unique<FontRecipeCommand>(
        *this, recipe_, std::move(next), std::string("Font: ") + what));
}

void FontEditorContext::draw(float dt) {
    draw_recipe_panel();
    draw_preview_panel();
    draw_shared_panels(dt);
}

void FontEditorContext::draw_recipe_panel() {
    if (!ImGui::Begin(ctx_title("Font").c_str())) {
        ImGui::End();
        return;
    }
    if (!loaded_) {
        ImGui::TextDisabled("Recipe could not be read, see the console");
        ImGui::End();
        return;
    }
    ImGui::TextDisabled("%s", recipe_path_.c_str());
    ImGui::Separator();

    // Source font file, relative to the recipe
    ImGui::TextUnformatted("Source");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##source", source_buf_, sizeof(source_buf_));
    if (ImGui::IsItemDeactivatedAfterEdit() && recipe_.source != source_buf_) {
        auto next = recipe_;
        next.source = source_buf_;
        commit(std::move(next), "source");
    }
    {
        auto source = font_source_path(recipe_abs_path_, recipe_);
        if (!std::filesystem::exists(source)) {
            ImGui::TextColored({0.95f, 0.5f, 0.3f, 1.0f}, "File not found beside the recipe");
        }
    }

    ImGui::Spacing();
    ImGui::TextUnformatted("Charset");
    for (const auto& set : named_charsets()) {
        bool on = has_range(recipe_, set.range);
        std::string label(set.name);
        char range_text[48];
        std::snprintf(range_text, sizeof(range_text), "  (U+%04X-%04X)", set.range.first, set.range.last);
        label += range_text;
        if (ImGui::Checkbox(label.c_str(), &on)) {
            auto next = recipe_;
            if (on) {
                next.charset.push_back(set.range);
            } else {
                std::erase_if(next.charset, [&](const CharsetRange& r) {
                    return r.first == set.range.first && r.last == set.range.last;
                });
            }
            commit(std::move(next), on ? "add charset" : "remove charset");
        }
    }
    ImGui::TextUnformatted("Ranges");
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Extra codepoints as U+XXXX or U+XXXX-YYYY, separated by spaces");
    }
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##ranges", ranges_buf_, sizeof(ranges_buf_));
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        auto next = recipe_;
        std::erase_if(next.charset, [](const CharsetRange& r) {
            for (const auto& set : named_charsets()) {
                if (set.range.first == r.first && set.range.last == r.last) return true;
            }
            return false;
        });
        std::istringstream tokens(ranges_buf_);
        std::string token;
        bool ok = true;
        while (tokens >> token) {
            auto parsed = parse_charset_entry(token);
            if (!parsed) {
                FJELL_CORE_WARN("Font editor: charset {}", parsed.error());
                ok = false;
                continue;
            }
            next.charset.push_back(*parsed);
        }
        if (ok) {
            commit(std::move(next), "ranges");
        } else {
            copy_to(ranges_buf_, sizeof(ranges_buf_), custom_ranges_text(recipe_));
        }
    }

    ImGui::Spacing();
    ImGui::TextUnformatted("Cell size (px)");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputInt("##cell", &cell_size_edit_, 0, 0);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        int clamped = std::clamp(cell_size_edit_, 8, 512);
        if (static_cast<uint32_t>(clamped) != recipe_.cell_size) {
            auto next = recipe_;
            next.cell_size = static_cast<uint32_t>(clamped);
            next.px_range = std::min(next.px_range, static_cast<float>(clamped) * 0.5f - 0.5f);
            commit(std::move(next), "cell size");
        } else {
            cell_size_edit_ = clamped;
        }
    }
    ImGui::TextUnformatted("Distance range (px)");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputFloat("##range", &px_range_edit_, 0.0f, 0.0f, "%.1f");
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        float clamped = std::clamp(px_range_edit_, 1.0f, static_cast<float>(recipe_.cell_size) * 0.5f - 0.5f);
        if (clamped != recipe_.px_range) {
            auto next = recipe_;
            next.px_range = clamped;
            commit(std::move(next), "distance range");
        } else {
            px_range_edit_ = clamped;
        }
    }
    bool kerning = recipe_.kerning;
    if (ImGui::Checkbox("Bake kerning", &kerning)) {
        auto next = recipe_;
        next.kerning = kerning;
        commit(std::move(next), "kerning");
    }

    ImGui::Separator();
    auto& fonts = deps_.vulkan_context->font_library();
    const FontAtlas* atlas = fonts.get(recipe_abs_path_);
    switch (fonts.status(recipe_abs_path_)) {
        case FontLibrary::Status::unknown:
        case FontLibrary::Status::pending:
            ImGui::TextDisabled("Baking...");
            break;
        case FontLibrary::Status::failed:
            ImGui::TextColored({0.95f, 0.5f, 0.3f, 1.0f}, "Bake failed, see the console");
            break;
        case FontLibrary::Status::ready:
            break;
    }
    if (atlas) {
        ImGui::Text("Atlas %ux%u, %zu glyphs", atlas->atlas_width(), atlas->atlas_height(),
                    atlas->glyph_count());
        auto tex = const_cast<FontAtlas*>(atlas)->imgui_texture();
        if (tex) {
            float avail = ImGui::GetContentRegionAvail().x;
            float scale = avail / static_cast<float>(std::max(atlas->atlas_width(), 1u));
            ImGui::Image(reinterpret_cast<ImTextureID>(tex),
                         {avail, static_cast<float>(atlas->atlas_height()) * scale});
        }
    }
    ImGui::End();
}

void FontEditorContext::draw_preview_panel() {
    if (!ImGui::Begin(ctx_title("Font Preview").c_str())) {
        ImGui::End();
        return;
    }
    ImGui::SetNextItemWidth(-140.0f);
    ImGui::InputText("##sample", sample_buf_, sizeof(sample_buf_));
    if (ImGui::IsItemDeactivatedAfterEdit()) sample_dirty_ = true;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::DragFloat("##size", &sample_size_, 1.0f, 8.0f, 200.0f, "%.0f px")) {
        sample_dirty_ = true;
    }

    // What the next render should be sized to.
    ImVec2 avail = ImGui::GetContentRegionAvail();
    wanted_width_ = static_cast<uint32_t>(std::max(avail.x, 64.0f));
    wanted_height_ = static_cast<uint32_t>(std::max(avail.y, 64.0f));

    if (preview_target_) {
        auto tex = preview_target_->imgui_texture();
        if (tex) {
            ImGui::Image(reinterpret_cast<ImTextureID>(tex),
                         {static_cast<float>(preview_width_), static_cast<float>(preview_height_)});
        }
    } else {
        ImGui::TextDisabled("Waiting for the atlas");
    }
    ImGui::End();
}

void FontEditorContext::render_previews(VulkanContext& vk, Scene* /*scene*/, float /*dt*/,
                                        const DirectionalLight& /*light*/, ThreadPool& /*pool*/) {
    if (!loaded_) return;
    auto& fonts = vk.font_library();
    // A rebuild in flight means the sample would draw in the fallback; wait
    // for the atlas and draw once with the real one.
    if (fonts.status(recipe_abs_path_) != FontLibrary::Status::ready) {
        sample_dirty_ = true;
        return;
    }
    if (wanted_width_ == 0 || wanted_height_ == 0) return;
    bool resized = wanted_width_ != preview_width_ || wanted_height_ != preview_height_;
    if (!sample_dirty_ && !resized) return;
    sample_dirty_ = false;

    if (!preview_ui_) {
        preview_ui_ = std::make_unique<GameUi>();
        vk.init_game_ui(*preview_ui_, OffscreenPass::COLOR_FORMAT);
        preview_ui_->set_resource_manager(deps_.resource_manager);
    }
    if (resized || !preview_target_) {
        // The previous frame's ImGui draw may still read the old target, so
        // it goes through the deferred deleter rather than away at once.
        if (preview_target_) vk.gpu().deferred_deleter().retire(std::move(preview_target_));
        preview_width_ = wanted_width_;
        preview_height_ = wanted_height_;
        preview_target_ = std::make_unique<OffscreenPass>(vk.device_ref(), vk.allocator(),
                                                          preview_width_, preview_height_,
                                                          VK_SAMPLE_COUNT_1_BIT);
    }
    load_font_sample(*preview_ui_, recipe_abs_path_, sample_buf_, sample_size_);
    preview_ui_->set_design_frame({static_cast<float>(preview_width_),
                                   static_cast<float>(preview_height_)});
    preview_ui_->update(static_cast<float>(preview_width_), static_cast<float>(preview_height_),
                        vk.current_frame_index(), 0.0f, nullptr);
    vk.render_game_ui_preview(*preview_ui_, *preview_target_);
}

} // namespace fjell
