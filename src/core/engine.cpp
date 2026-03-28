#include "core/engine.hpp"
#include "core/base_path.hpp"
#include "core/commands.hpp"
#include "core/game_builder.hpp"
#include "core/layer.hpp"
#include "core/log.hpp"
#include "core/profiler.hpp"
#include "animation/animation_subsystem.hpp"
#include "audio/audio_subsystem.hpp"
#include "physics/physics_subsystem.hpp"
#include "renderer/vfx_subsystem.hpp"
#include "scripting/script_subsystem.hpp"
#include "ui/game_ui/ui_subsystem.hpp"
#include "platform/platform.hpp"
#include "physics/physics_world.hpp"
#include "ui/console.hpp"
#include "ui/theme.hpp"
#include "ui/animation_editor_context.hpp"
#include "ui/material_editor_context.hpp"
#include "ui/ui_editor_context.hpp"
#include "ui/vfx_editor_context.hpp"
#include "ui/editor_context.hpp"
#include "ui/scene_editor_context.hpp"
#include "scene/camera_node.hpp"
#include "scene/preset.hpp"
#include "renderer/asset_loader.hpp"
#include "renderer/bindless.hpp"
#include "renderer/resource_manager.hpp"
#include "renderer/vulkan_context.hpp"
#include "renderer/material.hpp"
#include "renderer/material_asset.hpp"
#include "renderer/mesh_component.hpp"
#include "renderer/pipeline_cache.hpp"
#include "renderer/texture.hpp"

#include <imgui.h>
#include <imgui_internal.h>
#include <ImGuizmo.h>
#include <GLFW/glfw3.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>

namespace fjell {

namespace {

class ViewportLayer : public Layer {
public:
    explicit ViewportLayer(bool* hovered)
        : Layer("Viewport"), hovered_{hovered} {}

    [[nodiscard]] bool wants_input() const override {
        return *hovered_ && !ImGui::GetIO().WantCaptureKeyboard;
    }

private:
    bool* hovered_;
};

} // namespace

template <typename T, typename... Args>
T* Engine::add_subsystem(Args&&... args) {
    auto ptr = std::make_unique<T>(std::forward<Args>(args)...);
    auto* raw = ptr.get();
    subsystems_.push_back(std::move(ptr));
    return raw;
}

Engine::Engine(std::string_view title, uint32_t width, uint32_t height,
               const std::filesystem::path& project_path)
    : window_{title, width, height}
    , vulkan_context_{std::make_unique<VulkanContext>(window_)}
{
    // Register subsystems
    animation_subsystem_ = add_subsystem<AnimationSubsystem>(
        vulkan_context_->device(), vulkan_context_->physical_device());
    audio_subsystem_     = add_subsystem<AudioSubsystem>();
    script_subsystem_    = add_subsystem<ScriptSubsystem>();
    physics_subsystem_   = add_subsystem<PhysicsSubsystem>();
    vfx_subsystem_       = add_subsystem<VfxSubsystem>();

    // Phase groups
    play_phase_0_.push_back(script_subsystem_);
    play_phase_1_.push_back(physics_subsystem_);
    play_phase_2_.push_back(vfx_subsystem_);
    early_always_.push_back(animation_subsystem_);
    late_always_.push_back(audio_subsystem_);

    vulkan_context_->set_animation_system(&animation_subsystem_->system());
#ifndef NDEBUG
    shader_watcher_.emplace(base_path::resolve("../shaders"), base_path::resolve("shaders"));
#endif

    // Check for game mode (game.fjell next to binary)
    auto game_config_path = base_path::resolve("game.fjell");
    if (std::filesystem::exists(game_config_path)) {
        game_mode_ = true;
        std::ifstream gf(game_config_path);
        if (gf.is_open()) {
            auto gj = nlohmann::json::parse(gf, nullptr, false);
            if (!gj.is_discarded() && gj.contains("window")) {
                window_.set_title(gj["window"].value("title", "Fjell Game"));
            }
        }
    }

    // Open project if path given
    if (!project_path.empty()) {
        project_mgr_.open(project_path);
        if (project_mgr_.active()) {
            window_.set_title("Fjell - " + project_mgr_.active()->config().name);
        }
    }

    window_.set_input(&input_);
    input_.attach(window_.handle());

    imgui_layer_ = std::make_unique<ImGuiLayer>(
        window_.handle(), vulkan_context_->instance(),
        vulkan_context_->physical_device(), vulkan_context_->device(),
        vulkan_context_->graphics_family(), vulkan_context_->graphics_queue(),
        vulkan_context_->swapchain_format(), vulkan_context_->image_count(),
        base_path::resolve("engine_assets/fonts"));

    vulkan_context_->set_overlay_callback([this](VkCommandBuffer cmd) {
        imgui_layer_->render(cmd);
    });

    // Initialize game UI (owned by Engine, rendered via callback)
    vulkan_context_->init_game_ui(game_ui_,
        base_path::resolve("engine_assets/fonts/Inter-Regular.ttf"),
        vulkan_context_->swapchain_format());

    vulkan_context_->set_game_ui_callback(
        [this](
               VkCommandBuffer cmd, VkDescriptorSet global_set,
               VkDescriptorSet bindless_set, VkExtent2D extent,
               uint32_t frame_index) {
            // FPS counter (shared across all UI instances)
            fps_accum_ += clock_.delta_time();
            ++fps_count_;
            if (fps_accum_ >= 0.5f) {
                int fps = static_cast<int>(static_cast<float>(fps_count_) / fps_accum_ + 0.5f);
                game_ui_.context().set("fps", std::to_string(fps));
                fps_accum_ = 0.0f;
                fps_count_ = 0;
            }

            // Editor preview UI (non-play mode, loaded from UI editor context)
            if (game_ui_.loaded()) {
                game_ui_.update(static_cast<float>(extent.width),
                                static_cast<float>(extent.height),
                                frame_index, clock_.delta_time(), &input_);
                game_ui_.record(cmd, global_set, bindless_set, extent,
                                frame_index);
            }

            // Runtime screen-space UI instances from UiWidgetComponent nodes (play mode)
            for (auto& inst : ui_subsystem_->instances()) {
                if (inst.render_mode == UiWidgetComponent::RenderMode::world_space) continue;
                if (!inst.game_ui || !inst.game_ui->loaded()) continue;
                inst.game_ui->context().set("fps", std::to_string(fps_count_));
                inst.game_ui->update(static_cast<float>(extent.width),
                                     static_cast<float>(extent.height),
                                     frame_index, clock_.delta_time(), &input_);
                inst.game_ui->record(cmd, global_set, bindless_set, extent,
                                     frame_index);
            }
        });

    // Create UI subsystem (after game_ui_ font atlas is ready)
    ui_subsystem_ = add_subsystem<UiSubsystem>(*vulkan_context_, game_ui_);
    play_phase_0_.push_back(ui_subsystem_);

    // Worldspace UI callback (renders in color pass after transparent objects)
    vulkan_context_->set_world_ui_callback(
        [this](VkCommandBuffer cmd, VkDescriptorSet global_set,
               VkDescriptorSet bindless_set, VkExtent2D extent,
               uint32_t frame_index) {
            for (const auto& rec : ui_subsystem_->world_records()) {
                rec.game_ui->update(rec.push.viewport_width,
                                     rec.push.viewport_height,
                                     frame_index, clock_.delta_time(), nullptr);
                rec.game_ui->record_world(cmd, global_set, bindless_set,
                                           extent, frame_index, rec.push);
            }
        });

    resource_manager_ = std::make_unique<ResourceManager>(
        vulkan_context_->device(), vulkan_context_->physical_device(),
        vulkan_context_->command_pool(), vulkan_context_->graphics_queue(),
        vulkan_context_->bindless_textures(), vulkan_context_->mega_buffer(),
        vulkan_context_->meshlet_buffer(), vulkan_context_->pipeline_cache());

    asset_loader_ = std::make_unique<AssetLoader>(
        thread_pool_, *resource_manager_,
        vulkan_context_->device(), vulkan_context_->physical_device(),
        vulkan_context_->command_pool(), vulkan_context_->graphics_queue(),
        vulkan_context_->mega_buffer(), vulkan_context_->meshlet_buffer());

    default_texture_ = resource_manager_->load_texture(
        base_path::resolve("engine_assets/textures/test.png")).value_or(TextureHandle{});
    ScriptManager::set_resource_manager(resource_manager_.get());
    ScriptManager::set_active_project(project_mgr_.active());
    ScriptManager::set_vfx_subsystem(vfx_subsystem_);
    ScriptManager::set_audio_subsystem(audio_subsystem_);
    ui_subsystem_->set_script_manager(&script_subsystem_->manager());
    // Determine which scene to load
    if (project_mgr_.active()) {
        current_scene_path_ = project_mgr_.active()->resolve(project_mgr_.active()->config().default_scene);
    } else if (game_mode_) {
        // In game mode, scenes are alongside the binary
        current_scene_path_ = base_path::resolve("scenes/main.fjell");
    } else {
        current_scene_path_ = base_path::resolve(SCENE_FILE);
    }

    if (std::filesystem::exists(current_scene_path_)) {
        auto result = Scene::load(current_scene_path_, *resource_manager_, project_mgr_.active());
        if (result) {
            scene_ = std::move(*result);
        } else {
            FJELL_CORE_ERROR("{}", result.error());
            create_default_scene();
        }
        vulkan_context_->sync_custom_depth_pipelines();
    } else {
        create_default_scene();
    }

    // Load game scripts
    if (game_mode_) {
        auto game_so = base_path::resolve("game.so");
        if (std::filesystem::exists(game_so)) {
            if (auto r = script_subsystem_->manager().load(game_so); !r) {
                FJELL_CORE_ERROR("{}", r.error());
            } else {
                script_subsystem_->manager().deserialize_all(scene_);
            }
        }
    } else {
        load_project_scripts();
    }

    icon_cache_ = std::make_unique<IconCache>(
        vulkan_context_->device(), vulkan_context_->physical_device(),
        vulkan_context_->command_pool(), vulkan_context_->graphics_queue(),
        base_path::resolve("engine_assets/icons"));
    IconCache::set_instance(icon_cache_.get());

    content_browser_.init(icon_cache_.get(), &code_editor_,
                          vulkan_context_.get(), resource_manager_.get());
    code_editor_.on_save = [this](const std::string& path, Language lang) {
        if (lang == Language::Fjsl || lang == Language::Glsl) {
            // Copy to build shader dir so compile_entry reads the updated source
            namespace fs = std::filesystem;
            auto build_shader_dir = fs::path(base_path::dir()) / "shaders";
            auto filename = fs::path(path).filename();
            auto dest = build_shader_dir / filename;
            if (fs::exists(build_shader_dir)) {
                std::error_code ec;
                fs::copy_file(path, dest, fs::copy_options::overwrite_existing, ec);
            }
            vulkan_context_->reload_pipeline();
        }
    };
    connections_.push_back(content_browser_.on_add_model.bind(
        [this](const std::string& path) {
            scene_context_->add_model(path);
            scene_context_->command_history().execute(std::make_unique<AddObjectCommand>(
                scene_, scene_context_->selected_id(), scene_context_->selected_id_ptr()));
        }));
    connections_.push_back(content_browser_.on_load_scene.bind(
        [this](const std::string& path) { load_scene(path); }));
    connections_.push_back(content_browser_.on_open_asset.bind(
        [this](const std::string& path, const std::string& type) {
            auto* entry = context_registry_.find(type);
            if (!entry) return;

            // Reuse existing context if one matches
            for (int i = 0; i < static_cast<int>(contexts_.size()); ++i) {
                if (entry->matches(*contexts_[i], path)) {
                    entry->open(*contexts_[i], path);
                    switch_context(i);
                    return;
                }
            }

            // Create new context
            auto ctx = entry->create(path);
            contexts_.push_back(std::move(ctx));
            switch_context(static_cast<int>(contexts_.size()) - 1);
        }));
    connections_.push_back(content_browser_.on_create_preset.bind(
        [this](uint32_t node_id, const std::string& directory) {
            auto preset = Preset::from_subtree(scene_, node_id);
            auto* node = scene_.find(node_id);
            std::string name = node ? node->name : "preset";
            auto path = std::filesystem::path(directory) / (name + ".fjp");
            // Avoid overwriting — append number if file exists
            if (std::filesystem::exists(path)) {
                for (int i = 1; i < 100; ++i) {
                    auto alt = std::filesystem::path(directory) / (name + "_" + std::to_string(i) + ".fjp");
                    if (!std::filesystem::exists(alt)) { path = alt; break; }
                }
            }
            auto result = preset.save(path.string());
            if (!result) {
                FJELL_CORE_ERROR("Failed to save preset: {}", result.error());
            }
        }));
    connections_.push_back(content_browser_.on_instantiate_preset.bind(
        [this](const std::string& rel_path) {
            const auto* project = project_mgr_.active();
            auto resolved = base_path::resolve_asset(rel_path, project);
            auto result = Preset::load(resolved, *resource_manager_, project);
            if (!result) {
                FJELL_CORE_ERROR("Failed to load preset: {}", result.error());
                return;
            }
            auto root_id = result->instantiate(scene_);
            if (auto* root = scene_.find(root_id)) {
                root->preset_source = rel_path;
            }
            if (scene_context_) {
                scene_context_->set_selected_id(root_id);
            }
        }));
    if (project_mgr_.active()) {
        content_browser_.set_root(project_mgr_.active()->root());
    }

    stats_panel_.init(vulkan_context_.get());

    bind_menu_bar();

    // Apply saved layout for projects opened before signals were connected
    if (project_mgr_.active()) {
        auto& layout = project_mgr_.active()->config().layout;
        if (!layout.empty()) {
            auto lp = project_mgr_.active()->root() / "layouts" / (layout + ".ini");
            if (std::filesystem::exists(lp)) {
                pending_layout_load_ = lp.string();
                menu_bar_.set_active_layout(layout);
                has_saved_layout_ = true;
            }
        }
    }

    layer_stack_.push_layer(std::make_unique<ViewportLayer>(&viewport_hovered_));

    // Create scene editor context
    {
        SceneEditorDeps deps{
            .scene = &scene_,
            .vulkan_context = vulkan_context_.get(),
            .resource_manager = resource_manager_.get(),
            .asset_loader = asset_loader_.get(),
            .input = &input_,
            .clock = &clock_,
            .script_subsystem = script_subsystem_,
            .physics_subsystem = physics_subsystem_,
            .audio_subsystem = audio_subsystem_,
            .vfx_subsystem = vfx_subsystem_,
            .project_mgr = &project_mgr_,
            .icon_cache = icon_cache_.get(),
            .light = &light_,
            .game_ui = &game_ui_,
            .ui_subsystem = ui_subsystem_,
            .thread_pool = &thread_pool_,
            .window = &window_,
            .game_mode = game_mode_,
        };
        auto ctx = std::make_unique<SceneEditorContext>(deps);
        ctx->init();
        scene_context_ = ctx.get();
        contexts_.push_back(std::move(ctx));
        active_context_idx_ = 0;
    }

    history_panel_.init(&scene_context_->command_history());

    // Register context factories for asset types
    context_registry_.register_type("material", {
        .create = [this](const std::string& path) -> std::unique_ptr<EditorContext> {
            MaterialEditorDeps deps{
                .vulkan_context = vulkan_context_.get(),
                .resource_manager = resource_manager_.get(),
                .input = &input_,
                .clock = &clock_,
                .project_mgr = &project_mgr_,
                .icon_cache = icon_cache_.get(),
                .code_editor = &code_editor_
            };
            auto ctx = std::make_unique<MaterialEditorContext>(deps);
            ctx->init();
            ctx->open_material(path);
            return ctx;
        },
        .open = [](EditorContext&, const std::string&) {},
        .matches = [](const EditorContext& ctx, const std::string& path) {
            return ctx.asset_path() == path;
        }
    });

    context_registry_.register_type("ui", {
        .create = [this](const std::string& path) -> std::unique_ptr<EditorContext> {
            UiEditorDeps deps{
                .vulkan_context = vulkan_context_.get(),
                .resource_manager = resource_manager_.get(),
                .input = &input_,
                .clock = &clock_,
                .project_mgr = &project_mgr_,
                .icon_cache = icon_cache_.get(),
                .code_editor = &code_editor_,
                .engine_game_ui = &game_ui_,
                .script_manager = &script_subsystem_->manager()
            };
            auto ctx = std::make_unique<UiEditorContext>(deps);
            ctx->init();
            ctx->open_ui(path);
            return ctx;
        },
        .open = [](EditorContext&, const std::string&) {},
        .matches = [](const EditorContext& ctx, const std::string& path) {
            return ctx.asset_path() == path;
        }
    });

    context_registry_.register_type("vfx", {
        .create = [this](const std::string& path) -> std::unique_ptr<EditorContext> {
            VfxEditorDeps deps{
                .vulkan_context = vulkan_context_.get(),
                .resource_manager = resource_manager_.get(),
                .input = &input_,
                .clock = &clock_,
                .project_mgr = &project_mgr_,
                .icon_cache = icon_cache_.get()
            };
            auto ctx = std::make_unique<VfxEditorContext>(deps);
            ctx->init();
            ctx->open_effect(path);
            return ctx;
        },
        .open = [](EditorContext&, const std::string&) {},
        .matches = [](const EditorContext& ctx, const std::string& path) {
            return ctx.asset_path() == path;
        }
    });

    // Animation is special: reuses any existing animation context (matches by type, not path)
    auto anim_factory = [this](const std::string&) -> std::unique_ptr<EditorContext> {
        AnimationEditorDeps deps{
            .vulkan_context = vulkan_context_.get(),
            .resource_manager = resource_manager_.get(),
            .input = &input_,
            .clock = &clock_,
            .animation_subsystem = animation_subsystem_,
            .project_mgr = &project_mgr_,
            .icon_cache = icon_cache_.get()
        };
        auto ctx = std::make_unique<AnimationEditorContext>(deps);
        ctx->init();
        return ctx;
    };
    auto anim_matches = [](const EditorContext& ctx, const std::string&) {
        return dynamic_cast<const AnimationEditorContext*>(&ctx) != nullptr;
    };

    context_registry_.register_type("skeleton", {
        .create = [anim_factory](const std::string& path) {
            auto ctx = anim_factory(path);
            static_cast<AnimationEditorContext&>(*ctx).open_skeleton(path);
            return ctx;
        },
        .open = [](EditorContext& ctx, const std::string& path) {
            static_cast<AnimationEditorContext&>(ctx).open_skeleton(path);
        },
        .matches = anim_matches
    });

    context_registry_.register_type("animation", {
        .create = [anim_factory](const std::string& path) {
            auto ctx = anim_factory(path);
            static_cast<AnimationEditorContext&>(*ctx).open_clip(path);
            return ctx;
        },
        .open = [](EditorContext& ctx, const std::string& path) {
            static_cast<AnimationEditorContext&>(ctx).open_clip(path);
        },
        .matches = anim_matches
    });

    context_registry_.register_type("animset", {
        .create = [anim_factory](const std::string& path) {
            auto ctx = anim_factory(path);
            static_cast<AnimationEditorContext&>(*ctx).open_skeleton(path);
            return ctx;
        },
        .open = [](EditorContext& ctx, const std::string& path) {
            static_cast<AnimationEditorContext&>(ctx).open_skeleton(path);
        },
        .matches = anim_matches
    });

    FJELL_CORE_INFO("Engine initialized");
}

Engine::~Engine() {
    vulkan_context_->wait_idle();
    vulkan_context_->set_overlay_callback(nullptr);
    vulkan_context_->set_game_ui_callback(nullptr);
    game_ui_.destroy();
    for (auto& ctx : contexts_) ctx->shutdown();
    contexts_.clear();
    scene_context_ = nullptr;
    scene_ = {};
    resource_manager_.reset();
    IconCache::set_instance(nullptr);
    icon_cache_.reset();
    // Release ImGui descriptor sets before ImGui shuts down
    vulkan_context_->release_imgui_textures();
    imgui_layer_.reset();
    FJELL_CORE_INFO("Engine shut down");
}

void Engine::save_material_assets() {
    auto* cache = vulkan_context_->pipeline_cache();
    auto& bindless = vulkan_context_->bindless_textures();
    if (!cache) return;

    for (auto& obj : scene_.objects()) {
        auto* mesh = obj->components.get<MeshComponent>();
        if (!mesh || !mesh->material) continue;
        if (!mesh->material_name.ends_with(".fjmat")) continue;

        uint32_t pid = mesh->material->pipeline_id();
        if (pid >= cache->shader_count()) continue;

        const auto& meta = cache->metadata(pid);
        uint32_t param_count = mesh->material->custom_param_count();
        if (param_count == 0) continue;

        const auto& gpu = bindless.material_at(mesh->material->material_id());

        // Read current params back from CPU buffer
        std::string mat_path = base_path::resolve_asset(
            mesh->material_name, project_mgr_.active());

        MaterialAsset asset;
        try {
            asset = load_material_asset(mat_path);
        } catch (...) {
            continue;
        }

        // Update custom param values from live buffer
        for (const auto& u : meta.uniforms) {
            if (u.type == UniformType::Sampler2D) continue;
            uint32_t fcount = uniform_float_count(u.type);
            std::vector<float> vals(fcount);
            for (uint32_t i = 0; i < fcount; ++i) {
                vals[i] = bindless.read_param(gpu.param_offset + u.slot + i);
            }
            asset.params[u.name] = vals;
        }

        // Save uv_scale from base params
        float uv_x = gpu.base_params[7];
        float uv_y = gpu.base_params[8];
        if (uv_x != 1.0f || uv_y != 1.0f) {
            asset.params["uv_scale"] = {uv_x, uv_y};
        } else {
            asset.params.erase("uv_scale");
        }

        save_material_asset(mat_path, asset);
    }
}

void Engine::create_default_scene() {
    vulkan_context_->wait_idle();
    auto tex = resource_manager_->load_texture(
        base_path::resolve("engine_assets/textures/test.png")).value_or(TextureHandle{});
    auto mat = resource_manager_->create_material("default", tex);

    auto plane = resource_manager_->create_primitive("plane").value_or(ModelHandle{});
    scene_.add(plane, mat, *resource_manager_,
               {{0.0f, 0.0f, 0.0f}, {}, {10.0f, 1.0f, 10.0f}},
               "Ground", "primitive:plane");

    auto cube = resource_manager_->create_primitive("cube").value_or(ModelHandle{});
    scene_.add(cube, mat, *resource_manager_,
               {{0.0f, 0.5f, 0.0f}},
               "Cube", "primitive:cube");

    auto sphere = resource_manager_->create_primitive("sphere").value_or(ModelHandle{});
    scene_.add(sphere, mat, *resource_manager_,
               {{2.5f, 0.5f, 0.0f}},
               "Sphere", "primitive:sphere");

    FJELL_CORE_INFO("Default scene created");
}

void Engine::setup_dockspace() {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);

    ImGuiWindowFlags host_flags =
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_NoDocking;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});

    ImGui::Begin("##DockHost", nullptr, host_flags);
    ImGui::PopStyleVar(3);

    // Per-context dockspaces — each context gets its own
    // Only draw the active one; the others retain their layout in ImGui state
    for (int i = 0; i < static_cast<int>(contexts_.size()); ++i) {
        auto& ctx = *contexts_[static_cast<size_t>(i)];
        char id_buf[32];
        std::snprintf(id_buf, sizeof(id_buf), "CtxDock%d", i);
        ImGuiID ctx_dock_id = ImGui::GetID(id_buf);
        ctx.set_dockspace_id(ctx_dock_id);
        ctx.set_context_index(i);

        if (i == active_context_idx_) {
            // Active: draw the dockspace
            ImGui::DockSpace(ctx_dock_id, {0.0f, 0.0f},
                             ImGuiDockNodeFlags_PassthruCentralNode);
        } else {
            // Inactive: keep node alive but don't render
            // (ImGui retains layout state as long as the DockSpace ID persists)
            ImGui::DockSpace(ctx_dock_id, {0.0f, 0.0f},
                             ImGuiDockNodeFlags_PassthruCentralNode |
                             ImGuiDockNodeFlags_KeepAliveOnly);
        }
    }

    // Build layout for contexts that haven't been set up yet
    for (int i = 0; i < static_cast<int>(contexts_.size()); ++i) {
        auto& ctx = *contexts_[static_cast<size_t>(i)];
        if (ctx.dockspace_built()) continue;
        ctx.mark_dockspace_built();

        // Scene context: skip layout if restoring saved layout
        if (i == 0 && has_saved_layout_) {
            scene_context_->request_focus_viewport();
            continue;
        }

        ImGuiID ctx_dock_id = ctx.dockspace_id();
        ImGui::DockBuilderRemoveNode(ctx_dock_id);
        ImGui::DockBuilderAddNode(ctx_dock_id, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(ctx_dock_id, viewport->WorkSize);

        ImGuiID main_area = ctx_dock_id;

        if (i == 0) {
            scene_context_->request_focus_viewport();
        }

        ctx.setup_dockspace(main_area);
        ImGui::DockBuilderFinish(ctx_dock_id);
    }

    ImGui::End();
}

// Hierarchy and Properties panels extracted to ui/hierarchy_panel.cpp and ui/properties_panel.cpp

void Engine::draw_console() {
    auto& sink = log::console_sink();
    if (sink) {
        sink->draw("Console");
    }
}

void Engine::draw_code_editor() {
    code_editor_.draw("Code Editor");
}

void Engine::load_project_scripts() {
    if (!project_mgr_.active()) return;
    auto so_path = project_mgr_.active()->root() / "game.so";
    if (std::filesystem::exists(so_path)) {
        if (auto r = script_subsystem_->manager().load(so_path); !r) {
            FJELL_CORE_ERROR("{}", r.error());
            return;
        }
        script_subsystem_->manager().deserialize_all(scene_);
#ifndef NDEBUG
        script_subsystem_->manager().watch(so_path);
#endif
    }
}

void Engine::load_scene(const std::string& path) {
    vulkan_context_->wait_idle();
    current_scene_path_ = path;
    auto result = Scene::load(path, *resource_manager_, project_mgr_.active());
    if (result) {
        scene_ = std::move(*result);
    } else {
        FJELL_CORE_ERROR("{}", result.error());
        scene_.clear();
    }
    scene_context_->set_selected_id(0);
    scene_context_->command_history().clear();
    vulkan_context_->sync_custom_depth_pipelines();
}

void Engine::bind_menu_bar() {
    connections_.push_back(menu_bar_.on_save_scene.bind(
        [this]() {
            save_material_assets();
            if (auto r = scene_.save(current_scene_path_); !r) {
                FJELL_CORE_ERROR("{}", r.error());
            }
        }));
    connections_.push_back(menu_bar_.on_load_scene.bind(
        [this]() { load_scene(current_scene_path_); }));
    connections_.push_back(menu_bar_.on_close_project.bind(
        [this]() { project_mgr_.close(); }));
    connections_.push_back(menu_bar_.on_build_game.bind(
        [this]() {
            if (project_mgr_.active()) {
                auto output = project_mgr_.active()->root() / "build";
                auto result = GameBuilder::build(*project_mgr_.active(), output);
                if (!result) {
                    FJELL_CORE_ERROR("{}", result.error());
                }
            }
        }));
    connections_.push_back(menu_bar_.on_build_scripts.bind(
        [this]() { project_mgr_.build_scripts(script_subsystem_->manager(), scene_); }));
    connections_.push_back(menu_bar_.on_new_script.bind(
        [this](const std::string& name) { project_mgr_.create_script(name); }));
    connections_.push_back(menu_bar_.on_start_play.bind(
        [this]() { scene_context_->start_play(); }));
    connections_.push_back(menu_bar_.on_stop_play.bind(
        [this]() { scene_context_->stop_play(); }));
    connections_.push_back(menu_bar_.on_open_project.bind(
        [this](const std::filesystem::path& path) {
            project_mgr_.open(path);
            if (project_mgr_.active()) {
                current_scene_path_ = project_mgr_.active()->resolve(
                    project_mgr_.active()->config().default_scene);
                if (std::filesystem::exists(current_scene_path_)) {
                    load_scene(current_scene_path_);
                } else {
                    scene_.clear();
                    create_default_scene();
                }
                load_project_scripts();
            }
        }));
    connections_.push_back(menu_bar_.on_create_project.bind(
        [this](const std::filesystem::path& path, const std::string& name) {
            project_mgr_.create(path, name);
            if (project_mgr_.active()) {
                current_scene_path_ = project_mgr_.active()->resolve(
                    project_mgr_.active()->config().default_scene);
                scene_.clear();
                create_default_scene();
            }
        }));
    // React to project open/close from any source
    connections_.push_back(project_mgr_.on_opened.bind(
        [this](const std::string& name) {
            window_.set_title("Fjell - " + name);
            content_browser_.set_root(project_mgr_.active()->root());
            // Defer layout restore
            auto& layout = project_mgr_.active()->config().layout;
            if (!layout.empty()) {
                auto lp = project_mgr_.active()->root() / "layouts" / (layout + ".ini");
                if (std::filesystem::exists(lp)) {
                    pending_layout_load_ = lp.string();
                    menu_bar_.set_active_layout(layout);
                    has_saved_layout_ = true;
                }
            }
        }));
    connections_.push_back(project_mgr_.on_closed.bind(
        [this]() {
            current_scene_path_ = base_path::resolve(SCENE_FILE);
            window_.set_title("Fjell Engine");
            content_browser_.set_root({});
        }));
}

void Engine::draw_ui() {
    FJELL_PROFILE_SCOPE_N("draw_ui");
    // Update window title with unsaved indicator
    {
        std::string title = "Fjell";
        if (project_mgr_.active()) title += " - " + project_mgr_.active()->config().name;
        if (scene_.is_dirty()) title += " *";
        window_.set_title(title);
    }

    // Global shortcuts (Ctrl+S, Ctrl+Z/Y, Ctrl+Space)
    if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S)) {
        code_editor_.save_active();
        active_context().save();
        if (&active_context() == scene_context_) {
            save_material_assets();
            if (auto r = scene_.save(current_scene_path_); !r) {
                FJELL_CORE_ERROR("{}", r.error());
            }
        }
    }
    if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Space)) {
        content_browser_.toggle_visibility();
    }
    if (!ImGui::GetIO().WantTextInput && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z)) {
        if (ImGui::GetIO().KeyShift) {
            active_context().command_history().redo();
        } else {
            active_context().command_history().undo();
        }
    }
    if (!ImGui::GetIO().WantTextInput && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y)) {
        active_context().command_history().redo();
    }

    // Context-specific shortcuts
    active_context().handle_shortcuts();

    // Apply deferred layout load
    if (!pending_layout_load_.empty()) {
        ImGui::LoadIniSettingsFromDisk(pending_layout_load_.c_str());
        pending_layout_load_.clear();
        // Focus viewport next frame (layout overrides focus in the same frame)
        scene_context_->request_focus_viewport();
    }

    menu_bar_.set_project(project_mgr_.active());
    menu_bar_.set_playing(active_context().is_playing());
    menu_bar_.set_scene_path(current_scene_path_);
    menu_bar_.set_vulkan_context(vulkan_context_.get());
    menu_bar_.set_viewport_manager(&scene_context_->viewport_manager());
    menu_bar_.draw();

    draw_context_tabs();
    setup_dockspace();

    // Active context draws its own panels
    active_context().draw(clock_.delta_time());
    viewport_hovered_ = active_context().any_viewport_hovered();

    // Shared panels — each drawn with a per-context window name so they
    // dock independently in each context's dockspace
    char stats_title[32], history_title[32], console_title[32], code_title[32];
    std::snprintf(stats_title, sizeof(stats_title), "Stats##ctx%d", active_context_idx_);
    std::snprintf(history_title, sizeof(history_title), "History##ctx%d", active_context_idx_);
    std::snprintf(console_title, sizeof(console_title), "Console##ctx%d", active_context_idx_);
    std::snprintf(code_title, sizeof(code_title), "Code Editor##ctx%d", active_context_idx_);

    content_browser_.draw();
    stats_panel_.draw(clock_.delta_time(), stats_title);
    history_panel_.draw(history_title);
    if (auto& sink = log::console_sink(); sink) {
        sink->draw(console_title);
    }
    ImGui::PushID(active_context_idx_);
    code_editor_.draw(code_title);
    ImGui::PopID();
}

TickContext Engine::make_frame_context() {
    return TickContext{
        .scene = scene_,
        .input = input_,
        .clock = clock_,
        .dt = clock_.delta_time(),
        .thread_pool = thread_pool_,
        .physics = physics_subsystem_->world(),
        .audio = &audio_subsystem_->engine(),
        .vfx = nullptr,
        .project = project_mgr_.active(),
    };
}

void Engine::flush_pending() {
    if (scene_.pending_destroy().empty()) return;
    script_subsystem_->manager().trace_doomed_refs(scene_, scene_.pending_destroy());
    if (physics_subsystem_->world()) {
        for (uint32_t id : scene_.pending_destroy()) {
            physics_subsystem_->world()->remove_body(id);
        }
    }
    scene_.flush_destroyed();
}

void Engine::set_scene_override(const std::string& scene_name) {
    std::string filename = scene_name;
    if (!filename.ends_with(".fjell")) filename += ".fjell";
    std::string path;
    if (project_mgr_.active()) {
        path = project_mgr_.active()->resolve("scenes/" + filename);
    } else {
        path = base_path::resolve("scenes/" + filename);
    }
    if (std::filesystem::exists(path)) {
        load_scene(path);
    } else {
        FJELL_CORE_ERROR("Scene not found: {}", path);
    }
}

void Engine::run() {
    FJELL_CORE_INFO("Entering main loop");

    // Game mode or --play: start playing immediately
    if (game_mode_ || play_immediate_) {
        scene_context_->start_play();
    }

    while (!window_.should_close()) {
        // Phase 1: Submit previous frame to GPU (non-blocking)
        vulkan_context_->submit_and_present();

        FJELL_PROFILE_FRAME;

        // Phase 2: CPU work (overlaps with GPU executing the submitted frame)
        asset_loader_->poll();
        input_.update();
        window_.poll_events();
        clock_.tick();

#ifndef NDEBUG
        if (shader_watcher_ && shader_watcher_->poll()) {
            vulkan_context_->reload_pipeline();
        }
#endif

        auto ctx = make_frame_context();

        if (active_context().is_playing()) {
            // Update camera state for worldspace UI billboard computation
            for (const auto& obj : scene_.objects()) {
                if (auto* cam_node = dynamic_cast<const CameraNode*>(obj.get())) {
                    float aspect = scene_context_->viewport_manager().active_aspect();
                    ui_subsystem_->set_camera(
                        cam_node->position(),
                        cam_node->view_matrix(),
                        cam_node->projection_matrix(aspect));
                    break;
                }
            }

            // Phase 0: Scripts
            for (auto* s : play_phase_0_) s->tick(ctx);
            flush_pending();

            // Phase 1: Physics + collision dispatch
            for (auto* s : play_phase_1_) s->tick(ctx);
            if (physics_subsystem_->world()) {
                script_subsystem_->manager().dispatch_collision_events(
                    scene_, *physics_subsystem_->world());
            }
            flush_pending();

            // Phase 2: VFX
            for (auto* s : play_phase_2_) s->tick(ctx);

            if (scene_context_->play_wants_stop()) {
                scene_context_->stop_play();
            }
        }

#ifndef NDEBUG
        if (auto r = script_subsystem_->manager().poll_reload(scene_); !r) {
            FJELL_CORE_ERROR("{}", r.error());
        }
#endif

        // Always-tick subsystems (before world matrices)
        for (auto* s : early_always_) s->tick(ctx);
        scene_.compute_world_matrices(&thread_pool_);

        // Always-tick subsystems (after world matrices)
        for (auto* s : late_always_) s->tick(ctx);

        // Phase 3: Wait for frame slot — GPU is done with this slot after this
        if (!vulkan_context_->begin_frame(thread_pool_)) continue;

        // Resize offscreen passes — safe because begin_frame's fence guarantees
        // the GPU is no longer referencing this frame slot's resources
        vulkan_context_->set_active_sources(scene_context_->viewport_manager().active_source_mask());
        active_context().apply_resize(*vulkan_context_);

        imgui_layer_->begin_frame();
        if (active_context().is_playing()) {
            active_context().draw_play_overlay();
        } else {
            draw_ui();
        }
        imgui_layer_->end_frame();

        auto requests = active_context().build_render_requests();

        // In play mode, override the active viewport's camera with the scene CameraNode
        if (active_context().is_playing() && !requests.empty()) {
            for (const auto& obj : scene_.objects()) {
                if (auto* cam_node = dynamic_cast<const CameraNode*>(obj.get())) {
                    float aspect = scene_context_->viewport_manager().active_aspect();
                    requests[0].view = cam_node->view_matrix();
                    requests[0].projection = cam_node->projection_matrix(aspect);
                    requests[0].camera_position = cam_node->position();
                    break;
                }
            }
        }

        // Contexts with 3D viewports return their scene (main or preview).
        // Contexts without one (e.g. UI editor) return nullptr — fall back to main scene
        // so the renderer always has valid geometry to reference.
        Scene* render_scene = active_context().render_scene();
        if (!render_scene) render_scene = &scene_;

        vulkan_context_->sync_sprite_textures(*render_scene, *resource_manager_);
        vulkan_context_->set_viewport_offset(scene_context_->viewport_manager().active_viewport_pos());

        // Phase 4: Record commands
        vulkan_context_->record_frame(requests, light_, render_scene, clock_.delta_time(),
                                       scene_context_->selected_id(), thread_pool_,
                                       active_context().is_playing());
    }

    // Submit the last recorded frame before shutting down
    vulkan_context_->submit_and_present();
    FJELL_CORE_INFO("Exiting main loop");
}

EditorContext& Engine::active_context() {
    return *contexts_[static_cast<size_t>(active_context_idx_)];
}

const EditorContext& Engine::active_context() const {
    return *contexts_[static_cast<size_t>(active_context_idx_)];
}

void Engine::switch_context(int index) {
    if (index == active_context_idx_) return;
    pending_context_switch_ = index;
}

void Engine::draw_context_tabs() {
    ImGuiViewport* viewport = ImGui::GetMainViewport();

    // Tab sizing — push FramePadding early so bar_height accounts for it
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {14.0f, 8.0f});
    float bar_height = ImGui::GetFrameHeight() + 4.0f;

    // Match menu bar background, minimal window padding
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::GetStyleColorVec4(ImGuiCol_MenuBarBg));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {4.0f, 2.0f});
    if (!ImGui::BeginViewportSideBar("##ContextBar", viewport, ImGuiDir_Up, bar_height,
                                      ImGuiWindowFlags_NoScrollbar |
                                      ImGuiWindowFlags_NoScrollWithMouse)) {
        ImGui::End();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
        return;
    }
    ImGui::PopStyleVar();   // WindowPadding
    ImGui::PopStyleColor(); // WindowBg

    // Tab colors
    ImGui::PushStyleColor(ImGuiCol_Tab, theme::srgb(0.067f, 0.071f, 0.078f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_TabHovered, theme::srgb(0.169f, 0.176f, 0.192f));
    ImGui::PushStyleColor(ImGuiCol_TabSelected, theme::srgb(0.118f, 0.122f, 0.133f));
    ImGui::PushStyleVar(ImGuiStyleVar_TabRounding, 4.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_TabBorderSize, 1.0f);

    // Indent tabs to align with the viewport (past the left dock panels)
    float indent = scene_context_->viewport_manager().active_viewport_pos().x;
    if (indent > 4.0f) {
        ImGui::SetCursorPosX(indent);
    }

    // Apply pending context switch — set the tab flag so ImGui selects it
    bool has_pending = pending_context_switch_ >= 0
        && pending_context_switch_ < static_cast<int>(contexts_.size())
        && pending_context_switch_ != active_context_idx_;

    if (has_pending) {
        // Perform the actual context transition now
        vulkan_context_->wait_idle();
        contexts_[static_cast<size_t>(active_context_idx_)]->on_exit();
        active_context_idx_ = pending_context_switch_;
        contexts_[static_cast<size_t>(active_context_idx_)]->on_enter();
        history_panel_.init(&active_context().command_history());
    }
    pending_context_switch_ = -1;

    if (ImGui::BeginTabBar("##ContextTabs", ImGuiTabBarFlags_Reorderable)) {
        int close_idx = -1;
        for (int i = 0; i < static_cast<int>(contexts_.size()); ++i) {
            ImGui::PushID(i);
            bool closeable = (i != 0);
            bool open = true;
            ImGuiTabItemFlags flags = (has_pending && i == active_context_idx_)
                ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
            if (ImGui::BeginTabItem(contexts_[static_cast<size_t>(i)]->name(),
                                     closeable ? &open : nullptr, flags)) {
                // Tab clicked by user — switch if different from active
                if (!has_pending && i != active_context_idx_) {
                    vulkan_context_->wait_idle();
                    contexts_[static_cast<size_t>(active_context_idx_)]->on_exit();
                    active_context_idx_ = i;
                    contexts_[static_cast<size_t>(i)]->on_enter();
                    history_panel_.init(&active_context().command_history());
                }
                ImGui::EndTabItem();
            }
            if (closeable && !open) close_idx = i;
            ImGui::PopID();
        }
        ImGui::EndTabBar();

        // Defer close until after the tab bar is fully drawn
        if (close_idx >= 0) {
            vulkan_context_->wait_idle();

            // If closing the active tab, switch away first
            if (close_idx == active_context_idx_) {
                int new_idx = close_idx > 0 ? close_idx - 1 : 0;
                contexts_[close_idx]->on_exit();
                active_context_idx_ = new_idx;
                contexts_[active_context_idx_]->on_enter();
                history_panel_.init(&active_context().command_history());
            }

            contexts_[close_idx]->shutdown();
            contexts_.erase(contexts_.begin() + close_idx);

            // Adjust active index if a tab before it was removed
            if (close_idx < active_context_idx_) {
                active_context_idx_--;
            }
        }
    }

    ImGui::PopStyleVar(2);   // TabRounding, TabBorderSize
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar();    // FramePadding

    ImGui::End();
}

} // namespace fjell
