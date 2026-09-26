#include "ui/OpenGLUIRenderBackend.hpp"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include <glad/gl.h>
#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace ui {
namespace {

constexpr int kAtlasWidth = 1024;
constexpr int kAtlasHeight = 1024;
constexpr int kFirstChar = 32;
constexpr int kCharCount = 95; // 32..126

const char* kEmbeddedVertShader = R"(#version 450 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec4 aColor;
layout(location = 2) in vec2 aUV;

uniform vec2 uDisplaySize;

out vec4 vColor;
out vec2 vUV;

void main() {
    vec2 ndc;
    ndc.x = (aPos.x / uDisplaySize.x) * 2.0 - 1.0;
    ndc.y = 1.0 - (aPos.y / uDisplaySize.y) * 2.0;
    gl_Position = vec4(ndc, 0.0, 1.0);
    vColor = aColor;
    vUV = aUV;
}
)";

const char* kEmbeddedFragShader = R"(#version 450 core
in vec4 vColor;
in vec2 vUV;

uniform sampler2D uFontTexture;
uniform int uUseTexture;

out vec4 FragColor;

void main() {
    if (uUseTexture != 0) {
        float alpha = texture(uFontTexture, vUV).r;
        FragColor = vec4(vColor.rgb, vColor.a * alpha);
    } else {
        FragColor = vColor;
    }
}
)";

std::vector<unsigned char> ReadBinaryFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }
    file.seekg(0, std::ios::end);
    const std::streamsize size = file.tellg();
    if (size <= 0) {
        return {};
    }
    file.seekg(0, std::ios::beg);
    std::vector<unsigned char> buffer(static_cast<std::size_t>(size));
    if (!file.read(reinterpret_cast<char*>(buffer.data()), size)) {
        return {};
    }
    return buffer;
}

std::string ReadTextFile(const std::string& path) {
    std::ifstream file(path);
    if (!file) {
        return {};
    }
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

GLuint CompileShader(GLenum type, const std::string& source) {
    const GLuint shader = glCreateShader(type);
    const char* src = source.c_str();
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);
    GLint status = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (status == GL_FALSE) {
        GLint logLength = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &logLength);
        std::string log(static_cast<std::size_t>(std::max(logLength, 1)), '\0');
        glGetShaderInfoLog(shader, logLength, nullptr, log.data());
        glDeleteShader(shader);
        std::cerr << "UI shader compile error:\n" << log << "\n";
        return 0;
    }
    return shader;
}

GLuint LinkProgram(GLuint vertexShader, GLuint fragmentShader) {
    if (vertexShader == 0 || fragmentShader == 0) {
        return 0;
    }
    const GLuint program = glCreateProgram();
    glAttachShader(program, vertexShader);
    glAttachShader(program, fragmentShader);
    glLinkProgram(program);
    GLint status = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &status);
    if (status == GL_FALSE) {
        GLint logLength = 0;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &logLength);
        std::string log(static_cast<std::size_t>(std::max(logLength, 1)), '\0');
        glGetProgramInfoLog(program, logLength, nullptr, log.data());
        glDeleteProgram(program);
        std::cerr << "UI shader link error:\n" << log << "\n";
        return 0;
    }
    return program;
}

} // namespace

OpenGLUIRenderBackend::OpenGLUIRenderBackend() = default;

OpenGLUIRenderBackend::~OpenGLUIRenderBackend() {
    Shutdown();
}

bool OpenGLUIRenderBackend::Initialize(GLFWwindow* window, const char* contentRoot) {
    window_ = window;

    std::string vertSrc;
    std::string fragSrc;
    if (contentRoot != nullptr) {
        vertSrc = ReadTextFile(std::string(contentRoot) + "/shaders/ui.vert");
        fragSrc = ReadTextFile(std::string(contentRoot) + "/shaders/ui.frag");
    }
    if (vertSrc.empty()) {
        vertSrc = kEmbeddedVertShader;
    }
    if (fragSrc.empty()) {
        fragSrc = kEmbeddedFragShader;
    }

    const GLuint vs = CompileShader(GL_VERTEX_SHADER, vertSrc);
    const GLuint fs = CompileShader(GL_FRAGMENT_SHADER, fragSrc);
    program_ = LinkProgram(vs, fs);
    if (vs != 0) glDeleteShader(vs);
    if (fs != 0) glDeleteShader(fs);

    if (program_ == 0) {
        std::cerr << "Failed to initialize UI shader program\n";
        return false;
    }

    locDisplaySize_ = glGetUniformLocation(program_, "uDisplaySize");
    locUseTexture_ = glGetUniformLocation(program_, "uUseTexture");
    locFontTexture_ = glGetUniformLocation(program_, "uFontTexture");

    glUseProgram(program_);
    if (locFontTexture_ >= 0) {
        glUniform1i(locFontTexture_, 0);
    }
    glUseProgram(0);

    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, 0, nullptr, GL_STREAM_DRAW);

    constexpr GLsizei stride = static_cast<GLsizei>(sizeof(UiVertex));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(offsetof(UiVertex, r)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(offsetof(UiVertex, u)));
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    if (!LoadFonts(contentRoot)) {
        std::cerr << "Warning: TrueType fonts failed, falling back to embedded bitmap font\n";
        BuildFallbackBitmapFont();
    }

    batch_.reserve(8192);
    return true;
}

void OpenGLUIRenderBackend::Shutdown() {
    if (fontTexture_ != 0) {
        glDeleteTextures(1, &fontTexture_);
        fontTexture_ = 0;
    }
    if (vbo_ != 0) {
        glDeleteBuffers(1, &vbo_);
        vbo_ = 0;
    }
    if (vao_ != 0) {
        glDeleteVertexArrays(1, &vao_);
        vao_ = 0;
    }
    if (program_ != 0) {
        glDeleteProgram(program_);
        program_ = 0;
    }
    fontInitialized_ = false;
}

bool OpenGLUIRenderBackend::LoadFonts(const char* contentRoot) {
    std::vector<std::string> regularCandidates;
    std::vector<std::string> monoCandidates;

    if (contentRoot != nullptr) {
        regularCandidates.push_back(std::string(contentRoot) + "/assets/fonts/DejaVuSans.ttf");
        monoCandidates.push_back(std::string(contentRoot) + "/assets/fonts/DejaVuSansMono.ttf");
    }
#if defined(_WIN32)
    if (const char* windowsDirectory = std::getenv("WINDIR")) {
        const std::string fonts = std::string(windowsDirectory) + "/Fonts/";
        regularCandidates.push_back(fonts + "arial.ttf");
        regularCandidates.push_back(fonts + "segoeui.ttf");
        monoCandidates.push_back(fonts + "consola.ttf");
        monoCandidates.push_back(fonts + "cour.ttf");
    } else {
        regularCandidates.push_back("C:/Windows/Fonts/arial.ttf");
        regularCandidates.push_back("C:/Windows/Fonts/segoeui.ttf");
        monoCandidates.push_back("C:/Windows/Fonts/consola.ttf");
        monoCandidates.push_back("C:/Windows/Fonts/cour.ttf");
    }
#endif
    regularCandidates.push_back("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");
    regularCandidates.push_back("/usr/share/fonts/TTF/DejaVuSans.ttf");
    regularCandidates.push_back("/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf");
    regularCandidates.push_back("/usr/share/fonts/truetype/freefont/FreeSans.ttf");

    monoCandidates.push_back("/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf");
    monoCandidates.push_back("/usr/share/fonts/TTF/DejaVuSansMono.ttf");
    monoCandidates.push_back("/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf");

    std::vector<unsigned char> regularBytes;
    for (const auto& path : regularCandidates) {
        regularBytes = ReadBinaryFile(path);
        if (!regularBytes.empty()) break;
    }

    if (regularBytes.empty()) {
        return false;
    }

    std::vector<unsigned char> monoBytes;
    for (const auto& path : monoCandidates) {
        monoBytes = ReadBinaryFile(path);
        if (!monoBytes.empty()) break;
    }
    if (monoBytes.empty()) {
        monoBytes = regularBytes;
    }

    stbtt_fontinfo regularInfo{};
    if (stbtt_InitFont(&regularInfo, regularBytes.data(), stbtt_GetFontOffsetForIndex(regularBytes.data(), 0)) == 0) {
        return false;
    }

    stbtt_fontinfo monoInfo{};
    if (stbtt_InitFont(&monoInfo, monoBytes.data(), stbtt_GetFontOffsetForIndex(monoBytes.data(), 0)) == 0) {
        return false;
    }

    fontRegular_.pixelHeight = 36.0F;
    int rAscent = 0, rDescent = 0, rLineGap = 0;
    stbtt_GetFontVMetrics(&regularInfo, &rAscent, &rDescent, &rLineGap);
    const float scaleR = stbtt_ScaleForPixelHeight(&regularInfo, fontRegular_.pixelHeight);
    fontRegular_.ascent = static_cast<float>(rAscent) * scaleR;
    fontRegular_.descent = static_cast<float>(rDescent) * scaleR;
    fontRegular_.lineGap = static_cast<float>(rLineGap) * scaleR;

    fontMono_.pixelHeight = 28.0F;
    int mAscent = 0, mDescent = 0, mLineGap = 0;
    stbtt_GetFontVMetrics(&monoInfo, &mAscent, &mDescent, &mLineGap);
    const float scaleM = stbtt_ScaleForPixelHeight(&monoInfo, fontMono_.pixelHeight);
    fontMono_.ascent = static_cast<float>(mAscent) * scaleM;
    fontMono_.descent = static_cast<float>(mDescent) * scaleM;
    fontMono_.lineGap = static_cast<float>(mLineGap) * scaleM;

    std::vector<unsigned char> atlasBitmap(kAtlasWidth * kAtlasHeight, 0);
    stbtt_pack_context packContext{};
    if (stbtt_PackBegin(&packContext, atlasBitmap.data(), kAtlasWidth, kAtlasHeight, 0, 1, nullptr) == 0) {
        return false;
    }
    stbtt_PackSetOversampling(&packContext, 1, 1);

    std::array<stbtt_packedchar, kCharCount> packedRegular{};
    if (stbtt_PackFontRange(&packContext, regularBytes.data(), 0, fontRegular_.pixelHeight, kFirstChar, kCharCount, packedRegular.data()) == 0) {
        stbtt_PackEnd(&packContext);
        return false;
    }

    std::array<stbtt_packedchar, kCharCount> packedMono{};
    if (stbtt_PackFontRange(&packContext, monoBytes.data(), 0, fontMono_.pixelHeight, kFirstChar, kCharCount, packedMono.data()) == 0) {
        stbtt_PackEnd(&packContext);
        return false;
    }

    stbtt_PackEnd(&packContext);

    for (int i = 0; i < kCharCount; ++i) {
        const int codepoint = kFirstChar + i;
        const auto& pr = packedRegular[static_cast<std::size_t>(i)];
        GlyphInfo& gr = fontRegular_.glyphs[static_cast<std::size_t>(codepoint)];
        gr.advance = pr.xadvance;
        gr.x0 = pr.xoff;
        gr.y0 = pr.yoff;
        gr.x1 = pr.xoff2;
        gr.y1 = pr.yoff2;
        gr.u0 = static_cast<float>(pr.x0) / static_cast<float>(kAtlasWidth);
        gr.v0 = static_cast<float>(pr.y0) / static_cast<float>(kAtlasHeight);
        gr.u1 = static_cast<float>(pr.x1) / static_cast<float>(kAtlasWidth);
        gr.v1 = static_cast<float>(pr.y1) / static_cast<float>(kAtlasHeight);

        const auto& pm = packedMono[static_cast<std::size_t>(i)];
        GlyphInfo& gm = fontMono_.glyphs[static_cast<std::size_t>(codepoint)];
        gm.advance = pm.xadvance;
        gm.x0 = pm.xoff;
        gm.y0 = pm.yoff;
        gm.x1 = pm.xoff2;
        gm.y1 = pm.yoff2;
        gm.u0 = static_cast<float>(pm.x0) / static_cast<float>(kAtlasWidth);
        gm.v0 = static_cast<float>(pm.y0) / static_cast<float>(kAtlasHeight);
        gm.u1 = static_cast<float>(pm.x1) / static_cast<float>(kAtlasWidth);
        gm.v1 = static_cast<float>(pm.y1) / static_cast<float>(kAtlasHeight);
    }

    glGenTextures(1, &fontTexture_);
    glBindTexture(GL_TEXTURE_2D, fontTexture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(
        GL_TEXTURE_2D,
        0,
        GL_R8,
        kAtlasWidth,
        kAtlasHeight,
        0,
        GL_RED,
        GL_UNSIGNED_BYTE,
        atlasBitmap.data()
    );
    glBindTexture(GL_TEXTURE_2D, 0);

    fontInitialized_ = true;
    std::cout << "[UI] Valve VGUI TrueType font atlas generated successfully (" << kAtlasWidth << "x" << kAtlasHeight << ")\n";
    return true;
}

void OpenGLUIRenderBackend::BuildFallbackBitmapFont() {
    fontRegular_.pixelHeight = 16.0F;
    fontRegular_.ascent = 12.0F;
    fontRegular_.descent = -4.0F;
    fontRegular_.lineGap = 0.0F;
    fontMono_ = fontRegular_;

    std::vector<unsigned char> atlasBitmap(kAtlasWidth * kAtlasHeight, 255);
    for (int c = 32; c < 127; ++c) {
        GlyphInfo& g = fontRegular_.glyphs[static_cast<std::size_t>(c)];
        g.advance = 8.0F;
        g.x0 = 0.0F;
        g.y0 = -12.0F;
        g.x1 = 7.0F;
        g.y1 = 2.0F;
        g.u0 = 0.0F;
        g.v0 = 0.0F;
        g.u1 = 0.01F;
        g.v1 = 0.01F;
        fontMono_.glyphs[static_cast<std::size_t>(c)] = g;
    }

    glGenTextures(1, &fontTexture_);
    glBindTexture(GL_TEXTURE_2D, fontTexture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(
        GL_TEXTURE_2D, 0, GL_R8, kAtlasWidth, kAtlasHeight, 0, GL_RED, GL_UNSIGNED_BYTE, atlasBitmap.data()
    );
    glBindTexture(GL_TEXTURE_2D, 0);
    fontInitialized_ = true;
}

void OpenGLUIRenderBackend::UpdateInput(int framebufferWidth, int framebufferHeight) {
    framebufferWidth_ = std::max(framebufferWidth, 1);
    framebufferHeight_ = std::max(framebufferHeight, 1);

    for (int i = 0; i < 3; ++i) {
        const bool down = (window_ != nullptr)
            && (glfwGetMouseButton(window_, GLFW_MOUSE_BUTTON_LEFT + i) == GLFW_PRESS);
        mousePressed_[i] = down && !mouseDown_[i];
        mouseReleased_[i] = !down && mouseDown_[i];
        mouseDown_[i] = down;
    }
}

void OpenGLUIRenderBackend::BeginRender(int framebufferWidth, int framebufferHeight) {
    framebufferWidth_ = std::max(framebufferWidth, 1);
    framebufferHeight_ = std::max(framebufferHeight, 1);
    batch_.clear();
    drawingSolid_ = true;

    // Save current OpenGL states so 3D pipeline is not affected
    glGetBooleanv(GL_DEPTH_TEST, reinterpret_cast<GLboolean*>(&savedDepthTest_));
    glGetBooleanv(GL_DEPTH_WRITEMASK, reinterpret_cast<GLboolean*>(&savedDepthMask_));
    glGetBooleanv(GL_BLEND, reinterpret_cast<GLboolean*>(&savedBlend_));
    glGetBooleanv(GL_CULL_FACE, reinterpret_cast<GLboolean*>(&savedCullFace_));
    glGetIntegerv(GL_BLEND_SRC_RGB, &savedBlendSrcRgb_);
    glGetIntegerv(GL_BLEND_DST_RGB, &savedBlendDstRgb_);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &savedBlendSrcAlpha_);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &savedBlendDstAlpha_);
    glGetIntegerv(GL_CURRENT_PROGRAM, &savedProgram_);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &savedVao_);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &savedActiveTexture_);

    // Set 2D UI render state
    glViewport(0, 0, framebufferWidth_, framebufferHeight_);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

void OpenGLUIRenderBackend::Flush() {
    if (batch_.empty() || program_ == 0) {
        return;
    }

    glUseProgram(program_);
    glUniform2f(locDisplaySize_, static_cast<float>(framebufferWidth_), static_cast<float>(framebufferHeight_));
    glUniform1i(locUseTexture_, drawingSolid_ ? 0 : 1);
    if (!drawingSolid_ && fontTexture_ != 0) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, fontTexture_);
    }

    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(batch_.size() * sizeof(UiVertex)), batch_.data(), GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(batch_.size()));
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    if (!drawingSolid_) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    batch_.clear();
}

void OpenGLUIRenderBackend::EndRender() {
    Flush();

    // Restore saved OpenGL states
    if (savedDepthTest_) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    glDepthMask(savedDepthMask_ ? GL_TRUE : GL_FALSE);
    if (savedCullFace_) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    if (savedBlend_) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    glBlendFuncSeparate(savedBlendSrcRgb_, savedBlendDstRgb_, savedBlendSrcAlpha_, savedBlendDstAlpha_);
    glActiveTexture(static_cast<GLenum>(savedActiveTexture_));
    glUseProgram(static_cast<GLuint>(savedProgram_));
    glBindVertexArray(static_cast<GLuint>(savedVao_));
}

Vec2 OpenGLUIRenderBackend::DisplaySize() const {
    return {static_cast<float>(framebufferWidth_), static_cast<float>(framebufferHeight_)};
}

Vec2 OpenGLUIRenderBackend::MousePosition() const {
    if (window_ == nullptr) {
        return {};
    }
    double x = 0.0, y = 0.0;
    glfwGetCursorPos(window_, &x, &y);
    int winW = 1, winH = 1;
    glfwGetWindowSize(window_, &winW, &winH);
    const float sx = static_cast<float>(framebufferWidth_) / static_cast<float>(std::max(winW, 1));
    const float sy = static_cast<float>(framebufferHeight_) / static_cast<float>(std::max(winH, 1));
    return {static_cast<float>(x) * sx, static_cast<float>(y) * sy};
}

bool OpenGLUIRenderBackend::IsMouseButtonDown(MouseButton button) const {
    const int idx = static_cast<int>(button);
    return idx >= 0 && idx < 3 && mouseDown_[idx];
}

bool OpenGLUIRenderBackend::WasMouseButtonPressed(MouseButton button) const {
    const int idx = static_cast<int>(button);
    return idx >= 0 && idx < 3 && mousePressed_[idx];
}

bool OpenGLUIRenderBackend::WasMouseButtonReleased(MouseButton button) const {
    const int idx = static_cast<int>(button);
    return idx >= 0 && idx < 3 && mouseReleased_[idx];
}

void OpenGLUIRenderBackend::PushSolidQuad(float x0, float y0, float x1, float y1, Color color) {
    if ((!drawingSolid_ && !batch_.empty()) || batch_.size() >= 4096) {
        Flush();
    }
    drawingSolid_ = true;
    const UiVertex v[6] = {
        {x0, y0, color.r, color.g, color.b, color.a, 0.0F, 0.0F},
        {x1, y0, color.r, color.g, color.b, color.a, 0.0F, 0.0F},
        {x1, y1, color.r, color.g, color.b, color.a, 0.0F, 0.0F},
        {x0, y0, color.r, color.g, color.b, color.a, 0.0F, 0.0F},
        {x1, y1, color.r, color.g, color.b, color.a, 0.0F, 0.0F},
        {x0, y1, color.r, color.g, color.b, color.a, 0.0F, 0.0F},
    };
    batch_.insert(batch_.end(), std::begin(v), std::end(v));
}

void OpenGLUIRenderBackend::PushTexturedQuad(
    float x0, float y0, float x1, float y1,
    Color color,
    float u0, float v0, float u1, float v1
) {
    if ((drawingSolid_ && !batch_.empty()) || batch_.size() >= 4096) {
        Flush();
    }
    drawingSolid_ = false;
    const UiVertex v[6] = {
        {x0, y0, color.r, color.g, color.b, color.a, u0, v0},
        {x1, y0, color.r, color.g, color.b, color.a, u1, v0},
        {x1, y1, color.r, color.g, color.b, color.a, u1, v1},
        {x0, y0, color.r, color.g, color.b, color.a, u0, v0},
        {x1, y1, color.r, color.g, color.b, color.a, u1, v1},
        {x0, y1, color.r, color.g, color.b, color.a, u0, v1},
    };
    batch_.insert(batch_.end(), std::begin(v), std::end(v));
}

void OpenGLUIRenderBackend::DrawFilledRect(const Vec2& min, const Vec2& max, Color color) {
    PushSolidQuad(min.x, min.y, max.x, max.y, color);
}

void OpenGLUIRenderBackend::DrawRectBorder(const Vec2& min, const Vec2& max, Color color, float thickness) {
    DrawFilledRect({min.x, min.y}, {max.x, min.y + thickness}, color);
    DrawFilledRect({min.x, max.y - thickness}, {max.x, max.y}, color);
    DrawFilledRect({min.x, min.y}, {min.x + thickness, max.y}, color);
    DrawFilledRect({max.x - thickness, min.y}, {max.x, max.y}, color);
}

void OpenGLUIRenderBackend::DrawLine(const Vec2& p0, const Vec2& p1, Color color, float thickness) {
    const float halfT = thickness * 0.5F;
    if (std::abs(p0.y - p1.y) < 0.001F) {
        const float xMin = std::min(p0.x, p1.x);
        const float xMax = std::max(p0.x, p1.x);
        DrawFilledRect({xMin, p0.y - halfT}, {xMax, p0.y + halfT}, color);
        return;
    }
    if (std::abs(p0.x - p1.x) < 0.001F) {
        const float yMin = std::min(p0.y, p1.y);
        const float yMax = std::max(p0.y, p1.y);
        DrawFilledRect({p0.x - halfT, yMin}, {p0.x + halfT, yMax}, color);
        return;
    }
    const float dx = p1.x - p0.x;
    const float dy = p1.y - p0.y;
    const float len = std::hypot(dx, dy);
    if (len < 0.0001F) return;
    const float nx = -dy / len * halfT;
    const float ny = dx / len * halfT;

    if ((!drawingSolid_ && !batch_.empty()) || batch_.size() >= 4096) {
        Flush();
    }
    drawingSolid_ = true;

    const UiVertex v[6] = {
        {p0.x + nx, p0.y + ny, color.r, color.g, color.b, color.a, 0.0F, 0.0F},
        {p1.x + nx, p1.y + ny, color.r, color.g, color.b, color.a, 0.0F, 0.0F},
        {p1.x - nx, p1.y - ny, color.r, color.g, color.b, color.a, 0.0F, 0.0F},
        {p0.x + nx, p0.y + ny, color.r, color.g, color.b, color.a, 0.0F, 0.0F},
        {p1.x - nx, p1.y - ny, color.r, color.g, color.b, color.a, 0.0F, 0.0F},
        {p0.x - nx, p0.y - ny, color.r, color.g, color.b, color.a, 0.0F, 0.0F},
    };
    batch_.insert(batch_.end(), std::begin(v), std::end(v));
}

void OpenGLUIRenderBackend::DrawBevelRect(
    const Vec2& min,
    const Vec2& max,
    Color fill,
    Color lightBorder,
    Color darkBorder,
    float thickness
) {
    DrawFilledRect(min, max, fill);
    // Top and Left: light bevel
    DrawFilledRect({min.x, min.y}, {max.x, min.y + thickness}, lightBorder);
    DrawFilledRect({min.x, min.y}, {min.x + thickness, max.y}, lightBorder);
    // Bottom and Right: dark bevel
    DrawFilledRect({min.x, max.y - thickness}, {max.x, max.y}, darkBorder);
    DrawFilledRect({max.x - thickness, min.y}, {max.x, max.y}, darkBorder);
}

float OpenGLUIRenderBackend::MeasureText(
    std::string_view text,
    float fontSize,
    FontStyle style
) const {
    const FontData& font = (style == FontStyle::Monospace) ? fontMono_ : fontRegular_;
    const float scale = fontSize / font.pixelHeight;
    float width = 0.0F;
    for (unsigned char raw : text) {
        if (raw < 32 || raw > 126) {
            if (raw == '\t') width += font.glyphs[' '].advance * scale * 4.0F;
            continue;
        }
        const GlyphInfo& glyph = font.glyphs[raw];
        width += glyph.advance * scale;
    }
    return width;
}

float OpenGLUIRenderBackend::TextLineHeight(
    float fontSize,
    FontStyle style
) const {
    const FontData& font = (style == FontStyle::Monospace) ? fontMono_ : fontRegular_;
    const float scale = fontSize / font.pixelHeight;
    return (font.ascent - font.descent + font.lineGap) * scale;
}

void OpenGLUIRenderBackend::DrawText(
    const Vec2& position,
    std::string_view text,
    float fontSize,
    Color color,
    TextAlign align,
    bool glow
) {
    if (glow) {
        const Color glowCol{color.r, color.g, color.b, color.a * 0.35F};
        for (int dx = -1; dx <= 1; ++dx) {
            for (int dy = -1; dy <= 1; ++dy) {
                if (dx == 0 && dy == 0) continue;
                DrawTextEx(
                    {position.x + static_cast<float>(dx), position.y + static_cast<float>(dy)},
                    text,
                    fontSize,
                    glowCol,
                    align,
                    false,
                    FontStyle::Regular
                );
            }
        }
    }
    DrawTextEx(position, text, fontSize, color, align, false, FontStyle::Regular);
}

void OpenGLUIRenderBackend::DrawTextEx(
    const Vec2& position,
    std::string_view text,
    float fontSize,
    Color color,
    TextAlign align,
    bool shadow,
    FontStyle style
) {
    if (text.empty()) return;
    const FontData& font = (style == FontStyle::Monospace) ? fontMono_ : fontRegular_;
    const float scale = fontSize / font.pixelHeight;
    const float totalWidth = MeasureText(text, fontSize, style);

    float startX = position.x;
    if (align == TextAlign::Center) {
        startX -= totalWidth * 0.5F;
    } else if (align == TextAlign::Right) {
        startX -= totalWidth;
    }

    const float baseline = position.y + font.ascent * scale;

    auto renderString = [&](float xOff, float yOff, Color col) {
        float curX = startX + xOff;
        for (unsigned char raw : text) {
            if (raw < 32 || raw > 126) {
                if (raw == '\t') {
                    curX += font.glyphs[' '].advance * scale * 4.0F;
                }
                continue;
            }
            const GlyphInfo& glyph = font.glyphs[raw];
            if (raw > 32 && glyph.u1 > glyph.u0 && glyph.v1 > glyph.v0) {
                const float x0 = curX + glyph.x0 * scale;
                const float y0 = baseline + glyph.y0 * scale + yOff;
                const float x1 = curX + glyph.x1 * scale;
                const float y1 = baseline + glyph.y1 * scale + yOff;
                PushTexturedQuad(x0, y0, x1, y1, col, glyph.u0, glyph.v0, glyph.u1, glyph.v1);
            }
            curX += glyph.advance * scale;
        }
    };

    if (shadow) {
        const Color shadowCol{0.0F, 0.0F, 0.0F, color.a * 0.85F};
        renderString(1.0F, 1.0F, shadowCol);
    }
    renderString(0.0F, 0.0F, color);
}

} // namespace ui
