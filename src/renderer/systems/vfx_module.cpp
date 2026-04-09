#include "renderer/systems/vfx_module.hpp"
#include "ui/imgui_curve_editor.hpp"

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <cstring>
#include <filesystem>

namespace fjell {

// ─── Helper: write header + data to buffer ──────────────────────────────

static uint32_t write_header(uint8_t* buffer, ModuleType type, uint32_t payload_words) {
    auto* hdr = reinterpret_cast<GpuModuleHeader*>(buffer);
    hdr->type = static_cast<uint32_t>(type);
    hdr->size = 2 + payload_words; // header (2 words) + payload
    return hdr->size * 4;
}

static void write_float(uint8_t* buffer, uint32_t offset, float v) {
    std::memcpy(buffer + offset, &v, 4);
}

static void write_uint(uint8_t* buffer, uint32_t offset, uint32_t v) {
    std::memcpy(buffer + offset, &v, 4);
}

static void write_vec3(uint8_t* buffer, uint32_t offset, const glm::vec3& v) {
    std::memcpy(buffer + offset, &v, 12);
}

static void write_vec4(uint8_t* buffer, uint32_t offset, const glm::vec4& v) {
    std::memcpy(buffer + offset, &v, 16);
}

// ─── Factory ─────────────────────────────────────────────────────────────

std::unique_ptr<VfxModule> create_module(const std::string& type_name) {
    if (type_name == "SpawnRate")        return std::make_unique<SpawnRateModule>();
    if (type_name == "InitialLifetime")  return std::make_unique<InitialLifetimeModule>();
    if (type_name == "InitialPosition")  return std::make_unique<InitialPositionModule>();
    if (type_name == "InitialVelocity")  return std::make_unique<InitialVelocityModule>();
    if (type_name == "InitialColor")     return std::make_unique<InitialColorModule>();
    if (type_name == "InitialSize")      return std::make_unique<InitialSizeModule>();
    if (type_name == "Gravity")          return std::make_unique<GravityModule>();
    if (type_name == "ColorOverLife")    return std::make_unique<ColorOverLifeModule>();
    if (type_name == "SizeOverLife")     return std::make_unique<SizeOverLifeModule>();
    if (type_name == "Drag")             return std::make_unique<DragModule>();
    if (type_name == "Noise")            return std::make_unique<NoiseModule>();
    if (type_name == "Attract")          return std::make_unique<AttractModule>();
    if (type_name == "VelocityLimit")    return std::make_unique<VelocityLimitModule>();
    if (type_name == "InitialStretch")   return std::make_unique<InitialStretchModule>();
    if (type_name == "Burst")            return std::make_unique<BurstModule>();
    if (type_name == "InitialRotation") return std::make_unique<InitialRotationModule>();
    if (type_name == "InitialTexture") return std::make_unique<InitialTextureModule>();
    if (type_name == "Flipbook")        return std::make_unique<FlipbookModule>();
    if (type_name == "Collision")       return std::make_unique<CollisionModule>();
    if (type_name == "Billboard")        return std::make_unique<BillboardModule>();
    if (type_name == "VelocityInheritance") return std::make_unique<VelocityInheritanceModule>();
    if (type_name == "CurveColorOverLife") return std::make_unique<CurveColorOverLifeModule>();
    if (type_name == "CurveSizeOverLife") return std::make_unique<CurveSizeOverLifeModule>();
    if (type_name == "Trail")            return std::make_unique<TrailModule>();
    if (type_name == "LitParticle")      return std::make_unique<LitParticleModule>();
    if (type_name == "MeshParticle")     return std::make_unique<MeshParticleModule>();
    if (type_name == "Wind")             return std::make_unique<WindModule>();
    if (type_name == "EmitOverDistance") return std::make_unique<EmitOverDistanceModule>();
    if (type_name == "Orbital")          return std::make_unique<OrbitalModule>();
    if (type_name == "Emissive")         return std::make_unique<EmissiveModule>();
    if (type_name == "Distortion")       return std::make_unique<DistortionModule>();
    return nullptr;
}

// ─── Pin descriptors ────────────────────────────────────────────────────

std::vector<PinDescriptor> SpawnRateModule::pin_descriptors() {
    return {{"rate", PinDataType::float_type, &rate, {}, 0.0f, 10000.0f, 1.0f}};
}

std::vector<PinDescriptor> InitialLifetimeModule::pin_descriptors() {
    return {
        {"min", PinDataType::float_type, &lifetime_min, {}, 0.01f, 60.0f, 0.01f},
        {"max", PinDataType::float_type, &lifetime_max, {}, 0.01f, 60.0f, 0.01f},
    };
}

std::vector<PinDescriptor> InitialPositionModule::pin_descriptors() {
    return {
        {"shape", PinDataType::enum_type, &shape, {"Point", "Sphere", "Box", "Cone", "Ring", "Edge", "Disk"}},
        {"radius", PinDataType::float_type, &radius, {}, 0.0f, 100.0f, 0.01f},
        {"half_extents", PinDataType::vec3_type, &half_extents, {}, 0.0f, 100.0f, 0.01f},
        {"edge_length", PinDataType::float_type, &edge_length, {}, 0.0f, 100.0f, 0.01f},
    };
}

std::vector<PinDescriptor> InitialVelocityModule::pin_descriptors() {
    return {
        {"mode", PinDataType::enum_type, &mode, {"Directional", "Random", "Cone"}},
        {"velocity_min", PinDataType::vec3_type, &velocity_min, {}, -100.0f, 100.0f, 0.1f},
        {"velocity_max", PinDataType::vec3_type, &velocity_max, {}, -100.0f, 100.0f, 0.1f},
        {"direction", PinDataType::vec3_type, &direction, {}, -1.0f, 1.0f, 0.01f},
        {"angle", PinDataType::float_type, &angle, {}, 0.0f, 180.0f, 0.5f},
        {"speed_min", PinDataType::float_type, &speed_min, {}, 0.0f, 100.0f, 0.1f},
        {"speed_max", PinDataType::float_type, &speed_max, {}, 0.0f, 100.0f, 0.1f},
    };
}

std::vector<PinDescriptor> InitialColorModule::pin_descriptors() {
    return {{"color", PinDataType::vec4_type, &color, {}, 0.0f, 1.0f, 0.01f}};
}

std::vector<PinDescriptor> InitialSizeModule::pin_descriptors() {
    return {
        {"min", PinDataType::float_type, &size_min, {}, 0.0f, 100.0f, 0.01f},
        {"max", PinDataType::float_type, &size_max, {}, 0.0f, 100.0f, 0.01f},
    };
}

std::vector<PinDescriptor> GravityModule::pin_descriptors() {
    return {{"force", PinDataType::vec3_type, &force, {}, -100.0f, 100.0f, 0.1f}};
}

std::vector<PinDescriptor> ColorOverLifeModule::pin_descriptors() {
    return {
        {"start", PinDataType::vec4_type, &color_start, {}, 0.0f, 1.0f, 0.01f},
        {"end", PinDataType::vec4_type, &color_end, {}, 0.0f, 1.0f, 0.01f},
    };
}

std::vector<PinDescriptor> SizeOverLifeModule::pin_descriptors() {
    return {
        {"start", PinDataType::float_type, &size_start, {}, 0.0f, 100.0f, 0.01f},
        {"end", PinDataType::float_type, &size_end, {}, 0.0f, 100.0f, 0.01f},
    };
}

std::vector<PinDescriptor> DragModule::pin_descriptors() {
    return {{"drag", PinDataType::float_type, &drag, {}, 0.0f, 10.0f, 0.01f}};
}

std::vector<PinDescriptor> NoiseModule::pin_descriptors() {
    return {
        {"strength", PinDataType::float_type, &strength, {}, 0.0f, 100.0f, 0.1f},
        {"frequency", PinDataType::float_type, &frequency, {}, 0.01f, 50.0f, 0.1f},
        {"scroll_speed", PinDataType::float_type, &scroll_speed, {}, 0.0f, 10.0f, 0.1f},
    };
}

std::vector<PinDescriptor> AttractModule::pin_descriptors() {
    return {
        {"position", PinDataType::vec3_type, &position, {}, -100.0f, 100.0f, 0.1f},
        {"strength", PinDataType::float_type, &strength, {}, 0.0f, 100.0f, 0.1f},
        {"radius", PinDataType::float_type, &radius, {}, 0.0f, 100.0f, 0.1f},
    };
}

std::vector<PinDescriptor> VelocityLimitModule::pin_descriptors() {
    return {{"max_speed", PinDataType::float_type, &max_speed, {}, 0.0f, 1000.0f, 0.1f}};
}

std::vector<PinDescriptor> CollisionModule::pin_descriptors() {
    return {
        {"response", PinDataType::enum_type, &response, {"Bounce", "Slide", "Kill"}},
        {"source", PinDataType::enum_type, &source, {"SDF", "Depth", "Both"}},
        {"restitution", PinDataType::float_type, &restitution, {}, 0.0f, 1.0f, 0.01f},
        {"friction", PinDataType::float_type, &friction, {}, 0.0f, 1.0f, 0.01f},
    };
}

std::vector<PinDescriptor> InitialStretchModule::pin_descriptors() {
    return {{"stretch", PinDataType::float_type, &stretch, {}, 0.0f, 10.0f, 0.01f}};
}

std::vector<PinDescriptor> InitialTextureModule::pin_descriptors() {
    return {{"bindless_index", PinDataType::uint_type, &bindless_index, {}}};
}

std::vector<PinDescriptor> InitialRotationModule::pin_descriptors() {
    return {
        {"rotation_min", PinDataType::float_type, &rotation_min, {}, 0.0f, 6.2832f, 0.01f},
        {"rotation_max", PinDataType::float_type, &rotation_max, {}, 0.0f, 6.2832f, 0.01f},
        {"speed", PinDataType::float_type, &speed, {}, -20.0f, 20.0f, 0.1f},
        {"speed_variance", PinDataType::float_type, &speed_variance, {}, 0.0f, 20.0f, 0.1f},
    };
}

std::vector<PinDescriptor> BurstModule::pin_descriptors() {
    return {{"count", PinDataType::uint_type, &count, {}}};
}

std::vector<PinDescriptor> FlipbookModule::pin_descriptors() {
    return {
        {"columns", PinDataType::uint_type, &columns, {}},
        {"rows", PinDataType::uint_type, &rows, {}},
        {"frame_count", PinDataType::uint_type, &frame_count, {}},
        {"fps", PinDataType::float_type, &fps, {}, 1.0f, 120.0f, 0.5f},
        {"mode", PinDataType::enum_type, &mode, {"Over Lifetime", "FPS Loop", "FPS Clamp"}},
    };
}

// BillboardModule has no parameters — uses default empty pin_descriptors()

// ─── SpawnRate ───────────────────────────────────────────────────────────

uint32_t SpawnRateModule::write_gpu_data(uint8_t* buffer) const {
    uint32_t total = write_header(buffer, type(), 1);
    write_float(buffer, 8, rate);
    return total;
}

bool SpawnRateModule::draw_inspector() {
    return ImGui::DragFloat("Rate", &rate, 1.0f, 0.0f, 10000.0f);
}

void SpawnRateModule::to_json(nlohmann::json& j) const {
    j["type"] = "SpawnRate";
    j["rate"] = rate;
}

void SpawnRateModule::from_json(const nlohmann::json& j) {
    rate = j.value("rate", 50.0f);
}

// ─── InitialLifetime ─────────────────────────────────────────────────────

uint32_t InitialLifetimeModule::write_gpu_data(uint8_t* buffer) const {
    uint32_t total = write_header(buffer, type(), 2);
    write_float(buffer, 8, lifetime_min);
    write_float(buffer, 12, lifetime_max);
    return total;
}

bool InitialLifetimeModule::draw_inspector() {
    bool changed = false;
    changed |= ImGui::DragFloat("Min", &lifetime_min, 0.01f, 0.01f, 60.0f);
    changed |= ImGui::DragFloat("Max", &lifetime_max, 0.01f, 0.01f, 60.0f);
    if (lifetime_max < lifetime_min) lifetime_max = lifetime_min;
    return changed;
}

void InitialLifetimeModule::to_json(nlohmann::json& j) const {
    j["type"] = "InitialLifetime";
    j["min"] = lifetime_min;
    j["max"] = lifetime_max;
}

void InitialLifetimeModule::from_json(const nlohmann::json& j) {
    lifetime_min = j.value("min", 1.0f);
    lifetime_max = j.value("max", 3.0f);
}

// ─── InitialPosition ─────────────────────────────────────────────────────

uint32_t InitialPositionModule::write_gpu_data(uint8_t* buffer) const {
    // shape(1) + radius(1) + half_extents(3) + edge_length(1) + pad(2) = 8 words
    uint32_t total = write_header(buffer, type(), 8);
    write_uint(buffer, 8, static_cast<uint32_t>(shape));
    write_float(buffer, 12, radius);
    write_vec3(buffer, 16, half_extents);
    write_float(buffer, 28, edge_length);
    write_float(buffer, 32, 0.0f); // pad
    write_float(buffer, 36, 0.0f); // pad
    return total;
}

bool InitialPositionModule::draw_inspector() {
    bool changed = false;
    const char* shapes[] = {"Point", "Sphere", "Box", "Cone", "Ring", "Edge", "Disk"};
    int s = static_cast<int>(shape);
    if (ImGui::Combo("Shape", &s, shapes, 7)) {
        shape = static_cast<SpawnShape>(s);
        changed = true;
    }
    if (shape == SpawnShape::sphere || shape == SpawnShape::cone ||
        shape == SpawnShape::ring || shape == SpawnShape::disk) {
        changed |= ImGui::DragFloat("Radius", &radius, 0.01f, 0.0f, 100.0f);
    }
    if (shape == SpawnShape::box) {
        changed |= ImGui::DragFloat3("Half Extents", &half_extents.x, 0.01f, 0.0f, 100.0f);
    }
    if (shape == SpawnShape::edge) {
        changed |= ImGui::DragFloat("Length", &edge_length, 0.01f, 0.0f, 100.0f);
    }
    return changed;
}

void InitialPositionModule::to_json(nlohmann::json& j) const {
    j["type"] = "InitialPosition";
    const char* names[] = {"point", "sphere", "box", "cone", "ring", "edge", "disk"};
    j["shape"] = names[static_cast<int>(shape)];
    j["radius"] = radius;
    j["half_extents"] = {half_extents.x, half_extents.y, half_extents.z};
    j["edge_length"] = edge_length;
}

void InitialPositionModule::from_json(const nlohmann::json& j) {
    auto s = j.value("shape", "point");
    if (s == "sphere") shape = SpawnShape::sphere;
    else if (s == "box") shape = SpawnShape::box;
    else if (s == "cone") shape = SpawnShape::cone;
    else if (s == "ring") shape = SpawnShape::ring;
    else if (s == "edge") shape = SpawnShape::edge;
    else if (s == "disk") shape = SpawnShape::disk;
    else shape = SpawnShape::point;

    radius = j.value("radius", 0.1f);
    edge_length = j.value("edge_length", 1.0f);
    if (j.contains("half_extents")) {
        auto& h = j["half_extents"];
        half_extents = {h[0].get<float>(), h[1].get<float>(), h[2].get<float>()};
    }
}

// ─── InitialVelocity ─────────────────────────────────────────────────────

uint32_t InitialVelocityModule::write_gpu_data(uint8_t* buffer) const {
    // mode(1) + pad(1) + vel_min(3) + pad(1) + vel_max(3) + pad(1)
    // + direction(3) + angle(1) + speed_min(1) + speed_max(1) + pad(2) = 18 words
    uint32_t total = write_header(buffer, type(), 16);
    write_uint(buffer, 8, static_cast<uint32_t>(mode));
    write_float(buffer, 12, 0.0f); // pad
    write_vec3(buffer, 16, velocity_min);
    write_float(buffer, 28, 0.0f); // pad
    write_vec3(buffer, 32, velocity_max);
    write_float(buffer, 44, 0.0f); // pad
    write_vec3(buffer, 48, direction);
    write_float(buffer, 60, glm::radians(angle));
    write_float(buffer, 64, speed_min);
    write_float(buffer, 68, speed_max);
    write_float(buffer, 72, 0.0f); // pad
    write_float(buffer, 76, 0.0f); // pad
    return total;
}

bool InitialVelocityModule::draw_inspector() {
    bool changed = false;
    const char* modes[] = {"Directional", "Random", "Cone"};
    int m = static_cast<int>(mode);
    if (ImGui::Combo("Mode", &m, modes, 3)) {
        mode = static_cast<VelocityMode>(m);
        changed = true;
    }
    if (mode == VelocityMode::random) {
        changed |= ImGui::DragFloat3("Min", &velocity_min.x, 0.1f);
        changed |= ImGui::DragFloat3("Max", &velocity_max.x, 0.1f);
    } else if (mode == VelocityMode::directional) {
        changed |= ImGui::DragFloat3("Min", &velocity_min.x, 0.1f);
        changed |= ImGui::DragFloat3("Max", &velocity_max.x, 0.1f);
    } else if (mode == VelocityMode::cone) {
        changed |= ImGui::DragFloat3("Direction", &direction.x, 0.01f);
        changed |= ImGui::DragFloat("Angle", &angle, 0.5f, 0.0f, 90.0f);
        changed |= ImGui::DragFloat("Speed Min", &speed_min, 0.1f, 0.0f, 100.0f);
        changed |= ImGui::DragFloat("Speed Max", &speed_max, 0.1f, 0.0f, 100.0f);
    }
    return changed;
}

void InitialVelocityModule::to_json(nlohmann::json& j) const {
    j["type"] = "InitialVelocity";
    const char* mode_names[] = {"directional", "random", "cone"};
    j["mode"] = mode_names[static_cast<int>(mode)];
    j["velocity_min"] = {velocity_min.x, velocity_min.y, velocity_min.z};
    j["velocity_max"] = {velocity_max.x, velocity_max.y, velocity_max.z};
    j["direction"] = {direction.x, direction.y, direction.z};
    j["angle"] = angle;
    j["speed_min"] = speed_min;
    j["speed_max"] = speed_max;
}

void InitialVelocityModule::from_json(const nlohmann::json& j) {
    auto m = j.value("mode", "random");
    if (m == "directional") mode = VelocityMode::directional;
    else if (m == "cone") mode = VelocityMode::cone;
    else mode = VelocityMode::random;

    if (j.contains("velocity_min")) {
        auto& v = j["velocity_min"];
        velocity_min = {v[0].get<float>(), v[1].get<float>(), v[2].get<float>()};
    }
    if (j.contains("velocity_max")) {
        auto& v = j["velocity_max"];
        velocity_max = {v[0].get<float>(), v[1].get<float>(), v[2].get<float>()};
    }
    if (j.contains("direction")) {
        auto& d = j["direction"];
        direction = {d[0].get<float>(), d[1].get<float>(), d[2].get<float>()};
    }
    angle = j.value("angle", 30.0f);
    speed_min = j.value("speed_min", 2.0f);
    speed_max = j.value("speed_max", 5.0f);
}

// ─── InitialColor ────────────────────────────────────────────────────────

uint32_t InitialColorModule::write_gpu_data(uint8_t* buffer) const {
    uint32_t total = write_header(buffer, type(), 4);
    write_vec4(buffer, 8, color);
    return total;
}

bool InitialColorModule::draw_inspector() {
    return ImGui::ColorEdit4("Color", &color.x);
}

void InitialColorModule::to_json(nlohmann::json& j) const {
    j["type"] = "InitialColor";
    j["color"] = {color.r, color.g, color.b, color.a};
}

void InitialColorModule::from_json(const nlohmann::json& j) {
    if (j.contains("color")) {
        auto& c = j["color"];
        color = {c[0].get<float>(), c[1].get<float>(), c[2].get<float>(), c[3].get<float>()};
    }
}

// ─── InitialSize ─────────────────────────────────────────────────────────

uint32_t InitialSizeModule::write_gpu_data(uint8_t* buffer) const {
    uint32_t total = write_header(buffer, type(), 2);
    write_float(buffer, 8, size_min);
    write_float(buffer, 12, size_max);
    return total;
}

bool InitialSizeModule::draw_inspector() {
    bool changed = false;
    changed |= ImGui::DragFloat("Min", &size_min, 0.001f, 0.001f, 10.0f);
    changed |= ImGui::DragFloat("Max", &size_max, 0.001f, 0.001f, 10.0f);
    if (size_max < size_min) size_max = size_min;
    return changed;
}

void InitialSizeModule::to_json(nlohmann::json& j) const {
    j["type"] = "InitialSize";
    j["min"] = size_min;
    j["max"] = size_max;
}

void InitialSizeModule::from_json(const nlohmann::json& j) {
    size_min = j.value("min", 0.05f);
    size_max = j.value("max", 0.1f);
}

// ─── Gravity ─────────────────────────────────────────────────────────────

uint32_t GravityModule::write_gpu_data(uint8_t* buffer) const {
    uint32_t total = write_header(buffer, type(), 4);
    write_vec3(buffer, 8, force);
    write_float(buffer, 20, 0.0f); // pad to 4-word boundary
    return total;
}

bool GravityModule::draw_inspector() {
    return ImGui::DragFloat3("Force", &force.x, 0.1f);
}

void GravityModule::to_json(nlohmann::json& j) const {
    j["type"] = "Gravity";
    j["force"] = {force.x, force.y, force.z};
}

void GravityModule::from_json(const nlohmann::json& j) {
    if (j.contains("force")) {
        auto& f = j["force"];
        force = {f[0].get<float>(), f[1].get<float>(), f[2].get<float>()};
    }
}

// ─── Wind ───────────────────────────────────────────────────────────────

std::vector<PinDescriptor> WindModule::pin_descriptors() {
    return {
        {"direction", PinDataType::vec3_type, &direction, {}, -1.0f, 1.0f, 0.01f},
        {"strength", PinDataType::float_type, &strength, {}, 0.0f, 100.0f, 0.1f},
    };
}

uint32_t WindModule::write_gpu_data(uint8_t* buffer) const {
    // header(2) + direction(3) + strength(1) = 6 words
    uint32_t total = write_header(buffer, type(), 6);
    write_vec3(buffer, 8, direction);
    write_float(buffer, 20, strength);
    return total;
}

bool WindModule::draw_inspector() {
    bool changed = false;
    if (ImGui::DragFloat3("Direction", &direction.x, 0.01f, -1.0f, 1.0f)) changed = true;
    if (ImGui::DragFloat("Strength", &strength, 0.1f, 0.0f, 100.0f)) changed = true;
    return changed;
}

void WindModule::to_json(nlohmann::json& j) const {
    j["type"] = "Wind";
    j["direction"] = {direction.x, direction.y, direction.z};
    j["strength"] = strength;
}

void WindModule::from_json(const nlohmann::json& j) {
    if (j.contains("direction")) {
        auto& d = j["direction"];
        direction = {d[0].get<float>(), d[1].get<float>(), d[2].get<float>()};
    }
    strength = j.value("strength", 1.0f);
}

// ─── Orbital ────────────────────────────────────────────────────────────

std::vector<PinDescriptor> OrbitalModule::pin_descriptors() {
    return {
        {"mode", PinDataType::enum_type, &mode, {"Vortex", "Orbit", "Swirl"}},
        {"axis", PinDataType::vec3_type, &axis, {}, -1.0f, 1.0f, 0.01f},
        {"speed", PinDataType::float_type, &speed, {}, -20.0f, 20.0f, 0.1f},
        {"pull", PinDataType::float_type, &pull, {}, -10.0f, 10.0f, 0.1f},
    };
}

uint32_t OrbitalModule::write_gpu_data(uint8_t* buffer) const {
    // header(2) + mode(1) + axis(3) + speed(1) + pull(1) = 8 words
    uint32_t total = write_header(buffer, type(), 8);
    write_uint(buffer, 8, static_cast<uint32_t>(mode));
    write_vec3(buffer, 12, axis);
    write_float(buffer, 24, speed);
    write_float(buffer, 28, pull);
    return total;
}

bool OrbitalModule::draw_inspector() {
    bool changed = false;
    const char* modes[] = {"Vortex", "Orbit", "Swirl"};
    int m = static_cast<int>(mode);
    if (ImGui::Combo("Mode", &m, modes, 3)) {
        mode = static_cast<OrbitalMode>(m);
        changed = true;
    }
    if (ImGui::DragFloat3("Axis / Center", &axis.x, 0.01f, -1.0f, 1.0f)) changed = true;
    if (ImGui::DragFloat("Speed", &speed, 0.1f, -20.0f, 20.0f)) changed = true;
    if (mode == OrbitalMode::swirl) {
        if (ImGui::DragFloat("Pull", &pull, 0.1f, -10.0f, 10.0f)) changed = true;
    }
    return changed;
}

void OrbitalModule::to_json(nlohmann::json& j) const {
    j["type"] = "Orbital";
    const char* mode_names[] = {"vortex", "orbit", "swirl"};
    j["mode"] = mode_names[static_cast<int>(mode)];
    j["axis"] = {axis.x, axis.y, axis.z};
    j["speed"] = speed;
    if (mode == OrbitalMode::swirl) {
        j["pull"] = pull;
    }
}

void OrbitalModule::from_json(const nlohmann::json& j) {
    auto m = j.value("mode", "vortex");
    if (m == "orbit") mode = OrbitalMode::orbit;
    else if (m == "swirl") mode = OrbitalMode::swirl;
    else mode = OrbitalMode::vortex;
    if (j.contains("axis")) {
        auto& a = j["axis"];
        axis = {a[0].get<float>(), a[1].get<float>(), a[2].get<float>()};
    }
    speed = j.value("speed", 2.0f);
    pull = j.value("pull", 0.0f);
}

// ─── ColorOverLife ───────────────────────────────────────────────────────

uint32_t ColorOverLifeModule::write_gpu_data(uint8_t* buffer) const {
    uint32_t total = write_header(buffer, type(), 8);
    write_vec4(buffer, 8, color_start);
    write_vec4(buffer, 24, color_end);
    return total;
}

bool ColorOverLifeModule::draw_inspector() {
    bool changed = false;
    changed |= ImGui::ColorEdit4("Start", &color_start.x);
    changed |= ImGui::ColorEdit4("End", &color_end.x);
    return changed;
}

void ColorOverLifeModule::to_json(nlohmann::json& j) const {
    j["type"] = "ColorOverLife";
    j["start"] = {color_start.r, color_start.g, color_start.b, color_start.a};
    j["end"] = {color_end.r, color_end.g, color_end.b, color_end.a};
}

void ColorOverLifeModule::from_json(const nlohmann::json& j) {
    if (j.contains("start")) {
        auto& c = j["start"];
        color_start = {c[0].get<float>(), c[1].get<float>(), c[2].get<float>(), c[3].get<float>()};
    }
    if (j.contains("end")) {
        auto& c = j["end"];
        color_end = {c[0].get<float>(), c[1].get<float>(), c[2].get<float>(), c[3].get<float>()};
    }
}

// ─── SizeOverLife ────────────────────────────────────────────────────────

uint32_t SizeOverLifeModule::write_gpu_data(uint8_t* buffer) const {
    uint32_t total = write_header(buffer, type(), 2);
    write_float(buffer, 8, size_start);
    write_float(buffer, 12, size_end);
    return total;
}

bool SizeOverLifeModule::draw_inspector() {
    bool changed = false;
    changed |= ImGui::DragFloat("Start", &size_start, 0.001f, 0.001f, 10.0f);
    changed |= ImGui::DragFloat("End", &size_end, 0.001f, 0.001f, 10.0f);
    return changed;
}

void SizeOverLifeModule::to_json(nlohmann::json& j) const {
    j["type"] = "SizeOverLife";
    j["start"] = size_start;
    j["end"] = size_end;
}

void SizeOverLifeModule::from_json(const nlohmann::json& j) {
    size_start = j.value("start", 0.1f);
    size_end = j.value("end", 0.02f);
}

// ─── Drag ────────────────────────────────────────────────────────────────

uint32_t DragModule::write_gpu_data(uint8_t* buffer) const {
    uint32_t total = write_header(buffer, type(), 2);
    write_float(buffer, 8, drag);
    write_float(buffer, 12, 0.0f); // pad
    return total;
}

bool DragModule::draw_inspector() {
    return ImGui::DragFloat("Drag", &drag, 0.01f, 0.0f, 10.0f);
}

void DragModule::to_json(nlohmann::json& j) const {
    j["type"] = "Drag";
    j["drag"] = drag;
}

void DragModule::from_json(const nlohmann::json& j) {
    drag = j.value("drag", 0.1f);
}

// ─── Noise ───────────────────────────────────────────────────────────

uint32_t NoiseModule::write_gpu_data(uint8_t* buffer) const {
    uint32_t total = write_header(buffer, type(), 4);
    write_float(buffer, 8, strength);
    write_float(buffer, 12, frequency);
    write_float(buffer, 16, scroll_speed);
    write_float(buffer, 20, 0.0f); // pad
    return total;
}

bool NoiseModule::draw_inspector() {
    bool changed = false;
    changed |= ImGui::DragFloat("Strength", &strength, 0.1f, 0.0f, 100.0f);
    changed |= ImGui::DragFloat("Frequency", &frequency, 0.1f, 0.01f, 50.0f);
    changed |= ImGui::DragFloat("Scroll Speed", &scroll_speed, 0.1f, 0.0f, 10.0f);
    return changed;
}

void NoiseModule::to_json(nlohmann::json& j) const {
    j["type"] = "Noise";
    j["strength"] = strength;
    j["frequency"] = frequency;
    j["scroll_speed"] = scroll_speed;
}

void NoiseModule::from_json(const nlohmann::json& j) {
    strength = j.value("strength", 1.0f);
    frequency = j.value("frequency", 2.0f);
    scroll_speed = j.value("scroll_speed", 0.5f);
}

// ─── Attract ─────────────────────────────────────────────────────────

uint32_t AttractModule::write_gpu_data(uint8_t* buffer) const {
    uint32_t total = write_header(buffer, type(), 6);
    write_vec3(buffer, 8, position);
    write_float(buffer, 20, strength);
    write_float(buffer, 24, radius);
    write_float(buffer, 28, 0.0f); // pad
    return total;
}

bool AttractModule::draw_inspector() {
    bool changed = false;
    changed |= ImGui::DragFloat3("Position", &position.x, 0.1f);
    changed |= ImGui::DragFloat("Strength", &strength, 0.1f, 0.0f, 100.0f);
    changed |= ImGui::DragFloat("Radius", &radius, 0.1f, 0.01f, 1000.0f);
    return changed;
}

void AttractModule::to_json(nlohmann::json& j) const {
    j["type"] = "Attract";
    j["position"] = {position.x, position.y, position.z};
    j["strength"] = strength;
    j["radius"] = radius;
}

void AttractModule::from_json(const nlohmann::json& j) {
    if (j.contains("position")) {
        auto& p = j["position"];
        position = {p[0].get<float>(), p[1].get<float>(), p[2].get<float>()};
    }
    strength = j.value("strength", 5.0f);
    radius = j.value("radius", 10.0f);
}

// ─── VelocityLimit ───────────────────────────────────────────────────

uint32_t VelocityLimitModule::write_gpu_data(uint8_t* buffer) const {
    uint32_t total = write_header(buffer, type(), 2);
    write_float(buffer, 8, max_speed);
    write_float(buffer, 12, 0.0f); // pad
    return total;
}

bool VelocityLimitModule::draw_inspector() {
    return ImGui::DragFloat("Max Speed", &max_speed, 0.1f, 0.0f, 1000.0f);
}

void VelocityLimitModule::to_json(nlohmann::json& j) const {
    j["type"] = "VelocityLimit";
    j["max_speed"] = max_speed;
}

void VelocityLimitModule::from_json(const nlohmann::json& j) {
    max_speed = j.value("max_speed", 10.0f);
}

// ─── Collision ──────────────────────────────────────────────────────────

uint32_t CollisionModule::write_gpu_data(uint8_t* buffer) const {
    // payload: response(1) + source(1) + restitution(1) + friction(1)
    //        + sdf_min(3) + pad(1) + sdf_max(3) + pad(1) = 12 words
    uint32_t total = write_header(buffer, type(), 12);
    write_uint(buffer, 8, static_cast<uint32_t>(response));
    write_uint(buffer, 12, static_cast<uint32_t>(source));
    write_float(buffer, 16, restitution);
    write_float(buffer, 20, friction);
    // SDF volume bounds (hardcoded to match SceneSdfPass defaults for now)
    write_vec3(buffer, 24, glm::vec3(-10.0f));
    write_float(buffer, 36, 0.0f);
    write_vec3(buffer, 40, glm::vec3(10.0f));
    write_float(buffer, 52, 0.0f);
    return total;
}

bool CollisionModule::draw_inspector() {
    bool changed = false;
    const char* sources[] = {"SDF (static geometry)", "Depth Buffer (displaced)", "Both"};
    int s = static_cast<int>(source);
    if (ImGui::Combo("Source", &s, sources, 3)) {
        source = static_cast<Source>(s);
        changed = true;
    }
    const char* modes[] = {"Bounce", "Slide", "Kill"};
    int r = static_cast<int>(response);
    if (ImGui::Combo("Response", &r, modes, 3)) {
        response = static_cast<Response>(r);
        changed = true;
    }
    if (response != Response::kill) {
        if (ImGui::SliderFloat("Restitution", &restitution, 0.0f, 1.0f)) changed = true;
        if (ImGui::SliderFloat("Friction", &friction, 0.0f, 1.0f)) changed = true;
    }
    return changed;
}

void CollisionModule::to_json(nlohmann::json& j) const {
    j["type"] = "Collision";
    const char* mode_names[] = {"bounce", "slide", "kill"};
    const char* src_names[] = {"sdf", "depth", "both"};
    j["response"] = mode_names[static_cast<int>(response)];
    j["source"] = src_names[static_cast<int>(source)];
    j["restitution"] = restitution;
    j["friction"] = friction;
}

void CollisionModule::from_json(const nlohmann::json& j) {
    std::string r = j.value("response", "bounce");
    if (r == "slide") response = Response::slide;
    else if (r == "kill") response = Response::kill;
    else response = Response::bounce;
    std::string s = j.value("source", "sdf");
    if (s == "depth") source = Source::depth;
    else if (s == "both") source = Source::both;
    else source = Source::sdf;
    restitution = j.value("restitution", 0.5f);
    friction = j.value("friction", 0.2f);
}

// ─── InitialStretch ──────────────────────────────────────────────────────

uint32_t InitialStretchModule::write_gpu_data(uint8_t* buffer) const {
    uint32_t total = write_header(buffer, type(), 2);
    write_float(buffer, 8, stretch);
    write_float(buffer, 12, 0.0f); // pad
    return total;
}

bool InitialStretchModule::draw_inspector() {
    return ImGui::DragFloat("Stretch", &stretch, 0.1f, 0.0f, 100.0f,
                            "%.1f", ImGuiSliderFlags_None);
}

void InitialStretchModule::to_json(nlohmann::json& j) const {
    j["type"] = "InitialStretch";
    j["stretch"] = stretch;
}

void InitialStretchModule::from_json(const nlohmann::json& j) {
    stretch = j.value("stretch", 0.0f);
}

// ─── InitialTexture ─────────────────────────────────────────────────────

uint32_t InitialTextureModule::write_gpu_data(uint8_t* buffer) const {
    uint32_t total = write_header(buffer, type(), 2);
    write_uint(buffer, 8, bindless_index);
    write_uint(buffer, 12, 0); // pad
    return total;
}

bool InitialTextureModule::draw_inspector() {
    // Texture assignment is handled by the editor context (drag-drop).
    // This just shows the current state.
    if (!texture_path.empty()) {
        auto filename = std::filesystem::path(texture_path).filename().string();
        ImGui::TextDisabled("%s", filename.c_str());
    } else {
        ImGui::TextDisabled("(none — drag texture here)");
    }
    return false;
}

void InitialTextureModule::to_json(nlohmann::json& j) const {
    j["type"] = "InitialTexture";
    j["texture"] = texture_path;
    // bindless_index is runtime-only — not serialized
}

void InitialTextureModule::from_json(const nlohmann::json& j) {
    texture_path = j.value("texture", "");
    // bindless_index is resolved at runtime, not loaded from JSON
}

// ─── InitialRotation ────────────────────────────────────────────────────

uint32_t InitialRotationModule::write_gpu_data(uint8_t* buffer) const {
    uint32_t total = write_header(buffer, type(), 4);
    write_float(buffer, 8, rotation_min);
    write_float(buffer, 12, rotation_max);
    write_float(buffer, 16, speed);
    write_float(buffer, 20, speed_variance);
    return total;
}

bool InitialRotationModule::draw_inspector() {
    bool changed = false;
    float deg_min = glm::degrees(rotation_min);
    float deg_max = glm::degrees(rotation_max);
    if (ImGui::DragFloat("Min (deg)", &deg_min, 1.0f, -360.0f, 360.0f)) {
        rotation_min = glm::radians(deg_min);
        changed = true;
    }
    if (ImGui::DragFloat("Max (deg)", &deg_max, 1.0f, -360.0f, 360.0f)) {
        rotation_max = glm::radians(deg_max);
        changed = true;
    }
    float spd_deg = glm::degrees(speed);
    float var_deg = glm::degrees(speed_variance);
    if (ImGui::DragFloat("Speed (deg/s)", &spd_deg, 1.0f, -3600.0f, 3600.0f)) {
        speed = glm::radians(spd_deg);
        changed = true;
    }
    if (ImGui::DragFloat("Variance (deg/s)", &var_deg, 1.0f, 0.0f, 3600.0f)) {
        speed_variance = glm::radians(var_deg);
        changed = true;
    }
    return changed;
}

void InitialRotationModule::to_json(nlohmann::json& j) const {
    j["type"] = "InitialRotation";
    j["rotation_min"] = rotation_min;
    j["rotation_max"] = rotation_max;
    j["speed"] = speed;
    j["speed_variance"] = speed_variance;
}

void InitialRotationModule::from_json(const nlohmann::json& j) {
    rotation_min = j.value("rotation_min", 0.0f);
    rotation_max = j.value("rotation_max", 6.2832f);
    speed = j.value("speed", 0.0f);
    speed_variance = j.value("speed_variance", 0.0f);
}

// ─── Burst ──────────────────────────────────────────────────────────────

uint32_t BurstModule::write_gpu_data(uint8_t* /*buffer*/) const {
    // Burst is CPU-only emission logic — no GPU payload
    return 0;
}

bool BurstModule::draw_inspector() {
    int c = static_cast<int>(count);
    bool changed = ImGui::DragInt("Count", &c, 1.0f, 1, 100000);
    if (changed) count = static_cast<uint32_t>(std::max(c, 1));
    return changed;
}

void BurstModule::to_json(nlohmann::json& j) const {
    j["type"] = "Burst";
    j["count"] = count;
}

void BurstModule::from_json(const nlohmann::json& j) {
    count = j.value("count", 100u);
}

// ─── EmitOverDistance ────────────────────────────────────────────────────

std::vector<PinDescriptor> EmitOverDistanceModule::pin_descriptors() {
    return {{"particles_per_meter", PinDataType::float_type, &particles_per_meter, {}, 0.1f, 200.0f, 0.5f}};
}

uint32_t EmitOverDistanceModule::write_gpu_data(uint8_t* /*buffer*/) const {
    return 0; // CPU-only module, no GPU data
}

bool EmitOverDistanceModule::draw_inspector() {
    return ImGui::DragFloat("Particles / m", &particles_per_meter, 0.5f, 0.1f, 200.0f);
}

void EmitOverDistanceModule::to_json(nlohmann::json& j) const {
    j["type"] = "EmitOverDistance";
    j["particles_per_meter"] = particles_per_meter;
}

void EmitOverDistanceModule::from_json(const nlohmann::json& j) {
    particles_per_meter = j.value("particles_per_meter", 10.0f);
}

// ─── Flipbook ───────────────────────────────────────────────────────────

uint32_t FlipbookModule::pack_params() const {
    uint32_t fc = frame_count > 0 ? frame_count : (columns * rows);
    fc = std::min(fc, 255u);
    uint32_t cols = std::min(columns, 255u);
    uint32_t fps_enc = std::min(static_cast<uint32_t>(fps * 4.0f), 255u);
    uint32_t m = static_cast<uint32_t>(mode);
    return cols | (fc << 8) | (fps_enc << 16) | (m << 24);
}

uint32_t FlipbookModule::write_gpu_data(uint8_t* buffer) const {
    uint32_t total = write_header(buffer, type(), 2);
    write_uint(buffer, 8, pack_params());
    write_uint(buffer, 12, 0); // pad
    return total;
}

bool FlipbookModule::draw_inspector() {
    bool changed = false;
    int c = static_cast<int>(columns);
    int r = static_cast<int>(rows);
    int fc = static_cast<int>(frame_count);
    float f = fps;

    if (ImGui::DragInt("Columns", &c, 0.1f, 1, 64)) {
        columns = static_cast<uint32_t>(std::max(c, 1));
        changed = true;
    }
    if (ImGui::DragInt("Rows", &r, 0.1f, 1, 64)) {
        rows = static_cast<uint32_t>(std::max(r, 1));
        changed = true;
    }
    if (ImGui::DragInt("Frame Count", &fc, 0.1f, 0, 255)) {
        frame_count = static_cast<uint32_t>(std::max(fc, 0));
        changed = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(0 = cols*rows)");

    if (ImGui::DragFloat("FPS", &f, 0.5f, 0.25f, 63.75f)) {
        fps = f;
        changed = true;
    }

    const char* modes[] = {"Over Lifetime", "FPS (Loop)", "FPS (Clamp)"};
    int mi = static_cast<int>(mode);
    if (ImGui::Combo("Mode", &mi, modes, 3)) {
        mode = static_cast<Mode>(mi);
        changed = true;
    }

    // Preview info
    uint32_t total = frame_count > 0 ? frame_count : columns * rows;
    ImGui::TextDisabled("%u frames (%ux%u grid)", total, columns, rows);

    return changed;
}

void FlipbookModule::to_json(nlohmann::json& j) const {
    j["type"] = "Flipbook";
    j["columns"] = columns;
    j["rows"] = rows;
    j["frame_count"] = frame_count;
    j["fps"] = fps;
    const char* mode_names[] = {"over_lifetime", "fps_loop", "fps_clamp"};
    j["mode"] = mode_names[static_cast<int>(mode)];
}

void FlipbookModule::from_json(const nlohmann::json& j) {
    columns = j.value("columns", 4u);
    rows = j.value("rows", 4u);
    frame_count = j.value("frame_count", 0u);
    fps = j.value("fps", 24.0f);
    std::string m = j.value("mode", "over_lifetime");
    if (m == "fps_loop") mode = Mode::fps_loop;
    else if (m == "fps_clamp") mode = Mode::fps_clamp;
    else mode = Mode::over_lifetime;
}

// ─── Billboard ───────────────────────────────────────────────────────────

uint32_t BillboardModule::write_gpu_data(uint8_t* /*buffer*/) const {
    // Billboard has no GPU params — it's handled by the render pipeline selection
    return 0;
}

bool BillboardModule::draw_inspector() {
    ImGui::TextDisabled("Camera-facing quad (default)");
    return false;
}

void BillboardModule::to_json(nlohmann::json& j) const {
    j["type"] = "Billboard";
}

void BillboardModule::from_json(const nlohmann::json& /*j*/) {
    // no params
}

// ─── VelocityInheritance ────────────────────────────────────────────────

std::vector<PinDescriptor> VelocityInheritanceModule::pin_descriptors() {
    return {{"factor", PinDataType::float_type, &factor, {}, 0.0f, 1.0f, 0.01f}};
}

uint32_t VelocityInheritanceModule::write_gpu_data(uint8_t* buffer) const {
    uint32_t total = write_header(buffer, type(), 2);
    write_float(buffer, 8, factor);
    write_float(buffer, 12, 0.0f); // pad
    return total;
}

bool VelocityInheritanceModule::draw_inspector() {
    return ImGui::SliderFloat("Factor", &factor, 0.0f, 1.0f);
}

void VelocityInheritanceModule::to_json(nlohmann::json& j) const {
    j["type"] = "VelocityInheritance";
    j["factor"] = factor;
}

void VelocityInheritanceModule::from_json(const nlohmann::json& j) {
    factor = j.value("factor", 1.0f);
}

// ─── CurveColorOverLife ─────────────────────────────────────────────────

uint32_t CurveColorOverLifeModule::write_gpu_data(uint8_t* buffer) const {
    // Layout: header(2) + sample_count(1) + pad(1) + R[32] + G[32] + B[32] + A[32]
    constexpr uint32_t payload_words = 2 + kCurveLutSamples * 4; // 130
    uint32_t total = write_header(buffer, type(), payload_words);
    write_uint(buffer, 8, kCurveLutSamples);
    write_uint(buffer, 12, 0); // pad
    uint32_t off = 16;
    float lut[kCurveLutSamples];
    curve_r.bake(lut, kCurveLutSamples);
    for (uint32_t i = 0; i < kCurveLutSamples; ++i) write_float(buffer, off + i * 4, lut[i]);
    off += kCurveLutSamples * 4;
    curve_g.bake(lut, kCurveLutSamples);
    for (uint32_t i = 0; i < kCurveLutSamples; ++i) write_float(buffer, off + i * 4, lut[i]);
    off += kCurveLutSamples * 4;
    curve_b.bake(lut, kCurveLutSamples);
    for (uint32_t i = 0; i < kCurveLutSamples; ++i) write_float(buffer, off + i * 4, lut[i]);
    off += kCurveLutSamples * 4;
    curve_a.bake(lut, kCurveLutSamples);
    for (uint32_t i = 0; i < kCurveLutSamples; ++i) write_float(buffer, off + i * 4, lut[i]);
    return total;
}

bool CurveColorOverLifeModule::draw_inspector() {
    bool changed = false;
    if (ImGui::TreeNode("Red")) {
        changed |= CurveEditor("##r", curve_r);
        ImGui::TreePop();
    }
    if (ImGui::TreeNode("Green")) {
        changed |= CurveEditor("##g", curve_g);
        ImGui::TreePop();
    }
    if (ImGui::TreeNode("Blue")) {
        changed |= CurveEditor("##b", curve_b);
        ImGui::TreePop();
    }
    if (ImGui::TreeNode("Alpha")) {
        changed |= CurveEditor("##a", curve_a);
        ImGui::TreePop();
    }
    ImGui::Separator();
    if (ImGui::Button("Linear")) {
        curve_r = Curve::linear(1.0f, 1.0f);
        curve_g = Curve::linear(0.8f, 0.1f);
        curve_b = Curve::linear(0.2f, 0.0f);
        curve_a = Curve::linear(1.0f, 0.0f);
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Fade Out")) {
        curve_r = Curve::linear(1.0f, 1.0f);
        curve_g = Curve::linear(1.0f, 1.0f);
        curve_b = Curve::linear(1.0f, 1.0f);
        curve_a = Curve::ease_in(1.0f, 0.0f);
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Pulse")) {
        curve_a = Curve::bell(1.0f);
        changed = true;
    }
    return changed;
}

void CurveColorOverLifeModule::to_json(nlohmann::json& j) const {
    j["type"] = "CurveColorOverLife";
    curve_r.to_json(j["r"]);
    curve_g.to_json(j["g"]);
    curve_b.to_json(j["b"]);
    curve_a.to_json(j["a"]);
}

void CurveColorOverLifeModule::from_json(const nlohmann::json& j) {
    if (j.contains("r")) curve_r.from_json(j["r"]);
    if (j.contains("g")) curve_g.from_json(j["g"]);
    if (j.contains("b")) curve_b.from_json(j["b"]);
    if (j.contains("a")) curve_a.from_json(j["a"]);
}

// ─── CurveSizeOverLife ──────────────────────────────────────────────────

uint32_t CurveSizeOverLifeModule::write_gpu_data(uint8_t* buffer) const {
    // Layout: header(2) + sample_count(1) + pad(1) + samples[32]
    constexpr uint32_t payload_words = 2 + kCurveLutSamples; // 34
    uint32_t total = write_header(buffer, type(), payload_words);
    write_uint(buffer, 8, kCurveLutSamples);
    write_uint(buffer, 12, 0); // pad
    float lut[kCurveLutSamples];
    curve.bake(lut, kCurveLutSamples);
    for (uint32_t i = 0; i < kCurveLutSamples; ++i) {
        write_float(buffer, 16 + i * 4, lut[i]);
    }
    return total;
}

bool CurveSizeOverLifeModule::draw_inspector() {
    bool changed = CurveEditor("##size", curve, {-1, 150}, 0.0f, 1.0f);
    ImGui::Separator();
    if (ImGui::Button("Linear")) {
        curve = Curve::linear(0.1f, 0.02f);
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Shrink")) {
        curve = Curve::ease_in(0.1f, 0.0f);
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Grow")) {
        curve = Curve::ease_out(0.0f, 0.1f);
        changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Pulse")) {
        curve = Curve::bell(0.15f);
        changed = true;
    }
    return changed;
}

void CurveSizeOverLifeModule::to_json(nlohmann::json& j) const {
    j["type"] = "CurveSizeOverLife";
    curve.to_json(j["curve"]);
}

void CurveSizeOverLifeModule::from_json(const nlohmann::json& j) {
    if (j.contains("curve")) curve.from_json(j["curve"]);
}

// ─── MeshParticle ───────────────────────────────────────────────────────

std::vector<PinDescriptor> MeshParticleModule::pin_descriptors() {
    return {{"shape", PinDataType::enum_type, &shape, {"Cube", "Sphere", "Custom"}}};
}

uint32_t MeshParticleModule::write_gpu_data(uint8_t* /*buffer*/) const {
    return 0; // render module — no GPU module data
}

bool MeshParticleModule::draw_inspector() {
    bool changed = false;
    const char* shapes[] = {"Cube", "Sphere", "Custom"};
    int s = static_cast<int>(shape);
    if (ImGui::Combo("Shape", &s, shapes, 3)) {
        shape = static_cast<ParticleMeshShape>(s);
        changed = true;
    }
    if (shape == ParticleMeshShape::custom) {
        char buf[256]{};
        std::strncpy(buf, model_path.c_str(), sizeof(buf) - 1);
        if (ImGui::InputText("Model Path", buf, sizeof(buf))) {
            model_path = buf;
            changed = true;
        }
    }
    {
        char mat_buf[256]{};
        std::strncpy(mat_buf, material_path.c_str(), sizeof(mat_buf) - 1);
        if (ImGui::InputText("Material (.fjmat)", mat_buf, sizeof(mat_buf))) {
            material_path = mat_buf;
            resolved_material_id = 0; // force re-resolve
            changed = true;
        }
    }
    return changed;
}

void MeshParticleModule::to_json(nlohmann::json& j) const {
    j["type"] = "MeshParticle";
    const char* shape_names[] = {"cube", "sphere", "custom"};
    j["shape"] = shape_names[static_cast<int>(shape)];
    if (shape == ParticleMeshShape::custom && !model_path.empty()) {
        j["model_path"] = model_path;
    }
    if (!material_path.empty()) {
        j["material"] = material_path;
    }
}

void MeshParticleModule::from_json(const nlohmann::json& j) {
    auto s = j.value("shape", "cube");
    if (s == "sphere") shape = ParticleMeshShape::sphere;
    else if (s == "custom") shape = ParticleMeshShape::custom;
    else shape = ParticleMeshShape::cube;
    model_path = j.value("model_path", "");
    material_path = j.value("material", "");
}

// ─── LitParticle ────────────────────────────────────────────────────────

std::vector<PinDescriptor> LitParticleModule::pin_descriptors() {
    return {
        {"metallic", PinDataType::float_type, &metallic, {}, 0.0f, 1.0f, 0.01f},
        {"roughness", PinDataType::float_type, &roughness, {}, 0.0f, 1.0f, 0.01f},
    };
}

uint32_t LitParticleModule::write_gpu_data(uint8_t* /*buffer*/) const {
    return 0; // render module — no GPU module data
}

bool LitParticleModule::draw_inspector() {
    bool changed = false;
    changed |= ImGui::SliderFloat("Metallic", &metallic, 0.0f, 1.0f);
    changed |= ImGui::SliderFloat("Roughness", &roughness, 0.04f, 1.0f);
    return changed;
}

void LitParticleModule::to_json(nlohmann::json& j) const {
    j["type"] = "LitParticle";
    j["metallic"] = metallic;
    j["roughness"] = roughness;
}

void LitParticleModule::from_json(const nlohmann::json& j) {
    metallic = j.value("metallic", 0.0f);
    roughness = j.value("roughness", 0.5f);
}

// ─── Distortion ─────────────────────────────────────────────────────────

std::vector<PinDescriptor> DistortionModule::pin_descriptors() {
    return {
        {"strength", PinDataType::float_type, &strength, {}, 0.0f, 1.0f, 0.001f},
    };
}

uint32_t DistortionModule::write_gpu_data(uint8_t* buffer) const {
    // strength(1) + normal_map_index(1) = 2 words
    uint32_t total = write_header(buffer, type(), 2);
    write_float(buffer, 8, strength);
    write_uint(buffer, 12, normal_map_index);
    return total;
}

bool DistortionModule::draw_inspector() {
    bool changed = false;
    changed |= ImGui::DragFloat("Strength", &strength, 0.001f, 0.0f, 1.0f);
    // TODO: normal map texture picker
    return changed;
}

void DistortionModule::to_json(nlohmann::json& j) const {
    j["type"] = "Distortion";
    j["strength"] = strength;
    if (!normal_map_path.empty()) j["normal_map"] = normal_map_path;
}

void DistortionModule::from_json(const nlohmann::json& j) {
    strength = j.value("strength", 0.05f);
    normal_map_path = j.value("normal_map", "");
}

// ─── Emissive ───────────────────────────────────────────────────────────

std::vector<PinDescriptor> EmissiveModule::pin_descriptors() {
    return {
        {"color", PinDataType::vec3_type, &color, {}, 0.0f, 1.0f, 0.01f},
        {"intensity", PinDataType::float_type, &intensity, {}, 0.0f, 50.0f, 0.1f},
    };
}

uint32_t EmissiveModule::write_gpu_data(uint8_t* buffer) const {
    // header(2) + color(3) + intensity(1) + pad(2) = 8 words (4-word aligned)
    uint32_t total = write_header(buffer, type(), 6);
    write_vec3(buffer, 8, color);
    write_float(buffer, 20, intensity);
    return total; // 24 bytes
}

bool EmissiveModule::draw_inspector() {
    bool changed = false;
    if (ImGui::ColorEdit3("Color", &color.x)) changed = true;
    if (ImGui::DragFloat("Intensity", &intensity, 0.1f, 0.0f, 50.0f)) changed = true;
    return changed;
}

void EmissiveModule::to_json(nlohmann::json& j) const {
    j["type"] = "Emissive";
    j["color"] = {color.x, color.y, color.z};
    j["intensity"] = intensity;
}

void EmissiveModule::from_json(const nlohmann::json& j) {
    if (j.contains("color")) {
        auto& c = j["color"];
        color = {c[0].get<float>(), c[1].get<float>(), c[2].get<float>()};
    }
    intensity = j.value("intensity", 5.0f);
}

// ─── Trail ──────────────────────────────────────────────────────────────

uint32_t TrailModule::write_gpu_data(uint8_t* /*buffer*/) const {
    // Trail has no GPU module data — it's handled via emitter flags and the trail pipeline
    return 0;
}

bool TrailModule::draw_inspector() {
    bool changed = false;
    changed |= ImGui::DragFloat("Width", &width, 0.001f, 0.001f, 10.0f);
    changed |= ImGui::SliderFloat("Width Over Life", &width_over_life, 0.0f, 1.0f,
                                   "%.2f (0=taper, 1=constant)");
    changed |= ImGui::Checkbox("Face Camera", &face_camera);
    return changed;
}

void TrailModule::to_json(nlohmann::json& j) const {
    j["type"] = "Trail";
    j["width"] = width;
    j["width_over_life"] = width_over_life;
    j["face_camera"] = face_camera;
}

void TrailModule::from_json(const nlohmann::json& j) {
    width = j.value("width", 0.1f);
    width_over_life = j.value("width_over_life", 1.0f);
    face_camera = j.value("face_camera", true);
}

} // namespace fjell
