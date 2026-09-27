#include "ui/MenuRenderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace ui {
namespace {

void DrawVignette(IUIRenderBackend& backend, const Vec2& size) {
    const Color edge{0.0F, 0.0F, 0.0F, 0.38F};
    const Color left{0.0F, 0.0F, 0.0F, 0.42F};
    const Color leftInner{0.0F, 0.0F, 0.0F, 0.22F};
    backend.DrawFilledRect({0.0F, 0.0F}, {size.x, size.y * 0.14F}, edge);
    backend.DrawFilledRect({0.0F, size.y * 0.86F}, {size.x, size.y}, edge);
    backend.DrawFilledRect({0.0F, 0.0F}, {size.x * 0.36F, size.y}, left);
    backend.DrawFilledRect({0.0F, 0.0F}, {size.x * 0.18F, size.y}, leftInner);
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
        backend.DrawText(pos, kMainMenuItems[i].label, kMainMenuItemFont, color, TextAlign::Left, glow);
    }
}

void MenuRenderer::RenderOptionsPanel(
    IUIRenderBackend& backend,
    const MainMenu& menu,
    float /*timeSeconds*/,
    std::span<const std::string> mapLabels
) const {
    const Vec2 display = backend.DisplaySize();
    const float panelW = 560.0F;
    const float panelH = 430.0F;
    const Vec2 panelMin{(display.x - panelW) * 0.5F, (display.y - panelH) * 0.5F};
    const Vec2 panelMax{panelMin.x + panelW, panelMin.y + panelH};

    backend.DrawFilledRect(
        {panelMin.x + 6.0F, panelMin.y + 6.0F},
        {panelMax.x + 6.0F, panelMax.y + 6.0F},
        Color{0.0F, 0.0F, 0.0F, 0.40F}
    );
    backend.DrawBevelRect(panelMin, panelMax, Color::ValveBg(), Color::ValveBevelLight(), Color::ValveBevelDark(), 2.0F);

    const Vec2 titleMin{panelMin.x + 2.0F, panelMin.y + 2.0F};
    const Vec2 titleMax{panelMax.x - 2.0F, panelMin.y + 36.0F};
    backend.DrawFilledRect(titleMin, titleMax, Color::ValveTitleBar());
    backend.DrawLine({titleMin.x, titleMax.y}, {titleMax.x, titleMax.y}, Color::ValveBorder(), 1.0F);
    backend.DrawTextEx(
        {titleMin.x + 14.0F, titleMin.y + 9.0F},
        "Options",
        18.0F,
        Color::ValveOrange(),
        TextAlign::Left,
        true,
        FontStyle::Regular
    );

    const Vec2 contentMin{panelMin.x + 16.0F, titleMax.y + 12.0F};
    const Vec2 contentMax{panelMax.x - 16.0F, panelMax.y - 16.0F};
    backend.DrawBevelRect(
        contentMin,
        contentMax,
        Color{0.14F, 0.15F, 0.16F, 0.95F},
        Color::ValveBevelDark(),
        Color::ValveBevelLight(),
        1.5F
    );

    const OptionsSettings& opts = menu.Options();
    const int selectedField = menu.OptionsFieldIndex();

    auto drawOptionLine = [&](int fieldIndex, float y, const char* text) {
        const bool selected = fieldIndex == selectedField;
        const Color color = selected ? Color::ValveOrange() : Color::ValveTextPrimary();
        backend.DrawTextEx(
            {contentMin.x + 20.0F, y},
            text,
            18.0F,
            color,
            TextAlign::Left,
            true,
            FontStyle::Regular
        );
    };

    char lineBuffer[256];
    std::snprintf(lineBuffer, sizeof(lineBuffer), "Fullscreen: %s", opts.fullscreen ? "On" : "Off");
    drawOptionLine(0, contentMin.y + 24.0F, lineBuffer);

    std::snprintf(lineBuffer, sizeof(lineBuffer), "Mouse sensitivity: %.2f", opts.mouseSensitivity);
    drawOptionLine(1, contentMin.y + 64.0F, lineBuffer);

    std::snprintf(lineBuffer, sizeof(lineBuffer), "Master volume: %.0f%%", opts.masterVolume * 100.0F);
    drawOptionLine(2, contentMin.y + 104.0F, lineBuffer);

    std::snprintf(lineBuffer, sizeof(lineBuffer), "SFX volume: %.0f%%", opts.sfxVolume * 100.0F);
    drawOptionLine(3, contentMin.y + 144.0F, lineBuffer);

    const std::string mapLabel = mapLabels.empty()
        ? "Demo Cube"
        : mapLabels[static_cast<std::size_t>(
              std::clamp(opts.selectedMapIndex, 0, static_cast<int>(mapLabels.size()) - 1)
          )];
    std::snprintf(lineBuffer, sizeof(lineBuffer), "Map: %s", mapLabel.c_str());
    drawOptionLine(4, contentMin.y + 184.0F, lineBuffer);

    backend.DrawTextEx(
        {contentMin.x + 20.0F, contentMax.y - 64.0F},
        "Up/Down — Field    Left/Right — Value",
        15.0F,
        Color::ValveTextDim(),
        TextAlign::Left,
        true,
        FontStyle::Regular
    );
    backend.DrawTextEx(
        {contentMin.x + 20.0F, contentMax.y - 38.0F},
        "Enter — Apply    Esc — Cancel",
        15.0F,
        Color::ValveTextDim(),
        TextAlign::Left,
        true,
        FontStyle::Regular
    );
}

void MenuRenderer::RenderChapterSelect(
    IUIRenderBackend& backend,
    const MainMenu& menu,
    float timeSeconds,
    std::span<const ChapterInfo> chapters
) const {
    const int visible = VisibleChapterRows(backend.DisplaySize().y);
    const int scroll = menu.ChapterScroll(visible);
    const int selected = menu.ChapterIndex();

    for (int row = 0; row < visible; ++row) {
        const int index = scroll + row;
        if (index < 0 || index >= static_cast<int>(chapters.size())) {
            break;
        }
        const bool highlighted = index == selected;
        Color color = highlighted ? Color::Hl2Hover() : Color::Hl2Default();
        if (highlighted) {
            const float pulse = 0.5F + 0.5F * std::sin(timeSeconds * 4.5F);
            color.a = 0.85F + 0.15F * pulse;
        }
        const Vec2 pos{
            kChapterListLeft,
            kChapterListTop + static_cast<float>(row) * kChapterLineHeight
        };
        backend.DrawText(pos, chapters[static_cast<std::size_t>(index)].title, 20.0F, color, TextAlign::Left, highlighted);
    }

    const Vec2 previewMin{kChapterPreviewX, kChapterPreviewY};
    const Vec2 previewMax{kChapterPreviewX + kChapterPreviewW, kChapterPreviewY + kChapterPreviewH};
    backend.DrawFilledRect(previewMin, previewMax, Color{0.05F, 0.05F, 0.06F, 0.92F});
    if (selected >= 0 && selected < static_cast<int>(chapters.size())) {
        const unsigned int preview = chapters[static_cast<std::size_t>(selected)].previewTexture;
        if (preview != 0) {
            const ChapterInfo& chapter = chapters[static_cast<std::size_t>(selected)];
            backend.DrawImage(
                {previewMin.x + 2.0F, previewMin.y + 2.0F},
                {previewMax.x - 2.0F, previewMax.y - 2.0F},
                preview,
                chapter.previewU0,
                chapter.previewV0,
                chapter.previewU1,
                chapter.previewV1
            );
        } else {
            backend.DrawText(
                {previewMin.x + 16.0F, previewMin.y + 96.0F},
                "NO PREVIEW",
                18.0F,
                Color::Hl2Dim()
            );
        }
    }
    backend.DrawRectBorder(previewMin, previewMax, Color::Hl2PanelBorder(), 2.0F);

    for (int index = 0; index < kDifficultyCount; ++index) {
        const bool highlighted = index == menu.DifficultyIndex();
        const Color color = highlighted ? Color::Hl2Hover() : Color::Hl2Dim();
        backend.DrawText(
            {kChapterPreviewX + static_cast<float>(index) * kChapterDifficultyGap, kChapterDifficultyY},
            kDifficultyLabels[index],
            20.0F,
            color,
            TextAlign::Left,
            highlighted
        );
    }

    backend.DrawText(
        {kChapterListLeft, backend.DisplaySize().y - 48.0F},
        "ENTER — START    ESC — BACK    LEFT/RIGHT — DIFFICULTY",
        16.0F,
        Color::Hl2Dim()
    );
}

void MenuRenderer::Render(
    IUIRenderBackend& backend,
    const MainMenu& menu,
    float timeSeconds,
    std::span<const std::string> mapLabels,
    std::string_view gameLabel,
    std::span<const ChapterInfo> chapters,
    const MenuBackgroundLayer& background
) const {
    const Vec2 display = backend.DisplaySize();

    if (background.overlayAlpha > 0.001F) {
        Color overlay = Color::Hl2Overlay();
        overlay.a = background.overlayAlpha;
        backend.DrawFilledRect({0.0F, 0.0F}, display, overlay);
    }

    if (background.drawVignette) {
        DrawVignette(backend, display);
    }

    const std::string title = gameLabel.empty() ? std::string("BLUEPRINT ENGINE") : std::string(gameLabel);
    backend.DrawText(
        {kMainMenuLeft, 72.0F},
        title,
        kMainMenuTitleFont,
        Color::Hl2Active(),
        TextAlign::Left,
        true
    );

    if (menu.IsChapterSelectOpen()) {
        RenderChapterSelect(backend, menu, timeSeconds, chapters);
    } else {
        RenderMainList(backend, menu, timeSeconds, {kMainMenuLeft, kMainMenuTop}, kMainMenuLineHeight);
        backend.DrawText(
            {kMainMenuLeft, display.y - 36.0F},
            "Blueprint Engine",
            14.0F,
            Color::Hl2Dim()
        );
    }

    if (menu.IsOptionsOpen()) {
        backend.DrawFilledRect({0.0F, 0.0F}, display, Color{0.0F, 0.0F, 0.0F, 0.45F});
        RenderOptionsPanel(backend, menu, timeSeconds, mapLabels);
    }
}

} // namespace ui
