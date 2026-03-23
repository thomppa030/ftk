#include "core/engine.hpp"
#include "core/base_path.hpp"
#include "core/commands.hpp"
#include "core/game_builder.hpp"
#include "core/layer.hpp"
#include "core/log.hpp"
#include "core/profiler.hpp"
#include "platform/platform.hpp"
#include "physics/physics_world.hpp"
#include "ui/console.hpp"
#include "scene/camera_node.hpp"
#include "scene/light_node.hpp"
#include "renderer/arrow_component.hpp"
#include "renderer/mesh_component.hpp"
#include "renderer/skeletal_mesh_component.hpp"
#include "renderer/sprite_desc.hpp"
#include "renderer/asset_loader.hpp"
#include "renderer/bindless.hpp"
#include "renderer/resource_manager.hpp"
#include "renderer/vulkan_context.hpp"
#include "renderer/fjsl_parser.hpp"
#include "renderer/material.hpp"
#include "renderer/material_asset.hpp"
#include "renderer/model.hpp"
#include "renderer/skeletal_model.hpp"
#include "animation/animator_component.hpp"
#include "animation/skeleton.hpp"
#include "animation/animation_clip.hpp"
#include "renderer/pipeline_cache.hpp"
#include "renderer/sprite_desc.hpp"
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

Engine::Engine(std::string_view title, uint32_t width, uint32_t height,
               const std::filesystem::path& project_path)
    : window_{title, width, height}
    , vulkan_context_{std::make_unique<VulkanContext>(window_)}
    , animation_system_{vulkan_context_->device(), vulkan_context_->physical_device()}
{
    vulkan_context_->set_animation_system(&animation_system_);
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
    } else {
        create_default_scene();
    }

    // Load game scripts
    if (game_mode_) {
        auto game_so = base_path::resolve("game.so");
        if (std::filesystem::exists(game_so)) {
            if (auto r = script_manager_.load(game_so); !r) {
                FJELL_CORE_ERROR("{}", r.error());
            } else {
                script_manager_.deserialize_all(scene_);
            }
        }
    } else {
        load_project_scripts();
    }

    icon_cache_ = std::make_unique<IconCache>(
        vulkan_context_->device(), vulkan_context_->physical_device(),
        vulkan_context_->command_pool(), vulkan_context_->graphics_queue(),
        base_path::resolve("engine_assets/icons"));

#ifdef FJELL_HAS_NVIM_PANEL
    content_browser_.init(icon_cache_.get(), &nvim_panel_,
                          vulkan_context_.get(), resource_manager_.get());
#else
    content_browser_.init(icon_cache_.get(), nullptr,
                          vulkan_context_.get(), resource_manager_.get());
#endif
    connections_.push_back(content_browser_.on_add_model.bind(
        [this](const std::string& path) {
            add_model_to_scene(path);
            command_history_.execute(std::make_unique<AddObjectCommand>(
                scene_, selected_id_, &selected_id_));
        }));
    connections_.push_back(content_browser_.on_load_scene.bind(
        [this](const std::string& path) { load_scene(path); }));
    if (project_mgr_.active()) {
        content_browser_.set_root(project_mgr_.active()->root());
    }

    hierarchy_panel_.init(&scene_, &viewport_manager_, &selected_id_, &command_history_,
                          icon_cache_.get());
    connections_.push_back(hierarchy_panel_.on_add_primitive.bind(
        [this](const std::string& type) {
            add_primitive_to_scene(type);
            command_history_.execute(std::make_unique<AddObjectCommand>(
                scene_, selected_id_, &selected_id_));
        }));
    connections_.push_back(hierarchy_panel_.on_remove_object.bind(
        [this](uint32_t id) {
            command_history_.execute(std::make_unique<RemoveObjectCommand>(
                scene_, id, &selected_id_,
                [this]() { vulkan_context_->wait_idle(); }));
        }));
    connections_.push_back(hierarchy_panel_.on_duplicate.bind(
        [this]() {
            duplicate_selection();
        }));

    properties_panel_.init(&scene_, &selected_id_, &gizmo_op_, &light_,
                           &script_manager_, &command_history_,
                           &vulkan_context_->grid_pass().settings(),
                           vulkan_context_->pipeline_cache(),
                           &vulkan_context_->bindless_textures());

    stats_panel_.init(vulkan_context_.get());

    history_panel_.init(&command_history_);

    node_editor_panel_.init(&scene_, &selected_id_);

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

    viewport_manager_.init(vulkan_context_->device_ref(), vulkan_context_->msaa_samples());
    viewport_manager_.add_viewport(ViewportSource::final_color, "Viewport");

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
    node_editor_panel_.shutdown();
    scene_ = {};
    resource_manager_.reset();
    icon_cache_.reset();
    viewport_manager_.release_textures();
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

void Engine::add_model_to_scene(const std::string& path) {
    vulkan_context_->wait_idle();

    auto model_result = resource_manager_->load_model(
        base_path::resolve_asset(path, project_mgr_.active()));
    if (!model_result) {
        FJELL_CORE_ERROR("{}", model_result.error());
        return;
    }
    auto model_handle = *model_result;

    auto tex = resource_manager_->load_texture(
        base_path::resolve("engine_assets/textures/test.png")).value_or(TextureHandle{});
    auto mat_handle = resource_manager_->create_material("default", tex);
    std::string mat_name = "default";

    auto* model = resource_manager_->get(model_handle);
    if (model && !model->textures().empty()) {
        mat_name = path + "_mat";
        mat_handle = resource_manager_->create_material_from_texture(
            mat_name,
            const_cast<Texture*>(model->textures()[0].get()));
    }

    auto filename = std::filesystem::path(path).stem().string();
    selected_id_ = scene_.add(model_handle, mat_handle, *resource_manager_,
                              {}, filename, path, mat_name);
    FJELL_CORE_INFO("Added model to scene: {}", path);
}

void Engine::add_skeletal_model_to_scene(const std::string& mesh_path,
                                          const std::string& skel_path,
                                          const std::string& anim_path) {
    vulkan_context_->wait_idle();

    auto resolved_mesh = base_path::resolve_asset(mesh_path, project_mgr_.active());
    auto resolved_skel = base_path::resolve_asset(skel_path, project_mgr_.active());

    auto skel_result = resource_manager_->load_skeletal_model(resolved_mesh);
    if (!skel_result) {
        FJELL_CORE_ERROR("{}", skel_result.error());
        return;
    }
    auto skel_model_handle = *skel_result;
    auto* skel_model = resource_manager_->get(skel_model_handle);
    if (!skel_model) {
        FJELL_CORE_ERROR("Failed to load skeletal model: {}", mesh_path);
        return;
    }

    // Create scene node with SkeletalMeshComponent
    auto filename = std::filesystem::path(mesh_path).stem().string();
    auto node = std::make_unique<SceneNode>();
    node->name = filename;
    node->source = mesh_path;

    // Set up skeletal mesh component
    auto& smc = node->components.add<SkeletalMeshComponent>();
    smc.skeletal_model_handle = skel_model_handle;
    smc.skeletal_model = skel_model;

    // Default material
    auto tex = resource_manager_->load_texture(
        base_path::resolve("engine_assets/textures/test.png")).value_or(TextureHandle{});
    smc.material_handle = resource_manager_->create_material("skel_default", tex);
    smc.material = resource_manager_->get(smc.material_handle);

    // Load skeleton (owned by the component)
    smc.skeleton = std::make_shared<Skeleton>(Skeleton::load(resolved_skel));

    // Set up animator
    smc.animator = std::make_unique<AnimatorComponent>();
    smc.animator->init(smc.skeleton.get());

    // Play animation clip if provided
    if (!anim_path.empty()) {
        auto resolved_anim = base_path::resolve_asset(anim_path, project_mgr_.active());
        auto clip = std::make_shared<AnimationClip>(AnimationClip::load(resolved_anim));
        smc.animator->play(clip.get());
        smc.clips.push_back(clip);
        FJELL_CORE_INFO("Playing animation: {}", anim_path);
    }

    selected_id_ = scene_.add_node(std::move(node));
    FJELL_CORE_INFO("Added skeletal model to scene: {}", mesh_path);
}

void Engine::add_primitive_to_scene(const std::string& type) {
    vulkan_context_->wait_idle();

    // Empty node: no mesh
    if (type == "empty") {
        selected_id_ = scene_.add_empty();
        return;
    }

    // Folder: non-spatial organizational node
    if (type == "folder") {
        selected_id_ = scene_.add_folder();
        return;
    }

    // Camera node
    if (type == "camera") {
        auto node = std::make_unique<CameraNode>();
        node->name = "Camera";
        auto& sp = node->components.add<SpriteComponent>();
        sp.texture_path = "engine_assets/icons/icon_camera.png";
        sp.editor_only = true;
        node->components.add<ArrowComponent>();
        selected_id_ = scene_.add_node(std::move(node));
        return;
    }

    // Light nodes
    if (type == "light:directional") {
        auto node = std::make_unique<DirectionalLightNode>();
        node->name = "Directional Light";
        auto& sp = node->components.add<SpriteComponent>();
        sp.texture_path = "engine_assets/icons/icon_dir_light.png";
        sp.editor_only = true;
        selected_id_ = scene_.add_node(std::move(node));
        return;
    }
    if (type == "light:point") {
        auto node = std::make_unique<PointLightNode>();
        node->name = "Point Light";
        auto& sp = node->components.add<SpriteComponent>();
        sp.texture_path = "engine_assets/icons/icon_point_light.png";
        sp.editor_only = true;
        selected_id_ = scene_.add_node(std::move(node));
        return;
    }
    if (type == "light:spot") {
        auto node = std::make_unique<SpotLightNode>();
        node->name = "Spot Light";
        auto& sp = node->components.add<SpriteComponent>();
        sp.texture_path = "engine_assets/icons/icon_point_light.png";
        sp.editor_only = true;
        selected_id_ = scene_.add_node(std::move(node));
        return;
    }
    if (type == "volumetric_fog") {
        auto node = std::make_unique<VolumetricFogNode>();
        node->name = "Volumetric Fog";
        selected_id_ = scene_.add_node(std::move(node));
        return;
    }
    if (type == "world_environment") {
        auto node = std::make_unique<WorldEnvironmentNode>();
        node->name = "World Environment";
        selected_id_ = scene_.add_node(std::move(node));
        return;
    }

    // Sprite objects: plane primitive with a sprite component
    if (type == "sprite") {
        auto model_result = resource_manager_->create_primitive("plane");
        if (!model_result) return;

        auto tex = resource_manager_->load_texture(
            base_path::resolve("engine_assets/textures/test.png")).value_or(TextureHandle{});
        auto mat = resource_manager_->create_material("default", tex);

        selected_id_ = scene_.add(*model_result, mat, *resource_manager_,
                                  {}, "Sprite", "primitive:plane");
        auto* obj = scene_.find(selected_id_);
        if (obj) {
            obj->components.add<SpriteComponent>();
        }
        return;
    }

    auto model_result = resource_manager_->create_primitive(type);
    if (!model_result) return;

    auto tex = resource_manager_->load_texture(
        base_path::resolve("engine_assets/textures/test.png")).value_or(TextureHandle{});
    auto mat = resource_manager_->create_material("default", tex);

    std::string name = type;
    name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));

    selected_id_ = scene_.add(*model_result, mat, *resource_manager_,
                              {}, name, "primitive:" + type);
}

void Engine::duplicate_selection() {
    auto& sel = hierarchy_panel_.selection();
    if (sel.empty()) return;

    vulkan_context_->wait_idle();
    std::vector<uint32_t> ids(sel.begin(), sel.end());
    auto cmd = std::make_unique<DuplicateCommand>(scene_, ids, &selected_id_);
    command_history_.execute(std::move(cmd));

    // Update multi-selection to the new duplicates
    auto* dup_cmd = static_cast<DuplicateCommand*>(command_history_.commands().back().get());
    hierarchy_panel_.clear_selection();
    for (uint32_t id : dup_cmd->created_ids()) {
        hierarchy_panel_.add_to_selection(id);
    }
}

void Engine::delete_selection() {
    auto& sel = hierarchy_panel_.selection();
    if (sel.empty()) return;

    vulkan_context_->wait_idle();
    // Delete in reverse order to avoid invalidating IDs
    std::vector<uint32_t> ids(sel.begin(), sel.end());
    for (uint32_t id : ids) {
        command_history_.execute(std::make_unique<RemoveObjectCommand>(
            scene_, id, &selected_id_,
            [this]() { vulkan_context_->wait_idle(); }));
    }
    hierarchy_panel_.clear_selection();
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

    ImGuiID dockspace_id = ImGui::GetID("MainDockspace");
    ImGui::DockSpace(dockspace_id, {0.0f, 0.0f},
                     ImGuiDockNodeFlags_PassthruCentralNode);

    // Build default layout on first run (skip if restoring a saved layout)
    if (!layout_built_) {
        layout_built_ = true;
        focus_viewport_ = true;
        if (has_saved_layout_) {
            ImGui::End();
            return;
        }

        ImGui::DockBuilderRemoveNode(dockspace_id);
        ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspace_id, viewport->WorkSize);

        // Viewport is the parent — split everything else off from it
        ImGuiID viewport_node = dockspace_id;

        // Right panel: Hierarchy + Properties
        ImGuiID right = ImGui::DockBuilderSplitNode(viewport_node, ImGuiDir_Right, 0.22f, nullptr, &viewport_node);
        ImGuiID right_bottom = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.50f, nullptr, &right);

        // Stats/Console strip below viewport (tabbed)
        ImGuiID stats_strip = ImGui::DockBuilderSplitNode(viewport_node, ImGuiDir_Down, 0.20f, nullptr, &viewport_node);

        // Code Editor below stats
        ImGuiID editor = ImGui::DockBuilderSplitNode(stats_strip, ImGuiDir_Down, 0.65f, nullptr, &stats_strip);

        ImGui::DockBuilderDockWindow("Viewport", viewport_node);
        ImGui::DockBuilderDockWindow("Stats", stats_strip);
        ImGui::DockBuilderDockWindow("Console", stats_strip);
        ImGui::DockBuilderDockWindow("Code Editor", editor);
        ImGui::DockBuilderDockWindow("Hierarchy", right);
        ImGui::DockBuilderDockWindow("History", right);
        ImGui::DockBuilderDockWindow("Properties", right_bottom);
        ImGui::DockBuilderFinish(dockspace_id);
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
        if (auto r = script_manager_.load(so_path); !r) {
            FJELL_CORE_ERROR("{}", r.error());
            return;
        }
        script_manager_.deserialize_all(scene_);
#ifndef NDEBUG
        script_manager_.watch(so_path);
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
    selected_id_ = 0;
    command_history_.clear();
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
        [this]() { project_mgr_.build_scripts(script_manager_, scene_); }));
    connections_.push_back(menu_bar_.on_new_script.bind(
        [this](const std::string& name) { project_mgr_.create_script(name); }));
    connections_.push_back(menu_bar_.on_start_play.bind(
        [this]() { start_play(); }));
    connections_.push_back(menu_bar_.on_stop_play.bind(
        [this]() { stop_play(); }));
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

    // Global shortcuts
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
            command_history_.redo();
        } else {
            command_history_.undo();
        }
    }
    if (!ImGui::GetIO().WantTextInput && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y)) {
        command_history_.redo();
    }

    // Duplicate (Ctrl+D)
    if (!ImGui::GetIO().WantTextInput && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D)) {
        duplicate_selection();
    }
    // Delete selection
    if (!ImGui::GetIO().WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Delete)) {
        delete_selection();
    }

    // Gizmo keyboard shortcuts (skip when typing in a text field or navigating)
    bool navigating = input_.is_mouse_button_held(GLFW_MOUSE_BUTTON_RIGHT);
    if (!ImGui::GetIO().WantTextInput && !navigating) {
        if (ImGui::IsKeyPressed(ImGuiKey_W)) gizmo_op_ = 0; // translate
        if (ImGui::IsKeyPressed(ImGuiKey_E)) gizmo_op_ = 1; // rotate
        if (ImGui::IsKeyPressed(ImGuiKey_R)) gizmo_op_ = 2; // scale
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) hierarchy_panel_.clear_selection();
    }

    // Apply deferred layout load
    if (!pending_layout_load_.empty()) {
        ImGui::LoadIniSettingsFromDisk(pending_layout_load_.c_str());
        pending_layout_load_.clear();
    }

    menu_bar_.set_project(project_mgr_.active());
    menu_bar_.set_playing(is_playing());
    menu_bar_.set_scene_path(current_scene_path_);
    menu_bar_.set_vulkan_context(vulkan_context_.get());
    menu_bar_.set_viewport_manager(&viewport_manager_);
    menu_bar_.draw();
    setup_dockspace();
    viewport_manager_.draw(*vulkan_context_, scene_, selected_id_, input_, clock_.delta_time(), gizmo_op_, &command_history_);
    viewport_hovered_ = viewport_manager_.any_hovered();
    hierarchy_panel_.draw();
    properties_panel_.draw();
    content_browser_.draw();
    stats_panel_.draw(clock_.delta_time());
    history_panel_.draw();
    node_editor_panel_.draw();
    draw_console();
    draw_code_editor();

    if (focus_viewport_) {
        ImGui::SetWindowFocus("Viewport");
        focus_viewport_ = false;
    }
}

void Engine::start_play() {
    command_history_.clear();
    script_manager_.serialize_all(scene_);
    play_session_ = std::make_unique<PlaySession>(scene_);
    script_manager_.prepare(scene_, input_, clock_, play_session_->physics_world());
    script_manager_.deserialize_all(scene_);
    script_manager_.call_begin_play(scene_);
    audio_engine_.start_play(scene_, project_mgr_.active());
    vulkan_context_->set_playing(true);
    game_ui_.mark_dirty();
    input_.set_cursor_captured(window_.handle(), true);
    selected_id_ = 0;
    FJELL_CORE_INFO("Play mode started");
}

void Engine::stop_play() {
    if (game_mode_) {
        // In game mode, ESC quits the application
        glfwSetWindowShouldClose(window_.handle(), GLFW_TRUE);
        return;
    }
    audio_engine_.stop_play();
    script_manager_.call_end_play(scene_);
    play_session_->restore(scene_);
    script_manager_.deserialize_all(scene_);
    vulkan_context_->set_playing(false);
    input_.set_cursor_captured(window_.handle(), false);
    play_session_.reset();
    focus_viewport_ = true;
    FJELL_CORE_INFO("Play mode stopped");
}

void Engine::draw_play_overlay() {
    // In play mode, resize the active viewport's offscreen to fill the window
    auto w = window_.width();
    auto h = window_.height();
    viewport_manager_.resize_active(w, h);

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoScrollbar;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
    ImGui::Begin("##PlayView", nullptr, flags);
    ImGui::PopStyleVar();

    // Scene image fills the window — use active viewport's offscreen
    auto tex_id = viewport_manager_.active_texture(*vulkan_context_);
    if (tex_id) {
        ImGui::Image(tex_id, viewport->WorkSize);
    }

    // Overlay bar at top center
    float bar_width = 220.0f;
    float bar_height = 32.0f;
    float bar_x = viewport->WorkPos.x + (viewport->WorkSize.x - bar_width) * 0.5f;
    float bar_y = viewport->WorkPos.y + 8.0f;

    ImGui::SetNextWindowPos({bar_x, bar_y});
    ImGui::SetNextWindowSize({bar_width, bar_height});
    ImGui::SetNextWindowBgAlpha(0.6f);
    ImGui::Begin("##PlayBar", nullptr,
                 ImGuiWindowFlags_NoTitleBar |
                 ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoScrollbar |
                 ImGuiWindowFlags_NoDocking |
                 ImGuiWindowFlags_NoFocusOnAppearing);

    ImGui::Text("Press ESC to stop");
    ImGui::SameLine();
    if (ImGui::SmallButton("Stop")) {
        stop_play();
    }
    ImGui::End();

    ImGui::End(); // ##PlayView
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
        window_.poll_events();
        clock_.tick();

#ifndef NDEBUG
        if (shader_watcher_ && shader_watcher_->poll()) {
            vulkan_context_->reload_pipeline();
        }
#endif

        float dt = clock_.delta_time();

        if (is_playing()) {
            auto flush_pending = [&] {
                if (scene_.pending_destroy().empty()) return;
                script_manager_.trace_doomed_refs(scene_, scene_.pending_destroy());
                if (play_session_->physics_world()) {
                    for (uint32_t id : scene_.pending_destroy()) {
                        play_session_->physics_world()->remove_body(id);
                    }
                }
                scene_.flush_destroyed();
            };

            script_manager_.update(scene_, input_, clock_, play_session_->physics_world());
            flush_pending();
            play_session_->physics_step(dt, scene_);
            if (play_session_->physics_world()) {
                script_manager_.dispatch_collision_events(scene_, *play_session_->physics_world());
            }
            flush_pending();
            if (play_session_->wants_stop(input_)) {
                stop_play();
            }
        }

#ifndef NDEBUG
        if (auto r = script_manager_.poll_reload(scene_); !r) {
            FJELL_CORE_ERROR("{}", r.error());
        }
#endif

        animation_system_.update(scene_, dt);
        scene_.compute_world_matrices(&thread_pool_);

        // Update audio listener from active camera
        if (is_playing()) {
            for (const auto& obj : scene_.objects()) {
                if (auto* cam_node = dynamic_cast<const CameraNode*>(obj.get())) {
                    glm::vec3 pos = cam_node->position();
                    glm::vec3 fwd = -glm::normalize(glm::vec3(cam_node->world_matrix_[2]));
                    glm::vec3 up = glm::normalize(glm::vec3(cam_node->world_matrix_[1]));
                    audio_engine_.update(scene_, pos, fwd, up);
                    break;
                }
            }
        }

        // Resize offscreen passes before ImGui frame so descriptors stay valid
        viewport_manager_.apply_resize(*vulkan_context_);

        imgui_layer_->begin_frame();
        if (is_playing()) {
            draw_play_overlay();
        } else {
            draw_ui();
        }
        imgui_layer_->end_frame();

        auto requests = viewport_manager_.build_render_requests();

        // In play mode, override the active viewport's camera with the scene CameraNode
        if (is_playing() && !requests.empty()) {
            for (const auto& obj : scene_.objects()) {
                if (auto* cam_node = dynamic_cast<const CameraNode*>(obj.get())) {
                    float aspect = viewport_manager_.active_aspect();
                    requests[0].view = cam_node->view_matrix();
                    requests[0].projection = cam_node->projection_matrix(aspect);
                    requests[0].camera_position = cam_node->position();
                    break;
                }
            }
        }

        vulkan_context_->sync_sprite_textures(scene_, *resource_manager_);
        vulkan_context_->set_viewport_offset(viewport_manager_.active_viewport_pos());
        vulkan_context_->set_active_sources(viewport_manager_.active_source_mask());

        // Phase 3: Wait for frame slot, record commands
        vulkan_context_->wait_and_record(requests, light_, &scene_, clock_.delta_time(), selected_id_, thread_pool_, is_playing());
    }

    // Submit the last recorded frame before shutting down
    vulkan_context_->submit_and_present();
    FJELL_CORE_INFO("Exiting main loop");
}

} // namespace fjell
