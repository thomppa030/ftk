#include "core/engine.hpp"
#include "scripting/script_call.hpp"
#include "core/asset_database.hpp"
#include "scene/light_node.hpp"
#include "core/asset_references.hpp"
#include "core/base_path.hpp"
#include "ui/asset_picker.hpp"
#include "ui/asset_ref_widget.hpp"
#include "ui/engine_file_browser.hpp"
#include "ui/engine_imgui_files.hpp"
#include "core/input_component.hpp"
#include "core/commands.hpp"
#include "core/game_builder.hpp"
#include "core/log.hpp"
#include "core/profiler.hpp"
#include "animation/animation_subsystem.hpp"
#include "audio/audio_subsystem.hpp"
#include "environment/environment_resolver.hpp"
#include "environment/environment_subsystem.hpp"
#include "net/net_manager.hpp"
#include "net/net_subsystem.hpp"
#include "physics/physics_subsystem.hpp"
#include "renderer/systems/sprite_anim_subsystem.hpp"
#include "renderer/systems/vfx_subsystem.hpp"
#include "scripting/script_globals.hpp"
#include "scripting/script_subsystem.hpp"
#include "scripting/debug_draw_queue.hpp"
#include "scripting/screen_debug_queue.hpp"
#include "ui/game_ui/ui_subsystem.hpp"
#include "ui/game_ui/font_library.hpp"
#include "platform/platform.hpp"
#include "platform/shared_library.hpp"
#include "physics/physics_world.hpp"
#include "ui/console.hpp"
#include "ui/kit/asset_kind.hpp"
#include "ui/kit/icons.hpp"
#include "ui/kit/status_bar.hpp"
#include "ui/kit/tabs.hpp"
#include "ui/kit/viewport_toolbar.hpp"
#include "ui/theme.hpp"
#include "ui/animation_editor_context.hpp"
#include "ui/blend_space_editor_context.hpp"
#include "ui/material_editor_context.hpp"
#include "ui/mesh_editor_context.hpp"
#include "ui/day_profile_editor_context.hpp"
#include "ui/font_editor_context.hpp"
#include "ui/water_editor_context.hpp"
#include "ui/weather_editor_context.hpp"
#include "ui/ui_editor_context.hpp"
#include "ui/input_mapping_editor_context.hpp"
#include "ui/vfx_editor_context.hpp"
#include "ui/engine_editor_context.hpp"
#include "ui/scene_editor_context.hpp"
#include "scene/camera_node.hpp"
#include "scene/camera_subsystem.hpp"
#include "scene/preset.hpp"
#include "renderer/resources/asset_loader.hpp"
#include "renderer/bindless.hpp"
#include "renderer/resources/resource_manager.hpp"
#include "renderer/vulkan_context.hpp"
#include "renderer/render_settings.hpp"
#include "renderer/resources/material.hpp"
#include "renderer/resources/material_asset.hpp"
#include "renderer/components/mesh_component.hpp"
#include "renderer/components/terrain_component.hpp"
#include "renderer/components/water_component.hpp"
#include "renderer/resources/terrain_asset.hpp"
#include "renderer/pipeline_cache.hpp"
#include "renderer/resources/texture.hpp"

#include <imgui.h>
#include <imgui_internal.h>
#include <ImGuizmo.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <format>
#include <functional>
#include <fstream>
#include <sstream>
#include <unordered_set>

namespace fjell {

template <typename T, typename... Args>
T* Engine::add_subsystem(Args&&... args) {
    auto ptr = std::make_unique<T>(std::forward<Args>(args)...);
    auto* raw = ptr.get();
    subsystems_.push_back(std::move(ptr));
    return raw;
}

Engine::Engine(std::string_view title, uint32_t width, uint32_t height,
               const std::filesystem::path& project_path,
               const std::string& create_name)
    : window_{title, width, height}
    , vulkan_context_{std::make_unique<VulkanContext>(window_)}
{
    // Font bakes run on the pool from here on, project fonts included.
    vulkan_context_->font_library().set_thread_pool(&thread_pool_);

    auto* gpu_core = &vulkan_context_->gpu().core();
    import_dialog_.set_gpu(gpu_core);
    menu_bar_.set_gpu(gpu_core);
    set_up_file_browser(import_file_browser_, gpu_core);

    init_subsystems();
    init_project(project_path, create_name);
    init_ui();
    init_resources();
    init_scene();
    init_content_browser();
    init_editor();
    FJELL_CORE_INFO("Engine initialized");
}

void Engine::init_subsystems() {
    animation_subsystem_ = add_subsystem<AnimationSubsystem>(vulkan_context_->gpu_device());
    audio_subsystem_     = add_subsystem<AudioSubsystem>();
    environment_subsystem_ = add_subsystem<EnvironmentSubsystem>();
    script_subsystem_    = add_subsystem<ScriptSubsystem>();
    physics_subsystem_   = add_subsystem<PhysicsSubsystem>();
    vfx_subsystem_       = add_subsystem<VfxSubsystem>();
    sprite_anim_subsystem_ = add_subsystem<SpriteAnimSubsystem>();
    camera_subsystem_    = add_subsystem<CameraSubsystem>();

    // Subsystems that hold each other are wired once they all exist: these
    // are pointers into subsystems_, and one taken before its subsystem is
    // constructed is null for the run.
    environment_subsystem_->set_audio(audio_subsystem_);
    environment_subsystem_->set_scripts(script_subsystem_);
    environment_subsystem_->set_vfx(vfx_subsystem_);
    physics_subsystem_->set_environment(environment_subsystem_);

    net_manager_ = std::make_unique<NetManager>();
    // Forward connection events to script components (server-side). These fire
    // from the transport poll inside NetReceive's tick — main thread, scene safe.
    net_manager_->on_peer_connected = [this](PeerId peer) {
        script_subsystem_->manager().dispatch_player_connected(scene_, peer);
    };
    net_manager_->on_peer_disconnected = [this](PeerId peer) {
        script_subsystem_->manager().dispatch_player_disconnected(scene_, peer);
    };
    auto& net_scripts = script_subsystem_->manager();
    net_receive_subsystem_ = add_subsystem<NetReceiveSubsystem>(*net_manager_, net_scripts);
    net_send_subsystem_    = add_subsystem<NetSendSubsystem>(*net_manager_, net_scripts);
    // NetManager resolves preset spawns through the resource manager, delivered
    // per-frame via TickContext (created later in init_resources()).

    // NetReceive must run before scripts so they see up-to-date remote state.
    play_phase_0_.push_back(net_receive_subsystem_);
    play_phase_0_.push_back(script_subsystem_);
    play_phase_0_.push_back(camera_subsystem_);
    // The environment is an input to physics: the wind that pushes bodies and
    // the sea and wakes they float on are resolved there, and have to be this
    // frame's, so while playing it runs just ahead of the fixed steps. Outside
    // play it ticks every frame with the always subsystems instead. Either way
    // it resolves before anything renders: the frame's sun, sky and fog come
    // from it, not from the scene's nodes.
    play_phase_1_.push_back(environment_subsystem_);
    // The simulation, run once per fixed step rather than once per frame:
    // the environment's side first (the wind and the water physics reads),
    // then scripts' fixed_tick, so what they set moves with this step.
    play_fixed_.push_back(environment_subsystem_);
    play_fixed_.push_back(script_subsystem_);
    play_fixed_.push_back(physics_subsystem_);
    play_phase_2_.push_back(vfx_subsystem_);
    // Animation is an input to physics — bone-driven colliders have to be at
    // the pose the solver collides against, not one frame behind it — so while
    // playing it runs ahead of the play phases rather than with the always
    // subsystems. Outside play it still ticks every frame so the animation
    // editor can scrub.
    early_always_.push_back(sprite_anim_subsystem_);
    // NetSend runs after physics + compute_world_matrices so it transmits final
    // post-physics transforms, at its own fixed network rate.
    late_always_.push_back(net_send_subsystem_);
    late_play_.push_back(audio_subsystem_);

    vulkan_context_->set_animation_system(&animation_subsystem_->system());
#ifndef NDEBUG
    shader_watcher_.emplace(base_path::resolve("../shaders"), base_path::resolve("shaders"));
    fjgraph_watcher_.emplace(
        base_path::resolve("assets/render_graphs/default.fjgraph"),
        [this](const std::string& path) { vulkan_context_->reload_fjgraph(path); });
#endif
}

void Engine::init_project(const std::filesystem::path& project_path,
                          const std::string& create_name) {
    // Scaffold first so the open path below picks up the fresh manifest.
    // `project_path` is the project root either way — for a create, its parent
    // is the containing directory the user picked. Only the directories and
    // manifest are made here; init_scene() supplies the default scene once the
    // renderer and resource manager exist.
    if (!create_name.empty() && !project_path.empty()) {
        Project::create(project_path.parent_path(), create_name);
    }

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

    // A script writing what the simulation owns from outside its steps is
    // reported while a game is made; a shipped game does not look.
    script_subsystem_->manager().set_write_reports(!game_mode_);

    if (!project_path.empty()) {
        project_mgr_.open(project_path);
        if (project_mgr_.active()) {
            window_.set_title("Fjell - " + project_mgr_.active()->config().name);
            // Push the project's saved render settings into the live pipeline.
            // Safe here because VulkanContext + passes are already constructed
            // (see Engine ctor order: vulkan_context_ before init_project).
            vulkan_context_->set_render_settings(project_mgr_.active()->config().render);
            const auto tick_rate =
                static_cast<float>(project_mgr_.active()->config().network.tick_rate);
            net_send_subsystem_->set_tick_rate(tick_rate);
            net_manager_->set_tick_rate(tick_rate);  // match the interp timeline to send rate
            // The on_opened handler that does this for a project opened from
            // the menu is bound later in startup.
            prebake_project_fonts(project_mgr_.active()->root());
        }
    }

    input_.attach(window_);
}

void Engine::init_ui() {
    imgui_layer_ = std::make_unique<ImGuiLayer>(window_, vulkan_context_->gpu_device(),
                                                vulkan_context_->swapchain_format(), engine_imgui_files());

    vulkan_context_->set_overlay_callback([this](gpu::RenderEncoder& pass) { imgui_layer_->render(pass); });

    ui_subsystem_ = add_subsystem<UiSubsystem>(*vulkan_context_,
                                               script_subsystem_->manager());
    play_phase_0_.push_back(ui_subsystem_);

    vulkan_context_->set_world_ui_callback(
        [this](gpu::RenderEncoder& pass, gpu::BindGroup globals, gpu::BindGroup bindless,
               glm::uvec2 extent, uint32_t frame_index) {
            for (const auto& rec : ui_subsystem_->world_records()) {
                rec.game_ui->update(rec.push.viewport_width,
                                     rec.push.viewport_height,
                                     frame_index, clock_.delta_time(), nullptr);
                rec.game_ui->record_world(pass, globals, bindless, extent, frame_index, rec.push,
                                          rec.prev_mvp);
            }
        });
}

void Engine::init_resources() {
    resource_manager_ = std::make_unique<ResourceManager>(
        vulkan_context_->gpu_device(),
        vulkan_context_->bindless_textures(), vulkan_context_->mega_buffer(),
        vulkan_context_->meshlet_buffer(), vulkan_context_->pipeline_cache());

    resource_manager_->set_viewport_resolver([this](const std::string& name) -> uint32_t {
        return vulkan_context_->scene_viewport_bindless_index(name);
    });

    asset_loader_ = std::make_unique<AssetLoader>(
        thread_pool_, *resource_manager_,
        *vulkan_context_->mega_buffer(), *vulkan_context_->meshlet_buffer());

    default_texture_ = resource_manager_->load_texture(
        base_path::resolve("engine_assets/textures/test.png")).value_or(TextureHandle{});
    sprite_anim_subsystem_->set_resource_manager(resource_manager_.get());
    ui_subsystem_->set_resource_manager(resource_manager_.get());
    ui_subsystem_->set_net_manager(net_manager_.get());

    auto& ss = script_services();
    ss.input = &input_;
    ss.resources = resource_manager_.get();
    ss.project = project_mgr_.active();
    ss.vfx = vfx_subsystem_;
    ss.audio = audio_subsystem_;
    ss.camera = camera_subsystem_;
    ss.environment = environment_subsystem_;
    ss.vulkan = vulkan_context_.get();
    ss.input_actions = &input_action_map_;
    ss.timers = &script_subsystem_->manager().timers();
    ss.clock = &clock_;
    ss.fixed_timestep = &play_controller_.fixed_timestep();
    ss.step_input = &play_controller_.step_input();
    ss.net = net_manager_.get();
    debug_draw_queue_ = std::make_unique<DebugDrawQueue>();
    ss.debug_queue = debug_draw_queue_.get();
    vulkan_context_->set_debug_draw_queue(debug_draw_queue_.get());
    screen_debug_queue_ = std::make_unique<ScreenDebugQueue>();
    ss.screen_debug_queue = screen_debug_queue_.get();

    vulkan_context_->set_resource_manager(resource_manager_.get());
    vulkan_context_->set_project(project_mgr_.active());
    capture_service_.set_project(project_mgr_.active());
    resource_manager_->set_project(project_mgr_.active());
#ifndef NDEBUG
    if (shader_watcher_) {
        const auto* project = project_mgr_.active();
        shader_watcher_->watch_project(
            project ? project->resolve("shaders") : std::string{});
    }
#endif
}

void Engine::init_scene() {
    if (project_mgr_.active()) {
        current_scene_path_ = project_mgr_.active()->resolve(project_mgr_.active()->config().default_scene);
    } else if (game_mode_) {
        current_scene_path_ = base_path::resolve("scenes/main.fjell");
    } else {
        current_scene_path_ = base_path::resolve(SCENE_FILE);
    }

    if (std::filesystem::exists(current_scene_path_)) {
        auto result = Scene::load(current_scene_path_, *resource_manager_, project_mgr_.active());
        if (result) {
            vulkan_context_->release_scene_particles(scene_);
            scene_ = std::move(*result);
        } else {
            FJELL_CORE_ERROR("{}", result.error());
            create_default_scene();
        }
        vulkan_context_->sync_custom_depth_pipelines();
    } else {
        create_default_scene();
    }
    load_input_actions();

    if (game_mode_) {
        auto game_so = base_path::resolve(std::string("game") + platform::SHARED_LIB_EXT);
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
}

AssetThumbnailRenderer::Deps Engine::asset_thumbnail_deps() {
    return {
        .vulkan_context = vulkan_context_.get(),
        .resource_manager = resource_manager_.get(),
        .icon_cache = icon_cache_.get(),
        .thread_pool = &thread_pool_,
        .project = project_mgr_.active(),
    };
}

void Engine::init_content_browser() {
    icon_cache_ = std::make_unique<IconCache>(vulkan_context_->gpu_device(), thread_pool_,
                                              base_path::resolve("engine_assets/icons"));
    IconCache::set_instance(icon_cache_.get());

    AssetThumbnailRenderer::set_instance(&asset_thumbnails_);
    asset_picker::set_thread_pool(&thread_pool_);
    content_browser_.init(icon_cache_.get(), &asset_thumbnails_, resource_manager_.get());
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

            for (int i = 0; i < static_cast<int>(contexts_.size()); ++i) {
                if (entry->matches(*contexts_[i], path)) {
                    entry->open(*contexts_[i], path);
                    switch_context(i);
                    return;
                }
            }

            auto ctx = entry->create(path);
            contexts_.push_back(std::move(ctx));
            switch_context(static_cast<int>(contexts_.size()) - 1);
        }));
    connections_.push_back(content_browser_.on_create_preset.bind(
        [this](uint32_t node_id, const std::string& directory) {
            // The preset is written from what the scripts' entries hold, as a
            // scene is.
            if (!play_controller_.playing()) {
                script_subsystem_->manager().serialize_all(scene_);
            }
            auto preset = PresetTemplate::from_subtree(scene_, node_id);
            if (!preset) {
                FJELL_CORE_ERROR("{}", preset.error());
                return;
            }
            auto* node = scene_.find(node_id);
            std::string name = node ? node->name : "preset";
            auto path = std::filesystem::path(directory) / (name + ".fjp");
            if (std::filesystem::exists(path)) {
                for (int i = 1; i < 100; ++i) {
                    auto alt = std::filesystem::path(directory) / (name + "_" + std::to_string(i) + ".fjp");
                    if (!std::filesystem::exists(alt)) { path = alt; break; }
                }
            }
            auto result = preset->save(path.string(), project_mgr_.active());
            if (!result) {
                FJELL_CORE_ERROR("Failed to save preset: {}", result.error());
            }
        }));
    connections_.push_back(content_browser_.on_instantiate_preset.bind(
        [this](const std::string& rel_path) {
            const auto* project = project_mgr_.active();
            auto resolved = base_path::resolve_asset(rel_path, project);
            auto result = PresetTemplate::load(resolved, *resource_manager_, project);
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
    // Import model from content browser right-click
    connections_.push_back(content_browser_.on_asset_moved.bind(
        [this](const std::string& from, const std::string& to) {
            auto* proj = project_mgr_.active();
            if (!proj || !proj->asset_db()) return;
            auto* db = proj->asset_db();
            if (const auto* entry = db->find_by_path(from)) {
                db->notify_move(entry->id, to);
                (void)db->save();
            }
        }));
    content_browser_.find_references = [this](const std::string& path) {
        std::vector<std::string> out;
        auto* proj = project_mgr_.active();
        if (!proj || !proj->asset_db()) return out;
        const auto* entry = proj->asset_db()->find_by_path(path);
        if (!entry) return out;
        for (const auto& ref : find_asset_references(proj->root(), *proj->asset_db(),
                                                     entry->id)) {
            out.push_back(ref.relative_path);
        }
        return out;
    };

    connections_.push_back(content_browser_.on_asset_deleted.bind(
        [this](const std::string& path) {
            auto* proj = project_mgr_.active();
            if (!proj || !proj->asset_db()) return;
            auto* db = proj->asset_db();
            if (const auto* entry = db->find_by_path(path)) {
                db->remove(entry->id);
                (void)db->save();
            }
        }));
    connections_.push_back(content_browser_.on_import_model.bind(
        [this](const std::string& path) {
            auto abs = base_path::resolve_asset(path, project_mgr_.active());
            auto output_dir = project_mgr_.active()
                ? project_mgr_.active()->root() / "models"
                : std::filesystem::path{};
            if (auto* proj = project_mgr_.active()) {
                import_dialog_.set_project(proj->root(), proj->config().texture_import_dir);
            } else {
                import_dialog_.set_project({}, "assets/textures");
            }
            import_dialog_.open(abs, output_dir);
        }));

    // Import completion — refresh content browser
    connections_.push_back(import_dialog_.on_show_result.bind(
        [this](const std::filesystem::path& file) { content_browser_.navigate_to(file.parent_path()); }));
    connections_.push_back(import_dialog_.on_import_complete.bind(
        [this](bool success, const std::vector<std::string>& /*files*/) {
            if (success) {
                content_browser_.refresh_current_dir();
            }
        }));

    // Drag-and-drop files onto the editor window
    connections_.push_back(window_.on_files_dropped.bind(
        [this](const std::vector<std::string>& paths) {
            for (const auto& p : paths) {
                auto ext = std::filesystem::path(p).extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                if (ext == ".glb" || ext == ".gltf" || ext == ".fbx") {
                    auto output_dir = project_mgr_.active()
                        ? project_mgr_.active()->root() / "models"
                        : std::filesystem::path{};
                    import_dialog_.open(p, output_dir);
                    return; // one dialog at a time
                }
            }
        }));

    if (project_mgr_.active()) {
        content_browser_.set_root(project_mgr_.active()->root());
    }
}

void Engine::init_editor() {
    bind_menu_bar();

    settings_window_.init(vulkan_context_.get(), &project_mgr_);
    connections_.push_back(settings_window_.on_save.bind([this]() { save_project(); }));

    if (project_mgr_.active()) {
        auto& layout = project_mgr_.active()->config().layout;
        if (!layout.empty()) {
            load_layout(layout);
        }
    }

    play_controller_.init(PlayServices{
        .scene = &scene_,
        .input = &input_,
        .clock = &clock_,
        .input_map = &input_action_map_,
        .project_mgr = &project_mgr_,
        .resources = resource_manager_.get(),
        .vulkan = vulkan_context_.get(),
        .window = &window_,
        .screen_debug = screen_debug_queue_.get(),
        .scripts = script_subsystem_,
        .physics = physics_subsystem_,
        .vfx = vfx_subsystem_,
        .camera = camera_subsystem_,
        .audio = audio_subsystem_,
        .sprite_anim = sprite_anim_subsystem_,
        .environment = environment_subsystem_,
        .ui = ui_subsystem_,
    }, game_mode_);

    {
        SceneEditorDeps deps{
            .scene = &scene_,
            .vulkan_context = vulkan_context_.get(),
            .resource_manager = resource_manager_.get(),
            .asset_loader = asset_loader_.get(),
            .input = &input_,
            .clock = &clock_,
            .script_subsystem = script_subsystem_,
            .project_mgr = &project_mgr_,
            .icon_cache = icon_cache_.get(),
            .thread_pool = &thread_pool_,
            .window = &window_,
            .content_browser = &content_browser_,
            .net_manager = net_manager_.get(),
            .camera_subsystem = camera_subsystem_,
            .screen_debug_queue = screen_debug_queue_.get(),
            .play = &play_controller_,
            .physics = physics_subsystem_,
            .environment = environment_subsystem_,
        };
        auto ctx = std::make_unique<SceneEditorContext>(deps);
        ctx->init();
        scene_context_ = ctx.get();
        contexts_.push_back(std::move(ctx));
        active_context_idx_ = 0;
    }

    context_registry_.register_type("material", {
        .create = [this](const std::string& path) -> std::unique_ptr<EngineEditorContext> {
            MaterialEditorDeps deps{
                .vulkan_context = vulkan_context_.get(),
                .resource_manager = resource_manager_.get(),
                .input = &input_,
                .clock = &clock_,
                .project_mgr = &project_mgr_,
                .icon_cache = icon_cache_.get(),
                .asset_thumbnails = &asset_thumbnails_
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

    context_registry_.register_type("mesh", {
        .create = [this](const std::string& path) -> std::unique_ptr<EngineEditorContext> {
            MeshEditorDeps deps{
                .vulkan_context = vulkan_context_.get(),
                .resource_manager = resource_manager_.get(),
                .input = &input_,
                .clock = &clock_,
                .project_mgr = &project_mgr_,
                .icon_cache = icon_cache_.get(),
                .asset_thumbnails = &asset_thumbnails_,
                .import_model = [this](const std::string& source) {
                    content_browser_.on_import_model.broadcast(source);
                }
            };
            auto ctx = std::make_unique<MeshEditorContext>(deps);
            ctx->init();
            ctx->open_mesh(path);
            return ctx;
        },
        .open = [](EditorContext&, const std::string&) {},
        .matches = [](const EditorContext& ctx, const std::string& path) {
            return ctx.asset_path() == path;
        }
    });

    context_registry_.register_type("day_profile", {
        .create = [this](const std::string& path) -> std::unique_ptr<EngineEditorContext> {
            TrackAssetEditorDeps deps{
                .vulkan_context = vulkan_context_.get(),
                .resource_manager = resource_manager_.get(),
                .project_mgr = &project_mgr_,
                .input = &input_,
                .scene = &scene_,
            };
            auto ctx = std::make_unique<DayProfileEditorContext>(deps);
            ctx->init();
            ctx->open(path);
            return ctx;
        },
        .open = [](EditorContext&, const std::string&) {},
        .matches = [](const EditorContext& ctx, const std::string& path) {
            return ctx.asset_path() == path;
        }
    });

    context_registry_.register_type("weather", {
        .create = [this](const std::string& path) -> std::unique_ptr<EngineEditorContext> {
            TrackAssetEditorDeps deps{
                .vulkan_context = vulkan_context_.get(),
                .resource_manager = resource_manager_.get(),
                .project_mgr = &project_mgr_,
                .input = &input_,
                .scene = &scene_,
                .environment = environment_subsystem_,
            };
            auto ctx = std::make_unique<WeatherEditorContext>(deps);
            ctx->init();
            ctx->open(path);
            return ctx;
        },
        .open = [](EditorContext&, const std::string&) {},
        .matches = [](const EditorContext& ctx, const std::string& path) {
            return ctx.asset_path() == path;
        }
    });

    context_registry_.register_type("water", {
        .create = [this](const std::string& path) -> std::unique_ptr<EngineEditorContext> {
            WaterEditorDeps deps{
                .vulkan_context = vulkan_context_.get(),
                .resource_manager = resource_manager_.get(),
                .project_mgr = &project_mgr_,
                .input = &input_,
                .thread_pool = &thread_pool_,
                .icon_cache = icon_cache_.get(),
                .content_browser = &content_browser_,
                .scene = &scene_,
            };
            auto ctx = std::make_unique<WaterEditorContext>(deps);
            ctx->init();
            ctx->open_water(path);
            return ctx;
        },
        .open = [](EditorContext&, const std::string&) {},
        .matches = [](const EditorContext& ctx, const std::string& path) {
            return ctx.asset_path() == path;
        }
    });

    context_registry_.register_type("font", {
        .create = [this](const std::string& path) -> std::unique_ptr<EngineEditorContext> {
            FontEditorDeps deps{
                .vulkan_context = vulkan_context_.get(),
                .resource_manager = resource_manager_.get(),
                .project_mgr = &project_mgr_,
                .asset_thumbnails = &asset_thumbnails_,
            };
            auto ctx = std::make_unique<FontEditorContext>(deps);
            ctx->init();
            ctx->open_font(path);
            return ctx;
        },
        .open = [](EditorContext&, const std::string&) {},
        .matches = [](const EditorContext& ctx, const std::string& path) {
            return ctx.asset_path() == path;
        }
    });

    context_registry_.register_type("ui", {
        .create = [this](const std::string& path) -> std::unique_ptr<EngineEditorContext> {
            UiEditorDeps deps{
                .vulkan_context = vulkan_context_.get(),
                .resource_manager = resource_manager_.get(),
                .input = &input_,
                .clock = &clock_,
                .project_mgr = &project_mgr_,
                .icon_cache = icon_cache_.get(),
                .script_manager = &script_subsystem_->manager(),
                .widget_checks = &widget_checks_,
                .scene = &scene_,
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
        .create = [this](const std::string& path) -> std::unique_ptr<EngineEditorContext> {
            VfxEditorDeps deps{
                .vulkan_context = vulkan_context_.get(),
                .resource_manager = resource_manager_.get(),
                .input = &input_,
                .clock = &clock_,
                .project_mgr = &project_mgr_,
                .icon_cache = icon_cache_.get(),
                .content_browser = &content_browser_
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

    auto anim_factory = [this](const std::string&) -> std::unique_ptr<EngineEditorContext> {
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
    // Each skeleton and animset has its own tab; opening one that is open
    // shows its tab, and a clip opens in the tab of the skeleton it belongs to.
    auto anim_matches = [](const EditorContext& ctx, const std::string& path) {
        return dynamic_cast<const AnimationEditorContext*>(&ctx) != nullptr && ctx.asset_path() == path;
    };

    context_registry_.register_type("skeleton", {
        .create = [anim_factory](const std::string& path) {
            auto ctx = anim_factory(path);
            static_cast<AnimationEditorContext&>(*ctx).open_skeleton(path);
            return ctx;
        },
        .open = [](EditorContext&, const std::string&) {},
        .matches = anim_matches
    });

    context_registry_.register_type("animation", {
        .create = [anim_factory](const std::string& path) {
            auto ctx = anim_factory(path);
            static_cast<AnimationEditorContext&>(*ctx).open_clip(path);
            return ctx;
        },
        .open = [](EditorContext& ctx, const std::string& path) {
            static_cast<AnimationEditorContext&>(ctx).select_clip(path);
        },
        .matches = [](const EditorContext& ctx, const std::string& path) {
            const auto* anim = dynamic_cast<const AnimationEditorContext*>(&ctx);
            return anim != nullptr && anim->has_clip(path);
        }
    });

    context_registry_.register_type("animset", {
        .create = [anim_factory](const std::string& path) {
            auto ctx = anim_factory(path);
            static_cast<AnimationEditorContext&>(*ctx).open_animset(path);
            return ctx;
        },
        .open = [](EditorContext&, const std::string&) {},
        .matches = anim_matches
    });

    context_registry_.register_type("blend_space", {
        .create = [this](const std::string& path) -> std::unique_ptr<EngineEditorContext> {
            BlendSpaceEditorDeps deps{
                .vulkan_context = vulkan_context_.get(),
                .resource_manager = resource_manager_.get(),
                .input = &input_,
                .clock = &clock_,
                .animation_subsystem = animation_subsystem_,
                .project_mgr = &project_mgr_,
                .icon_cache = icon_cache_.get()
            };
            auto ctx = std::make_unique<BlendSpaceEditorContext>(deps);
            ctx->init();
            ctx->open(path);
            return ctx;
        },
        .open = [](EditorContext&, const std::string&) {},
        .matches = [](const EditorContext& ctx, const std::string& path) {
            return dynamic_cast<const BlendSpaceEditorContext*>(&ctx) != nullptr && ctx.asset_path() == path;
        }
    });

    auto input_factory = [this](const std::string&) -> std::unique_ptr<EngineEditorContext> {
        InputMappingEditorDeps deps{
            .vulkan_context = vulkan_context_.get(),
            .input = &input_,
            .project_mgr = &project_mgr_,
            .input_action_map = &input_action_map_
        };
        auto ctx = std::make_unique<InputMappingEditorContext>(deps);
        ctx->init();
        return ctx;
    };
    auto input_matches = [](const EditorContext& ctx, const std::string& path) {
        return dynamic_cast<const InputMappingEditorContext*>(&ctx) != nullptr && ctx.asset_path() == path;
    };

    context_registry_.register_type("input_mapping", {
        .create = [input_factory](const std::string& path) {
            auto ctx = input_factory(path);
            static_cast<InputMappingEditorContext&>(*ctx).open_context(path);
            return ctx;
        },
        .open = [](EditorContext&, const std::string&) {},
        .matches = input_matches
    });
}

Engine::~Engine() {
    // Quitting while still playing (window closed, process killed) skips
    // run()'s normal path to stop() entirely — nothing in the main loop calls
    // it on the way out. Without this, subsystems_.clear() below destroys
    // UiSubsystem's widget instances after ScriptSubsystem (registered
    // earlier, so destroyed first) has already unloaded game.so in
    // ScriptManager's destructor — a widget's on:click Connection still
    // holds a std::function whose vtable lives in that now-unloaded library,
    // and destroying it walks into freed memory. stop() runs the same
    // teardown play's own Stop button does, while every subsystem the game
    // depends on is still alive; it's a no-op if play was never started.
    play_controller_.stop();

    vulkan_context_->wait_idle();
    // Uploads are only ever submitted from submit_and_present(), which returns
    // early until a frame has been recorded. A run that ends before its first
    // frame — a bad scenario file, a failed init — leaves everything the
    // constructor staged sitting in an open batch. The teardown below frees
    // those buffers, and the batch's command buffer then goes invalid with
    // them, so UploadContext's own destructor would end and submit a command
    // buffer full of destroyed handles and then wait forever on a timeline
    // value nothing can signal. Draining here, while the resources are alive,
    // is a no-op on every path that rendered.
    vulkan_context_->upload_context().wait_all();
    vulkan_context_->set_overlay_callback(nullptr);
    AssetThumbnailRenderer::set_instance(nullptr);
    // Drops any directory walk still in flight, so nothing is waiting on the
    // pool once it goes.
    asset_picker::set_thread_pool(nullptr);
    asset_picker::invalidate();
    asset_thumbnails_.shutdown(asset_thumbnail_deps());
    for (auto& ctx : contexts_) ctx->shutdown();
    contexts_.clear();
    scene_context_ = nullptr;
    scene_ = {};
    // Free VMA-backed resources before VulkanContext (and its allocator) is destroyed.
    subsystems_.clear();
    asset_loader_.reset();
    resource_manager_.reset();
    content_browser_.shutdown();
    IconCache::set_instance(nullptr);
    icon_cache_.reset();
    imgui_layer_.reset();
    FJELL_CORE_INFO("Engine shut down");
}

void Engine::save_scene() {
    // Flush live script-component state into each node's serialized JSON before
    // writing — otherwise inspector edits (e.g. asset Refs) made since load are
    // lost, since the file is written from the stored JSON, not the instances.
    if (!play_controller_.playing()) {
        script_subsystem_->manager().serialize_all(scene_);
    }
    save_material_assets();
    save_terrain_assets();
    if (auto r = scene_.save(current_scene_path_, project_mgr_.active()); !r) {
        FJELL_CORE_ERROR("{}", r.error());
        return;
    }

    // The scene is in memory right now, which is the one time its
    // thumbnail is cheap: from the scene's camera when it has one, else
    // from wherever the editor viewport is looking.
    AssetThumbnailRenderer::View view;
    if (auto* scene_cam = camera_subsystem_->active_camera(scene_)) {
        view = {.view = scene_cam->view_matrix(), .projection = scene_cam->projection_matrix(1.0f),
                .position = scene_cam->position()};
    } else if (auto* editor_cam = scene_context_->viewport_manager().active_camera()) {
        view = {.view = editor_cam->view_matrix(), .projection = editor_cam->projection_matrix(1.0f),
                .position = editor_cam->position()};
    } else {
        return;
    }
    asset_thumbnails_.capture_scene(scene_, current_scene_path_, view);
    asset_thumbnails_.refresh(current_scene_path_);
}

void Engine::save_material_assets() {
    auto* cache = vulkan_context_->pipeline_cache();
    auto& bindless = vulkan_context_->bindless_textures();
    if (!cache) return;

    // Every component that carries an editable .fjmat reference. The
    // inspector reaches a material through the component registry, so any
    // component publishing one can have its params changed in place — and
    // every one of those has to be listed here or those edits are lost on
    // reload with nothing to say they were dropped.
    auto save_material = [&](const Material* material, const std::string& reference) {
        if (material == nullptr) return;
        if (!base_path::references_asset(reference, ".fjmat")) return;

        uint32_t pid = material->pipeline_id();
        if (pid >= cache->shader_count()) return;

        const auto& meta = cache->metadata(pid);
        const auto& gpu = bindless.material_at(material->material_id());

        // Read current params back from CPU buffer
        std::string mat_path = base_path::resolve_asset(reference, project_mgr_.active());

        MaterialAsset asset;
        try {
            asset = load_material_asset(mat_path);
        } catch (...) {
            return;
        }

        // Update custom param values from live buffer
        for (const auto& u : meta.uniforms) {
            if (u.type == UniformType::Sampler2D) continue;
            uint32_t fcount = uniform_float_count(u.type);
            std::vector<float> vals(fcount);
            for (uint32_t i = 0; i < fcount; ++i) {
                vals[i] = bindless.read_param(gpu.param_offset + u.slot + i);
            }
            decode_uniform_from_gpu(u, vals);
            asset.params[u.name] = vals;
        }

        // The inspector edits the base factors on the GPU row, so that is
        // where the current values are.
        write_base_params(asset, {
            .base_color_factor = material->params().base_color_factor,
            .metallic_factor = gpu.base_params[4],
            .roughness_factor = gpu.base_params[5],
            .uv_scale = {gpu.base_params[7], gpu.base_params[8]},
        });

        save_material_asset(mat_path, asset);
        asset_thumbnails_.refresh(mat_path);
    };

    for (auto& obj : scene_.objects()) {
        if (const auto* mesh = obj->components.get<MeshComponent>()) {
            save_material(mesh->material, mesh->material_name);
        }
        if (const auto* terrain = obj->components.get<TerrainComponent>()) {
            save_material(terrain->material, terrain->material_path);
        }
        if (const auto* water = obj->components.get<WaterComponent>()) {
            save_material(water->material, water->material_path);
        }
        if (const auto* fog = dynamic_cast<const VolumetricFogNode*>(obj.get())) {
            save_material(fog->material, fog->material_path);
        } else if (const auto* volume = dynamic_cast<const FogVolumeNode*>(obj.get())) {
            save_material(volume->material, volume->material_path);
        } else if (const auto* post = dynamic_cast<const PostProcessVolumeNode*>(obj.get())) {
            save_material(post->material, post->material_path);
        }
    }
}

// Sculpted heights live in the .fjterrain, not the scene file, so saving the
// scene writes every terrain a stroke has touched since it was loaded or last
// saved. An asset shared by several nodes is written once.
void Engine::save_terrain_assets() {
    for (auto& obj : scene_.objects()) {
        auto* tc = obj->components.get<TerrainComponent>();
        if (tc == nullptr || tc->terrain == nullptr || !tc->terrain->dirty()) continue;
        if (auto r = tc->terrain->save(); !r) {
            FJELL_CORE_ERROR("terrain: {}", r.error());
        }
    }
}

void Engine::create_project(const std::filesystem::path& parent, const std::string& name) {
    project_mgr_.create(parent, name);
    if (!project_mgr_.active()) return;

    window_.set_title("Fjell - " + project_mgr_.active()->config().name);
    current_scene_path_ =
        project_mgr_.active()->resolve(project_mgr_.active()->config().default_scene);
    scene_.clear();
    create_default_scene();
    notify_scene_replaced();
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
    // Reserve the bottom strip for the status bar so panels don't overlap it.
    ImVec2 host_size = viewport->WorkSize;
    host_size.y = std::max(0.0f, host_size.y - theme::STATUS_BAR);
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(host_size);
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

    (void)ui::begin_host_window("##DockHost", host_flags);

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
            context_layout_templates_["scene"] = ctx.dockspace_id();
            continue;
        }

        if (i == 0) {
            scene_context_->request_focus_viewport();
        }

        // Try to clone layout from an existing context of the same type,
        // then from saved JSON, then fall back to hardcoded default.
        const char* type = ctx.context_type();
        auto tmpl_it = context_layout_templates_.find(type);
        bool built = false;

        // 1. Clone from live context of the same type
        if (tmpl_it != context_layout_templates_.end() && tmpl_it->second != 0
            && tmpl_it->second != ctx.dockspace_id()) {
            if (ImGui::DockBuilderGetNode(tmpl_it->second) != nullptr) {
                clone_dockspace_from_template(ctx, tmpl_it->second);
                built = true;
            }
        }

        // 2. Rebuild from saved JSON layout
        // A layout saved before a window was renamed or added would leave
        // that window floating; the preset is used instead.
        if (!built && saved_context_layouts_.contains(type)
            && layout_names_every_window(saved_context_layouts_[type], ctx)) {
            rebuild_dockspace_from_json(ctx, saved_context_layouts_[type]);
            built = true;
        }

        // 3. Hardcoded default
        if (!built) {
            build_default_dockspace(ctx, viewport);
        }

        context_layout_templates_[type] = ctx.dockspace_id();
    }

    ImGui::End();
}

bool Engine::layout_names_every_window(const nlohmann::json& layout_json, const EditorContext& ctx) {
    std::unordered_set<std::string> saved;
    std::function<void(const nlohmann::json&)> collect = [&](const nlohmann::json& node) {
        if (node.contains("windows")) {
            for (const auto& name : node["windows"]) saved.insert(name.get<std::string>());
        }
        if (node.contains("children")) {
            for (const auto& child : node["children"]) collect(child);
        }
    };
    collect(layout_json);
    for (const auto& title : ctx.docked_window_names()) {
        // Saved layouts keep the name without its "##ctxN" suffix.
        if (!saved.contains(title.substr(0, title.find("##")))) return false;
    }
    return true;
}

void Engine::rebuild_dockspace_from_json(EditorContext& ctx, const nlohmann::json& layout_json) {
    ImGuiID dst_dock_id = ctx.dockspace_id();
    ImGui::DockBuilderRemoveNode(dst_dock_id);
    ImGui::DockBuilderAddNode(dst_dock_id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dst_dock_id, ImGui::GetMainViewport()->WorkSize);

    int ci = ctx.context_index();

    // Recursively rebuild the split tree, then dock windows into leaf nodes.
    // Splits must happen before docking — splitting a node with docked windows orphans them.
    struct DockCmd { std::string title; ImGuiID dock_id; };
    std::vector<DockCmd> dock_cmds;

    std::function<void(const nlohmann::json&, ImGuiID)> build_node;
    build_node = [&](const nlohmann::json& node, ImGuiID dock_id) {
        // Split first if this is a branch node
        if (node.contains("split")) {
            float ratio = node.value("ratio", 0.5f);
            ImGuiDir dir = (node["split"] == "x") ? ImGuiDir_Left : ImGuiDir_Up;
            ImGuiID child0 = 0, child1 = 0;
            ImGui::DockBuilderSplitNode(dock_id, dir, ratio, &child0, &child1);

            const auto& children = node["children"];
            if (children.size() >= 2) {
                build_node(children[0], child0);
                build_node(children[1], child1);
            }
        }

        // Collect window dock commands (applied after all splits)
        if (node.contains("windows")) {
            for (const auto& base_name : node["windows"]) {
                char title[128];
                std::snprintf(title, sizeof(title), "%s##ctx%d",
                              base_name.get<std::string>().c_str(), ci);
                dock_cmds.push_back({title, dock_id});
            }
        }
    };

    build_node(layout_json, dst_dock_id);
    for (const auto& cmd : dock_cmds) {
        ImGui::DockBuilderDockWindow(cmd.title.c_str(), cmd.dock_id);
    }
    ImGui::DockBuilderFinish(dst_dock_id);
}

void Engine::build_default_dockspace(EditorContext& ctx, ImGuiViewport* viewport) {
    ImGuiID ctx_dock_id = ctx.dockspace_id();
    ImGui::DockBuilderRemoveNode(ctx_dock_id);
    ImGui::DockBuilderAddNode(ctx_dock_id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(ctx_dock_id, viewport->WorkSize);
    ctx.setup_dockspace(ctx_dock_id);
    ImGui::DockBuilderFinish(ctx_dock_id);
}

void Engine::clone_dockspace_from_template(EditorContext& dst_ctx, ImGuiID src_dock_id) {
    auto dst_names = dst_ctx.docked_window_names();

    // Build source window names — either from a live context or from template
    // placeholder names ("CtxTmpl_material/Code Editor##ctx1", etc.)
    std::vector<std::string> src_names;
    EditorContext* src_ctx = nullptr;
    for (auto& c : contexts_) {
        if (c->dockspace_id() == src_dock_id) {
            src_ctx = c.get();
            break;
        }
    }

    if (src_ctx) {
        src_names = src_ctx->docked_window_names();
    } else {
        // Source is a saved template node. Templates store windows as
        // "CtxTmpl_{type}/{base_name}" where base_name has ##ctx suffix stripped.
        char tmpl_prefix[64];
        std::snprintf(tmpl_prefix, sizeof(tmpl_prefix), "CtxTmpl_%s/", dst_ctx.context_type());
        for (const auto& dname : dst_names) {
            std::string base = dname;
            auto pos = base.find("##ctx");
            if (pos != std::string::npos) base.erase(pos);
            src_names.push_back(std::string(tmpl_prefix) + base);
        }
    }

    // Walk the source dock tree and record the split structure + window→node mapping.
    ImGuiID dst_dock_id = dst_ctx.dockspace_id();
    ImGui::DockBuilderRemoveNode(dst_dock_id);
    ImGui::DockBuilderAddNode(dst_dock_id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dst_dock_id, ImGui::GetMainViewport()->WorkSize);

    // Build src window name → dock node ID map (from ImGui window state or settings)
    std::unordered_map<std::string, ImGuiID> src_window_dock;
    for (const auto& name : src_names) {
        if (auto* win = ImGui::FindWindowByName(name.c_str())) {
            src_window_dock[name] = win->DockId;
        } else if (auto* settings = ImGui::FindWindowSettingsByID(ImHashStr(name.c_str()))) {
            src_window_dock[name] = settings->DockId;
        }
    }

    // Recursively clone the split structure from src to dst
    struct SplitInfo { ImGuiID src_id, dst_id; };
    std::vector<SplitInfo> node_map;

    std::function<void(ImGuiDockNode*, ImGuiID)> clone_splits;
    clone_splits = [&](ImGuiDockNode* src_node, ImGuiID dst_id) {
        node_map.push_back({src_node->ID, dst_id});
        if (src_node->IsSplitNode()) {
            float total = src_node->ChildNodes[0]->SizeRef[src_node->SplitAxis]
                        + src_node->ChildNodes[1]->SizeRef[src_node->SplitAxis];
            float ratio = (total > 0.0f)
                ? src_node->ChildNodes[0]->SizeRef[src_node->SplitAxis] / total
                : 0.5f;

            ImGuiID child0 = 0, child1 = 0;
            ImGuiDir dir = (src_node->SplitAxis == ImGuiAxis_X) ? ImGuiDir_Left : ImGuiDir_Up;
            ImGui::DockBuilderSplitNode(dst_id, dir, ratio, &child0, &child1);

            clone_splits(src_node->ChildNodes[0], child0);
            clone_splits(src_node->ChildNodes[1], child1);
        }
    };

    auto* src_root = ImGui::DockBuilderGetNode(src_dock_id);
    if (src_root) {
        clone_splits(src_root, dst_dock_id);
    }

    // Dock destination windows into the cloned nodes by matching src layout
    for (size_t j = 0; j < src_names.size() && j < dst_names.size(); ++j) {
        auto it = src_window_dock.find(src_names[j]);
        if (it == src_window_dock.end()) continue;

        ImGuiID dst_node_id = 0;
        for (auto& [sid, did] : node_map) {
            if (sid == it->second) {
                dst_node_id = did;
                break;
            }
        }
        if (dst_node_id != 0) {
            ImGui::DockBuilderDockWindow(dst_names[j].c_str(), dst_node_id);
        }
    }

    ImGui::DockBuilderFinish(dst_dock_id);
}

// Hierarchy and Properties panels extracted to ui/hierarchy_panel.cpp and ui/properties_panel.cpp

void Engine::draw_console() {
    if (auto* sink = console_sink()) {
        sink->draw("Console");
    }
}

void Engine::load_project_scripts() {
    if (!project_mgr_.active()) return;
    auto so_path = project_mgr_.active()->root() / (std::string("game") + platform::SHARED_LIB_EXT);
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
        vulkan_context_->release_scene_particles(scene_);
        scene_ = std::move(*result);
        // The loaded nodes carry their scripts as saved data; the live
        // instances the inspector and play mode work on are made here.
        if (script_subsystem_->manager().is_loaded()) {
            script_subsystem_->manager().deserialize_all(scene_);
        }
    } else {
        FJELL_CORE_ERROR("{}", result.error());
        scene_.clear();
    }
    scene_context_->set_selected_id(0);
    scene_context_->command_history().clear();
    vulkan_context_->sync_custom_depth_pipelines();
    // The probes gathered light for the scene that left.
    vulkan_context_->restart_ddgi_probes();
    notify_scene_replaced();
    load_input_actions();
}

void Engine::notify_scene_replaced() {
    for (auto& subsystem : subsystems_) subsystem->notify_scene_replaced();
}


void Engine::load_input_actions() {
    int found = 0;
    for (const auto& obj : scene_.objects()) {
        const auto* ic = obj->components.get<InputComponent>();
        if (!ic) continue;
        ++found;

        auto dir = ic->actions_dir.empty() ? std::string("assets/input/actions") : ic->actions_dir;
        if (project_mgr_.active()) {
            auto resolved = project_mgr_.active()->resolve(dir);
            FJELL_CORE_DEBUG("InputActionMap: loading actions from {}", resolved);
            input_action_map_.load_actions_dir(resolved);
        }
    }
    if (found == 0) {
        FJELL_CORE_DEBUG("InputActionMap: no InputComponent found in scene");
    }
}

void Engine::bind_menu_bar() {
    connections_.push_back(menu_bar_.on_save_scene.bind(
        [this]() { save_scene(); }));
    connections_.push_back(menu_bar_.on_load_scene.bind(
        [this]() { load_scene(current_scene_path_); }));
    connections_.push_back(menu_bar_.on_save_project.bind([this]() { save_project(); }));
    connections_.push_back(menu_bar_.on_close_project.bind(
        [this]() { project_mgr_.close(); }));
    connections_.push_back(menu_bar_.on_open_settings.bind([this]() { settings_window_.toggle(); }));
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
    connections_.push_back(menu_bar_.on_open_project.bind(
        [this](const std::filesystem::path& path) {
            project_mgr_.open(path);
            vulkan_context_->set_project(project_mgr_.active());
            capture_service_.set_project(project_mgr_.active());
            if (project_mgr_.active()) {
                current_scene_path_ = project_mgr_.active()->resolve(
                    project_mgr_.active()->config().default_scene);
                if (std::filesystem::exists(current_scene_path_)) {
                    load_scene(current_scene_path_);
                } else {
                    scene_.clear();
                    create_default_scene();
                    notify_scene_replaced();
                }
                load_project_scripts();
            }
        }));
    connections_.push_back(menu_bar_.on_create_project.bind(
        [this](const std::filesystem::path& path, const std::string& name) {
            create_project(path, name);
        }));
    connections_.push_back(menu_bar_.on_save_layout.bind(
        [this](const std::string& name) { save_layout(name); }));
    connections_.push_back(menu_bar_.on_load_layout.bind(
        [this](const std::string& name) { load_layout(name); }));
    connections_.push_back(menu_bar_.on_import_model.bind(
        [this]() {
            auto output_dir = project_mgr_.active()
                ? project_mgr_.active()->root() / "models"
                : std::filesystem::path{};
            open_import_browser(output_dir);
        }));
    connections_.push_back(content_browser_.on_bake_font.bind(
        [this](const std::string& recipe) {
            vulkan_context_->font_library().prebake(recipe);
        }));
    connections_.push_back(content_browser_.on_import_into.bind(
        [this](const std::string& dir) { open_import_browser(dir); }));
    connections_.push_back(menu_bar_.on_open_capture.bind(
        [this]() { capture_popup_.open(); }));
    connections_.push_back(import_file_browser_.on_selected.bind(
        [this](const std::string& path) {
            if (auto* proj = project_mgr_.active()) {
                import_dialog_.set_project(proj->root(), proj->config().texture_import_dir);
            } else {
                import_dialog_.set_project({}, "assets/textures");
            }
            import_dialog_.open(path, import_output_dir_);
        }));

    // React to project open/close from any source
    connections_.push_back(project_mgr_.on_opened.bind(
        [this](const std::string& name) {
            window_.set_title("Fjell - " + name);
            content_browser_.set_root(project_mgr_.active()->root());
            prebake_project_fonts(project_mgr_.active()->root());
            // Defer layout restore
            auto& layout = project_mgr_.active()->config().layout;
            if (!layout.empty()) {
                load_layout(layout);
            }
        }));
    connections_.push_back(project_mgr_.on_closed.bind(
        [this]() {
            current_scene_path_ = base_path::resolve(SCENE_FILE);
            window_.set_title("Fjell Engine");
            content_browser_.set_root({});
        }));
}

nlohmann::json Engine::serialize_dockspace(EditorContext& ctx) {
    auto* root = ImGui::DockBuilderGetNode(ctx.dockspace_id());
    if (!root) return {};

    // Map source dock node ID → list of base window names docked there
    auto win_names = ctx.docked_window_names();
    std::unordered_map<ImGuiID, std::vector<std::string>> node_windows;
    for (const auto& wname : win_names) {
        ImGuiID dock_id = 0;
        if (auto* win = ImGui::FindWindowByName(wname.c_str())) {
            dock_id = win->DockId;
        }
        if (dock_id == 0) {
            if (auto* settings = ImGui::FindWindowSettingsByID(ImHashStr(wname.c_str()))) {
                dock_id = settings->DockId;
            }
        }
        if (dock_id == 0) continue;
        // Strip ##ctx suffix for stable naming
        std::string base = wname;
        auto pos = base.find("##ctx");
        if (pos != std::string::npos) base.erase(pos);
        node_windows[dock_id].push_back(base);
    }

    // Recursively serialize the dock tree
    int next_id = 0;
    std::function<nlohmann::json(ImGuiDockNode*)> serialize_node;
    serialize_node = [&](ImGuiDockNode* node) -> nlohmann::json {
        nlohmann::json j;
        int id = next_id++;
        j["id"] = id;

        if (node->IsSplitNode()) {
            float total = node->ChildNodes[0]->SizeRef[node->SplitAxis]
                        + node->ChildNodes[1]->SizeRef[node->SplitAxis];
            j["split"] = (node->SplitAxis == ImGuiAxis_X) ? "x" : "y";
            j["ratio"] = (total > 0.0f)
                ? node->ChildNodes[0]->SizeRef[node->SplitAxis] / total : 0.5f;
            j["children"] = nlohmann::json::array({
                serialize_node(node->ChildNodes[0]),
                serialize_node(node->ChildNodes[1])
            });
        }

        auto it = node_windows.find(node->ID);
        if (it != node_windows.end()) {
            j["windows"] = it->second;
        }

        return j;
    };

    return serialize_node(root);
}

// Every font recipe in the project is baked ahead of use, in the
// background, so a sheet that names one never has to wait for it. Hidden
// directories and the build tree are skipped, as the content browser skips
// them.
void Engine::prebake_project_fonts(const std::filesystem::path& root) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
    if (ec) return;
    for (; it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        const auto& entry = *it;
        auto name = entry.path().filename().string();
        if (entry.is_directory(ec) && (name.starts_with(".") || name == "build")) {
            it.disable_recursion_pending();
            continue;
        }
        if (entry.is_regular_file(ec) && entry.path().extension() == ".fjfont") {
            vulkan_context_->font_library().prebake(entry.path().string());
        }
    }
}

void Engine::check_project_widgets() {
    const Project* project = project_mgr_.active();
    if (project == nullptr || project->asset_db() == nullptr) return;
    const uint32_t library = script_subsystem_->manager().library_revision();
    if (project->root() == widgets_checked_root_ && library == widgets_checked_library_) return;
    if (project->root() != widgets_checked_root_) widget_checks_ = {}; // another game: all anew
    widgets_checked_root_ = project->root();
    widgets_checked_library_ = library;

    std::vector<std::string> widgets;
    project->asset_db()->for_each([&](const AssetEntry& entry) {
        if (entry.relative_path.ends_with(".fjwidget")) {
            widgets.push_back((project->root() / entry.relative_path).string());
        }
    });
    std::ranges::sort(widgets);
    const auto manifest = project->root() / "scripts" / "generated" / "manifest.json";
    for (const auto& p : widget_checks_.reload(manifest.string(), widgets)) {
        FJELL_CORE_WARN("{}:{}:{}: warning: {}", p.file, p.line, p.column, p.message);
    }
}

void Engine::open_import_browser(const std::filesystem::path& output_dir) {
    import_output_dir_ = output_dir;
    import_file_browser_.set_project(project_mgr_.active() ? project_mgr_.active()->root() : std::filesystem::path{});
    import_file_browser_.open("Import model", FileBrowser::Mode::select_file, "Import", {".glb", ".gltf", ".fbx"});
}

void Engine::save_layout(const std::string& name) {
    namespace fs = std::filesystem;
    auto layout_dir = project_mgr_.active()
        ? project_mgr_.active()->root() / "layouts"
        : fs::path(base_path::dir()) / "layouts";
    fs::create_directories(layout_dir);

    // Save ImGui dock state for the scene context
    auto ini_path = layout_dir / (name + ".ini");
    ImGui::SaveIniSettingsToDisk(ini_path.string().c_str());

    // Serialize each open context type's dockspace tree as JSON
    for (auto& ctx : contexts_) {
        const char* type = ctx->context_type();
        if (std::strcmp(type, "scene") == 0) continue;
        auto tree = serialize_dockspace(*ctx);
        if (!tree.is_null()) {
            saved_context_layouts_[type] = tree;
        }
    }

    // Write the context layouts sidecar
    auto ctx_path = layout_dir / (name + ".ctx.json");
    std::ofstream ctx_file(ctx_path);
    if (ctx_file.is_open()) {
        ctx_file << saved_context_layouts_.dump(2);
    }

    menu_bar_.set_active_layout(name);
    if (project_mgr_.active()) {
        project_mgr_.active()->config().layout = name;
        project_mgr_.active()->save();
    }
    FJELL_CORE_INFO("Layout saved: {}", ini_path.string());
}

void Engine::load_layout(const std::string& name) {
    namespace fs = std::filesystem;
    auto layout_dir = project_mgr_.active()
        ? project_mgr_.active()->root() / "layouts"
        : fs::path(base_path::dir()) / "layouts";

    auto ini_path = layout_dir / (name + ".ini");
    if (!fs::exists(ini_path)) return;

    // Load context layouts from sidecar JSON
    auto ctx_path = layout_dir / (name + ".ctx.json");
    if (fs::exists(ctx_path)) {
        std::ifstream ctx_file(ctx_path);
        if (ctx_file.is_open()) {
            try {
                saved_context_layouts_ = nlohmann::json::parse(ctx_file);
            } catch (const std::exception& e) {
                FJELL_CORE_WARN("Failed to parse {}: {}", ctx_path.string(), e.what());
                saved_context_layouts_ = {};
            }
        }
    }

    pending_layout_load_ = ini_path.string();
    menu_bar_.set_active_layout(name);
    has_saved_layout_ = true;
}

void Engine::draw_ui() {
    FJELL_PROFILE_SCOPE_N("draw_ui");
    // Update window title with unsaved indicator
    {
        std::string title = "Fjell";
        if (project_mgr_.active()) title += " - " + project_mgr_.active()->config().name;
        if (active_context().has_unsaved_changes()) title += " *";
        window_.set_title(title);
    }

    // Global shortcuts (Ctrl+S, Ctrl+Shift+S, Ctrl+Z/Y, Ctrl+Space)
    if (ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeyShift
        && ImGui::IsKeyPressed(ImGuiKey_S)) {
        if (auto saved = save_context(active_context()); !saved) {
            FJELL_CORE_ERROR("Couldn't save {}: {}", active_context().name(), saved.error());
        }
    }
    if (ImGui::GetIO().KeyCtrl && ImGui::GetIO().KeyShift
        && ImGui::IsKeyPressed(ImGuiKey_S)) {
        save_project();
    }
    if (!ImGui::IsAnyItemActive() && ImGui::GetIO().KeyCtrl &&
        ImGui::IsKeyPressed(ImGuiKey_Comma)) {
        settings_window_.toggle();
    }
    if (!ImGui::IsAnyItemActive() && ImGui::GetIO().KeyCtrl &&
        ImGui::IsKeyPressed(ImGuiKey_Space)) {
        content_browser_.toggle_visibility();
    }
    if (!ImGui::IsAnyItemActive() && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z)) {
        if (ImGui::GetIO().KeyShift) {
            active_context().command_history().redo();
        } else {
            active_context().command_history().undo();
        }
    }
    if (!ImGui::IsAnyItemActive() && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y)) {
        active_context().command_history().redo();
    }
    if (!ImGui::IsAnyItemActive() && ImGui::IsKeyPressed(ImGuiKey_F9)) {
        capture_popup_.open();
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
    menu_bar_.set_building(project_mgr_.is_building());
    menu_bar_.set_scene_path(current_scene_path_);
    menu_bar_.set_vulkan_context(vulkan_context_.get());
    menu_bar_.set_kit_gallery_flag(&show_kit_gallery_);
    menu_bar_.set_sdf_debug_flag(sdf_debug_panel_.open_flag());
    menu_bar_.set_viewport_manager(&scene_context_->viewport_manager());
    menu_bar_.draw();

    capture_popup_.draw(capture_service_, scene_context_->viewport_manager());
    draw_unsaved_dialogs();

    draw_context_tabs();
    active_context().draw_header();
    setup_dockspace();

    // Active context draws its own panels (including Stats, Console, History)
    active_context().draw_frame(clock_.delta_time());

    content_browser_.draw();
    settings_window_.draw();

    sdf_debug_panel_.draw(vulkan_context_->scene_sdf_pass());
    if (show_kit_gallery_) kit_gallery_.draw(&show_kit_gallery_);

    draw_status_bar();
}

TickContext Engine::make_frame_context() {
    return TickContext{
        .scene = scene_,
        .input = input_,
        .clock = clock_,
        .dt = clock_.game_delta_time(),
        .physics = physics_subsystem_->world(),
        .project = project_mgr_.active(),
        .resources = resource_manager_.get(),
        .workers = &thread_pool_,
    };
}

void Engine::run_fixed_steps() {
    FixedStepStats& stats = play_controller_.step_stats();

    // A frame over the limit counts for less game time than it took, and the
    // game runs slower through it. Said once as it starts, not every frame.
    if (clock_.game_delta_limited() && !game_time_limited_) {
        FJELL_CORE_WARN("A {:.0f} ms frame counts as {:.0f} ms of game time: a frame runs at "
                        "most {} simulation steps, so the game slows down until frames catch up",
                        clock_.delta_time() * 1000.0f, clock_.game_delta_time() * 1000.0f,
                        FixedTimestep::MAX_STEPS_PER_FRAME);
        ++stats.slowdowns;
    }
    game_time_limited_ = clock_.game_delta_limited();
    stats.dropped_seconds += clock_.game_delta_dropped();

    using StepClock = std::chrono::steady_clock;
    const auto seconds_since = [](StepClock::time_point start) {
        return std::chrono::duration<double>(StepClock::now() - start).count();
    };
    if (stats.parts.size() != play_fixed_.size()) {
        stats.parts.clear();
        for (const auto* s : play_fixed_) stats.parts.push_back({.name = s->name()});
    }
    for (auto& part : stats.parts) part.seconds = 0.0;
    stats.seconds = 0.0;

    FixedTimestep& timestep = play_controller_.fixed_timestep();
    const int steps = timestep.advance(clock_.game_delta_time());
    stats.steps = steps;
    if (steps > 0) physics_subsystem_->keep_frame_intents(scene_);
    for (int i = 0; i < steps; ++i) {
        FJELL_PROFILE_SCOPE_N("fixed_step");
        const auto step_start = StepClock::now();
        const FixedStepTime step = timestep.next_step();
        play_controller_.step_input().begin_step();
        scene_.begin_fixed_step(step.index);
        start_test_on_step();
        const FixedTickContext ctx{
            .scene = scene_,
            .dt = static_cast<float>(step.dt),
            .time = step.time,
            .index = step.index,
            .frame_steps_left = steps - i,
            .physics = physics_subsystem_->world(),
            .project = project_mgr_.active(),
            .resources = resource_manager_.get(),
            .workers = &thread_pool_,
        };
        for (std::size_t k = 0; k < play_fixed_.size(); ++k) {
            const auto part_start = StepClock::now();
            play_fixed_[k]->run_fixed(ctx);
            stats.parts[k].seconds += seconds_since(part_start);
        }

        // A step's collisions are answered, and whatever the answers spawned
        // or destroyed is settled, before the next step moves anything.
        if (physics_subsystem_->world()) {
            script_subsystem_->manager().dispatch_collision_events(
                scene_, *physics_subsystem_->world());
        }
        flush_pending();
        if (!paced_runs_.empty()) paced_runs_.back().record(scene_, physics_subsystem_->world());
        stats.seconds += seconds_since(step_start);
    }
    if (steps > 0) scene_.end_fixed_steps(timestep.index() - 1);
}

void Engine::flush_pending() {
    // Materialize nodes spawned mid-tick before anything else walks objects_.
    play_controller_.flush_spawned();

    if (scene_.pending_destroy().empty()) return;

    // Scripts on doomed nodes end first, while everything they might read is
    // still there. An end_play can destroy more, so this goes round until
    // every script on everything queued has ended; one already ended is gone,
    // so a node queued twice is not ended twice.
    auto& scripts = script_subsystem_->manager();
    for (std::size_t ended = 0; ended < scene_.pending_destroy().size();) {
        const std::span<const uint32_t> pending = scene_.pending_destroy();
        const std::vector<uint32_t> batch(pending.begin() + static_cast<std::ptrdiff_t>(ended),
                                          pending.end());
        ended = pending.size();
        scripts.end_play_destroyed(scene_, batch);
    }

    if (physics_subsystem_->world()) {
        for (uint32_t id : scene_.pending_destroy()) {
            physics_subsystem_->world()->destroy_body(id);
        }
    }
    scene_.flush_destroyed();
}

void Engine::handle_play_shortcuts() {
    // Only the scene context hosts play.
    if (&active_context() != scene_context_) return;

    // Read through raw_* / input_, not ImGui's IsKeyPressed/io.Key*: once the
    // game has input focus (see PlayController::input_focused), ImGui runs
    // with ImGuiConfigFlags_NoKeyboard set and clears its own key state every
    // frame (see draw_ui) — these shortcuts have to keep working regardless,
    // so they read the same unfiltered source the viewport camera does.
    bool ctrl = input_.raw_key_held(InputSource::key_lctrl) || input_.raw_key_held(InputSource::key_rctrl);
    bool shift = input_.raw_key_held(InputSource::key_lshift) || input_.raw_key_held(InputSource::key_rshift);
    bool alt = input_.raw_key_held(InputSource::key_lalt) || input_.raw_key_held(InputSource::key_ralt);

    if (!ImGui::IsAnyItemActive()) {
        if (ctrl && shift && input_.raw_key_pressed(InputSource::key_p)) {
            play_controller_.toggle_pause();
            return;
        }
        if (ctrl && alt && input_.raw_key_pressed(InputSource::key_p)) {
            play_controller_.step();
            return;
        }
        if (ctrl && input_.raw_key_pressed(InputSource::key_p)) {
            play_controller_.toggle();
        }
    }

    // Shift+Esc: release focus back to the editor first if the game
    // currently has it, stop only on a second press once there is nothing
    // left to release — matches the toolbar/overlay's own "Shift+Esc to
    // release" / "Shift+Esc to stop" messaging.
    if (shift && input_.raw_key_pressed(InputSource::key_escape)) {
        if (play_controller_.input_focused() &&
            play_controller_.presentation() == PlayPresentation::docked) {
            play_controller_.release_input();
        } else {
            play_controller_.stop();
        }
    }

    if (input_.raw_key_pressed(InputSource::key_f11)) {
        play_controller_.toggle_presentation();
    }

    // F10 works from fullscreen too — toggle_eject() drops to docked first
    // (without touching the project's saved presentation preference) since
    // flying free needs an actual viewport panel to render into and editor
    // chrome around it, neither of which fullscreen has. F8 never reached
    // Input at all on the dev machine (likely a driver/firmware binding
    // outside the engine's control), and F9 already opens the screenshot
    // capture popup (see draw_ui) — F10 is otherwise unclaimed.
    if (play_controller_.playing() && input_.raw_key_pressed(InputSource::key_f10)) {
        play_controller_.toggle_eject();
    }
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

Result<> Engine::set_run_tests(const std::string& filter) {
    // Tests drive play repeatedly, and in game mode stop() closes the window
    // instead of tearing the session down — the first test would end the run
    // and every later start() would be a no-op. Refuse rather than report a
    // suite that silently stopped after one test.
    if (game_mode_) {
        return make_error("--run-tests needs the editor build; a game build's "
                          "stop closes the window instead of ending a test");
    }
    if (project_mgr_.active() == nullptr) {
        return make_error("--run-tests needs a project");
    }

    auto scenes = discover_test_scenes(project_mgr_.active()->root().string(), filter);
    if (!scenes) return make_error(scenes.error());
    if (scenes->empty()) {
        return make_error(filter.empty()
                              ? std::string("No test scenes in the project's Tests directory")
                              : "No test scene matches '" + filter + "'");
    }

    test_scenes_ = std::move(*scenes);
    test_phase_ = TestPhase::load;
    FJELL_CORE_INFO("Running {} test(s)", test_scenes_.size());
    return {};
}

Result<> Engine::set_test_pacing(std::string_view list) {
    if (test_phase_ == TestPhase::idle) return make_error("--pacing needs --run-tests");
    auto pacings = parse_pacings(list);
    if (!pacings) return make_error(pacings.error());
    pacings_ = std::move(*pacings);
    std::string names;
    for (const Pacing& pacing : pacings_) names += (names.empty() ? "" : ", ") + pacing.name;
    FJELL_CORE_INFO("Pacing every test at {}", names);
    return {};
}

void Engine::finish_current_test(TestOutcome outcome, const std::string& message) {
    auto& report = test_reports_.emplace_back();
    report.name = test_.valid()
                      ? test_.type_name()
                      : std::filesystem::path(test_scenes_[test_index_]).stem().string();
    if (!pacings_.empty()) report.name += " (" + pacings_[pacing_index_].name + ")";
    report.scene = test_scenes_[test_index_];
    report.outcome = outcome;
    report.message = message;
    report.seconds = test_elapsed_;

    if (test_.valid()) {
        report.failures = test_.failures();
        if (report.message.empty()) report.message = test_.message();
        test_.cleanup_test();
    }

    // A script in the scene that wrote what the simulation owns from outside
    // its steps fails the test, however its own checks came out: what it
    // measured may not be what the game does.
    for (auto& written : script_subsystem_->manager().take_write_reports()) {
        report.failures.push_back(std::move(written));
    }
    if (report.outcome == TestOutcome::passed && !report.failures.empty()) {
        report.outcome = TestOutcome::failed;
        report.message = "a script wrote simulation state outside the fixed step";
    }

    test_ = {};
    test_phase_ = TestPhase::teardown;
}

namespace {

// A shipped game has no editor to show the report in. It goes beside the
// executable, or into the working directory when that cannot be written.
void write_crash_report(const std::string& message) {
    const std::string text = "The game stopped: " + message + "\n";
    for (const auto& path : {platform::executable_path().parent_path() / "crash_report.txt",
                             std::filesystem::path("crash_report.txt")}) {
        if (std::ofstream out(path); out << text) {
            FJELL_CORE_ERROR("Crash report written to {}", path.string());
            return;
        }
    }
}

} // namespace

void Engine::stop_on_missing_reference() {
    auto report = take_missing_reference_report();
    if (!report) return;
    FJELL_CORE_ERROR("{}", report->message);
    // The halt kept the rest of the frame's scripts from running. What ends
    // the session from here (end_play, the editor's scripts made again from
    // the snapshot) is game code too, and has to run.
    resume_script_calls();
    // A report from something that ran while stopping has no session left to
    // stop.
    if (!play_controller_.playing()) return;
    if (test_phase_ == TestPhase::prepare || test_phase_ == TestPhase::run) {
        finish_current_test(TestOutcome::failed, report->message);
        return;
    }
    if (game_mode_) {
        write_crash_report(report->message);
        stopped_by_error_ = true;
        window_.request_close();
        return;
    }
    play_controller_.stop();
    if (scene_context_ != nullptr && report->node_id != 0 && scene_.find(report->node_id) != nullptr) {
        scene_context_->set_selected_id(report->node_id);
    }
}

void Engine::tick_test_runner() {
    // Deliberately one transition per frame, so a stop and the next load
    // never share one.
    switch (test_phase_) {
        case TestPhase::idle:
        case TestPhase::done:
            return;

        case TestPhase::load: {
            if (test_index_ >= test_scenes_.size()) {
                tests_passed_ = all_passed(test_reports_);
                FJELL_CORE_INFO("Test results:\n{}", format_report(test_reports_));
                test_phase_ = TestPhase::done;
                window_.request_close();
                return;
            }
            if (!pacings_.empty()) {
                paced_frames_.emplace(pacings_[pacing_index_]);
                paced_runs_.emplace_back();
            }
            load_scene(test_scenes_[test_index_]);
            // What earlier tests' scripts were reported for is theirs.
            (void)script_subsystem_->manager().take_write_reports();
            // Every run plays from game time zero: the simulation starts
            // there, and whatever follows time (waves, gusts) has to be where
            // it was the last time this test ran.
            clock_.reset_game_time();
            // start() is what instantiates the scene's script components:
            // load_scene leaves every ScriptEntry holding only its type name.
            play_controller_.start();
            test_elapsed_ = 0.0f;
            test_ = TestHandle::find(scene_, script_subsystem_->manager());
            if (!test_.valid()) {
                finish_current_test(TestOutcome::not_run,
                                    "no FunctionalTest component in the scene");
                return;
            }
            test_phase_ = TestPhase::prepare;
            return;
        }

        case TestPhase::prepare: {
            if (test_.gone()) {
                finish_current_test(TestOutcome::failed, "the test's node was destroyed");
                return;
            }
            // start_test_on_step() starts it; this only times the wait.
            test_elapsed_ += clock_.delta_time();
            if (test_.preparation_time_limit() > 0.0f
                && test_elapsed_ > test_.preparation_time_limit()) {
                finish_current_test(TestOutcome::timed_out, "never became ready");
            }
            return;
        }

        case TestPhase::run: {
            // The script subsystem already ticked it this frame, and collision
            // events have dispatched, so a verdict reached this frame is
            // visible now rather than a frame late.
            test_elapsed_ += clock_.delta_time();
            if (test_.gone()) {
                finish_current_test(TestOutcome::failed,
                                    "the test's node was destroyed before it finished");
                return;
            }
            if (test_.finished()) {
                finish_current_test(
                    test_.passed() ? TestOutcome::passed : TestOutcome::failed, {});
                return;
            }
            if (test_.time_limit() > 0.0f && test_elapsed_ > test_.time_limit()) {
                finish_current_test(TestOutcome::timed_out, "never finished");
            }
            return;
        }

        case TestPhase::teardown: {
            // Physics, UI instances, timers and input contexts unwind here,
            // not in load_scene — a scene loaded over a live session would
            // alias node ids into the previous test's physics world.
            play_controller_.stop();
            if (!pacings_.empty() && ++pacing_index_ < pacings_.size()) {
                test_phase_ = TestPhase::load;  // the same scene at the next pacing
                return;
            }
            if (!pacings_.empty()) {
                compare_paced_runs();
                pacing_index_ = 0;
            }
            ++test_index_;
            test_phase_ = TestPhase::load;
            return;
        }
    }
}

void Engine::compare_paced_runs() {
    // Each run of the scene left one report, the last of them in order.
    const std::size_t first_report = test_reports_.size() - paced_runs_.size();
    for (std::size_t run = 1; run < paced_runs_.size(); ++run) {
        const auto divergence = paced_runs_.front().first_divergence(paced_runs_[run]);
        if (!divergence) continue;
        TestReport& report = test_reports_[first_report + run];
        report.failures.push_back(std::format(
            "diverged from {} at step {}: \"{}\" {} is {} at {} and {} at {}", pacings_.front().name,
            divergence->step, divergence->node, divergence->field, divergence->first,
            pacings_.front().name, divergence->second, pacings_[run].name));
        if (report.outcome == TestOutcome::passed) {
            report.outcome = TestOutcome::failed;
            report.message = "the simulation stepped differently at " + pacings_[run].name;
        }
    }
    paced_runs_.clear();
}

void Engine::start_test_on_step() {
    if (test_phase_ != TestPhase::prepare || !test_.is_ready()) return;
    test_elapsed_ = 0.0f;  // the limit is on the test, not the wait
    test_.start_test();
    test_phase_ = TestPhase::run;
}

Result<> Engine::set_scenario(const std::string& path) {
    auto scenario = load_environment_scenario(path);
    if (!scenario) return make_error(scenario.error());
    FJELL_CORE_INFO("Scenario {}: {} steps over {:.0f}s", path,
                    scenario->steps.size(), scenario->duration());
    scenario_.set_scenario(std::move(*scenario));
    return {};
}

void Engine::run() {
    FJELL_CORE_INFO("Entering main loop");

    // After the scene is loaded and its environment bound, since binding is
    // what starts the scene's own weather — an override applied earlier
    // would be the thing that got overridden.
    if (environment_overrides_.any() && environment_subsystem_ != nullptr
        && resource_manager_ != nullptr) {
        apply_environment_overrides(environment_overrides_, scene_,
                                    *environment_subsystem_, *resource_manager_);
    }

    // Game mode or --play: start playing immediately
    if (game_mode_ || play_immediate_) {
        play_controller_.start();
    }

    while (true) {
        // Closing with unsaved work asks first; the answer closes the window.
        if (window_.should_close()) {
            if (game_mode_ || quit_confirmed_ || !anything_unsaved()) break;
            window_.cancel_close();
            quit_asking_ = true;
        }

        // Phase 1: Submit previous frame to GPU (non-blocking)
        vulkan_context_->submit_and_present();

        FJELL_PROFILE_FRAME;

        // Phase 2: CPU work (overlaps with GPU executing the submitted frame)
        asset_loader_->poll();
        input_.update();
        window_.poll_events();
        // A character's move intent is the frame's, and input sets it: clear it
        // first, so a released stick leaves it zero.
        if (play_controller_.playing()) physics_subsystem_->clear_move_intents(scene_);
        // After poll_events, so the action map sees this frame's mouse and
        // scroll deltas: the events add them up during the poll, and
        // Input::update() clears them for the next frame. Reading before the
        // poll gave every axis bound to mouse motion a constant zero, while
        // keys and buttons kept working because they are latched state rather
        // than a per-frame delta.
        input_action_map_.update(input_);
        if (import_dialog_.is_open()) import_dialog_.tick();
        if (import_file_browser_.is_open()) import_file_browser_.tick();
        menu_bar_.tick_browsers();
        // A paced test run tells the clock each frame took what its pacing
        // says, however long it really took.
        if (paced_frames_) {
            clock_.advance(static_cast<float>(paced_frames_->next()));
        } else {
            clock_.tick();
        }
        // One clock for what is simulated and what is drawn: the renderer is
        // told the time here, on every iteration, including those that end up
        // drawing nothing.
        vulkan_context_->set_time(clock_.game_elapsed());
        play_controller_.begin_frame();

#ifndef NDEBUG
        if (shader_watcher_) {
            const auto changes = shader_watcher_->poll();
            if (changes.any()) {
                // An .fjsl is one pipeline; a raw stage is shared and reaches
                // whatever happens to read it. Rebuilding everything for a
                // single material edit bumps every pipeline's generation, and
                // every consumer watching one — the sky, a UI material —
                // rebuilds for a shader that did not change.
                if (changes.rebuild_everything) {
                    vulkan_context_->reload_pipeline();
                } else {
                    vulkan_context_->reload_shaders(changes.fjsl_paths);
                }
                // Which thumbnails a shader edit touches is not known here; the
                // timestamp check on each of them finds out.
                asset_thumbnails_.refresh_all();
            }
        }
        if (fjgraph_watcher_) { (void)fjgraph_watcher_->poll(); }
        // A grade saved from an image editor shows in the running scene.
        if (resource_manager_) resource_manager_->poll_lut_changes();
#endif

        auto ctx = make_frame_context();

        // Unscaled time deliberately: a scenario's seconds are the seconds
        // the capture runs for, so pausing or slowing the game does not
        // stretch the schedule the measurement is lined up against.
        if (!scenario_.empty() && environment_subsystem_ != nullptr
            && resource_manager_ != nullptr) {
            scenario_.tick(clock_.delta_time(), scene_, *environment_subsystem_,
                           *resource_manager_);
            if (scenario_.finished()) window_.request_close();
        }

        // Decided once for the frame. Play can start or stop between the play
        // phases and the always subsystems below (the test runner does both),
        // and animation and the environment, which tick in one or the other,
        // must tick exactly once.
        const bool play_phases_run = play_controller_.playing();
        if (play_phases_run) {
            // Update camera state for worldspace UI billboard computation
            if (auto* cam_node = camera_subsystem_->active_camera(scene_)) {
                float aspect = scene_context_->viewport_manager().active_aspect();
                ui_subsystem_->set_camera(
                    cam_node->position(),
                    cam_node->view_matrix(),
                    cam_node->projection_matrix(aspect));
            }

            // Animation before everything: scripts read the current pose, and
            // physics collides against it.
            animation_subsystem_->run(ctx);

            // Phase 0: Scripts
            for (auto* s : play_phase_0_) s->run(ctx);
            flush_pending();

            // Phase 1: Environment, then the simulation's fixed steps
            for (auto* s : play_phase_1_) s->run(ctx);
            run_fixed_steps();

            // What the steps moved is drawn blended between the last two of
            // them, whether this frame ran any or not, and the shaders see the
            // time that blend shows, so the waves are drawn at the moment the
            // hulls on them are.
            const FixedTimestep& timestep = play_controller_.fixed_timestep();
            scene_.interpolate(static_cast<float>(timestep.alpha()));
            vulkan_context_->set_time(static_cast<float>(timestep.render_time()));

            // Everything now sits where it will be drawn, so what follows it
            // on screen (cameras above all) places itself in late_tick.
            script_subsystem_->manager().late_update(scene_, clock_.game_delta_time());
            flush_pending();

            // Phase 2: VFX
            for (auto* s : play_phase_2_) s->run(ctx);
        }

        // Where the test runner ends play too: every script of the frame has
        // run, and nothing of the scene is in use.
        stop_on_missing_reference();

        // After the play phases, so a test that reached its verdict this frame
        // is read this frame: scripts have ticked and collision events have
        // dispatched by now.
        tick_test_runner();

        project_mgr_.poll_build(script_subsystem_->manager(), scene_);

#ifndef NDEBUG
        // Skip .so file watcher while a background build is running —
        // cmake writes a partial .so that would cause "invalid ELF header".
        // poll_build() handles the reload after the build finishes.
        if (!project_mgr_.is_building()) {
            if (auto r = script_subsystem_->manager().poll_reload(scene_); !r) {
                FJELL_CORE_ERROR("{}", r.error());
            }
        }
#endif
        if (!game_mode_) check_project_widgets();

        // Always-tick subsystems (before world matrices). Animation and the
        // environment have already ticked if the play phases ran.
        if (!play_phases_run) {
            animation_subsystem_->run(ctx);
            environment_subsystem_->run(ctx);
        }
        for (auto* s : early_always_) s->run(ctx);
        scene_.compute_world_matrices(&thread_pool_);

        // World-space UI reads node transforms to place its billboards, so it
        // must rebuild after compute_world_matrices() settles them for this
        // frame. ui_subsystem_->tick() alone runs in play_phase_0_, before
        // scripts and physics have moved anything this frame — if this were
        // built there instead, it would render one frame behind whatever
        // it's anchored to, most visible as a lag/smear while that node moves.
        // A UI's Models point into components, so they are bound here too:
        // after the frame's last destroy, before the UI is read at record.
        if (play_controller_.playing()) {
            ui_subsystem_->bind_models(ctx);
            ui_subsystem_->update_world_records(ctx);
        }

        // Always-tick subsystems (after world matrices)
        for (auto* s : late_always_) s->run(ctx);

        // Play-only subsystems that need world matrices (e.g. audio listener)
        if (play_controller_.playing()) {
            for (auto* s : late_play_) s->run(ctx);
        }

        // Phase 3: Wait for frame slot — GPU is done with this slot after this
        if (!vulkan_context_->begin_frame(thread_pool_)) continue;

        // A capture's read taken in an earlier frame is written out once
        // that frame has finished.
        capture_service_.finish_after_present(*vulkan_context_,
                                              scene_context_->viewport_manager());

        // Resize offscreen passes (using last frame's panel size) — safe
        // because begin_frame's fence guarantees the GPU is no longer
        // referencing this frame slot's resources.
        vulkan_context_->set_active_sources(scene_context_->viewport_manager().active_source_mask());
        active_context().apply_resize(*vulkan_context_);

        // Editor capture: resize the editor viewport + PP to the capture
        // resolution BEFORE draw_ui, so ImGui::Image shows the new post
        // output this frame.
        capture_service_.resize_for_capture(*vulkan_context_,
                                            scene_context_->viewport_manager());

        // Applies last frame's scene_image_rect() (set below, after
        // draw_ui()/draw_play_overlay() ran) to the HUD target, if its size
        // changed, before this frame's ImGui::Image(hud_texture()) calls.
        play_controller_.apply_pending_hud_resize();

        // While the game has input focus, ImGui must not also react to the
        // keyboard — a WASD move shouldn't nudge a still-open properties
        // field, and Tab/arrow-key navigation shouldn't steal focus back.
        // Global shortcuts that still have to work regardless (Shift+Esc,
        // Ctrl+P, F11, ...) read raw_* input directly in
        // handle_play_shortcuts() rather than through ImGui, since this
        // flag makes ImGui's own key state (io.KeyCtrl, IsKeyPressed, ...)
        // read as if nothing were held for as long as it's set.
        ImGuiIO& io = ImGui::GetIO();
        if (play_controller_.input_focused()) {
            io.ConfigFlags |= ImGuiConfigFlags_NoKeyboard;
        } else {
            io.ConfigFlags &= ~ImGuiConfigFlags_NoKeyboard;
        }

        imgui_layer_->begin_frame();
        // The scene editor's gizmos draw into ImGuizmo's own window, which
        // opens with each of the editor's frames and in its context, never a
        // dialog's.
        ImGuizmo::BeginFrame();
        handle_play_shortcuts();
        if (play_controller_.playing() &&
            play_controller_.presentation() == PlayPresentation::fullscreen) {
            active_context().draw_play_overlay();
        } else {
            draw_ui();
        }
        imgui_layer_->end_frame();

        // Editor capture (pane mode only): advance state machine. Scene
        // mode already did its resize above.
        capture_service_.prepare_for_record(*vulkan_context_,
                                            scene_context_->viewport_manager());

        auto requests = active_context().build_render_requests();

        // In play mode, override the active viewport's camera with the scene
        // CameraNode, and drop the editor-only passes (grid, collider debug)
        // that have no business in the running game. Skipped while ejected
        // (F8): the game keeps simulating with its own camera regardless —
        // UiSubsystem::set_camera and screen debug text below still read it
        // — only the rendered view comes from the viewport's own fly camera
        // instead, same as editing. The game's own render graph stays
        // active either way; ejecting flies free through the running game,
        // not back into an editor preview of it.
        if (play_controller_.playing() && !requests.empty()) {
            if (!play_controller_.ejected()) {
                if (auto* cam_node = camera_subsystem_->active_camera(scene_)) {
                    float aspect = scene_context_->viewport_manager().active_aspect();
                    requests[0].view = cam_node->view_matrix();
                    requests[0].projection = cam_node->projection_matrix(aspect);
                    requests[0].camera_position = cam_node->position();
                }
            }
            requests[0].viewport_graph =
                vulkan_context_->get_or_load_graph("assets/render_graphs/play.fjgraph");
        }

        // Contexts with 3D viewports return their scene (main or preview).
        // Contexts without one (e.g. UI editor) return nullptr — fall back to main scene
        // so the renderer always has valid geometry to reference.
        Scene* render_scene = active_context().render_scene();
        if (!render_scene) render_scene = &scene_;

        // The environment the render scene is lit by. The ticked scene's is
        // the subsystem's; an editor rendering its own scene resolves it here.
        EnvironmentState own_environment;
        const EnvironmentState* environment = &environment_subsystem_->state();
        if (render_scene != &scene_) {
            own_environment = resolve_environment(*render_scene);
            environment = &own_environment;
        }

        vulkan_context_->sync_sprite_textures(*render_scene, *resource_manager_, project_mgr_.active());
        vulkan_context_->sync_tilemap_textures(*render_scene, *resource_manager_, project_mgr_.active());
        vulkan_context_->set_viewport_offset(scene_context_->viewport_manager().active_viewport_pos());

        // The scene's effects move on once this tick, whichever view draws
        // them first.
        vulkan_context_->advance_scene_particles(*render_scene, clock_.game_delta_time());

        // Asset thumbnails, scene viewports (render-to-texture) and the
        // context's previews (the camera preview) record into the frame
        // ahead of its views.
        asset_thumbnails_.tick(asset_thumbnail_deps(), clock_.game_delta_time());
        vulkan_context_->render_scene_viewports(render_scene, clock_.game_delta_time(),
                                                 scene_context_->selected_id(),
                                                 *environment, thread_pool_);
        active_context().render_previews(*vulkan_context_, render_scene,
                                          clock_.game_delta_time(), *environment, thread_pool_);

        // Tell the controller what rect the scene image actually occupied
        // this frame — the docked panel or the fullscreen play view,
        // whichever ran during this frame's draw_ui()/draw_play_overlay()
        // above. The HUD itself is recorded later, from inside
        // record_frame(), once the main command buffer is open — see
        // PlayController::init.
        if (play_controller_.playing()) {
            auto rect = scene_context_->scene_image_rect();
            play_controller_.set_hud_rect(rect.pos, rect.size);
        }

        // Set active editor's render scene for SDF gathering
        vulkan_context_->set_active_render_scene(active_context().render_scene());

        // Phase 4: Record commands
        vulkan_context_->record_frame(requests, *environment, render_scene, clock_.game_delta_time(),
                                       scene_context_->selected_id(), thread_pool_);
        // Its read goes after everything the frame drew, UI included.
        capture_service_.record_read(*vulkan_context_, scene_context_->viewport_manager());

        // Age out screen debug text after the overlay has drawn it, so a
        // single-frame entry survives exactly the frame it was submitted in.
        // Unscaled dt: a paused or slowed clock shouldn't freeze a timed
        // message on screen.
        screen_debug_queue_->tick(clock_.delta_time());
    }

    // Submit the last recorded frame before shutting down
    vulkan_context_->submit_and_present();
    FJELL_CORE_INFO("Exiting main loop");
}

EngineEditorContext& Engine::active_context() {
    return *contexts_[static_cast<size_t>(active_context_idx_)];
}

const EngineEditorContext& Engine::active_context() const {
    return *contexts_[static_cast<size_t>(active_context_idx_)];
}

void Engine::switch_context(int index) {
    if (index == active_context_idx_) return;
    pending_context_switch_ = index;
}

void Engine::draw_context_tabs() {
    // Apply a close requested during the previous frame, before anything can
    // reference the closing context's textures again.
    apply_pending_context_close();

    auto bar = ui::ContextTabBar("##ContextBar");
    if (!bar) return;

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
            // ImGui draws a dot in place of the close button (the close
            // button still shows on hover) while the context has edits
            // that aren't saved.
            if (contexts_[static_cast<size_t>(i)]->has_unsaved_changes()) {
                flags |= ImGuiTabItemFlags_UnsavedDocument;
            }
            if (ImGui::BeginTabItem(contexts_[static_cast<size_t>(i)]->name(),
                                     closeable ? &open : nullptr, flags)) {
                // Tab clicked by user — switch if different from active
                if (!has_pending && i != active_context_idx_) {
                    vulkan_context_->wait_idle();
                    contexts_[static_cast<size_t>(active_context_idx_)]->on_exit();
                    active_context_idx_ = i;
                    contexts_[static_cast<size_t>(i)]->on_enter();
                    }
                ImGui::EndTabItem();
            }
            if (closeable && !open) close_idx = i;
            ImGui::PopID();
        }
        ImGui::EndTabBar();

        // Record the close; applied at the start of the next frame so no draw
        // command still references the closing context's viewport textures.
        if (close_idx >= 0) {
            if (contexts_[static_cast<size_t>(close_idx)]->has_unsaved_changes()) {
                close_asking_ = close_idx;
            } else {
                pending_context_close_ = close_idx;
            }
        }
    }

}

void Engine::apply_pending_context_close() {
    int close_idx = pending_context_close_;
    if (close_idx < 0) return;
    pending_context_close_ = -1;
    if (close_idx >= static_cast<int>(contexts_.size())) return;

    vulkan_context_->wait_idle();

    // If closing the active tab, switch away first
    if (close_idx == active_context_idx_) {
        int new_idx = close_idx > 0 ? close_idx - 1 : 0;
        contexts_[close_idx]->on_exit();
        active_context_idx_ = new_idx;
        contexts_[active_context_idx_]->on_enter();
    }

    // Update layout template: if this context was the template for its
    // type, pick another context of the same type (or leave stale).
    const char* closing_type = contexts_[close_idx]->context_type();
    ImGuiID closing_dock = contexts_[close_idx]->dockspace_id();
    auto tmpl_it = context_layout_templates_.find(closing_type);
    if (tmpl_it != context_layout_templates_.end() && tmpl_it->second == closing_dock) {
        ImGuiID replacement = 0;
        for (int j = 0; j < static_cast<int>(contexts_.size()); ++j) {
            if (j == close_idx) continue;
            if (std::strcmp(contexts_[j]->context_type(), closing_type) == 0) {
                replacement = contexts_[j]->dockspace_id();
                break;
            }
        }
        if (replacement != 0) {
            tmpl_it->second = replacement;
        }
    }

    contexts_[close_idx]->shutdown();
    contexts_.erase(contexts_.begin() + close_idx);

    // Adjust active index if a tab before it was removed
    if (close_idx < active_context_idx_) {
        active_context_idx_--;
    }
}

void Engine::save_project() {
    if (project_mgr_.active() == nullptr) return;
    snapshot_render_settings(*vulkan_context_, project_mgr_.active()->config().render);
    project_mgr_.active()->save();
}

Result<> Engine::save_context(EditorContext& ctx) {
    if (&ctx != scene_context_) return ctx.save();
    save_scene();
    if (scene_.is_dirty()) return make_error("the scene couldn't be written; the log says why");
    return {};
}

bool Engine::anything_unsaved() const {
    return std::any_of(contexts_.begin(), contexts_.end(),
                       [](const auto& ctx) { return ctx->has_unsaved_changes(); });
}

void Engine::draw_unsaved_dialogs() {
    static constexpr const char* CLOSE_DIALOG = "##close_unsaved";
    static constexpr const char* QUIT_DIALOG = "##quit_unsaved";

    // Closing a tab with unsaved changes.
    if (close_asking_ >= 0 && close_asking_ < static_cast<int>(contexts_.size())) {
        if (!ImGui::IsPopupOpen(CLOSE_DIALOG)) ui::open_dialog(CLOSE_DIALOG);
        EditorContext& ctx = *contexts_[static_cast<size_t>(close_asking_)];
        const std::string file = std::filesystem::path(ctx.asset_path()).filename().string();
        const std::string title = "Save changes to " + file + "?";
        switch (ui::unsaved_dialog(CLOSE_DIALOG, title.c_str(),
                                   "Closing the tab without saving loses the changes since you last saved.")) {
            case ui::UnsavedAnswer::Save:
                if (auto saved = save_context(ctx); saved) {
                    pending_context_close_ = close_asking_;
                } else {
                    FJELL_CORE_ERROR("Couldn't save {}: {}", file, saved.error());
                }
                close_asking_ = -1;
                break;
            case ui::UnsavedAnswer::DontSave:
                pending_context_close_ = close_asking_;
                close_asking_ = -1;
                break;
            case ui::UnsavedAnswer::Cancel: close_asking_ = -1; break;
            case ui::UnsavedAnswer::None: break;
        }
    }

    // Quitting with unsaved changes: one list of everything.
    if (!quit_asking_) return;
    if (!ImGui::IsPopupOpen(QUIT_DIALOG)) {
        quit_items_.clear();
        quit_contexts_.clear();
        for (const auto& ctx : contexts_) {
            if (!ctx->has_unsaved_changes()) continue;
            const std::filesystem::path path = ctx.get() == scene_context_ ? current_scene_path_ : ctx->asset_path();
            const auto& kind = ui::asset_kind(path.extension().string());
            quit_items_.push_back({.icon = kind.icon, .icon_colour = theme::category(kind.category),
                                   .name = path.filename().string(), .detail = {}, .save = true});
            const std::string files = ctx->saved_files();
            if (ctx.get() != scene_context_ && files != quit_items_.back().name) {
                quit_items_.back().detail = files;
            }
            quit_contexts_.push_back(ctx.get());
        }
        ui::open_dialog(QUIT_DIALOG);
    }
    const std::string title = quit_items_.size() == 1 ? std::string("1 asset has unsaved changes")
        : std::to_string(quit_items_.size()) + " assets have unsaved changes";
    switch (ui::unsaved_list_dialog(QUIT_DIALOG, title.c_str(), quit_items_)) {
        case ui::UnsavedAnswer::Save: {
            bool all_saved = true;
            for (std::size_t i = 0; i < quit_items_.size(); ++i) {
                if (!quit_items_[i].save) continue;
                if (auto saved = save_context(*quit_contexts_[i]); !saved) {
                    FJELL_CORE_ERROR("Couldn't save {}: {}", quit_items_[i].name, saved.error());
                    all_saved = false;
                }
            }
            quit_asking_ = false;
            // A save that failed keeps the editor open, its header saying why.
            if (all_saved) {
                quit_confirmed_ = true;
                window_.request_close();
            }
            break;
        }
        case ui::UnsavedAnswer::DontSave:
            quit_asking_ = false;
            quit_confirmed_ = true;
            window_.request_close();
            break;
        case ui::UnsavedAnswer::Cancel: quit_asking_ = false; break;
        case ui::UnsavedAnswer::None: break;
    }
}

void Engine::draw_status_bar() {
    auto bar = ui::StatusBar();
    if (!bar) return;

    // Left: the way into the content browser, then what is being edited
    // and in which mode.
    if (ui::status_button(ui::icon::content_browser, "Content", "Ctrl Space",
                          content_browser_.visible())) {
        content_browser_.toggle_visibility();
    }
    ui::status_rule();

    if (!current_scene_path_.empty()) {
        const std::string scene_name = std::filesystem::path(current_scene_path_).stem().string();
        ui::status_item(ui::icon::scene, scene_name, scene_.is_dirty());
        ui::status_rule();
    }

    if (play_controller_.paused()) {
        ui::status_pill(ui::icon::pause, "Paused", ui::StatusTone::Warning);
    } else if (play_controller_.playing()) {
        const float scale = play_controller_.time_scale();
        const std::string text = scale != 1.0f
            ? std::format("Playing \xc2\xb7 {:.2f} \xc3\x97", scale)
            : std::string("Playing");
        ui::status_pill(ui::icon::play, text.c_str(), ui::StatusTone::Success);
    } else {
        ui::status_pill(ui::icon::editing, "Editing", ui::StatusTone::Plain);
    }

    // Right: how the editor is running.
    bar.right();
    ui::status_item(nullptr, std::format("{:.2f} ms", clock_.delta_time() * 1000.0f));
}

} // namespace fjell
