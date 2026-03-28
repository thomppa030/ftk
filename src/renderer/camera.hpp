#pragma once

#include <glm/glm.hpp>

#include <cstdint>

namespace fjell {

class Input;

enum class EditorCameraMode : uint8_t { perspective, orthographic };

class Camera {
public:
  Camera(glm::vec3 target = {0.0f, 0.0f, 0.0f}, float distance = 10.0f,
         float yaw = -30.0f, float pitch = 25.0f);

  void update(const Input &input, float dt);
  void focus(const glm::vec3 &target, float object_radius = 1.0f);

  [[nodiscard]] glm::mat4 view_matrix() const;
  [[nodiscard]] glm::mat4 projection_matrix(float aspect) const;

  [[nodiscard]] glm::vec3 position() const { return position_; }
  [[nodiscard]] float fov() const { return fov_; }
  [[nodiscard]] float yaw() const { return yaw_; }
  [[nodiscard]] float pitch() const { return pitch_; }

  [[nodiscard]] EditorCameraMode mode() const { return mode_; }
  void set_mode(EditorCameraMode mode) { mode_ = mode; }
  [[nodiscard]] float ortho_size() const { return ortho_size_; }

private:
  void update_position();
  void update_focus(float dt);

  glm::vec3 target_;
  glm::vec3 position_;

  // Focus animation
  bool focusing_{false};
  glm::vec3 focus_from_{0.0f};
  glm::vec3 focus_to_{0.0f};
  float focus_dist_from_{0.0f};
  float focus_dist_to_{0.0f};
  float focus_t_{0.0f};

  float yaw_;
  float pitch_;
  float distance_;

  float move_speed_{1.0f};
  float orbit_sensitivity_{0.3f};
  float pan_sensitivity_{0.02f};
  float zoom_sensitivity_{1.5f};
  float min_distance_{0.5f};
  float max_distance_{500.0f};
  float fov_{45.0f};
  float near_{0.1f};
  float far_{1000.0f};

  EditorCameraMode mode_{EditorCameraMode::perspective};
  float ortho_size_{10.0f};
};

} // namespace fjell
