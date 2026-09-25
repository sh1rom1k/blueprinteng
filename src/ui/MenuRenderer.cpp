#include "ui/MenuRenderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ui {
namespace {

constexpr float kMenuLeft = 72.0F;
constexpr float kMenuTop = 220.0F;
constexpr float kLineHeight = 44.0F;
constexpr float kTitleFont = 34.0F;
constexpr float kItemFont = 26.0F;

void DrawVignette(IUIRenderBackend& backend, const Vec2& size) {
    const Color top{0.0F, 0.0F, 0.0F, 0.45F};
    const Color mid{0.0F, 0.0F, 0.0F, 0.08F};
    backend.DrawFilledRect({0.0F, 0.0F}, {size.x, size.y * 0.22F}, top);
    backend.DrawFilledRect({0.0F, size.y * 0.78F}, {size.x, size.y}, top);
    backend.DrawFilledRect({0.0F, 0.0F}, {size.x * 0.18F, size.y}, mid);
}

} // namespace

void MenuRenderer::RenderMainList(
    IUIRenderBackend& backend,
    const MainMenu& menu,
    float timeSeconds,
    const Vec2& origin,
    float lineHeight
) const {
    const int itemCount = static_cast<int>(MainMenuItem::Count);
    for (int i = 0; i < itemCount; ++i) {
        const bool highlighted = i == menu.HighlightedIndex();
        const bool active = i == menu.ActiveIndex();
        Color color = Color::Hl2Default();
        bool glow = false;

        if (highlighted) {
            color = Color::Hl2Hover();
            glow = true;
            const float pulse = 0.5F + 0.5F * std::sin(timeSeconds * 4.5F);
            color.a = 0.85F + 0.15F * pulse;
        }
        if (active) {
            color = Color::Hl2Active();
            glow = true;
        }

        const Vec2 pos{origin.x, origin.y + static_cast<float>(i) * lineHeight};
        backend.DrawText(pos, kMainMenuItems[i].label, kItemFont, color, TextAlign::Left, glow);
    }
}

void MenuRenderer::RenderOptionsPanel(
    IUIRenderBackend& backend,
    const MainMenu& menu,
    float /*timeSeconds*/,
    std::span<const std::string> mapLabels
) const {
    const Vec2 display = backend.DisplaySize();
    const float panelW = 520.0F;
    const float panelH = 380.0F;
    const Vec2 panelMin{(display.x - panelW) * 0.5F, (display.y - panelH) * 0.5F};
    const Vec2 panelMax{panelMin.x + panelW, panelMin.y + panelH};

    backend.DrawFilledRect(panelMin, panelMax, Color::Hl2PanelFill());
    backend.DrawRectBorder(panelMin, panelMax, Color::Hl2PanelBorder(), 2.0F);
    backend.DrawRectBorder(
        {panelMin.x + 6.0F, panelMin.y + 6.0F},
        {panelMax.x - 6.0F, panelMax.y - 6.0F},
        {0.35F, 0.37F, 0.40F, 0.55F},
        1.0F
    );

    const OptionsSettings& opts = menu.Options();
    const int selectedField = menu.OptionsFieldIndex();

    auto drawOptionLine = [&](int fieldIndex, float y, const char* text) {
        const bool selected = fieldIndex == selectedField;
        Color color = selected ? Color::Hl2Hover() : Color::Hl2Default();
        backend.DrawText({panelMin.x + 28.0F, y}, text, 20.0F, color, TextAlign::Left, selected);
    };

    backend.DrawText(
        {panelMin.x + 24.0F, panelMin.y + 20.0F},
        "OPTIONS",
        24.0F,
        Color::Hl2Hover(),
        TextAlign::Left,
        true
    );

    char lineBuffer[256];
    std::snprintf(
        lineBuffer,
        sizeof(lineBuffer),
        "FULLSCREEN: %s",
        opts.fullscreen ? "ON" : "OFF"
    );
    drawOptionLine(0, panelMin.y + 72.0F, lineBuffer);

    std::snprintf(
        lineBuffer,
        sizeof(lineBuffer),
        "MOUSE SENSITIVITY: %.2f",
        opts.mouseSensitivity
    );
    drawOptionLine(1, panelMin.y + 112.0F, lineBuffer);

    std::snprintf(
        lineBuffer,
        sizeof(lineBuffer),
        "MASTER VOLUME: %.0f%%",
        opts.masterVolume * 100.0F
    );
    drawOptionLine(2, panelMin.y + 152.0F, lineBuffer);

    std::snprintf(
        lineBuffer,
        sizeof(lineBuffer),
        "SFX VOLUME: %.0f%%",
        opts.sfxVolume * 100.0F
    );
    drawOptionLine(3, panelMin.y + 192.0F, lineBuffer);

    const std::string mapLabel = mapLabels.empty()
        ? "DEMO CUBE"
        : mapLabels[static_cast<std::size_t>(
              std::clamp(opts.selectedMapIndex, 0, static_cast<int>(mapLabels.size()) - 1)
          )];
    std::snprintf(lineBuffer, sizeof(lineBuffer), "MAP: %s", mapLabel.c_str());
    drawOptionLine(4, panelMin.y + 232.0F, lineBuffer);

    backend.DrawText(
        {panelMin.x + 28.0F, panelMax.y - 56.0F},
        "ENTER — APPLY    ESC — BACK",
        18.0F,
        Color::Hl2Dim()
    );
    backend.DrawText(
        {panelMin.x + 28.0F, panelMax.y - 88.0F},
        "UP/DOWN — FIELD    LEFT/RIGHT — VALUE",
        18.0F,
        Color::Hl2Dim()
    );
}

void MenuRenderer::Render(
    IUIRenderBackend& backend,
    const MainMenu& menu,
    float timeSeconds,
    std::span<const std::string> mapLabels,
    const MenuBackgroundLayer& background
) const {
    const Vec2 display = backend.DisplaySize();

    Color overlay = Color::Hl2Overlay();
    overlay.a = background.overlayAlpha;
    backend.DrawFilledRect({0.0F, 0.0F}, display, overlay);

    if (background.drawVignette) {
        DrawVignette(backend, display);
    }

    backend.DrawText(
        {kMenuLeft, 96.0F},
        "BLUEPRINT ENGINE",
        kTitleFont,
        Color::Hl2Active(),
        TextAlign::Left,
        true
    );
    backend.DrawText(
        {kMenuLeft, 138.0F},
        "SOURCE-STYLE FRONTEND",
        18.0F,
        Color::Hl2Dim()
    );

    RenderMainList(backend, menu, timeSeconds, {kMenuLeft, kMenuTop}, kLineHeight);

    if (menu.IsOptionsOpen()) {
        RenderOptionsPanel(backend, menu, timeSeconds, mapLabels);
    }
}

} // namespace ui
