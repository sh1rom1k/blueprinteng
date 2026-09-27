#include "ui/MenuBackground.hpp"

#include "BspLoader.hpp"
#include "SourceCoords.hpp"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>

namespace ui {
namespace {

std::string Lower(std::string value) {
    for (char& character : value) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    return value;
}

const std::string& Property(const BspMapEntity& entity, const char* key) {
    static const std::string empty;
    const std::string wanted = Lower(key);
    for (const auto& property : entity.properties) {
        if (Lower(property.first) == wanted) {
            return property.second;
        }
    }
    return empty;
}

const BspMapEntity* FindMenuCamera(const BspLoader& map) {
    const BspMapEntity* viewControl = nullptr;
    const BspMapEntity* pointCamera = nullptr;
    const BspMapEntity* playerStart = nullptr;
    for (const BspMapEntity& entity : map.MapEntities()) {
        const std::string classname = Lower(entity.classname);
        if (viewControl == nullptr && classname == "point_viewcontrol") {
            viewControl = &entity;
        } else if (pointCamera == nullptr && classname == "point_camera") {
            pointCamera = &entity;
        } else if (playerStart == nullptr
            && (classname == "info_player_start" || classname == "info_player_deathmatch")) {
            playerStart = &entity;
        }
    }
    if (viewControl != nullptr) {
        return viewControl;
    }
    if (pointCamera != nullptr) {
        return pointCamera;
    }
    return playerStart;
}

float CameraFov(const BspMapEntity& entity) {
    float fov = 75.0F;
    const std::string& fovText = Property(entity, "fov");
    if (!fovText.empty()) {
        try {
            fov = std::stof(fovText);
        } catch (const std::exception&) {
            fov = 75.0F;
        }
    }
    return std::clamp(fov, 10.0F, 140.0F);
}

MenuCameraFrame FrameFromBasis(
    const glm::vec3& origin,
    const glm::vec3& forward,
    const glm::vec3& right,
    const glm::vec3& up,
    float fovDegrees,
    float timeSeconds
) {
    constexpr float kPi = 3.14159265F;
    const float sway = std::sin(timeSeconds * (2.0F * kPi / 10.0F));
    const float swayUp = std::sin(timeSeconds * (2.0F * kPi / 10.0F) * 0.73F + 1.2F);
    const float yaw = std::sin(timeSeconds * 0.45F) * (1.2F * kPi / 180.0F);
    const float pitch = std::cos(timeSeconds * 0.37F) * (0.8F * kPi / 180.0F);

    MenuCameraFrame frame;
    frame.eye = origin + right * (sway * 0.12F) + up * (swayUp * 0.06F);
    const glm::vec3 look = forward + right * std::sin(yaw) + up * std::sin(pitch);
    const float lookLength = glm::length(look);
    frame.target = frame.eye + (lookLength > 1.0e-4F ? look / lookLength : forward);
    frame.up = up;
    frame.fovDegrees = fovDegrees;
    return frame;
}

} // namespace

bool IsBackgroundMapFile(const std::filesystem::path& path) {
    const std::string stem = Lower(path.stem().string());
    constexpr std::string_view kPrefix = "background";
    if (stem.size() <= kPrefix.size() || stem.compare(0, kPrefix.size(), kPrefix) != 0) {
        return false;
    }
    for (std::size_t index = kPrefix.size(); index < stem.size(); ++index) {
        if (std::isdigit(static_cast<unsigned char>(stem[index])) == 0) {
            return false;
        }
    }
    return true;
}

MenuCameraFrame BuildMenuCameraFrame(const BspLoader& map, float timeSeconds) {
    if (const BspMapEntity* entity = FindMenuCamera(map)) {
        float pitch = 0.0F;
        float yaw = 0.0F;
        float roll = 0.0F;
        const std::string& angles = Property(*entity, "angles");
        if (!angles.empty()) {
            std::sscanf(angles.c_str(), "%f %f %f", &pitch, &yaw, &roll);
        }
        glm::vec3 forward;
        glm::vec3 right;
        glm::vec3 up;
        SourceAngleBasis(pitch, yaw, roll, forward, right, up);
        glm::vec3 origin = entity->position;
        const std::string classname = Lower(entity->classname);
        if (classname == "info_player_start" || classname == "info_player_deathmatch") {
            origin.y += 64.0F * kSourceToWorld;
        }
        return FrameFromBasis(origin, forward, right, up, CameraFov(*entity), timeSeconds);
    }

    const glm::vec3 minimum = map.WorldMinimum();
    const glm::vec3 maximum = map.WorldMaximum();
    const glm::vec3 center = (minimum + maximum) * 0.5F;
    const glm::vec3 extent = maximum - minimum;
    const float radius = std::max(glm::length(extent) * 0.35F, 2.0F);
    const float orbit = timeSeconds * 0.12F;
    MenuCameraFrame frame;
    frame.eye = center + glm::vec3(std::cos(orbit) * radius, extent.y * 0.15F + 1.5F, std::sin(orbit) * radius);
    frame.target = center;
    frame.up = glm::vec3(0.0F, 1.0F, 0.0F);
    frame.fovDegrees = 75.0F;
    return frame;
}

} // namespace ui
