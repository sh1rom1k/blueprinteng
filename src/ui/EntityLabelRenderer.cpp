#include "ui/EntityLabelRenderer.hpp"

#include <glm/vec4.hpp>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace ui {
namespace {

bool WorldToScreen(
    const glm::vec3& worldPosition,
    const glm::mat4& viewProj,
    const Vec2& displaySize,
    Vec2& screenPosition
) {
    const glm::vec4 clipSpace = viewProj * glm::vec4(worldPosition, 1.0F);
    if (clipSpace.w <= 0.001F) {
        return false;
    }
    const glm::vec3 ndc = glm::vec3(clipSpace) / clipSpace.w;
    if (ndc.x < -1.05F || ndc.x > 1.05F || ndc.y < -1.05F || ndc.y > 1.05F) {
        return false;
    }
    screenPosition.x = (ndc.x * 0.5F + 0.5F) * displaySize.x;
    screenPosition.y = (1.0F - (ndc.y * 0.5F + 0.5F)) * displaySize.y;
    return true;
}

} // namespace

void EntityLabelRenderer::Render(
    IUIRenderBackend& backend,
    const std::vector<BspMapEntity>& entities,
    const glm::mat4& view,
    const glm::mat4& projection,
    const glm::vec3& cameraPosition,
    bool showExtendedProperties
) {
    const Vec2 displaySize = backend.DisplaySize();
    constexpr float kMaxLabelDistanceSq = 96.0F * 96.0F;
    constexpr float kFontSize = 14.0F;
    constexpr float kLineH = 17.0F;

    const glm::mat4 viewProj = projection * view;

    for (const BspMapEntity& entity : entities) {
        if (entity.classname == "info_node" || entity.classname.rfind("info_node", 0) == 0) {
            continue;
        }

        const glm::vec3 labelAnchor = entity.position + glm::vec3(0.0F, 0.55F, 0.0F);
        const glm::vec3 offset = labelAnchor - cameraPosition;
        if (glm::dot(offset, offset) > kMaxLabelDistanceSq) {
            continue;
        }

        Vec2 screenPos{};
        if (!WorldToScreen(labelAnchor, viewProj, displaySize, screenPos)) {
            continue;
        }

        std::vector<std::pair<std::string, Color>> lines;
        lines.emplace_back("[" + entity.classname + "]", Color::ValveOrange());
        lines.emplace_back("model: " + (entity.model.empty() ? std::string("(none)") : entity.model), Color{0.82F, 0.85F, 0.88F, 1.0F});
        lines.emplace_back("tex:   " + (entity.texture.empty() ? std::string("(none)") : entity.texture), Color{0.70F, 0.74F, 0.78F, 1.0F});

        if (showExtendedProperties) {
            for (const auto& prop : entity.properties) {
                if (prop.first == "classname" || prop.first == "model" || prop.first == "material"
                    || prop.first == "OverlayMaterial" || prop.first == "texture" || prop.first == "vgui_screen_name") {
                    continue;
                }
                lines.emplace_back(prop.first + ": " + prop.second, Color::ValveYellow());
            }
        }

        float maxW = 0.0F;
        for (const auto& line : lines) {
            maxW = std::max(maxW, backend.MeasureText(line.first, kFontSize, FontStyle::Monospace));
        }

        const Vec2 pad{8.0F, 6.0F};
        const Vec2 boxMin{screenPos.x - pad.x, screenPos.y - pad.y};
        const Vec2 boxMax{screenPos.x + maxW + pad.x, screenPos.y + static_cast<float>(lines.size()) * kLineH + pad.y};

        // Dark translucent slate box with 1px border
        backend.DrawFilledRect(boxMin, boxMax, Color{0.08F, 0.10F, 0.12F, 0.88F});
        backend.DrawRectBorder(boxMin, boxMax, Color{0.35F, 0.42F, 0.48F, 0.9F}, 1.0F);

        float textY = screenPos.y;
        for (const auto& line : lines) {
            backend.DrawTextEx({screenPos.x, textY}, line.first, kFontSize, line.second, TextAlign::Left, true, FontStyle::Monospace);
            textY += kLineH;
        }
    }
}

} // namespace ui
