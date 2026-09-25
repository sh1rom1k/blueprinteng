#pragma once

#include "ui/IUIRenderBackend.hpp"
#include "ui/MainMenu.hpp"

#include <span>
#include <string>

namespace ui {

struct MenuBackgroundLayer {
    float overlayAlpha = 0.35F;
    bool drawVignette = true;
};

class MenuRenderer {
public:
    void Render(
        IUIRenderBackend& backend,
        const MainMenu& menu,
        float timeSeconds,
        std::span<const std::string> mapLabels,
        const MenuBackgroundLayer& background = {}
    ) const;

private:
    void RenderMainList(
        IUIRenderBackend& backend,
        const MainMenu& menu,
        float timeSeconds,
        const Vec2& origin,
        float lineHeight
    ) const;

    void RenderOptionsPanel(
        IUIRenderBackend& backend,
        const MainMenu& menu,
        float timeSeconds,
        std::span<const std::string> mapLabels
    ) const;
};

} // namespace ui
