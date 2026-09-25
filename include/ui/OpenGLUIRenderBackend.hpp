#pragma once

#include "ui/IUIRenderBackend.hpp"

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

struct GLFWwindow;

namespace ui {

class OpenGLUIRenderBackend final : public IUIRenderBackend {
public:
    OpenGLUIRenderBackend();
    ~OpenGLUIRenderBackend() override;

    OpenGLUIRenderBackend(const OpenGLUIRenderBackend&) = delete;
    OpenGLUIRenderBackend& operator=(const OpenGLUIRenderBackend&) = delete;

    bool Initialize(GLFWwindow* window, const char* contentRoot);
    void Shutdown();

    // Frame-level input polling (does not touch any OpenGL state)
    void UpdateInput(int framebufferWidth, int framebufferHeight);

    // 2D UI render pass: isolates OpenGL state from 3D rendering
    void BeginRender(int framebufferWidth, int framebufferHeight);
    void EndRender();

    // Compatibility wrappers
    void BeginFrame(int framebufferWidth, int framebufferHeight) {
        UpdateInput(framebufferWidth, framebufferHeight);
    }
    void EndFrame() {
        // Handled via EndRender
    }

    Vec2 DisplaySize() const override;
    Vec2 MousePosition() const override;
    bool IsMouseButtonDown(MouseButton button) const override;
    bool WasMouseButtonPressed(MouseButton button) const override;
    bool WasMouseButtonReleased(MouseButton button) const override;

    void DrawFilledRect(const Vec2& min, const Vec2& max, Color color) override;
    void DrawRectBorder(const Vec2& min, const Vec2& max, Color color, float thickness) override;
    void DrawLine(const Vec2& p0, const Vec2& p1, Color color, float thickness = 1.0F) override;
    void DrawBevelRect(
        const Vec2& min,
        const Vec2& max,
        Color fill,
        Color lightBorder,
        Color darkBorder,
        float thickness = 1.0F
    ) override;

    void DrawText(
        const Vec2& position,
        std::string_view text,
        float fontSize,
        Color color,
        TextAlign align = TextAlign::Left,
        bool glow = false
    ) override;

    void DrawTextEx(
        const Vec2& position,
        std::string_view text,
        float fontSize,
        Color color,
        TextAlign align = TextAlign::Left,
        bool shadow = false,
        FontStyle style = FontStyle::Regular
    ) override;

    float MeasureText(
        std::string_view text,
        float fontSize,
        FontStyle style = FontStyle::Regular
    ) const override;

    float TextLineHeight(
        float fontSize,
        FontStyle style = FontStyle::Regular
    ) const override;

private:
    struct UiVertex {
        float x = 0.0F;
        float y = 0.0F;
        float r = 1.0F;
        float g = 1.0F;
        float b = 1.0F;
        float a = 1.0F;
        float u = 0.0F;
        float v = 0.0F;
    };

    struct GlyphInfo {
        float advance = 0.0F;
        float x0 = 0.0F;
        float y0 = 0.0F;
        float x1 = 0.0F;
        float y1 = 0.0F;
        float u0 = 0.0F;
        float v0 = 0.0F;
        float u1 = 0.0F;
        float v1 = 0.0F;
    };

    struct FontData {
        float pixelHeight = 36.0F;
        float ascent = 0.0F;
        float descent = 0.0F;
        float lineGap = 0.0F;
        std::array<GlyphInfo, 128> glyphs{};
    };

    void Flush();
    void PushSolidQuad(float x0, float y0, float x1, float y1, Color color);
    void PushTexturedQuad(
        float x0, float y0, float x1, float y1,
        Color color,
        float u0, float v0, float u1, float v1
    );

    bool LoadFonts(const char* contentRoot);
    void BuildFallbackBitmapFont();

    GLFWwindow* window_ = nullptr;
    int framebufferWidth_ = 1280;
    int framebufferHeight_ = 720;

    bool mouseDown_[3]{};
    bool mousePressed_[3]{};
    bool mouseReleased_[3]{};

    unsigned int program_ = 0;
    unsigned int vao_ = 0;
    unsigned int vbo_ = 0;
    int locDisplaySize_ = -1;
    int locUseTexture_ = -1;
    int locFontTexture_ = -1;

    unsigned int fontTexture_ = 0;
    FontData fontRegular_{};
    FontData fontMono_{};
    bool fontInitialized_ = false;

    std::vector<UiVertex> batch_;
    bool drawingSolid_ = true;

    // OpenGL state save/restore during 2D render pass
    unsigned char savedDepthTest_ = 1;
    unsigned char savedDepthMask_ = 1;
    unsigned char savedBlend_ = 0;
    unsigned char savedCullFace_ = 0;
    int savedBlendSrcRgb_ = 0;
    int savedBlendDstRgb_ = 0;
    int savedBlendSrcAlpha_ = 0;
    int savedBlendDstAlpha_ = 0;
    int savedProgram_ = 0;
    int savedVao_ = 0;
    int savedActiveTexture_ = 0;
};

} // namespace ui
