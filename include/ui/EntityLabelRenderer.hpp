#pragma once

#include "BspLoader.hpp"
#include "ui/IUIRenderBackend.hpp"

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <vector>

namespace ui {

class EntityLabelRenderer {
public:
    void Render(
        IUIRenderBackend& backend,
        const std::vector<BspMapEntity>& entities,
        const glm::mat4& view,
        const glm::mat4& projection,
        const glm::vec3& cameraPosition,
        bool showExtendedProperties
    );
};

} // namespace ui
