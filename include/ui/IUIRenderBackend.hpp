#pragma once

#include "ui/MenuTypes.hpp"

#include <string_view>

namespace ui {

enum class TextAlign : std::uint8_t {
    Left,
    Center,
    Right,
};

enum class MouseButton : std::int8_t {
    Left = 0,
    Right = 1,
    Middle = 2,
};

enum class FontStyle : std::uint8_t {
    Regular,
    Monospace,
};

class IUIRenderBackend {
public:
    virtual ~IUIRenderBackend() = default;

    virtual Vec2 DisplaySize() const = 0;
    virtual Vec2 MousePosition() const = 0;
    virtual bool IsMouseButtonDown(MouseButton button) const = 0;
    virtual bool WasMouseButtonPressed(MouseButton button) const = 0;
    virtual bool WasMouseButtonReleased(MouseButton button) const = 0;

    virtual void DrawFilledRect(const Vec2& min, const Vec2& max, Color color) = 0;
    virtual void DrawRectBorder(const Vec2& min, const Vec2& max, Color color, float thickness) = 0;
    virtual void DrawLine(const Vec2& p0, const Vec2& p1, Color color, float thickness = 1.0F) = 0;

    // Classic Valve bevel styling: light top/left, dark bottom/right
    virtual void DrawBevelRect(
        const Vec2& min,
        const Vec2& max,
        Color fill,
        Color lightBorder,
        Color darkBorder,
        float thickness = 1.0F
    ) = 0;

    virtual void DrawText(
        const Vec2& position,
        std::string_view text,
        float fontSize,
        Color color,
        TextAlign align = TextAlign::Left,
        bool glow = false
    ) = 0;

    virtual void DrawTextEx(
        const Vec2& position,
        std::string_view text,
        float fontSize,
        Color color,
        TextAlign align = TextAlign::Left,
        bool shadow = false,
        FontStyle style = FontStyle::Regular
    ) = 0;

    virtual float MeasureText(
        std::string_view text,
        float fontSize,
        FontStyle style = FontStyle::Regular
    ) const = 0;

    virtual float TextLineHeight(
        float fontSize,
        FontStyle style = FontStyle::Regular
    ) const = 0;
};

} // namespace ui
