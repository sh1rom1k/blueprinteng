#pragma once

#include "ui/IUIRenderBackend.hpp"
#include <string>

namespace ui {

struct NetGraphMetrics {
    float fps = 0.0F;
    float frameTimeMs = 0.0F;
    float ramMb = 0.0F;
    float vramUsedMb = 0.0F;
    float vramTotalMb = 0.0F;
    int framebufferWidth = 1280;
    int framebufferHeight = 720;
    std::string gpuName;

    // Gameplay data
    bool inGame = false;
    float playerPosX = 0.0F;
    float playerPosY = 0.0F;
    float playerPosZ = 0.0F;
    float cameraYaw = 0.0F;
    float cameraPitch = 0.0F;
    bool grounded = false;
    bool noclip = false;
    bool flashlightEnabled = false;
    bool shadowsEnabled = false;
    int pointLightCount = 0;
    int maxPointLights = 32;
    std::size_t propCount = 0;
    std::size_t bspFaceCount = 0;
    std::size_t bspTextureCount = 0;
    std::string mapName;
    bool showEntitySpawnLabels = false;
};

class ValveNetGraph {
public:
    void Render(IUIRenderBackend& backend, const NetGraphMetrics& metrics);
};

} // namespace ui
