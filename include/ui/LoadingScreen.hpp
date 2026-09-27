#pragma once

#include "ui/IUIRenderBackend.hpp"

#include <string_view>

namespace ui {

class LoadingScreen {
public:
    void Render(IUIRenderBackend& backend, std::string_view mapName, float progress) const;
};

} // namespace ui
