#pragma once

#include "ui/IUIRenderBackend.hpp"
#include "ui/IUISoundBackend.hpp"
#include "ui/MenuTypes.hpp"

namespace ui {

class VguiPauseMenu {
public:
    void Initialize(IUISoundBackend* soundBackend);
    void SyncFromEngine(const PauseGameplaySettings& engineSettings);
    [[nodiscard]] const PauseGameplaySettings& Settings() const { return settings_; }
    [[nodiscard]] PauseGameplaySettings& Settings() { return settings_; }

    PauseMenuAction HandleInput(const MenuInput& input);
    void Update(float deltaTime);
    void Render(IUIRenderBackend& backend, float timeSeconds);

    void Reset();

private:
    void PlayHoverSound();
    void PlaySelectSound();

    IUISoundBackend* sound_ = nullptr;
    PauseGameplaySettings settings_{};

    int activeTab_ = 0; // 0 = Display & Controls, 1 = Lighting & Shadows
    int highlightedIndex_ = 0;
    int lastHoveredIndex_ = -1;
    bool draggingSlider_ = false;
    float confirmCooldown_ = 0.0F;

    // Bounds for mouse interaction during Render
    struct Rect {
        float x0 = 0.0F, y0 = 0.0F, x1 = 0.0F, y1 = 0.0F;
        [[nodiscard]] bool Contains(const Vec2& pt) const {
            return pt.x >= x0 && pt.x <= x1 && pt.y >= y0 && pt.y <= y1;
        }
    };

    Rect tab0Rect_{};
    Rect tab1Rect_{};
    Rect sliderSensitivityRect_{};
    Rect btnClearLightsRect_{};
    Rect btnResumeRect_{};
    Rect btnMenuRect_{};
    Rect btnQuitRect_{};
    Rect chkFullscreenRect_{};
    Rect chkDebugOverlayRect_{};
    Rect chkSpawnLabelsRect_{};
    Rect chkFlashlightRect_{};
    Rect chkShadowsRect_{};
    Rect chkFlashlightShadowsRect_{};
    Rect chkPointLightShadowsRect_{};
};

} // namespace ui
