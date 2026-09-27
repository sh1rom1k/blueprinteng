#pragma once

#include "ui/ChapterCatalog.hpp"
#include "ui/IUIRenderBackend.hpp"
#include "ui/MainMenu.hpp"

#include <span>
#include <string>
#include <string_view>

namespace ui {

struct MenuBackgroundLayer {
    float overlayAlpha = 0.0F;
    bool drawVignette = true;
};

class MenuRenderer {
public:
    void Render(
        IUIRenderBackend& backend,
        const MainMenu& menu,
        float timeSeconds,
        std::span<const std::string> mapLabels,
        std::string_view gameLabel,
        std::span<const ChapterInfo> chapters,
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

    void RenderChapterSelect(
        IUIRenderBackend& backend,
        const MainMenu& menu,
        float timeSeconds,
        std::span<const ChapterInfo> chapters
    ) const;
};

} // namespace ui
