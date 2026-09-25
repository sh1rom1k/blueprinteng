#include "ui/ValveNetGraph.hpp"

#include <cstdio>
#include <string>

namespace ui {

void ValveNetGraph::Render(IUIRenderBackend& backend, const NetGraphMetrics& metrics) {
    const Vec2 display = backend.DisplaySize();

    // Top-Left cl_showpos / cl_showfps HUD (classic Source Engine)
    if (metrics.inGame) {
        constexpr float kHudX = 14.0F;
        float hudY = 14.0F;
        constexpr float kLineH = 18.0F;

        char buf[256];
        // FPS readout
        Color fpsColor = (metrics.fps >= 55.0F)
            ? Color::ValveGreen()
            : ((metrics.fps >= 30.0F) ? Color::ValveYellow() : Color::ValveRed());

        std::snprintf(buf, sizeof(buf), "fps: %5.1f   var: %4.2f ms", metrics.fps, metrics.frameTimeMs);
        backend.DrawTextEx({kHudX, hudY}, buf, 15.0F, fpsColor, TextAlign::Left, true, FontStyle::Monospace);
        hudY += kLineH;

        // Position
        std::snprintf(buf, sizeof(buf), "pos: %8.2f %8.2f %8.2f", metrics.playerPosX, metrics.playerPosY, metrics.playerPosZ);
        backend.DrawTextEx({kHudX, hudY}, buf, 15.0F, Color::White(), TextAlign::Left, true, FontStyle::Monospace);
        hudY += kLineH;

        // Angle & State
        std::snprintf(
            buf,
            sizeof(buf),
            "ang: yaw: %5.1f  pitch: %5.1f   [%s] [%s]",
            metrics.cameraYaw,
            metrics.cameraPitch,
            metrics.grounded ? "GROUND" : "AIR",
            metrics.noclip ? "NOCLIP" : "WALK"
        );
        backend.DrawTextEx({kHudX, hudY}, buf, 15.0F, Color::ValveCyan(), TextAlign::Left, true, FontStyle::Monospace);
    }

    // Bottom-Right NetGraph / System Metrics Slate (Source net_graph 3 style)
    constexpr float kBoxW = 480.0F;
    const float kBoxH = metrics.inGame ? 220.0F : 130.0F;
    const Vec2 boxMin{display.x - kBoxW - 14.0F, display.y - kBoxH - 14.0F};
    const Vec2 boxMax{display.x - 14.0F, display.y - 14.0F};

    // Dark translucent slate background
    backend.DrawFilledRect(boxMin, boxMax, Color{0.08F, 0.09F, 0.10F, 0.85F});
    backend.DrawBevelRect(boxMin, boxMax, Color{0.12F, 0.13F, 0.14F, 0.35F}, Color::ValveBevelDark(), Color{0.30F, 0.34F, 0.36F, 0.8F}, 1.0F);

    // Title line
    const Vec2 titleMin{boxMin.x, boxMin.y};
    const Vec2 titleMax{boxMax.x, boxMin.y + 24.0F};
    backend.DrawFilledRect(titleMin, titleMax, Color{0.14F, 0.16F, 0.18F, 0.95F});
    backend.DrawLine({titleMin.x, titleMax.y}, {titleMax.x, titleMax.y}, Color::ValveBorder(), 1.0F);

    backend.DrawTextEx(
        {boxMin.x + 10.0F, boxMin.y + 4.0F},
        "NET_GRAPH 3 [SOURCE DEV METRICS]",
        14.0F,
        Color::ValveOrange(),
        TextAlign::Left,
        true,
        FontStyle::Monospace
    );

    float curY = boxMin.y + 30.0F;
    constexpr float kRowH = 17.0F;
    char line[256];

    // FPS & Frametime
    Color fpsCol = (metrics.fps >= 55.0F)
        ? Color::ValveGreen()
        : ((metrics.fps >= 30.0F) ? Color::ValveYellow() : Color::ValveRed());
    std::snprintf(line, sizeof(line), "FPS:      %-6.1f (Frame: %-5.2f ms)", metrics.fps, metrics.frameTimeMs);
    backend.DrawTextEx({boxMin.x + 10.0F, curY}, line, 14.0F, fpsCol, TextAlign::Left, true, FontStyle::Monospace);
    curY += kRowH;

    // RAM & VRAM
    if (metrics.vramTotalMb > 0.0F) {
        std::snprintf(line, sizeof(line), "MEM:      RAM: %-5.1f MB  VRAM: %-5.1f / %-5.1f MB", metrics.ramMb, metrics.vramUsedMb, metrics.vramTotalMb);
    } else {
        std::snprintf(line, sizeof(line), "MEM:      RAM: %-5.1f MB  VRAM: Shared/N/A", metrics.ramMb);
    }
    backend.DrawTextEx({boxMin.x + 10.0F, curY}, line, 14.0F, Color{0.80F, 0.84F, 0.88F, 1.0F}, TextAlign::Left, true, FontStyle::Monospace);
    curY += kRowH;

    // GPU & Res
    std::string shortGpu = metrics.gpuName.substr(0, 26);
    std::snprintf(line, sizeof(line), "GPU:      %-26s (%dx%d)", shortGpu.c_str(), metrics.framebufferWidth, metrics.framebufferHeight);
    backend.DrawTextEx({boxMin.x + 10.0F, curY}, line, 14.0F, Color{0.80F, 0.84F, 0.88F, 1.0F}, TextAlign::Left, true, FontStyle::Monospace);
    curY += kRowH;

    if (metrics.inGame) {
        // World / Map
        std::string mapDisplay = metrics.mapName.empty() ? "demo room" : metrics.mapName;
        std::snprintf(line, sizeof(line), "WORLD:    %-16s (Faces: %zu  Tex: %zu)", mapDisplay.c_str(), metrics.bspFaceCount, metrics.bspTextureCount);
        backend.DrawTextEx({boxMin.x + 10.0F, curY}, line, 14.0F, Color::ValveYellow(), TextAlign::Left, true, FontStyle::Monospace);
        curY += kRowH;

        // Lights & Shadows
        std::snprintf(
            line,
            sizeof(line),
            "LIGHTS:   Point: %d/%d   Flashlight: %s   Shadows: %s",
            metrics.pointLightCount,
            metrics.maxPointLights,
            metrics.flashlightEnabled ? "ON" : "OFF",
            metrics.shadowsEnabled ? "ON" : "OFF"
        );
        backend.DrawTextEx({boxMin.x + 10.0F, curY}, line, 14.0F, Color{0.80F, 0.84F, 0.88F, 1.0F}, TextAlign::Left, true, FontStyle::Monospace);
        curY += kRowH;

        // Physics props
        std::snprintf(line, sizeof(line), "PHYSICS:  Jolt Rigidbodies: %zu active props", metrics.propCount);
        backend.DrawTextEx({boxMin.x + 10.0F, curY}, line, 14.0F, Color{0.80F, 0.84F, 0.88F, 1.0F}, TextAlign::Left, true, FontStyle::Monospace);
        curY += kRowH;
    }

    // Key hints
    std::snprintf(line, sizeof(line), "[F3] NetGraph  [F6] Labels  [F] Light  [G] Light  [H] Box");
    backend.DrawTextEx({boxMin.x + 10.0F, boxMax.y - 18.0F}, line, 13.0F, Color{0.55F, 0.58F, 0.60F, 0.9F}, TextAlign::Left, true, FontStyle::Monospace);
}

} // namespace ui
