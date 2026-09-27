#pragma once

#include <filesystem>

#include <glm/vec3.hpp>

class BspLoader;

namespace ui {

struct MenuCameraFrame {
    glm::vec3 eye{0.0F, 2.0F, 6.0F};
    glm::vec3 target{0.0F, 1.0F, 0.0F};
    glm::vec3 up{0.0F, 1.0F, 0.0F};
    float fovDegrees = 75.0F;
};

[[nodiscard]] bool IsBackgroundMapFile(const std::filesystem::path& path);
[[nodiscard]] MenuCameraFrame BuildMenuCameraFrame(const BspLoader& map, float timeSeconds);

} // namespace ui
