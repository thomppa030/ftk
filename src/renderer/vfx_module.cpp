#include "renderer/vfx_module.hpp"

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <cstring>

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
    if (type_name == "Billboard")        return std::make_unique<BillboardModule>();
    return nullptr;
}

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
    // shape(1) + radius(1) + half_extents(3) + pad(1) = 6 words
    uint32_t total = write_header(buffer, type(), 6);
    write_uint(buffer, 8, static_cast<uint32_t>(shape));
    write_float(buffer, 12, radius);
    write_vec3(buffer, 16, half_extents);
    write_float(buffer, 28, 0.0f); // pad
    return total;
}

bool InitialPositionModule::draw_inspector() {
    bool changed = false;
    const char* shapes[] = {"Point", "Sphere", "Box", "Cone"};
    int s = static_cast<int>(shape);
    if (ImGui::Combo("Shape", &s, shapes, 4)) {
        shape = static_cast<SpawnShape>(s);
        changed = true;
    }
    if (shape == SpawnShape::sphere) {
        changed |= ImGui::DragFloat("Radius", &radius, 0.01f, 0.0f, 100.0f);
    }
    if (shape == SpawnShape::box) {
        changed |= ImGui::DragFloat3("Half Extents", &half_extents.x, 0.01f, 0.0f, 100.0f);
    }
    return changed;
}

void InitialPositionModule::to_json(nlohmann::json& j) const {
    j["type"] = "InitialPosition";
    const char* names[] = {"point", "sphere", "box", "cone"};
    j["shape"] = names[static_cast<int>(shape)];
    j["radius"] = radius;
    j["half_extents"] = {half_extents.x, half_extents.y, half_extents.z};
}

void InitialPositionModule::from_json(const nlohmann::json& j) {
    auto s = j.value("shape", "point");
    if (s == "sphere") shape = SpawnShape::sphere;
    else if (s == "box") shape = SpawnShape::box;
    else if (s == "cone") shape = SpawnShape::cone;
    else shape = SpawnShape::point;

    radius = j.value("radius", 0.1f);
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

} // namespace fjell
