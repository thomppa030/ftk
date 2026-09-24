#include "ui/font_editor_context.hpp"
#include "core/commands.hpp"
#include "core/log.hpp"
#include "core/project_manager.hpp"
#include "renderer/gpu/deferred_deleter.hpp"
#include "renderer/offscreen_pass.hpp"
#include "renderer/vulkan_context.hpp"
#include "ui/asset_thumbnail_renderer.hpp"
#include "ui/game_ui/font_atlas.hpp"
#include "ui/game_ui/font_library.hpp"
#include "ui/game_ui/font_sample.hpp"
#include "ui/game_ui/game_ui.hpp"
#include "ui/kit/feedback.hpp"
#include "ui/kit/field.hpp"
#include "ui/kit/icons.hpp"
#include "ui/kit/row.hpp"
#include "ui/kit/section.hpp"
#include "ui/kit/text_field.hpp"
#include "ui/panel_widget.hpp"

#include <imgui.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <format>
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

} // namespace

FontEditorContext::FontEditorContext(FontEditorDeps deps) : deps_{deps} {}

EditorContext::DockPreset FontEditorContext::dock_preset() const {
    return {.preview = {"Preview"}, .inspector = {"Properties"}};
}

void FontEditorContext::init() {
    init_shared_panels(deps_.vulkan_context);
    history_panel().init(&command_history());
}

void FontEditorContext::shutdown() {
    // Edits never saved were only ever previewed: the font goes back to
    // what its file holds.
    if (loaded_ && has_unsaved_changes()) deps_.vulkan_context->font_library().reload(recipe_abs_path_);
    if (preview_ui_) {
        preview_ui_->destroy();
        preview_ui_.reset();
    }
    preview_target_.reset();
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
    em_size_edit_ = static_cast<int>(recipe_.em_size);
    px_range_edit_ = recipe_.px_range;
    sample_dirty_ = true;
    document_opened();

    // Asking is what starts the load; the panels show its progress.
    (void)deps_.vulkan_context->font_library().get(recipe_abs_path_);
}

std::string FontEditorContext::document() const {
    return loaded_ ? font_recipe_text(recipe_) : std::string{};
}

Result<> FontEditorContext::write() {
    if (!loaded_) return make_error("no font is open");
    if (auto written = save_font_recipe(recipe_abs_path_, recipe_); !written) return written;
    // The atlas is baked again from the file, into the cache everything
    // else reads; the thumbnail follows the recipe's timestamp.
    deps_.vulkan_context->font_library().reload(recipe_abs_path_);
    if (deps_.asset_thumbnails) deps_.asset_thumbnails->invalidate(recipe_abs_path_);
    return {};
}

Result<> FontEditorContext::restore(const std::string& text) {
    auto recipe = font_recipe_from_text(text, recipe_path_, nullptr);
    if (!recipe) return make_error(recipe.error());
    apply_recipe(*recipe);
    return {};
}

void FontEditorContext::apply_recipe(const FjFontRecipe& recipe) {
    recipe_ = recipe;
    em_size_edit_ = static_cast<int>(recipe_.em_size);
    px_range_edit_ = recipe_.px_range;
    // Previewed, not saved: the font shows the edit everywhere it draws,
    // and the file changes on Save.
    deps_.vulkan_context->font_library().preview(recipe_abs_path_, recipe_);
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
    auto panel = Panel(ui::icon::font, ctx_title("Properties").c_str());
    if (!panel) return;
    if (!loaded_) {
        ui::empty_state(ui::icon::font, "The recipe couldn't be read", "The console says why");
        return;
    }

    if (auto t = ui::PropertyTable("##font")) {
        ui::row("Font file", "The .ttf or .otf the atlas is baked from, next to the recipe.", [&] {
            std::string source = recipe_.source;
            if (ui::text_field("##source", source).committed && source != recipe_.source) {
                auto next = recipe_;
                next.source = source;
                commit(std::move(next), "font file");
            }
        });
        if (!std::filesystem::exists(font_source_path(recipe_abs_path_, recipe_))) {
            ui::status(ui::Severity::Warning, "No such file beside the recipe");
        }
    }

    ui::subheading("Characters");
    if (auto t = ui::PropertyTable("##charset")) {
        for (const auto& set : named_charsets()) {
            char range_text[48];
            std::snprintf(range_text, sizeof(range_text), "U+%04X to U+%04X", set.range.first, set.range.last);
            const std::string name(set.name);
            ui::row(name.c_str(), range_text, [&] {
                bool on = has_range(recipe_, set.range);
                if (ui::checkbox("##set", on).changed) {
                    auto next = recipe_;
                    if (on) {
                        next.charset.push_back(set.range);
                    } else {
                        std::erase_if(next.charset, [&](const CharsetRange& r) {
                            return r.first == set.range.first && r.last == set.range.last;
                        });
                    }
                    commit(std::move(next), on ? "add characters" : "remove characters");
                }
            });
        }
        ui::row("More ranges", "Extra codepoints as U+XXXX or U+XXXX-YYYY, separated by spaces.", [&] {
            std::string ranges = custom_ranges_text(recipe_);
            if (!ui::text_field("##ranges", ranges).committed) return;
            auto next = recipe_;
            std::erase_if(next.charset, [](const CharsetRange& r) {
                for (const auto& set : named_charsets()) {
                    if (set.range.first == r.first && set.range.last == r.last) return true;
                }
                return false;
            });
            std::istringstream tokens(ranges);
            std::string token;
            bool ok = true;
            while (tokens >> token) {
                auto parsed = parse_charset_entry(token);
                if (!parsed) {
                    FJELL_CORE_WARN("Font editor: characters {}", parsed.error());
                    ok = false;
                    continue;
                }
                next.charset.push_back(*parsed);
            }
            if (ok) commit(std::move(next), "ranges");
        });
    }

    ui::subheading("Atlas");
    if (auto t = ui::PropertyTable("##atlas")) {
        ui::row("Em size", "Atlas pixels per em, for every glyph whatever the charset.", [&] {
            if (ui::drag_int("##em", em_size_edit_, 0.5f, 8, 512, ui::Unit::Pixels).committed &&
                static_cast<uint32_t>(em_size_edit_) != recipe_.em_size) {
                auto next = recipe_;
                next.em_size = static_cast<uint32_t>(em_size_edit_);
                next.px_range = std::min(next.px_range, static_cast<float>(em_size_edit_) * 0.5f - 0.5f);
                commit(std::move(next), "em size");
            }
        });
        ui::row("Distance range", "How far the edge's distance is kept, in atlas pixels.", [&] {
            const float limit = static_cast<float>(recipe_.em_size) * 0.5f - 0.5f;
            if (ui::drag("##range", px_range_edit_,
                         {.speed = 0.1f, .min = 1.0f, .max = limit, .unit = ui::Unit::Pixels, .format = "%.1f"})
                    .committed &&
                px_range_edit_ != recipe_.px_range) {
                auto next = recipe_;
                next.px_range = px_range_edit_;
                commit(std::move(next), "distance range");
            }
        });
        ui::row("Kerning", [&] {
            bool kerning = recipe_.kerning;
            if (ui::checkbox("##kerning", kerning).changed) {
                auto next = recipe_;
                next.kerning = kerning;
                commit(std::move(next), "kerning");
            }
        });

        auto& fonts = deps_.vulkan_context->font_library();
        const FontAtlas* atlas = fonts.get(recipe_abs_path_);
        const FontLibrary::Status status = fonts.status(recipe_abs_path_);
        ui::row("Baked", [&] {
            if (status == FontLibrary::Status::ready && atlas != nullptr) {
                const std::string text = std::format("{} \xc3\x97 {} px, {} glyphs", atlas->atlas_width(),
                                                     atlas->atlas_height(), atlas->glyph_count());
                ui::readout(text.c_str());
            } else if (status == FontLibrary::Status::failed) {
                ui::readout("No");
            } else {
                ui::readout("Baking");
            }
        });
        if (status == FontLibrary::Status::failed) {
            ui::status(ui::Severity::Error, "The bake failed; the console says why");
        }
    }
    const FontAtlas* atlas = deps_.vulkan_context->font_library().get(recipe_abs_path_);
    if (atlas != nullptr) {
        if (auto tex = const_cast<FontAtlas*>(atlas)->imgui_texture()) {
            const float avail = ImGui::GetContentRegionAvail().x;
            const float scale = avail / static_cast<float>(std::max(atlas->atlas_width(), 1u));
            ImGui::Image(reinterpret_cast<ImTextureID>(tex),
                         {avail, static_cast<float>(atlas->atlas_height()) * scale});
        }
    }
}

void FontEditorContext::draw_preview_panel() {
    auto panel = Panel(ui::icon::preview, ctx_title("Preview").c_str());
    if (!panel) return;
    if (auto t = ui::PropertyTable("##sample")) {
        ui::row("Sample", [&] {
            if (ui::text_field("##sample", sample_text_).committed) sample_dirty_ = true;
        });
        ui::row("Size", [&] {
            if (ui::drag("##size", sample_size_,
                         {.speed = 1.0f, .min = 8.0f, .max = 200.0f, .unit = ui::Unit::Pixels, .format = "%.0f"})
                    .changed) {
                sample_dirty_ = true;
            }
        });
    }

    // What the next render should be sized to.
    ImVec2 avail = ImGui::GetContentRegionAvail();
    wanted_width_ = static_cast<uint32_t>(std::max(avail.x, 64.0f));
    wanted_height_ = static_cast<uint32_t>(std::max(avail.y, 64.0f));

    if (preview_target_) {
        if (auto tex = preview_target_->imgui_texture()) {
            ImGui::Image(reinterpret_cast<ImTextureID>(tex),
                         {static_cast<float>(preview_width_), static_cast<float>(preview_height_)});
        }
    } else {
        ui::empty_state(ui::icon::font, "Baking", "The sample draws once the atlas is ready");
    }
}

void FontEditorContext::render_previews(VulkanContext& vk, Scene* /*scene*/, float /*dt*/,
                                        const EnvironmentState& /*environment*/, ThreadPool& /*pool*/) {
    if (!loaded_) return;
    auto& fonts = vk.font_library();
    // A rebuild in flight means the sample would draw in the fallback; wait
    // for the atlas and draw once with the real one.
    if (fonts.status(recipe_abs_path_) != FontLibrary::Status::ready) {
        sample_dirty_ = true;
        return;
    }
    if (wanted_width_ == 0 || wanted_height_ == 0) return;
    const FontAtlas* atlas = fonts.get(recipe_abs_path_);
    if (atlas != nullptr && atlas->generation() != sample_generation_) sample_dirty_ = true;
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
    if (atlas != nullptr) sample_generation_ = atlas->generation();
    load_font_sample(*preview_ui_, recipe_abs_path_, sample_text_, sample_size_, atlas);
    preview_ui_->set_design_frame({static_cast<float>(preview_width_),
                                   static_cast<float>(preview_height_)});
    preview_ui_->update(static_cast<float>(preview_width_), static_cast<float>(preview_height_),
                        vk.current_frame_index(), 0.0f, nullptr);
    vk.render_game_ui_preview(*preview_ui_, *preview_target_);
}

} // namespace fjell
