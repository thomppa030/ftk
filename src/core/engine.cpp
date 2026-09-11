extern "C" void fjell_input_clear_connections();

#include "core/engine.hpp"
#include "core/base_path.hpp"
#include "ui/asset_ref_widget.hpp"
#include "core/input_component.hpp"
#include "core/commands.hpp"
#include "core/game_builder.hpp"
#include "core/layer.hpp"
#include "core/log.hpp"
#include "core/profiler.hpp"
#include "animation/animation_subsystem.hpp"
#include "audio/audio_subsystem.hpp"
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
#include "platform/platform.hpp"
#include "platform/shared_library.hpp"
#include "physics/physics_world.hpp"
#include "ui/console.hpp"
#include "ui/status_bar.hpp"
#include "ui/theme.hpp"
#include "ui/animation_editor_context.hpp"
#include "ui/blend_space_1d_editor_context.hpp"
#include "ui/blend_space_2d_editor_context.hpp"
#include "ui/material_editor_context.hpp"
#include "ui/mesh_editor_context.hpp"
#include "ui/ui_editor_context.hpp"
#include "ui/input_mapping_editor_context.hpp"
#include "ui/vfx_editor_context.hpp"
#include "ui/editor_context.hpp"
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
#include "renderer/pipeline_cache.hpp"
#include "renderer/resources/texture.hpp"

#include <imgui.h>
#include <imgui_internal.h>
#include <ImGuizmo.h>
#include <GLFW/glfw3.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstring>
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
    auto* gpu_core = &vulkan_context_->gpu().core();
    import_dialog_.set_gpu(gpu_core);
    menu_bar_.set_gpu(gpu_core);
    import_file_browser_.set_gpu(gpu_core);
    asset_file_browser_.set_gpu(gpu_core);

    // Icon provider for standalone file browsers (uses IconCache from the editor's context)
    auto icon_provider = [](const FileBrowser::Entry& entry) -> ImTextureID {
        auto* icons = IconCache::instance();
        if (!icons) return 0;
        const char* name = "node";
        if (entry.is_directory) {
            name = "folder";
        } else {
            auto ext = std::filesystem::path(entry.name).extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext == ".glb" || ext == ".gltf" || ext == ".fbx" || ext == ".fjmesh" || ext == ".fjskel")
                name = "mesh";
            else if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp")
                name = "texture";
            else if (ext == ".fjsl" || ext == ".vert" || ext == ".frag" || ext == ".comp" || ext == ".glsl")
                name = "shader";
            else if (ext == ".fjmat")
                name = "material";
            else if (ext == ".cpp" || ext == ".cc" || ext == ".cxx")
                name = "cpp";
            else if (ext == ".hpp" || ext == ".h" || ext == ".hxx")
                name = "header";
        }
        return icons->icon_for_current_context(name);
    };
    import_file_browser_.set_icon_provider(icon_provider);
    asset_file_browser_.set_icon_provider(icon_provider);

    init_subsystems();
    init_project(project_path);
    init_ui();
    init_resources();
    init_scene();
    init_content_browser();
    init_editor();
    FJELL_CORE_INFO("Engine initialized");
}

void Engine::init_subsystems() {
    animation_subsystem_ = add_subsystem<AnimationSubsystem>(
        vulkan_context_->device(), vulkan_context_->allocator());
    audio_subsystem_     = add_subsystem<AudioSubsystem>();
    script_subsystem_    = add_subsystem<ScriptSubsystem>();
    physics_subsystem_   = add_subsystem<PhysicsSubsystem>();
    vfx_subsystem_       = add_subsystem<VfxSubsystem>();
    sprite_anim_subsystem_ = add_subsystem<SpriteAnimSubsystem>();
    camera_subsystem_    = add_subsystem<CameraSubsystem>();

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
    play_phase_1_.push_back(physics_subsystem_);
    play_phase_2_.push_back(vfx_subsystem_);
    early_always_.push_back(animation_subsystem_);
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

void Engine::init_project(const std::filesystem::path& project_path) {
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
        }
    }

    window_.set_input(&input_);
    input_.attach(window_.handle());
}

void Engine::init_ui() {
    imgui_layer_ = std::make_unique<ImGuiLayer>(
        window_.handle(), vulkan_context_->instance(),
        vulkan_context_->physical_device(), vulkan_context_->device(),
        vulkan_context_->graphics_family(), vulkan_context_->graphics_queue(),
        vulkan_context_->swapchain_format(), vulkan_context_->image_count(),
        base_path::resolve("engine_assets/fonts"));

    vulkan_context_->set_overlay_callback([this](VkCommandBuffer cmd) {
        imgui_layer_->render(cmd);
    });

    vulkan_context_->init_game_ui(game_ui_,
        base_path::resolve("engine_assets/fonts/Inter-Regular.ttf"),
        vulkan_context_->swapchain_format());

    vulkan_context_->set_game_ui_callback(
        [this](
               VkCommandBuffer cmd, VkDescriptorSet global_set,
               VkDescriptorSet bindless_set, VkExtent2D extent,
               uint32_t frame_index) {
            // FPS is shown as an ImGui overlay in play mode (see
            // SceneEditorContext::draw_play_overlay). Pushing it into the
            // game-UI context every 0.5s forces a full Yoga layout rebuild,
            // so we keep it out of the HUD.

            if (!active_context().is_playing() && game_ui_.loaded()) {
                game_ui_.update(static_cast<float>(extent.width),
                                static_cast<float>(extent.height),
                                frame_index, clock_.delta_time(), &input_);
                game_ui_.record(cmd, global_set, bindless_set, extent,
                                frame_index);
            }

            for (auto& inst : ui_subsystem_->instances()) {
                if (inst.render_mode == UiWidgetComponent::RenderMode::world_space) continue;
                if (!inst.game_ui || !inst.game_ui->loaded()) continue;
                inst.game_ui->update(static_cast<float>(extent.width),
                                     static_cast<float>(extent.height),
                                     frame_index, clock_.delta_time(), &input_);
                inst.game_ui->record(cmd, global_set, bindless_set, extent,
                                     frame_index);
            }
        });

    ui_subsystem_ = add_subsystem<UiSubsystem>(*vulkan_context_, game_ui_,
                                               script_subsystem_->manager());
    play_phase_0_.push_back(ui_subsystem_);

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
}

void Engine::init_resources() {
    resource_manager_ = std::make_unique<ResourceManager>(
        vulkan_context_->device(), vulkan_context_->allocator(),
        vulkan_context_->physical_device(),
        vulkan_context_->upload_context(),
        vulkan_context_->bindless_textures(), vulkan_context_->mega_buffer(),
        vulkan_context_->meshlet_buffer(), vulkan_context_->pipeline_cache());

    resource_manager_->set_viewport_resolver([this](const std::string& name) -> uint32_t {
        return vulkan_context_->scene_viewport_bindless_index(name);
    });

    asset_loader_ = std::make_unique<AssetLoader>(
        thread_pool_, *resource_manager_,
        vulkan_context_->allocator(), vulkan_context_->device(),
        vulkan_context_->upload_context(),
        vulkan_context_->mega_buffer(), vulkan_context_->meshlet_buffer());

    default_texture_ = resource_manager_->load_texture(
        base_path::resolve("engine_assets/textures/test.png")).value_or(TextureHandle{});
    sprite_anim_subsystem_->set_resource_manager(resource_manager_.get());
    ui_subsystem_->set_resource_manager(resource_manager_.get());

    auto& ss = script_services();
    ss.input = &input_;
    ss.resources = resource_manager_.get();
    ss.project = project_mgr_.active();
    ss.vfx = vfx_subsystem_;
    ss.audio = audio_subsystem_;
    ss.camera = camera_subsystem_;
    ss.vulkan = vulkan_context_.get();
    ss.input_actions = &input_action_map_;
    ss.timers = &script_subsystem_->manager().timers();
    ss.clock = &clock_;
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

void Engine::init_content_browser() {
    icon_cache_ = std::make_unique<IconCache>(
        vulkan_context_->device(), vulkan_context_->physical_device(),
        vulkan_context_->command_pool(), vulkan_context_->graphics_queue(),
        base_path::resolve("engine_assets/icons"));
    IconCache::set_instance(icon_cache_.get());

    content_browser_.init(icon_cache_.get(),
                          vulkan_context_.get(), resource_manager_.get());
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
            auto preset = Preset::from_subtree(scene_, node_id);
            auto* node = scene_.find(node_id);
            std::string name = node ? node->name : "preset";
            auto path = std::filesystem::path(directory) / (name + ".fjp");
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

    if (project_mgr_.active()) {
        auto& layout = project_mgr_.active()->config().layout;
        if (!layout.empty()) {
            load_layout(layout);
        }
    }

    layer_stack_.push_layer(std::make_unique<ViewportLayer>(&viewport_hovered_));

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
            .light = &light_,
            .game_ui = &game_ui_,
            .thread_pool = &thread_pool_,
            .window = &window_,
            .content_browser = &content_browser_,
            .file_browser = &asset_file_browser_,
            .net_manager = net_manager_.get(),
            .camera_subsystem = camera_subsystem_,
            .screen_debug_queue = screen_debug_queue_.get(),
            .game_mode = game_mode_,
            .start_play = [this]() { start_play(); },
            .stop_play = [this]() { stop_play(); },
        };
        auto ctx = std::make_unique<SceneEditorContext>(deps);
        ctx->init();
        scene_context_ = ctx.get();
        contexts_.push_back(std::move(ctx));
        active_context_idx_ = 0;
    }

    context_registry_.register_type("material", {
        .create = [this](const std::string& path) -> std::unique_ptr<EditorContext> {
            MaterialEditorDeps deps{
                .vulkan_context = vulkan_context_.get(),
                .resource_manager = resource_manager_.get(),
                .input = &input_,
                .clock = &clock_,
                .project_mgr = &project_mgr_,
                .icon_cache = icon_cache_.get()
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
        .create = [this](const std::string& path) -> std::unique_ptr<EditorContext> {
            MeshEditorDeps deps{
                .vulkan_context = vulkan_context_.get(),
                .resource_manager = resource_manager_.get(),
                .input = &input_,
                .clock = &clock_,
                .project_mgr = &project_mgr_,
                .icon_cache = icon_cache_.get(),
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

    context_registry_.register_type("ui", {
        .create = [this](const std::string& path) -> std::unique_ptr<EditorContext> {
            UiEditorDeps deps{
                .vulkan_context = vulkan_context_.get(),
                .resource_manager = resource_manager_.get(),
                .input = &input_,
                .clock = &clock_,
                .project_mgr = &project_mgr_,
                .icon_cache = icon_cache_.get(),
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
            static_cast<AnimationEditorContext&>(*ctx).open_animset(path);
            return ctx;
        },
        .open = [](EditorContext& ctx, const std::string& path) {
            static_cast<AnimationEditorContext&>(ctx).open_animset(path);
        },
        .matches = anim_matches
    });

    context_registry_.register_type("blend1d", {
        .create = [this](const std::string& path) -> std::unique_ptr<EditorContext> {
            BlendSpace1DEditorDeps deps{
                .vulkan_context = vulkan_context_.get(),
                .resource_manager = resource_manager_.get(),
                .input = &input_,
                .clock = &clock_,
                .animation_subsystem = animation_subsystem_,
                .project_mgr = &project_mgr_,
                .icon_cache = icon_cache_.get()
            };
            auto ctx = std::make_unique<BlendSpace1DEditorContext>(deps);
            ctx->init();
            ctx->open(path);
            return ctx;
        },
        .open = [](EditorContext& ctx, const std::string& path) {
            static_cast<BlendSpace1DEditorContext&>(ctx).open(path);
        },
        .matches = [](const EditorContext& ctx, const std::string&) {
            return dynamic_cast<const BlendSpace1DEditorContext*>(&ctx) != nullptr;
        }
    });

    context_registry_.register_type("blend2d", {
        .create = [this](const std::string& path) -> std::unique_ptr<EditorContext> {
            BlendSpace2DEditorDeps deps{
                .vulkan_context = vulkan_context_.get(),
                .resource_manager = resource_manager_.get(),
                .input = &input_,
                .clock = &clock_,
                .animation_subsystem = animation_subsystem_,
                .project_mgr = &project_mgr_,
                .icon_cache = icon_cache_.get()
            };
            auto ctx = std::make_unique<BlendSpace2DEditorContext>(deps);
            ctx->init();
            ctx->open(path);
            return ctx;
        },
        .open = [](EditorContext& ctx, const std::string& path) {
            static_cast<BlendSpace2DEditorContext&>(ctx).open(path);
        },
        .matches = [](const EditorContext& ctx, const std::string&) {
            return dynamic_cast<const BlendSpace2DEditorContext*>(&ctx) != nullptr;
        }
    });

    auto input_factory = [this](const std::string&) -> std::unique_ptr<EditorContext> {
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
    auto input_matches = [](const EditorContext& ctx, const std::string&) {
        return dynamic_cast<const InputMappingEditorContext*>(&ctx) != nullptr;
    };

    context_registry_.register_type("input_mapping", {
        .create = [input_factory](const std::string& path) {
            auto ctx = input_factory(path);
            static_cast<InputMappingEditorContext&>(*ctx).open_context(path);
            return ctx;
        },
        .open = [](EditorContext& ctx, const std::string& path) {
            static_cast<InputMappingEditorContext&>(ctx).open_context(path);
        },
        .matches = input_matches
    });

    context_registry_.register_type("input_action", {
        .create = [input_factory](const std::string& path) {
            auto ctx = input_factory(path);
            static_cast<InputMappingEditorContext&>(*ctx).open_action(path);
            return ctx;
        },
        .open = [](EditorContext& ctx, const std::string& path) {
            static_cast<InputMappingEditorContext&>(ctx).open_action(path);
        },
        .matches = input_matches
    });
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
    // Free VMA-backed resources before VulkanContext (and its allocator) is destroyed.
    subsystems_.clear();
    asset_loader_.reset();
    resource_manager_.reset();
    IconCache::set_instance(nullptr);
    icon_cache_.reset();
    // Release ImGui descriptor sets before ImGui shuts down
    vulkan_context_->release_imgui_textures();
    imgui_layer_.reset();
    FJELL_CORE_INFO("Engine shut down");
}

void Engine::save_scene() {
    // Flush live script-component state into each node's serialized JSON before
    // writing — otherwise inspector edits (e.g. asset Refs) made since load are
    // lost, since the file is written from the stored JSON, not the instances.
    if (!active_context().is_playing()) {
        script_subsystem_->manager().serialize_all(scene_);
    }
    save_material_assets();
    if (auto r = scene_.save(current_scene_path_); !r) {
        FJELL_CORE_ERROR("{}", r.error());
    }
}

void Engine::save_material_assets() {
    auto* cache = vulkan_context_->pipeline_cache();
    auto& bindless = vulkan_context_->bindless_textures();
    if (!cache) return;

    for (auto& obj : scene_.objects()) {
        auto* mesh = obj->components.get<MeshComponent>();
        if (!mesh || !mesh->material) continue;
        if (!base_path::references_asset(mesh->material_name, ".fjmat")) continue;

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
    // Reserve the bottom strip for the status bar so panels don't overlap it.
    ImVec2 host_size = viewport->WorkSize;
    host_size.y = std::max(0.0f, host_size.y - StatusBar::HEIGHT);
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
        if (!built && saved_context_layouts_.contains(type)) {
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
    auto& sink = log::console_sink();
    if (sink) {
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
        scene_ = std::move(*result);
    } else {
        FJELL_CORE_ERROR("{}", result.error());
        scene_.clear();
    }
    scene_context_->set_selected_id(0);
    scene_context_->command_history().clear();
    vulkan_context_->sync_custom_depth_pipelines();
    // The scene was replaced wholesale, so the DDGI atlases need the same
    // rebuild-once-settled treatment as a project open or play transition.
    vulkan_context_->request_ddgi_settle_rebuild();
    load_input_actions();
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
            FJELL_CORE_INFO("InputActionMap: loading actions from {}", resolved);
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
    connections_.push_back(menu_bar_.on_save_project.bind(
        [this]() {
            if (project_mgr_.active()) {
                snapshot_render_settings(*vulkan_context_,
                                          project_mgr_.active()->config().render);
                project_mgr_.active()->save();
            }
        }));
    connections_.push_back(menu_bar_.on_close_project.bind(
        [this]() { project_mgr_.close(); }));
    connections_.push_back(menu_bar_.on_open_settings.bind(
        [this]() {
            active_context().project_settings_panel().toggle();
        }));
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
        [this]() { start_play(); }));
    connections_.push_back(menu_bar_.on_stop_play.bind(
        [this]() { stop_play(); }));
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
    connections_.push_back(menu_bar_.on_save_layout.bind(
        [this](const std::string& name) { save_layout(name); }));
    connections_.push_back(menu_bar_.on_load_layout.bind(
        [this](const std::string& name) { load_layout(name); }));
    connections_.push_back(menu_bar_.on_import_model.bind(
        [this]() {
            import_file_browser_.open("Import Model", FileBrowser::Mode::select_file,
                                      {".glb", ".gltf", ".fbx"});
        }));
    connections_.push_back(menu_bar_.on_open_capture.bind(
        [this]() { capture_popup_.open(); }));
    connections_.push_back(import_file_browser_.on_selected.bind(
        [this](const std::string& path) {
            auto output_dir = project_mgr_.active()
                ? project_mgr_.active()->root() / "models"
                : std::filesystem::path{};
            if (auto* proj = project_mgr_.active()) {
                import_dialog_.set_project(proj->root(), proj->config().texture_import_dir);
            } else {
                import_dialog_.set_project({}, "assets/textures");
            }
            import_dialog_.open(path, output_dir);
        }));

    // Asset reference widget browser → deliver result to pending widget
    connections_.push_back(asset_file_browser_.on_selected.bind(
        [this](const std::string& path) {
            std::string root;
            if (project_mgr_.active()) {
                root = project_mgr_.active()->root().string();
            }
            asset_ref_browse_accept(path, root);
        }));

    // React to project open/close from any source
    connections_.push_back(project_mgr_.on_opened.bind(
        [this](const std::string& name) {
            window_.set_title("Fjell - " + name);
            content_browser_.set_root(project_mgr_.active()->root());
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
        if (scene_.is_dirty()) title += " *";
        window_.set_title(title);
    }

    // Global shortcuts (Ctrl+S, Ctrl+Shift+S, Ctrl+Z/Y, Ctrl+Space)
    if (ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeyShift
        && ImGui::IsKeyPressed(ImGuiKey_S)) {
        active_context().save();
        if (&active_context() == scene_context_) {
            save_scene();
        }
    }
    if (ImGui::GetIO().KeyCtrl && ImGui::GetIO().KeyShift
        && ImGui::IsKeyPressed(ImGuiKey_S)) {
        if (project_mgr_.active()) {
            snapshot_render_settings(*vulkan_context_,
                                      project_mgr_.active()->config().render);
            project_mgr_.active()->save();
        }
    }
    if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Space)) {
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
    menu_bar_.set_playing(active_context().is_playing());
    menu_bar_.set_building(project_mgr_.is_building());
    menu_bar_.set_scene_path(current_scene_path_);
    menu_bar_.set_vulkan_context(vulkan_context_.get());
    menu_bar_.set_viewport_manager(&scene_context_->viewport_manager());
    menu_bar_.draw();

    capture_popup_.draw(capture_service_, scene_context_->viewport_manager());

    draw_context_tabs();
    setup_dockspace();

    // Active context draws its own panels (including Stats, Console, History)
    active_context().draw(clock_.delta_time());
    viewport_hovered_ = active_context().any_viewport_hovered();

    content_browser_.draw();

    vulkan_context_->draw_sdf_debug_window();

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
    };
}

void Engine::flush_pending() {
    // Materialize nodes spawned mid-tick before anything else walks objects_.
    auto spawned = scene_.flush_spawned();
    if (!spawned.empty() && active_context().is_playing()) {
        // A spawned node may carry an InputComponent (e.g. a player's camera);
        // load its contexts before begin_play so scripts can bind/activate input.
        load_input_contexts();
        // Give freshly-spawned nodes their script lifecycle (instantiate +
        // begin_play) — the play-start passes already ran without them.
        script_subsystem_->manager().initialize_spawned(scene_, spawned);
    }

    if (scene_.pending_destroy().empty()) return;
    script_subsystem_->manager().trace_doomed_refs(scene_, scene_.pending_destroy());
    if (physics_subsystem_->world()) {
        for (uint32_t id : scene_.pending_destroy()) {
            physics_subsystem_->world()->remove_body(id);
        }
    }
    scene_.flush_destroyed();
}

void Engine::load_input_contexts() {
    for (const auto& obj : scene_.objects()) {
        const auto* ic = obj->components.get<InputComponent>();
        if (!ic) continue;

        auto actions_dir = ic->actions_dir;
        if (actions_dir.empty()) actions_dir = "assets/input/actions";
        if (project_mgr_.active()) {
            input_action_map_.load_actions_dir(project_mgr_.active()->resolve(actions_dir));
        }

        for (const auto& ctx_path : ic->context_paths) {
            std::string resolved = ctx_path;
            if (project_mgr_.active()) {
                resolved = project_mgr_.active()->resolve(ctx_path);
            }
            input_action_map_.load_context(resolved);
            auto fname = std::filesystem::path(ctx_path).stem().string();
            input_action_map_.push_context(fname);
        }
    }
}

void Engine::start_play() {
    // Editor pre-play work (snapshot, clear undo)
    scene_context_->on_play_enter();

    // Serialize script state before play modifies it
    script_subsystem_->manager().serialize_all(scene_);

    // Phase 1: Physics + VFX (no physics dependency between them)
    {
        TickContext ctx{
            .scene = scene_,
            .input = input_,
            .clock = clock_,
            .dt = clock_.delta_time(),
            .physics = nullptr,
            .project = project_mgr_.active(),
        };
        physics_subsystem_->notify_play_start(ctx);
        vfx_subsystem_->notify_play_start(ctx);
    }

    // Phase 2: Everything else (physics world now exists)
    {
        TickContext ctx{
            .scene = scene_,
            .input = input_,
            .clock = clock_,
            .dt = clock_.delta_time(),
            .physics = physics_subsystem_->world(),
            .project = project_mgr_.active(),
        };
        camera_subsystem_->notify_play_start(ctx);
        audio_subsystem_->notify_play_start(ctx);
        sprite_anim_subsystem_->notify_play_start(ctx);

        // Load input contexts BEFORE scripts start (begin_play calls InputAPI::bind)
        load_input_contexts();

        // Clear stale input bindings from a prior reload's begin_play —
        // those captured `this` pointers to instances that were just replaced
        // by deserialize_all above.
        fjell_input_clear_connections();

        script_subsystem_->notify_play_start(ctx);
        ui_subsystem_->notify_play_start(ctx);

        // begin_play may have spawned nodes carrying InputComponents (e.g. a
        // server spawning each player + its camera). Re-scan so their contexts
        // load now that they're in the scene — the binds above already attached
        // to the actions; loading the context just enables dispatch.
        load_input_contexts();
    }

    vulkan_context_->set_playing(true);
    game_ui_.mark_dirty();
    FJELL_CORE_INFO("Play mode started");
}

void Engine::stop_play() {
    if (game_mode_) {
        glfwSetWindowShouldClose(window_.handle(), GLFW_TRUE);
        return;
    }

    TickContext ctx{
        .scene = scene_,
        .input = input_,
        .clock = clock_,
        .dt = clock_.delta_time(),
        .physics = physics_subsystem_->world(),
        .project = project_mgr_.active(),
    };

    // Reset time state before notifying subsystems
    clock_.set_time_scale(1.0f);
    clock_.set_paused(false);

    fjell_input_clear_connections();
    ui_subsystem_->notify_play_stop(ctx);
    camera_subsystem_->notify_play_stop(ctx);
    audio_subsystem_->notify_play_stop(ctx);
    sprite_anim_subsystem_->notify_play_stop(ctx);
    script_subsystem_->notify_play_stop(ctx);
    vfx_subsystem_->notify_play_stop(ctx);

    // Editor post-play work (restore scene snapshot)
    scene_context_->on_play_exit();

    script_subsystem_->manager().deserialize_all(scene_);
    physics_subsystem_->notify_play_stop(ctx);

    vulkan_context_->set_playing(false);
    if (input_.is_cursor_captured()) {
        input_.set_cursor_captured(window_.handle(), false);
    }
    FJELL_CORE_INFO("Play mode stopped");
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
        start_play();
    }

    while (!window_.should_close()) {
        // Phase 1: Submit previous frame to GPU (non-blocking)
        vulkan_context_->submit_and_present();

        FJELL_PROFILE_FRAME;

        // Phase 2: CPU work (overlaps with GPU executing the submitted frame)
        asset_loader_->poll();
        input_.update();
        input_action_map_.update(input_);
        window_.poll_events();
        if (import_dialog_.is_open()) import_dialog_.tick();
        if (import_file_browser_.is_open()) import_file_browser_.tick();
        if (asset_file_browser_.is_open()) asset_file_browser_.tick();
        menu_bar_.tick_browsers();
        clock_.tick();

#ifndef NDEBUG
        if (shader_watcher_ && shader_watcher_->poll()) {
            vulkan_context_->reload_pipeline();
        }
        if (fjgraph_watcher_) { (void)fjgraph_watcher_->poll(); }
#endif

        auto ctx = make_frame_context();

        if (active_context().is_playing()) {
            // Update camera state for worldspace UI billboard computation
            if (auto* cam_node = camera_subsystem_->active_camera(scene_)) {
                float aspect = scene_context_->viewport_manager().active_aspect();
                ui_subsystem_->set_camera(
                    cam_node->position(),
                    cam_node->view_matrix(),
                    cam_node->projection_matrix(aspect));
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
                stop_play();
            }
        }

        project_mgr_.poll_build(script_subsystem_->manager(), scene_,
                                active_context().is_playing());

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

        // Always-tick subsystems (before world matrices)
        for (auto* s : early_always_) s->tick(ctx);
        scene_.compute_world_matrices(&thread_pool_);

        // Always-tick subsystems (after world matrices)
        for (auto* s : late_always_) s->tick(ctx);

        // Play-only subsystems that need world matrices (e.g. audio listener)
        if (active_context().is_playing()) {
            for (auto* s : late_play_) s->tick(ctx);
        }

        // Phase 3: Wait for frame slot — GPU is done with this slot after this
        if (!vulkan_context_->begin_frame(thread_pool_)) continue;

        // If a capture was pending from the previous frame, the slot's
        // fence has settled and the rendered tonemap output is now safe
        // to read back.
        capture_service_.finish_after_present(*vulkan_context_,
                                              scene_context_->viewport_manager());

        // Resize offscreen passes (using last frame's panel size) — safe
        // because begin_frame's fence guarantees the GPU is no longer
        // referencing this frame slot's resources.
        vulkan_context_->set_active_sources(scene_context_->viewport_manager().active_source_mask());
        active_context().apply_resize(*vulkan_context_);

        // Editor capture: resize the editor viewport + PP to the capture
        // resolution BEFORE draw_ui, so ImGui::Image picks up the new
        // tonemap descriptor instead of one we're about to free.
        capture_service_.resize_for_capture(*vulkan_context_,
                                            scene_context_->viewport_manager());

        imgui_layer_->begin_frame();
        if (active_context().is_playing()) {
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

        // In play mode, override the active viewport's camera with the scene CameraNode
        if (active_context().is_playing() && !requests.empty()) {
            if (auto* cam_node = camera_subsystem_->active_camera(scene_)) {
                float aspect = scene_context_->viewport_manager().active_aspect();
                requests[0].view = cam_node->view_matrix();
                requests[0].projection = cam_node->projection_matrix(aspect);
                requests[0].camera_position = cam_node->position();
            }
        }

        // Contexts with 3D viewports return their scene (main or preview).
        // Contexts without one (e.g. UI editor) return nullptr — fall back to main scene
        // so the renderer always has valid geometry to reference.
        Scene* render_scene = active_context().render_scene();
        if (!render_scene) render_scene = &scene_;

        vulkan_context_->sync_sprite_textures(*render_scene, *resource_manager_, project_mgr_.active());
        vulkan_context_->sync_tilemap_textures(*render_scene, *resource_manager_, project_mgr_.active());
        vulkan_context_->set_viewport_offset(scene_context_->viewport_manager().active_viewport_pos());

        // Render scene viewports (render-to-texture, must complete before main frame)
        vulkan_context_->render_scene_viewports(render_scene, clock_.game_delta_time(),
                                                 scene_context_->selected_id(),
                                                 light_, thread_pool_);

        // Render auxiliary viewports (camera preview) in separate command buffers
        active_context().render_previews(*vulkan_context_, render_scene,
                                          clock_.game_delta_time(), light_, thread_pool_);

        // Set active editor's render scene for SDF gathering
        vulkan_context_->set_active_render_scene(active_context().render_scene());

        // Phase 4: Record commands
        vulkan_context_->record_frame(requests, light_, render_scene, clock_.game_delta_time(),
                                       scene_context_->selected_id(), thread_pool_,
                                       active_context().is_playing());

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
    }

    ImGui::PopStyleVar(2);   // TabRounding, TabBorderSize
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar();    // FramePadding

    ImGui::End();
}

void Engine::draw_status_bar() {
    if (!StatusBar::begin()) {
        StatusBar::end();
        return;
    }

    // ── Left side: project / scene / mode ─────────────────────────────
    const auto* project = project_mgr_.active();
    if (project != nullptr) {
        StatusBar::item(ICON_LC_PACKAGE, project->config().name);
        StatusBar::separator();
    }

    if (!current_scene_path_.empty()) {
        std::string scene_name = std::filesystem::path(current_scene_path_).stem().string();
        StatusBar::item(ICON_LC_FILE, scene_name);
        StatusBar::separator();
    }

    const bool playing = active_context().is_playing();
    StatusBar::item(playing ? ICON_LC_PLAY : ICON_LC_PENCIL,
                    playing ? "Playing" : "Editing");

    // Selected entity (scene context only — other contexts have no scene selection)
    if (scene_context_ != nullptr && &active_context() == scene_context_) {
        uint32_t sel = scene_context_->selected_id();
        if (sel != 0) {
            if (const auto* node = scene_.find(sel)) {
                StatusBar::separator();
                StatusBar::item(ICON_LC_BOX, node->name);
            }
        }
    }

    // ── Right side: frame time + FPS + GPU ────────────────────────────
    // Reserve enough width for "999.99 ms  ·  9999 FPS  ·  <gpu name>"
    // and push following items to the right edge.
    float dt_ms = clock_.delta_time() * 1000.0f;
    float fps = dt_ms > 0.0f ? 1000.0f / dt_ms : 0.0f;

    char frame_buf[64];
    std::snprintf(frame_buf, sizeof(frame_buf), "%.2f ms", static_cast<double>(dt_ms));
    char fps_buf[32];
    std::snprintf(fps_buf, sizeof(fps_buf), "%.0f FPS", static_cast<double>(fps));

    const std::string& gpu = vulkan_context_ ? vulkan_context_->gpu_name() : std::string{};

    // Rough width estimate; ImGui doesn't expose a "reserve from right" so
    // we measure all three items and the separators between them.
    float spacing = 16.0f + 16.0f; // two separators (8px each side)
    float reserve = ImGui::CalcTextSize(frame_buf).x
                  + ImGui::CalcTextSize(fps_buf).x
                  + ImGui::CalcTextSize(gpu.c_str()).x
                  + spacing + 60.0f; // padding + icons
    StatusBar::right_align(reserve);

    StatusBar::item(ICON_LC_GAUGE, frame_buf);
    StatusBar::separator();
    StatusBar::item(nullptr, fps_buf);
    if (!gpu.empty()) {
        StatusBar::separator();
        StatusBar::item(ICON_LC_MONITOR, gpu);
    }

    StatusBar::end();
}

} // namespace fjell
