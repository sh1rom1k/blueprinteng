#include "ui/VguiPauseMenu.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ui {
namespace {

constexpr float kWindowWidth = 540.0F;
constexpr float kWindowHeight = 490.0F;

void DrawVguiButton(
    IUIRenderBackend& backend,
    const Vec2& min,
    const Vec2& max,
    std::string_view label,
    bool hovered,
    bool primary = false,
    bool disabled = false
) {
    Color bg = disabled ? Color{0.18F, 0.19F, 0.20F, 0.9F}
                        : (hovered ? Color::ValveButtonHover() : Color::ValveButtonNormal());
    if (primary && !disabled && !hovered) {
        bg = Color{0.26F, 0.28F, 0.31F, 0.98F};
    }

    Color light = disabled ? Color{0.28F, 0.30F, 0.32F, 0.6F} : Color::ValveBevelLight();
    Color dark = disabled ? Color{0.10F, 0.11F, 0.12F, 0.9F} : Color::ValveBevelDark();

    if (hovered && !disabled) {
        light = Color::ValveOrange();
    }

    backend.DrawBevelRect(min, max, bg, light, dark, 1.5F);

    Color textColor = disabled ? Color::ValveTextDim()
                               : (hovered ? Color::ValveOrange() : Color::ValveTextPrimary());
    if (primary && !disabled && !hovered) {
        textColor = Color::White();
    }

    const float centerY = min.y + (max.y - min.y - 18.0F) * 0.5F;
    backend.DrawTextEx(
        {min.x + (max.x - min.x) * 0.5F, centerY},
        label,
        17.0F,
        textColor,
        TextAlign::Center,
        true,
        FontStyle::Regular
    );
}

void DrawVguiCheckbox(
    IUIRenderBackend& backend,
    const Vec2& boxMin,
    bool checked,
    std::string_view label,
    bool hovered,
    bool disabled = false
) {
    const Vec2 boxMax{boxMin.x + 16.0F, boxMin.y + 16.0F};

    // Sunken slot for checkbox
    Color slotBg = disabled ? Color{0.14F, 0.15F, 0.16F, 0.8F} : Color::ValveSlotBg();
    backend.DrawBevelRect(boxMin, boxMax, slotBg, Color::ValveBevelDark(), Color::ValveBevelLight(), 1.0F);

    if (checked) {
        Color checkCol = disabled ? Color::ValveTextDim() : Color::ValveOrange();
        backend.DrawFilledRect({boxMin.x + 3.0F, boxMin.y + 3.0F}, {boxMax.x - 3.0F, boxMax.y - 3.0F}, checkCol);
    }

    Color textCol = disabled ? Color::ValveTextDim()
                             : (hovered ? Color::ValveOrange() : Color::ValveTextPrimary());
    backend.DrawTextEx(
        {boxMax.x + 10.0F, boxMin.y - 1.0F},
        label,
        18.0F,
        textCol,
        TextAlign::Left,
        true,
        FontStyle::Regular
    );
}

void DrawVguiSlider(
    IUIRenderBackend& backend,
    const Vec2& trackMin,
    const Vec2& trackMax,
    float fraction, // 0..1
    std::string_view label,
    std::string_view valueText,
    bool hovered
) {
    // Label above
    backend.DrawTextEx(
        {trackMin.x, trackMin.y - 22.0F},
        label,
        17.0F,
        hovered ? Color::ValveOrange() : Color::ValveTextPrimary(),
        TextAlign::Left,
        true,
        FontStyle::Regular
    );

    // Value text on the right
    backend.DrawTextEx(
        {trackMax.x + 12.0F, trackMin.y - 6.0F},
        valueText,
        17.0F,
        Color::ValveYellow(),
        TextAlign::Left,
        true,
        FontStyle::Monospace
    );

    // Sunken track
    backend.DrawBevelRect(
        trackMin,
        trackMax,
        Color::ValveSlotBg(),
        Color::ValveBevelDark(),
        Color::ValveBevelLight(),
        1.0F
    );

    // Slider thumb
    const float trackW = trackMax.x - trackMin.x;
    const float thumbW = 14.0F;
    const float thumbH = 20.0F;
    const float thumbX = trackMin.x + fraction * (trackW - thumbW);
    const float thumbY = trackMin.y + (trackMax.y - trackMin.y - thumbH) * 0.5F;

    const Vec2 tMin{thumbX, thumbY};
    const Vec2 tMax{thumbX + thumbW, thumbY + thumbH};
    Color thumbBg = hovered ? Color::ValveButtonHover() : Color::ValveButtonNormal();
    Color thumbLight = hovered ? Color::ValveOrange() : Color::ValveBevelLight();
    backend.DrawBevelRect(tMin, tMax, thumbBg, thumbLight, Color::ValveBevelDark(), 1.0F);
}

} // namespace

void VguiPauseMenu::Initialize(IUISoundBackend* soundBackend) {
    sound_ = soundBackend;
    activeTab_ = 0;
    highlightedIndex_ = 0;
    lastHoveredIndex_ = -1;
    draggingSlider_ = false;
    confirmCooldown_ = 0.0F;
}

void VguiPauseMenu::Reset() {
    activeTab_ = 0;
    highlightedIndex_ = 0;
    lastHoveredIndex_ = -1;
    draggingSlider_ = false;
    confirmCooldown_ = 0.25F;
}

void VguiPauseMenu::SyncFromEngine(const PauseGameplaySettings& engineSettings) {
    settings_ = engineSettings;
}

void VguiPauseMenu::PlayHoverSound() {
    if (sound_ != nullptr) sound_->PlayHover();
}

void VguiPauseMenu::PlaySelectSound() {
    if (sound_ != nullptr) sound_->PlaySelect();
}

void VguiPauseMenu::Update(float deltaTime) {
    if (confirmCooldown_ > 0.0F) {
        confirmCooldown_ = std::max(0.0F, confirmCooldown_ - deltaTime);
    }
}

PauseMenuAction VguiPauseMenu::HandleInput(const MenuInput& input) {
    // ESC always resumes
    if (input.backPressed) {
        PlaySelectSound();
        return PauseMenuAction::Continue;
    }

    const Vec2 mouse = input.mousePosition;
    const bool mouseClicked = input.mouseLeftPressed;
    const bool mouseDown = input.mouseLeftDown;

    // Handle mouse click on tabs
    if (mouseClicked) {
        if (tab0Rect_.Contains(mouse)) {
            if (activeTab_ != 0) {
                activeTab_ = 0;
                PlaySelectSound();
            }
            return PauseMenuAction::None;
        }
        if (tab1Rect_.Contains(mouse)) {
            if (activeTab_ != 1) {
                activeTab_ = 1;
                PlaySelectSound();
            }
            return PauseMenuAction::None;
        }
    }

    // Handle bottom action buttons
    if (mouseClicked) {
        if (btnResumeRect_.Contains(mouse)) {
            PlaySelectSound();
            return PauseMenuAction::Continue;
        }
        if (btnMenuRect_.Contains(mouse)) {
            PlaySelectSound();
            return PauseMenuAction::MainMenu;
        }
        if (btnQuitRect_.Contains(mouse)) {
            PlaySelectSound();
            return PauseMenuAction::Exit;
        }
    }

    // Tab 0 interactions
    if (activeTab_ == 0) {
        if (mouseClicked && chkFullscreenRect_.Contains(mouse)) {
            settings_.fullscreen = !settings_.fullscreen;
            PlaySelectSound();
        }
        if (mouseClicked && chkDebugOverlayRect_.Contains(mouse)) {
            settings_.showDebugOverlay = !settings_.showDebugOverlay;
            PlaySelectSound();
        }
        if (mouseClicked && chkSpawnLabelsRect_.Contains(mouse)) {
            settings_.showEntitySpawnLabels = !settings_.showEntitySpawnLabels;
            PlaySelectSound();
        }

        // Slider dragging
        if (mouseClicked && sliderSensitivityRect_.Contains(mouse)) {
            draggingSlider_ = true;
        }
        if (input.mouseLeftReleased) {
            draggingSlider_ = false;
        }
        if (draggingSlider_ && mouseDown) {
            const float frac = std::clamp(
                (mouse.x - sliderSensitivityRect_.x0) / (sliderSensitivityRect_.x1 - sliderSensitivityRect_.x0),
                0.0F,
                1.0F
            );
            settings_.mouseSensitivity = 0.02F + frac * (0.40F - 0.02F);
        }
    }

    // Tab 1 interactions
    if (activeTab_ == 1) {
        if (mouseClicked && chkFlashlightRect_.Contains(mouse)) {
            settings_.flashlightEnabled = !settings_.flashlightEnabled;
            PlaySelectSound();
        }
        if (mouseClicked && chkShadowsRect_.Contains(mouse)) {
            settings_.shadowsEnabled = !settings_.shadowsEnabled;
            PlaySelectSound();
        }
        if (mouseClicked && settings_.shadowsEnabled && chkFlashlightShadowsRect_.Contains(mouse)) {
            settings_.flashlightShadows = !settings_.flashlightShadows;
            PlaySelectSound();
        }
        if (mouseClicked && settings_.shadowsEnabled && chkPointLightShadowsRect_.Contains(mouse)) {
            settings_.pointLightShadows = !settings_.pointLightShadows;
            PlaySelectSound();
        }
        if (mouseClicked && btnClearLightsRect_.Contains(mouse)) {
            PlaySelectSound();
            return PauseMenuAction::ClearPointLights;
        }
    }

    // Keyboard support: Left/Right switches tabs or adjusts slider
    if (input.leftPressed) {
        if (activeTab_ == 1) {
            activeTab_ = 0;
            PlaySelectSound();
        } else {
            settings_.mouseSensitivity = std::clamp(settings_.mouseSensitivity - 0.02F, 0.02F, 0.40F);
        }
    }
    if (input.rightPressed) {
        if (activeTab_ == 0) {
            activeTab_ = 1;
            PlaySelectSound();
        } else {
            settings_.mouseSensitivity = std::clamp(settings_.mouseSensitivity + 0.02F, 0.02F, 0.40F);
        }
    }

    return PauseMenuAction::None;
}

void VguiPauseMenu::Render(IUIRenderBackend& backend, float /*timeSeconds*/) {
    const Vec2 display = backend.DisplaySize();
    const Vec2 mouse = backend.MousePosition();

    // Dark background veil
    backend.DrawFilledRect({0.0F, 0.0F}, display, Color{0.0F, 0.0F, 0.0F, 0.65F});

    // Centered VGUI window frame
    const float winX = (display.x - kWindowWidth) * 0.5F;
    const float winY = (display.y - kWindowHeight) * 0.5F;
    const Vec2 winMin{winX, winY};
    const Vec2 winMax{winX + kWindowWidth, winY + kWindowHeight};

    // Window shadow & main body
    backend.DrawFilledRect({winMin.x + 6.0F, winMin.y + 6.0F}, {winMax.x + 6.0F, winMax.y + 6.0F}, Color{0.0F, 0.0F, 0.0F, 0.4F});
    backend.DrawBevelRect(winMin, winMax, Color::ValveBg(), Color::ValveBevelLight(), Color::ValveBevelDark(), 2.0F);

    // Titlebar
    const Vec2 titleMin{winMin.x + 2.0F, winMin.y + 2.0F};
    const Vec2 titleMax{winMax.x - 2.0F, winMin.y + 36.0F};
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

    // Tab strip
    const float tabY = winMin.y + 44.0F;
    const float tabH = 30.0F;
    const float tabW = 190.0F;

    tab0Rect_ = {winMin.x + 18.0F, tabY, winMin.x + 18.0F + tabW, tabY + tabH};
    tab1Rect_ = {tab0Rect_.x1 + 4.0F, tabY, tab0Rect_.x1 + 4.0F + tabW, tabY + tabH};

    const bool tab0Hover = tab0Rect_.Contains(mouse);
    const bool tab1Hover = tab1Rect_.Contains(mouse);

    // Tab 0
    Color t0Bg = (activeTab_ == 0) ? Color::ValveBg() : (tab0Hover ? Color{0.24F, 0.26F, 0.28F, 1.0F} : Color{0.18F, 0.19F, 0.20F, 1.0F});
    backend.DrawBevelRect(
        {tab0Rect_.x0, tab0Rect_.y0},
        {tab0Rect_.x1, tab0Rect_.y1},
        t0Bg,
        (activeTab_ == 0) ? Color::ValveOrange() : Color::ValveBevelLight(),
        Color::ValveBevelDark(),
        1.5F
    );
    backend.DrawTextEx(
        {tab0Rect_.x0 + tabW * 0.5F, tab0Rect_.y0 + 6.0F},
        "Video",
        16.0F,
        (activeTab_ == 0) ? Color::ValveOrange() : (tab0Hover ? Color::White() : Color::ValveTextDim()),
        TextAlign::Center,
        true,
        FontStyle::Regular
    );

    // Tab 1
    Color t1Bg = (activeTab_ == 1) ? Color::ValveBg() : (tab1Hover ? Color{0.24F, 0.26F, 0.28F, 1.0F} : Color{0.18F, 0.19F, 0.20F, 1.0F});
    backend.DrawBevelRect(
        {tab1Rect_.x0, tab1Rect_.y0},
        {tab1Rect_.x1, tab1Rect_.y1},
        t1Bg,
        (activeTab_ == 1) ? Color::ValveOrange() : Color::ValveBevelLight(),
        Color::ValveBevelDark(),
        1.5F
    );
    backend.DrawTextEx(
        {tab1Rect_.x0 + tabW * 0.5F, tab1Rect_.y0 + 6.0F},
        "Lighting",
        16.0F,
        (activeTab_ == 1) ? Color::ValveOrange() : (tab1Hover ? Color::White() : Color::ValveTextDim()),
        TextAlign::Center,
        true,
        FontStyle::Regular
    );

    // Main content panel
    const Vec2 contentMin{winMin.x + 18.0F, tabY + tabH};
    const Vec2 contentMax{winMax.x - 18.0F, winMax.y - 72.0F};
    backend.DrawBevelRect(contentMin, contentMax, Color{0.14F, 0.15F, 0.16F, 0.95F}, Color::ValveBevelDark(), Color::ValveBevelLight(), 1.5F);

    if (activeTab_ == 0) {
        // Tab 0 Content: Display & Controls
        float curY = contentMin.y + 24.0F;

        // Fullscreen checkbox
        chkFullscreenRect_ = {contentMin.x + 24.0F, curY, contentMax.x - 24.0F, curY + 22.0F};
        DrawVguiCheckbox(
            backend,
            {chkFullscreenRect_.x0, chkFullscreenRect_.y0 + 2.0F},
            settings_.fullscreen,
            "Fullscreen Mode (F11)",
            chkFullscreenRect_.Contains(mouse)
        );
        curY += 40.0F;

        // Mouse Sensitivity Slider
        const float sliderTrackW = 280.0F;
        sliderSensitivityRect_ = {contentMin.x + 24.0F, curY + 24.0F, contentMin.x + 24.0F + sliderTrackW, curY + 32.0F};
        char sensBuf[32];
        std::snprintf(sensBuf, sizeof(sensBuf), "%.2f", settings_.mouseSensitivity);
        const float sensFrac = std::clamp((settings_.mouseSensitivity - 0.02F) / (0.40F - 0.02F), 0.0F, 1.0F);
        DrawVguiSlider(
            backend,
            {sliderSensitivityRect_.x0, sliderSensitivityRect_.y0},
            {sliderSensitivityRect_.x1, sliderSensitivityRect_.y1},
            sensFrac,
            "Mouse Sensitivity",
            sensBuf,
            sliderSensitivityRect_.Contains(mouse) || draggingSlider_
        );
        curY += 66.0F;

        // NetGraph / Debug Overlay checkbox
        chkDebugOverlayRect_ = {contentMin.x + 24.0F, curY, contentMax.x - 24.0F, curY + 22.0F};
        DrawVguiCheckbox(
            backend,
            {chkDebugOverlayRect_.x0, chkDebugOverlayRect_.y0 + 2.0F},
            settings_.showDebugOverlay,
            "NetGraph / Engine Metrics Overlay (F3)",
            chkDebugOverlayRect_.Contains(mouse)
        );
        curY += 38.0F;

        // Entity Spawn Labels checkbox
        chkSpawnLabelsRect_ = {contentMin.x + 24.0F, curY, contentMax.x - 24.0F, curY + 22.0F};
        DrawVguiCheckbox(
            backend,
            {chkSpawnLabelsRect_.x0, chkSpawnLabelsRect_.y0 + 2.0F},
            settings_.showEntitySpawnLabels,
            "Entity Spawn Labels & Info (F6 / hold F5)",
            chkSpawnLabelsRect_.Contains(mouse)
        );
    } else {
        // Tab 1 Content: Lighting & Shadows
        float curY = contentMin.y + 20.0F;

        // Flashlight (F)
        chkFlashlightRect_ = {contentMin.x + 24.0F, curY, contentMax.x - 24.0F, curY + 22.0F};
        DrawVguiCheckbox(
            backend,
            {chkFlashlightRect_.x0, chkFlashlightRect_.y0 + 2.0F},
            settings_.flashlightEnabled,
            "Flashlight (F)",
            chkFlashlightRect_.Contains(mouse)
        );
        curY += 34.0F;

        // Dynamic Shadows
        chkShadowsRect_ = {contentMin.x + 24.0F, curY, contentMax.x - 24.0F, curY + 22.0F};
        DrawVguiCheckbox(
            backend,
            {chkShadowsRect_.x0, chkShadowsRect_.y0 + 2.0F},
            settings_.shadowsEnabled,
            "Dynamic Realtime Shadows",
            chkShadowsRect_.Contains(mouse)
        );
        curY += 34.0F;

        // Flashlight Shadows (indented)
        chkFlashlightShadowsRect_ = {contentMin.x + 48.0F, curY, contentMax.x - 24.0F, curY + 22.0F};
        DrawVguiCheckbox(
            backend,
            {chkFlashlightShadowsRect_.x0, chkFlashlightShadowsRect_.y0 + 2.0F},
            settings_.flashlightShadows,
            "Flashlight Spot Shadow Mapping",
            chkFlashlightShadowsRect_.Contains(mouse),
            !settings_.shadowsEnabled
        );
        curY += 34.0F;

        // Point Light Shadows (indented)
        chkPointLightShadowsRect_ = {contentMin.x + 48.0F, curY, contentMax.x - 24.0F, curY + 22.0F};
        DrawVguiCheckbox(
            backend,
            {chkPointLightShadowsRect_.x0, chkPointLightShadowsRect_.y0 + 2.0F},
            settings_.pointLightShadows,
            "Omni Point Light Depth Shadows",
            chkPointLightShadowsRect_.Contains(mouse),
            !settings_.shadowsEnabled
        );
        curY += 40.0F;

        // Point lights status and clear button
        char lightStr[64];
        std::snprintf(lightStr, sizeof(lightStr), "Point Lights (G key to place): %d / %d", settings_.pointLightCount, settings_.maxPointLights);
        backend.DrawTextEx(
            {contentMin.x + 24.0F, curY + 4.0F},
            lightStr,
            17.0F,
            Color::ValveYellow(),
            TextAlign::Left,
            true,
            FontStyle::Regular
        );

        btnClearLightsRect_ = {contentMax.x - 170.0F, curY - 4.0F, contentMax.x - 24.0F, curY + 26.0F};
        DrawVguiButton(
            backend,
            {btnClearLightsRect_.x0, btnClearLightsRect_.y0},
            {btnClearLightsRect_.x1, btnClearLightsRect_.y1},
            "Clear Lights",
            btnClearLightsRect_.Contains(mouse),
            false,
            settings_.pointLightCount == 0
        );
    }

    // Bottom Action Buttons
    const float btnH = 34.0F;
    const float btnY = winMax.y - 54.0F;
    const float btnW = 155.0F;

    btnResumeRect_ = {winMin.x + 18.0F, btnY, winMin.x + 18.0F + btnW, btnY + btnH};
    btnMenuRect_ = {btnResumeRect_.x1 + 16.0F, btnY, btnResumeRect_.x1 + 16.0F + btnW, btnY + btnH};
    btnQuitRect_ = {btnMenuRect_.x1 + 16.0F, btnY, winMax.x - 18.0F, btnY + btnH};

    DrawVguiButton(
        backend,
        {btnResumeRect_.x0, btnResumeRect_.y0},
        {btnResumeRect_.x1, btnResumeRect_.y1},
        "Resume",
        btnResumeRect_.Contains(mouse),
        true
    );

    DrawVguiButton(
        backend,
        {btnMenuRect_.x0, btnMenuRect_.y0},
        {btnMenuRect_.x1, btnMenuRect_.y1},
        "Main Menu",
        btnMenuRect_.Contains(mouse),
        false
    );

    DrawVguiButton(
        backend,
        {btnQuitRect_.x0, btnQuitRect_.y0},
        {btnQuitRect_.x1, btnQuitRect_.y1},
        "Quit",
        btnQuitRect_.Contains(mouse),
        false
    );
}

} // namespace ui
