#include "ui/LoadingScreen.hpp"

#include <algorithm>
#include <string>

namespace ui {

void LoadingScreen::Render(IUIRenderBackend& backend, std::string_view mapName, float progress) const {
    const Vec2 display = backend.DisplaySize();
    backend.DrawFilledRect({0.0F, 0.0F}, display, Color::Black());

    constexpr float kWidth = 440.0F;
    constexpr float kHeight = 124.0F;
    const float originX = (display.x - kWidth) * 0.5F;
    const float originY = (display.y - kHeight) * 0.5F;
    const Vec2 panelMin{originX, originY};
    const Vec2 panelMax{originX + kWidth, originY + kHeight};

    backend.DrawFilledRect(
        {panelMin.x + 5.0F, panelMin.y + 5.0F},
        {panelMax.x + 5.0F, panelMax.y + 5.0F},
        Color{0.0F, 0.0F, 0.0F, 0.45F}
    );
    backend.DrawBevelRect(panelMin, panelMax, Color::ValveBg(), Color::ValveBevelLight(), Color::ValveBevelDark(), 2.0F);

    const Vec2 titleMin{panelMin.x + 2.0F, panelMin.y + 2.0F};
    const Vec2 titleMax{panelMax.x - 2.0F, panelMin.y + 32.0F};
    backend.DrawFilledRect(titleMin, titleMax, Color::ValveTitleBar());
    backend.DrawLine({titleMin.x, titleMax.y}, {titleMax.x, titleMax.y}, Color::ValveBorder(), 1.0F);
    backend.DrawTextEx(
        {titleMin.x + 14.0F, titleMin.y + 7.0F},
        "Loading...",
        18.0F,
        Color::ValveOrange(),
        TextAlign::Left,
        true,
        FontStyle::Regular
    );

    const std::string mapLabel = mapName.empty() ? "..." : std::string(mapName);
    backend.DrawTextEx(
        {panelMin.x + 16.0F, panelMin.y + 46.0F},
        mapLabel,
        16.0F,
        Color::ValveTextPrimary(),
        TextAlign::Left,
        true,
        FontStyle::Regular
    );

    const float barMinX = panelMin.x + 16.0F;
    const float barMaxX = panelMax.x - 16.0F;
    const float barMinY = panelMin.y + 80.0F;
    const float barMaxY = panelMin.y + 98.0F;
    backend.DrawBevelRect(
        {barMinX, barMinY},
        {barMaxX, barMaxY},
        Color::ValveSlotBg(),
        Color::ValveBevelDark(),
        Color::ValveBevelLight(),
        1.0F
    );

    const float clamped = std::clamp(progress, 0.0F, 1.0F);
    const float fillMaxX = barMinX + 2.0F + (barMaxX - barMinX - 4.0F) * clamped;
    if (fillMaxX > barMinX + 2.0F) {
        backend.DrawFilledRect(
            {barMinX + 2.0F, barMinY + 2.0F},
            {fillMaxX, barMaxY - 2.0F},
            Color::ValveOrange()
        );
    }
}

} // namespace ui
