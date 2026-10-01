#include "renderer/systems/vfx_module.hpp"
#include "ftk/math/color_space.hpp"
#include "renderer/resources/asset_shader_type.hpp"

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

// A colour field is authored in sRGB — in the asset, the inspector, and any
// expression feeding the pin — and the simulation multiplies linear light,
// so the conversion happens here, on the way into the module buffer.
static void write_color(uint8_t* buffer, uint32_t offset, const glm::vec3& srgb) {
    const glm::vec3 v = color_space::srgb_to_linear(srgb);
    std::memcpy(buffer + offset, &v, sizeof(v));
}

static void write_color(uint8_t* buffer, uint32_t offset, const glm::vec4& srgb) {
    const glm::vec4 v = color_space::srgb_to_linear(srgb);
    std::memcpy(buffer + offset, &v, sizeof(v));
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
    if (type_name == "SubEmitter")       return std::make_unique<SubEmitterModule>();
    return nullptr;
}

// ─── Pin descriptors ────────────────────────────────────────────────────

std::vector<PinDescriptor> SpawnRateModule::pin_descriptors() {
    return {{.name = "rate", .data_type = PinDataType::float_type, .value_ptr = &rate, .range_min = 0.0f, .range_max = 10000.0f,
             .drag_speed = 1.0f, .help = "Particles a second.", .format = "%.0f"}};
}

std::vector<PinDescriptor> InitialLifetimeModule::pin_descriptors() {
    return {
        {.name = "min", .data_type = PinDataType::float_type, .value_ptr = &lifetime_min, .range_min = 0.01f, .range_max = 60.0f,
         .label = "Min", .group = "Lifetime", .unit = PinUnit::seconds},
        {.name = "max", .data_type = PinDataType::float_type, .value_ptr = &lifetime_max, .range_min = 0.01f, .range_max = 60.0f,
         .label = "Max", .group = "Lifetime", .unit = PinUnit::seconds, .at_least = 0},
    };
}

std::vector<PinDescriptor> InitialPositionModule::pin_descriptors() {
    const bool round = shape == SpawnShape::sphere || shape == SpawnShape::cone || shape == SpawnShape::ring ||
                       shape == SpawnShape::disk;
    return {
        {.name = "shape", .data_type = PinDataType::enum_type, .value_ptr = &shape,
         .enum_names = {"Point", "Sphere", "Box", "Cone", "Ring", "Edge", "Disk"}},
        {.name = "radius", .data_type = PinDataType::float_type, .value_ptr = &radius, .range_max = 100.0f, .unit = PinUnit::metres,
         .shown = round},
        {.name = "half_extents", .data_type = PinDataType::vec3_type, .value_ptr = &half_extents, .range_max = 100.0f,
         .unit = PinUnit::metres, .shown = shape == SpawnShape::box},
        {.name = "edge_length", .data_type = PinDataType::float_type, .value_ptr = &edge_length, .range_max = 100.0f, .label = "Length",
         .unit = PinUnit::metres, .shown = shape == SpawnShape::edge},
    };
}

std::vector<PinDescriptor> InitialVelocityModule::pin_descriptors() {
    const bool cone = mode == VelocityMode::cone;
    return {
        {.name = "mode", .data_type = PinDataType::enum_type, .value_ptr = &mode, .enum_names = {"Directional", "Random", "Cone"}},
        {.name = "velocity_min", .data_type = PinDataType::vec3_type, .value_ptr = &velocity_min, .range_min = -100.0f,
         .range_max = 100.0f, .drag_speed = 0.1f, .label = "Min", .unit = PinUnit::metres_per_second, .shown = !cone},
        {.name = "velocity_max", .data_type = PinDataType::vec3_type, .value_ptr = &velocity_max, .range_min = -100.0f,
         .range_max = 100.0f, .drag_speed = 0.1f, .label = "Max", .unit = PinUnit::metres_per_second, .shown = !cone},
        {.name = "direction", .data_type = PinDataType::vec3_type, .value_ptr = &direction, .range_min = -1.0f, .range_max = 1.0f,
         .shown = cone},
        {.name = "angle", .data_type = PinDataType::float_type, .value_ptr = &angle, .range_max = 90.0f, .drag_speed = 0.5f,
         .help = "Half the cone's opening.", .unit = PinUnit::degrees, .format = "%.1f", .shown = cone},
        {.name = "speed_min", .data_type = PinDataType::float_type, .value_ptr = &speed_min, .range_max = 100.0f, .drag_speed = 0.1f,
         .label = "Min", .group = "Speed", .unit = PinUnit::metres_per_second, .shown = cone},
        {.name = "speed_max", .data_type = PinDataType::float_type, .value_ptr = &speed_max, .range_max = 100.0f, .drag_speed = 0.1f,
         .label = "Max", .group = "Speed", .unit = PinUnit::metres_per_second, .at_least = 5, .shown = cone},
    };
}

std::vector<PinDescriptor> InitialColorModule::pin_descriptors() {
    return {{.name = "color", .data_type = PinDataType::vec4_type, .value_ptr = &color, .widget = PinWidget::colour}};
}

std::vector<PinDescriptor> InitialSizeModule::pin_descriptors() {
    return {
        {.name = "min", .data_type = PinDataType::float_type, .value_ptr = &size_min, .range_min = 0.001f, .range_max = 10.0f,
         .drag_speed = 0.001f, .label = "Min", .group = "Size", .unit = PinUnit::metres, .format = "%.3f"},
        {.name = "max", .data_type = PinDataType::float_type, .value_ptr = &size_max, .range_min = 0.001f, .range_max = 10.0f,
         .drag_speed = 0.001f, .label = "Max", .group = "Size", .unit = PinUnit::metres, .format = "%.3f",
         .at_least = 0},
    };
}

std::vector<PinDescriptor> GravityModule::pin_descriptors() {
    return {{.name = "force", .data_type = PinDataType::vec3_type, .value_ptr = &force, .range_min = -1000.0f, .range_max = 1000.0f,
             .drag_speed = 0.1f, .help = "Acceleration, in metres a second squared."}};
}

std::vector<PinDescriptor> ColorOverLifeModule::pin_descriptors() {
    return {
        {.name = "start", .data_type = PinDataType::vec4_type, .value_ptr = &color_start, .widget = PinWidget::colour},
        {.name = "end", .data_type = PinDataType::vec4_type, .value_ptr = &color_end, .widget = PinWidget::colour},
    };
}

std::vector<PinDescriptor> SizeOverLifeModule::pin_descriptors() {
    return {
        {.name = "start", .data_type = PinDataType::float_type, .value_ptr = &size_start, .range_min = 0.001f, .range_max = 10.0f,
         .drag_speed = 0.001f, .unit = PinUnit::metres, .format = "%.3f"},
        {.name = "end", .data_type = PinDataType::float_type, .value_ptr = &size_end, .range_min = 0.001f, .range_max = 10.0f,
         .drag_speed = 0.001f, .unit = PinUnit::metres, .format = "%.3f"},
    };
}

std::vector<PinDescriptor> DragModule::pin_descriptors() {
    return {{.name = "drag", .data_type = PinDataType::float_type, .value_ptr = &drag, .range_max = 10.0f,
             .help = "How fast a particle loses speed."}};
}

std::vector<PinDescriptor> NoiseModule::pin_descriptors() {
    return {
        {.name = "strength", .data_type = PinDataType::float_type, .value_ptr = &strength, .range_max = 100.0f, .drag_speed = 0.1f},
        {.name = "frequency", .data_type = PinDataType::float_type, .value_ptr = &frequency, .range_min = 0.01f, .range_max = 50.0f,
         .drag_speed = 0.1f, .help = "How small the noise's swirls are: higher is finer."},
        {.name = "scroll_speed", .data_type = PinDataType::float_type, .value_ptr = &scroll_speed, .range_max = 10.0f, .drag_speed = 0.1f},
    };
}

std::vector<PinDescriptor> AttractModule::pin_descriptors() {
    return {
        {.name = "position", .data_type = PinDataType::vec3_type, .value_ptr = &position, .range_min = -1000.0f, .range_max = 1000.0f,
         .drag_speed = 0.1f, .unit = PinUnit::metres},
        {.name = "strength", .data_type = PinDataType::float_type, .value_ptr = &strength, .range_max = 100.0f, .drag_speed = 0.1f},
        {.name = "radius", .data_type = PinDataType::float_type, .value_ptr = &radius, .range_min = 0.01f, .range_max = 1000.0f,
         .drag_speed = 0.1f, .help = "How far the pull reaches.", .unit = PinUnit::metres},
    };
}

std::vector<PinDescriptor> VelocityLimitModule::pin_descriptors() {
    return {{.name = "max_speed", .data_type = PinDataType::float_type, .value_ptr = &max_speed, .range_max = 1000.0f, .drag_speed = 0.1f,
             .unit = PinUnit::metres_per_second}};
}

std::vector<PinDescriptor> CollisionModule::pin_descriptors() {
    const bool bounces = response != Response::kill;
    return {
        {.name = "response", .data_type = PinDataType::enum_type, .value_ptr = &response, .enum_names = {"Bounce", "Slide", "Kill"}},
        {.name = "source", .data_type = PinDataType::enum_type, .value_ptr = &source,
         .enum_names = {"Static geometry", "Depth buffer", "Both", "Shelter above"}, .label = "Against",
         .help = "Static geometry is the scene's baked distance field; the depth buffer also sees what moves or "
                 "is displaced, but only on screen."},
        {.name = "restitution", .data_type = PinDataType::float_type, .value_ptr = &restitution, .range_max = 1.0f,
         .help = "How much of its speed a particle keeps off a bounce.", .widget = PinWidget::slider,
         .shown = bounces},
        {.name = "friction", .data_type = PinDataType::float_type, .value_ptr = &friction, .range_max = 1.0f, .widget = PinWidget::slider,
         .shown = bounces},
        {.name = "radius_scale", .data_type = PinDataType::float_type, .value_ptr = &radius_scale, .range_max = 2.0f, .label = "Radius",
         .help = "The particle's collision radius, as a share of its size.", .unit = PinUnit::times,
         .widget = PinWidget::slider},
    };
}

std::vector<PinDescriptor> InitialStretchModule::pin_descriptors() {
    return {{.name = "stretch", .data_type = PinDataType::float_type, .value_ptr = &stretch, .range_max = 100.0f, .drag_speed = 0.1f,
             .help = "0 draws a square; above it the particle streaks along its velocity.", .format = "%.1f"}};
}

std::vector<PinDescriptor> InitialTextureModule::pin_descriptors() {
    // Only the texture, an asset: nothing to link.
    return {};
}

std::vector<PinDescriptor> InitialRotationModule::pin_descriptors() {
    return {
        {.name = "rotation_min", .data_type = PinDataType::float_type, .value_ptr = &rotation_min, .range_min = -360.0f,
         .range_max = 360.0f, .drag_speed = 1.0f, .label = "Min", .group = "Rotation", .unit = PinUnit::degrees,
         .radians = true, .format = "%.0f"},
        {.name = "rotation_max", .data_type = PinDataType::float_type, .value_ptr = &rotation_max, .range_min = -360.0f,
         .range_max = 360.0f, .drag_speed = 1.0f, .label = "Max", .group = "Rotation", .unit = PinUnit::degrees,
         .radians = true, .format = "%.0f"},
        {.name = "speed", .data_type = PinDataType::float_type, .value_ptr = &speed, .range_min = -3600.0f, .range_max = 3600.0f,
         .drag_speed = 1.0f, .label = "Spin", .help = "Degrees a second.", .unit = PinUnit::degrees, .radians = true,
         .format = "%.0f"},
        {.name = "speed_variance", .data_type = PinDataType::float_type, .value_ptr = &speed_variance, .range_max = 3600.0f,
         .drag_speed = 1.0f, .label = "Spin variance",
         .help = "How far each particle's spin differs, either way, in degrees a second.", .unit = PinUnit::degrees,
         .radians = true, .format = "%.0f"},
    };
}

std::vector<PinDescriptor> BurstModule::pin_descriptors() {
    return {{.name = "count", .data_type = PinDataType::uint_type, .value_ptr = &count, .range_min = 1.0f, .range_max = 100000.0f,
             .drag_speed = 1.0f}};
}

std::vector<PinDescriptor> FlipbookModule::pin_descriptors() {
    return {
        {.name = "columns", .data_type = PinDataType::uint_type, .value_ptr = &columns, .range_min = 1.0f, .range_max = 64.0f,
         .drag_speed = 0.1f},
        {.name = "rows", .data_type = PinDataType::uint_type, .value_ptr = &rows, .range_min = 1.0f, .range_max = 64.0f, .drag_speed = 0.1f},
        {.name = "frame_count", .data_type = PinDataType::uint_type, .value_ptr = &frame_count, .range_max = 255.0f, .drag_speed = 0.1f,
         .label = "Frames", .help = "0 plays every cell of the grid."},
        {.name = "fps", .data_type = PinDataType::float_type, .value_ptr = &fps, .range_min = 0.25f, .range_max = 63.75f, .drag_speed = 0.5f,
         .label = "Rate", .help = "Frames a second.", .shown = mode != Mode::over_lifetime},
        {.name = "mode", .data_type = PinDataType::enum_type, .value_ptr = &mode,
         .enum_names = {"Over lifetime", "At a rate, looping", "At a rate, once"}},
    };
}

// BillboardModule has no parameters — uses default empty pin_descriptors()

// ─── SpawnRate ───────────────────────────────────────────────────────────

uint32_t SpawnRateModule::write_gpu_data(uint8_t* buffer) const {
    uint32_t total = write_header(buffer, type(), 1);
    write_float(buffer, 8, rate);
    return total;
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
    write_color(buffer, 8, color);
    return total;
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
        {.name = "direction", .data_type = PinDataType::vec3_type, .value_ptr = &direction, .range_min = -1.0f, .range_max = 1.0f,
         .disabled = use_global},
        {.name = "strength", .data_type = PinDataType::float_type, .value_ptr = &strength, .range_max = 100.0f, .drag_speed = 0.1f},
    };
}

uint32_t WindModule::write_gpu_data(uint8_t* buffer) const {
    // header(2) + direction(3) + strength(1) + use_global(1) = 7 words
    uint32_t total = write_header(buffer, type(), 7);
    write_vec3(buffer, 8, direction);
    write_float(buffer, 20, strength);
    write_uint(buffer, 24, use_global ? 1u : 0u);
    return total;
}

void WindModule::to_json(nlohmann::json& j) const {
    j["type"] = "Wind";
    j["direction"] = {direction.x, direction.y, direction.z};
    j["strength"] = strength;
    if (use_global) j["use_global"] = true;
}

void WindModule::from_json(const nlohmann::json& j) {
    if (j.contains("direction")) {
        auto& d = j["direction"];
        direction = {d[0].get<float>(), d[1].get<float>(), d[2].get<float>()};
    }
    strength = j.value("strength", 1.0f);
    use_global = j.value("use_global", false);
}

// ─── Orbital ────────────────────────────────────────────────────────────

std::vector<PinDescriptor> OrbitalModule::pin_descriptors() {
    return {
        {.name = "mode", .data_type = PinDataType::enum_type, .value_ptr = &mode, .enum_names = {"Vortex", "Orbit", "Swirl"}},
        {.name = "axis", .data_type = PinDataType::vec3_type, .value_ptr = &axis, .range_min = -1.0f, .range_max = 1.0f,
         .label = mode == OrbitalMode::orbit ? "Center" : "Axis"},
        {.name = "speed", .data_type = PinDataType::float_type, .value_ptr = &speed, .range_min = -20.0f, .range_max = 20.0f,
         .drag_speed = 0.1f, .help = "Turn rate, in radians a second."},
        {.name = "pull", .data_type = PinDataType::float_type, .value_ptr = &pull, .range_min = -10.0f, .range_max = 10.0f,
         .drag_speed = 0.1f, .help = "How hard the swirl draws particles in.", .shown = mode == OrbitalMode::swirl},
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
    write_color(buffer, 8, color_start);
    write_color(buffer, 24, color_end);
    return total;
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
    //        + sdf_min(3) + radius_scale(1) + sdf_max(3) + pad(1) = 12 words
    uint32_t total = write_header(buffer, type(), 12);
    write_uint(buffer, 8, static_cast<uint32_t>(response));
    write_uint(buffer, 12, static_cast<uint32_t>(source));
    write_float(buffer, 16, restitution);
    write_float(buffer, 20, friction);
    write_vec3(buffer, 24, sdf_volume_min);
    write_float(buffer, 36, radius_scale);
    write_vec3(buffer, 40, sdf_volume_max);
    write_float(buffer, 52, 0.0f);
    return total;
}

void CollisionModule::to_json(nlohmann::json& j) const {
    j["type"] = "Collision";
    const char* mode_names[] = {"bounce", "slide", "kill"};
    const char* src_names[] = {"sdf", "depth", "both", "overhead"};
    j["response"] = mode_names[static_cast<int>(response)];
    j["source"] = src_names[static_cast<int>(source)];
    j["restitution"] = restitution;
    j["friction"] = friction;
    j["radius_scale"] = radius_scale;
}

void CollisionModule::from_json(const nlohmann::json& j) {
    std::string r = j.value("response", "bounce");
    if (r == "slide") response = Response::slide;
    else if (r == "kill") response = Response::kill;
    else response = Response::bounce;
    std::string s = j.value("source", "sdf");
    if (s == "depth") source = Source::depth;
    else if (s == "both") source = Source::both;
    else if (s == "overhead") source = Source::overhead;
    else source = Source::sdf;
    restitution = j.value("restitution", 0.5f);
    friction = j.value("friction", 0.2f);
    radius_scale = j.value("radius_scale", 0.5f);
}

// ─── InitialStretch ──────────────────────────────────────────────────────

uint32_t InitialStretchModule::write_gpu_data(uint8_t* buffer) const {
    uint32_t total = write_header(buffer, type(), 2);
    write_float(buffer, 8, stretch);
    write_float(buffer, 12, 0.0f); // pad
    return total;
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

// ─── Asset field extension sets ──────────────────────────────────────────

std::span<const std::string> VfxModule::texture_extensions() {
    static const std::string exts[] = {".png", ".jpg", ".jpeg", ".tga"};
    return exts;
}

std::span<const std::string> VfxModule::material_extensions() {
    static const std::string exts[] = {".fjmat"};
    return exts;
}

std::span<const std::string> VfxModule::mesh_extensions() {
    static const std::string exts[] = {".fjmesh"};
    return exts;
}

std::vector<VfxModule::AssetField> InitialTextureModule::asset_fields() {
    return {{"Texture", &texture_path, texture_extensions()}};
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

void BurstModule::to_json(nlohmann::json& j) const {
    j["type"] = "Burst";
    j["count"] = count;
}

void BurstModule::from_json(const nlohmann::json& j) {
    count = j.value("count", 100u);
}

// ─── EmitOverDistance ────────────────────────────────────────────────────

std::vector<PinDescriptor> EmitOverDistanceModule::pin_descriptors() {
    return {{.name = "particles_per_meter", .data_type = PinDataType::float_type, .value_ptr = &particles_per_meter, .range_min = 0.1f,
             .range_max = 200.0f, .drag_speed = 0.5f, .label = "Per metre",
             .help = "Particles for each metre the emitter moves.", .format = "%.1f"}};
}

uint32_t EmitOverDistanceModule::write_gpu_data(uint8_t* /*buffer*/) const {
    return 0; // CPU-only module, no GPU data
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

void BillboardModule::to_json(nlohmann::json& j) const {
    j["type"] = "Billboard";
}

void BillboardModule::from_json(const nlohmann::json& /*j*/) {
    // no params
}

// ─── VelocityInheritance ────────────────────────────────────────────────

std::vector<PinDescriptor> VelocityInheritanceModule::pin_descriptors() {
    return {{.name = "factor", .data_type = PinDataType::float_type, .value_ptr = &factor, .range_max = 1.0f,
             .help = "How much of the emitter's own velocity a new particle starts with.", .unit = PinUnit::times,
             .widget = PinWidget::slider}};
}

uint32_t VelocityInheritanceModule::write_gpu_data(uint8_t* buffer) const {
    uint32_t total = write_header(buffer, type(), 2);
    write_float(buffer, 8, factor);
    write_float(buffer, 12, 0.0f); // pad
    return total;
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
    // The curves are authored in sRGB and sampled there, so a bake is the
    // designer's ramp; each sample then converts like any other colour.
    for (const Curve* channel : {&curve_r, &curve_g, &curve_b}) {
        channel->bake(lut, kCurveLutSamples);
        for (uint32_t i = 0; i < kCurveLutSamples; ++i) {
            write_float(buffer, off + i * 4, color_space::srgb_to_linear(lut[i]));
        }
        off += kCurveLutSamples * 4;
    }
    curve_a.bake(lut, kCurveLutSamples);
    for (uint32_t i = 0; i < kCurveLutSamples; ++i) write_float(buffer, off + i * 4, lut[i]);
    return total;
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

void CurveSizeOverLifeModule::to_json(nlohmann::json& j) const {
    j["type"] = "CurveSizeOverLife";
    curve.to_json(j["curve"]);
}

void CurveSizeOverLifeModule::from_json(const nlohmann::json& j) {
    if (j.contains("curve")) curve.from_json(j["curve"]);
}

// ─── MeshParticle ───────────────────────────────────────────────────────

std::vector<PinDescriptor> MeshParticleModule::pin_descriptors() {
    return {{.name = "shape", .data_type = PinDataType::enum_type, .value_ptr = &shape, .enum_names = {"Cube", "Sphere", "Custom"}}};
}

uint32_t MeshParticleModule::write_gpu_data(uint8_t* /*buffer*/) const {
    return 0; // render module — no GPU module data
}

std::vector<VfxModule::AssetField> MeshParticleModule::asset_fields() {
    std::vector<AssetField> fields;
    if (shape == ParticleMeshShape::custom) {
        fields.push_back({"Model", &model_path, mesh_extensions()});
    }
    // Mesh particles draw through the mesh pipelines, which only a spatial
    // shader has.
    fields.push_back({"Material", &material_path, material_extensions(),
                      only_shader_type(ShaderType::Spatial)});
    return fields;
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
        {.name = "metallic", .data_type = PinDataType::float_type, .value_ptr = &metallic, .range_max = 1.0f, .widget = PinWidget::slider},
        {.name = "roughness", .data_type = PinDataType::float_type, .value_ptr = &roughness, .range_min = 0.04f, .range_max = 1.0f,
         .widget = PinWidget::slider},
    };
}

uint32_t LitParticleModule::write_gpu_data(uint8_t* /*buffer*/) const {
    return 0; // render module — no GPU module data
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
    return {{.name = "strength", .data_type = PinDataType::float_type, .value_ptr = &strength, .range_max = 1.0f, .drag_speed = 0.001f,
             .format = "%.3f"}};
}

uint32_t DistortionModule::write_gpu_data(uint8_t* buffer) const {
    // strength(1) + normal_map_index(1) = 2 words
    uint32_t total = write_header(buffer, type(), 2);
    write_float(buffer, 8, strength);
    write_uint(buffer, 12, normal_map_index);
    return total;
}

std::vector<VfxModule::AssetField> DistortionModule::asset_fields() {
    return {{"Normal Map", &normal_map_path, texture_extensions()}};
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
        {.name = "color", .data_type = PinDataType::vec3_type, .value_ptr = &color, .widget = PinWidget::colour},
        {.name = "intensity", .data_type = PinDataType::float_type, .value_ptr = &intensity, .range_max = 50.0f, .drag_speed = 0.1f},
    };
}

uint32_t EmissiveModule::write_gpu_data(uint8_t* buffer) const {
    // header(2) + color(3) + intensity(1) + pad(2) = 8 words (4-word aligned)
    uint32_t total = write_header(buffer, type(), 6);
    write_color(buffer, 8, color);
    write_float(buffer, 20, intensity);
    return total; // 24 bytes
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

// ─── SubEmitter ─────────────────────────────────────────────────────────

std::vector<PinDescriptor> SubEmitterModule::pin_descriptors() {
    return {
        {.name = "trigger", .data_type = PinDataType::enum_type, .value_ptr = &trigger, .enum_names = {"Death", "Collision", "Both"}},
        {.name = "child_emitter_index", .data_type = PinDataType::uint_type, .value_ptr = &child_emitter_index, .range_max = 255.0f,
         .drag_speed = 0.1f, .label = "Child emitter", .help = "The emitter, by its place in the effect, that is fired."},
        {.name = "burst_count", .data_type = PinDataType::uint_type, .value_ptr = &burst_count, .range_min = 1.0f, .range_max = 10000.0f,
         .drag_speed = 0.2f, .label = "Burst", .help = "Particles fired each time."},
        {.name = "velocity_inherit", .data_type = PinDataType::float_type, .value_ptr = &velocity_inherit, .range_max = 1.0f,
         .unit = PinUnit::times, .widget = PinWidget::slider},
    };
}

uint32_t SubEmitterModule::write_gpu_data(uint8_t* buffer) const {
    // trigger(1) + child_emitter(1) + burst_count(1) + vel_inherit(1) = 4 words
    uint32_t total = write_header(buffer, type(), 4);
    write_uint(buffer, 8, static_cast<uint32_t>(trigger));
    write_uint(buffer, 12, child_emitter_index);
    write_uint(buffer, 16, burst_count);
    write_float(buffer, 20, velocity_inherit);
    return total;
}

void SubEmitterModule::to_json(nlohmann::json& j) const {
    j["type"] = "SubEmitter";
    const char* names[] = {"death", "collision", "both"};
    j["trigger"] = names[static_cast<int>(trigger)];
    j["child_emitter_index"] = child_emitter_index;
    j["burst_count"] = burst_count;
    j["velocity_inherit"] = velocity_inherit;
}

void SubEmitterModule::from_json(const nlohmann::json& j) {
    auto t = j.value("trigger", "death");
    if (t == "collision") trigger = SubEmitterTrigger::collision;
    else if (t == "both") trigger = SubEmitterTrigger::both;
    else trigger = SubEmitterTrigger::death;

    child_emitter_index = j.value("child_emitter_index", 1u);
    burst_count = j.value("burst_count", 5u);
    velocity_inherit = j.value("velocity_inherit", 0.5f);
}

// ─── Trail ──────────────────────────────────────────────────────────────

uint32_t TrailModule::write_gpu_data(uint8_t* /*buffer*/) const {
    // Trail has no GPU module data — it's handled via emitter flags and the trail pipeline
    return 0;
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
