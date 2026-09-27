#pragma once

#include <cmath>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

// 1 Source unit is about one inch. World space is meters.
inline constexpr float kSourceToWorld = 1.0F / 52.0F;

inline glm::vec3 SourceDirectionToWorld(float sourceX, float sourceY, float sourceZ) {
    return glm::vec3(sourceX, sourceZ, -sourceY);
}

inline glm::vec3 SourcePointToWorld(float sourceX, float sourceY, float sourceZ) {
    return SourceDirectionToWorld(sourceX, sourceY, sourceZ) * kSourceToWorld;
}

inline void SourceAngleBasis(
    float pitchDegrees,
    float yawDegrees,
    float rollDegrees,
    glm::vec3& forward,
    glm::vec3& right,
    glm::vec3& up
) {
    const float pitch = pitchDegrees * (3.14159265F / 180.0F);
    const float yaw = yawDegrees * (3.14159265F / 180.0F);
    const float roll = rollDegrees * (3.14159265F / 180.0F);
    const float sinPitch = std::sin(pitch);
    const float cosPitch = std::cos(pitch);
    const float sinYaw = std::sin(yaw);
    const float cosYaw = std::cos(yaw);
    const float sinRoll = std::sin(roll);
    const float cosRoll = std::cos(roll);
    const float sourceForwardX = cosPitch * cosYaw;
    const float sourceForwardY = cosPitch * sinYaw;
    const float sourceForwardZ = -sinPitch;
    const float sourceRightX = -sinRoll * sinPitch * cosYaw + cosRoll * sinYaw;
    const float sourceRightY = -sinRoll * sinPitch * sinYaw - cosRoll * cosYaw;
    const float sourceRightZ = -sinRoll * cosPitch;
    const float sourceUpX = cosRoll * sinPitch * cosYaw + sinRoll * sinYaw;
    const float sourceUpY = cosRoll * sinPitch * sinYaw - sinRoll * cosYaw;
    const float sourceUpZ = cosRoll * cosPitch;
    auto normalizeOr = [](const glm::vec3& direction, const glm::vec3& fallback) {
        const float length = glm::length(direction);
        if (length <= 1.0e-6F) {
            return fallback;
        }
        return direction / length;
    };
    forward = normalizeOr(SourceDirectionToWorld(sourceForwardX, sourceForwardY, sourceForwardZ), glm::vec3(1.0F, 0.0F, 0.0F));
    right = normalizeOr(SourceDirectionToWorld(sourceRightX, sourceRightY, sourceRightZ), glm::vec3(0.0F, 0.0F, 1.0F));
    up = normalizeOr(SourceDirectionToWorld(sourceUpX, sourceUpY, sourceUpZ), glm::vec3(0.0F, 1.0F, 0.0F));
}

// Columns map converted model axes (source X, source Z, source -Y) into world space.
inline glm::mat4 SourceAnglesToWorldMatrix(float pitchDegrees, float yawDegrees, float rollDegrees) {
    glm::vec3 forward;
    glm::vec3 right;
    glm::vec3 up;
    SourceAngleBasis(pitchDegrees, yawDegrees, rollDegrees, forward, right, up);
    glm::mat4 rotation(1.0F);
    rotation[0] = glm::vec4(forward, 0.0F);
    rotation[1] = glm::vec4(up, 0.0F);
    rotation[2] = glm::vec4(right, 0.0F);
    return rotation;
}
