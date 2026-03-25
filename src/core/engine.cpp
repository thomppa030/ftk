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
#include "platform/platform.hpp"
#include "physics/physics_world.hpp"
#include "ui/console.hpp"
#include "ui/animation_editor_context.hpp"
#include "ui/editor_context.hpp"
#include "ui/scene_editor_context.hpp"
#include "scene/camera_node.hpp"
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
            if (game_ui_.loaded()) {
                fps_accum_ += clock_.delta_time();
                ++fps_count_;
                if (fps_accum_ >= 0.5f) {
                    int fps = static_cast<int>(static_cast<float>(fps_count_) / fps_accum_ + 0.5f);
                    game_ui_.context().set("fps", std::to_string(fps));
                    fps_accum_ = 0.0f;
                    fps_count_ = 0;
                }
                game_ui_.update(static_cast<float>(extent.width),
                                static_cast<float>(extent.height),
                                frame_index, clock_.delta_time(), &input_);
                game_ui_.record(cmd, global_set, bindless_set, extent,
                                frame_index);
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

#ifdef FJELL_HAS_NVIM_PANEL
    content_browser_.init(icon_cache_.get(), &nvim_panel_,
                          vulkan_context_.get(), resource_manager_.get());
#else
    content_browser_.init(icon_cache_.get(), nullptr,
                          vulkan_context_.get(), resource_manager_.get());
#endif
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
            if (type == "skeleton" || type == "animation" || type == "animset") {
                // Reuse existing animation editor if open
                for (int i = 0; i < static_cast<int>(contexts_.size()); ++i) {
                    if (auto* anim_ctx = dynamic_cast<AnimationEditorContext*>(contexts_[i].get())) {
                        if (type == "animation") {
                            anim_ctx->open_clip(path);
                        } else {
                            anim_ctx->open_skeleton(path);
                        }
                        switch_context(i);
                        return;
                    }
                }
                // Create new animation editor context
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
                if (type == "skeleton") {
                    ctx->open_skeleton(path);
                } else {
                    ctx->open_clip(path);
                }
                contexts_.push_back(std::move(ctx));
                switch_context(static_cast<int>(contexts_.size()) - 1);
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

    // Load game UI — check project first, then fall back to engine assets
    {
        auto proj_ui = base_path::resolve_asset("assets/ui/test.fjui", project_mgr_.active());
        auto engine_ui = base_path::resolve("assets/ui/test.fjui");
        auto ui_path = std::filesystem::exists(proj_ui) ? proj_ui : engine_ui;
        auto css_path = ui_path.substr(0, ui_path.size() - 5) + ".fjss";
        if (std::filesystem::exists(ui_path)) {
            game_ui_.load(ui_path, css_path);

            // Seed demo context values for the test HUD
            auto& ctx = game_ui_.context();
            ctx.set("player.health", "72");
            ctx.set("player.max_health", "100");
            ctx.set("show_debug", "1");
            ctx.set("fps", "60");
            ctx.set("player.pos", "0, 0, 0");
            ctx.set("active_slot", "2"); // slot 2 starts active
            connections_.push_back(ctx.bind_action("select_slot_1", [&ctx] {
                FJELL_CORE_INFO("UI: slot 1 clicked");
                ctx.set("active_slot", "1");
            }));
            connections_.push_back(ctx.bind_action("select_slot_2", [&ctx] {
                FJELL_CORE_INFO("UI: slot 2 clicked");
                ctx.set("active_slot", "2");
            }));
            connections_.push_back(ctx.bind_action("select_slot_3", [&ctx] {
                FJELL_CORE_INFO("UI: slot 3 clicked");
                ctx.set("active_slot", "3");
            }));
            connections_.push_back(ctx.bind_action("select_slot_4", [&ctx] {
                FJELL_CORE_INFO("UI: slot 4 clicked");
                ctx.set("active_slot", "4");
            }));
        }
    }

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

    // Context tab bar (above dockspace, only when multiple contexts open)
    if (contexts_.size() > 1) {
        draw_context_tabs();
        ImGui::Separator();
    }

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
#ifdef FJELL_HAS_NVIM_PANEL
    nvim_panel_.poll();
    nvim_panel_.draw();
#endif
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
        save_material_assets();
        if (auto r = scene_.save(current_scene_path_); !r) {
            FJELL_CORE_ERROR("{}", r.error());
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

    setup_dockspace();

    // Context tab bar (dockable panel)
    draw_context_tabs();

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
#ifdef FJELL_HAS_NVIM_PANEL
    nvim_panel_.poll();
    ImGui::PushID(active_context_idx_);
    nvim_panel_.draw();
    ImGui::PopID();
#endif
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
    vulkan_context_->wait_idle();
    contexts_[static_cast<size_t>(active_context_idx_)]->on_exit();
    active_context_idx_ = index;
    contexts_[static_cast<size_t>(active_context_idx_)]->on_enter();
    history_panel_.init(&active_context().command_history());
}

void Engine::draw_context_tabs() {
    if (ImGui::Begin("Editors")) {
        if (ImGui::BeginTabBar("##ContextTabs")) {
            for (int i = 0; i < static_cast<int>(contexts_.size()); ++i) {
                ImGui::PushID(i);
                bool closeable = (i != 0);
                bool open = true;
                if (ImGui::BeginTabItem(contexts_[static_cast<size_t>(i)]->name(),
                                         closeable ? &open : nullptr)) {
                    if (i != active_context_idx_) switch_context(i);
                    ImGui::EndTabItem();
                }
                ImGui::PopID();
                if (closeable && !open) {
                    contexts_[i]->shutdown();
                    contexts_.erase(contexts_.begin() + i);
                    if (active_context_idx_ >= static_cast<int>(contexts_.size())) {
                        switch_context(static_cast<int>(contexts_.size()) - 1);
                    }
                    break;
                }
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
}

} // namespace fjell
