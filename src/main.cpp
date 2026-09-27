#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <cmath>
#include <random>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#if defined(__linux__)
#include <unistd.h>
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <glad/gl.h>
#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/vec4.hpp>

#include "AudioSystem.hpp"
#include "BspLoader.hpp"
#include "GameFileSystem.hpp"
#include "GameSimulation.hpp"
#include "Mesh.hpp"
#include "PhysicsWorld.hpp"
#include "PlayerController.hpp"
#include "Shader.hpp"
#include "SourceCoords.hpp"
#include "ShadowManager.hpp"
#include "ui/ChapterCatalog.hpp"
#include "ui/IUIRenderBackend.hpp"
#include "ui/IUISoundBackend.hpp"
#include "ui/MainMenu.hpp"
#include "ui/MenuBackground.hpp"
#include "ui/MenuRenderer.hpp"
#include "ui/OpenGLUIRenderBackend.hpp"
#include "ui/VguiPauseMenu.hpp"
#include "ui/ValveNetGraph.hpp"
#include "ui/EntityLabelRenderer.hpp"
#include "ui/LoadingScreen.hpp"
#include "ui/WavUISoundBackend.hpp"

namespace {
constexpr int kWindowWidth = 1280;
constexpr int kWindowHeight = 720;

struct Camera {
    glm::vec3 position{0.0F, 0.0F, 3.0F};
    float yaw = -90.0F;
    float pitch = 0.0F;
    float lastMouseX = static_cast<float>(kWindowWidth) * 0.5F;
    float lastMouseY = static_cast<float>(kWindowHeight) * 0.5F;
    bool firstMouse = true;
    bool mouseCaptured = true;
    float sensitivity = 0.1F;

    glm::vec3 Front() const {
        const float yawRadians = glm::radians(yaw);
        const float pitchRadians = glm::radians(pitch);
        return glm::normalize(glm::vec3(
            std::cos(yawRadians) * std::cos(pitchRadians),
            std::sin(pitchRadians),
            std::sin(yawRadians) * std::cos(pitchRadians)
        ));
    }
};

struct WindowState {
    bool fullscreen = false;
    bool f11WasPressed = false;
    bool f12WasPressed = false;
    bool f2WasPressed = false;
    bool f3WasPressed = false;
    bool escapeWasPressed = false;
    bool vWasPressed = false;
    bool gWasPressed = false;
    bool hWasPressed = false;
    bool fWasPressed = false;
    bool f6WasPressed = false;
    bool noclip = false;
    int windowedX = 0;
    int windowedY = 0;
    int windowedWidth = kWindowWidth;
    int windowedHeight = kWindowHeight;
};

struct PointLight {
    glm::vec3 position{0.0F};
    glm::vec3 color{1.0F, 0.9F, 0.7F};
    float intensity = 2.0F;
    float constant = 1.0F;
    float linear = 0.09F;
    float quadratic = 0.032F;
};

struct SpotLight {
    glm::vec3 position{0.0F};
    glm::vec3 direction{0.0F, 0.0F, -1.0F};
    glm::vec3 color{1.0F, 1.0F, 1.0F};
    float intensity = 3.5F;
    float innerCutOff = glm::cos(glm::radians(12.5F));
    float outerCutOff = glm::cos(glm::radians(17.5F));
    float constant = 1.0F;
    float linear = 0.07F;
    float quadratic = 0.017F;
    bool enabled = true;
};

constexpr std::size_t kMaxPointLights = 32;
constexpr int kMaxMapPointLights = 24;
constexpr int kMaxMapSpots = 8;

PointLight ToPointLight(const BspMapLight& light) {
    PointLight point;
    point.position = light.position;
    point.color = light.color;
    point.intensity = light.intensity;
    point.constant = light.constant;
    point.linear = light.linear;
    point.quadratic = light.quadratic;
    return point;
}

std::vector<PointLight> SelectFramePointLights(
    const BspLoader* map,
    const std::vector<PointLight>& playerLights,
    const glm::vec3& camera
) {
    std::vector<PointLight> selected;
    if (map != nullptr) {
        std::vector<const BspMapLight*> points;
        for (const BspMapLight& light : map->MapLights()) {
            if (light.kind == BspLightKind::Point && light.intensity > 0.001F) {
                points.push_back(&light);
            }
        }
        std::sort(points.begin(), points.end(), [&camera](const BspMapLight* a, const BspMapLight* b) {
            const glm::vec3 da = a->position - camera;
            const glm::vec3 db = b->position - camera;
            return glm::dot(da, da) < glm::dot(db, db);
        });
        const std::size_t count = std::min(points.size(), static_cast<std::size_t>(kMaxMapPointLights));
        selected.reserve(count + playerLights.size());
        for (std::size_t index = 0; index < count; ++index) {
            selected.push_back(ToPointLight(*points[index]));
        }
    }
    for (const PointLight& light : playerLights) {
        if (selected.size() >= kMaxPointLights) {
            break;
        }
        selected.push_back(light);
    }
    return selected;
}

void UploadMapSpotsAndSun(const Shader& shader, const BspLoader* map, const glm::vec3& camera) {
    if (map == nullptr) {
        shader.SetInt("NumMapSpots", 0);
        shader.SetSunUniform(glm::vec3(0.0F, 1.0F, 0.0F), glm::vec3(1.0F), 0.0F, glm::vec3(0.0F));
        return;
    }
    std::vector<const BspMapLight*> spots;
    for (const BspMapLight& light : map->MapLights()) {
        if (light.kind == BspLightKind::Spot && light.intensity > 0.001F) {
            spots.push_back(&light);
        }
    }
    std::sort(spots.begin(), spots.end(), [&camera](const BspMapLight* a, const BspMapLight* b) {
        const glm::vec3 da = a->position - camera;
        const glm::vec3 db = b->position - camera;
        return glm::dot(da, da) < glm::dot(db, db);
    });
    const int count = static_cast<int>(std::min(spots.size(), static_cast<std::size_t>(kMaxMapSpots)));
    shader.SetInt("NumMapSpots", count);
    for (int index = 0; index < count; ++index) {
        const BspMapLight& light = *spots[static_cast<std::size_t>(index)];
        shader.SetMapSpotUniform(
            index,
            light.position,
            light.direction,
            light.color,
            light.intensity,
            light.innerCutOff,
            light.outerCutOff,
            light.constant,
            light.linear,
            light.quadratic,
            true
        );
    }
    if (map->HasEnvironmentLight()) {
        const BspMapLight& sun = map->EnvironmentLight();
        shader.SetSunUniform(sun.direction, sun.color, sun.intensity, sun.ambient);
    } else {
        shader.SetSunUniform(glm::vec3(0.0F, 1.0F, 0.0F), glm::vec3(1.0F), 0.0F, glm::vec3(0.0F));
    }
}

std::vector<float> WithStyleChannel(const std::vector<float>& source) {
    std::vector<float> vertices;
    vertices.reserve((source.size() / 10U) * Mesh::kFloatsPerVertex);
    for (std::size_t index = 0; index + 9 < source.size(); index += 10) {
        vertices.insert(
            vertices.end(),
            source.begin() + static_cast<std::ptrdiff_t>(index),
            source.begin() + static_cast<std::ptrdiff_t>(index + 10)
        );
        vertices.push_back(0.0F);
    }
    return vertices;
}

void UploadFog(const Shader& shader, const BspFog& fog, const glm::vec3& camera) {
    shader.SetBool("FogEnabled", fog.enabled);
    shader.SetVec3("FogColor", fog.color);
    shader.SetFloat("FogStart", fog.start);
    shader.SetFloat("FogEnd", fog.end);
    shader.SetFloat("FogMaxDensity", fog.maxDensity);
    shader.SetVec3("CameraPos", camera);
}

void UploadLightStyles(const Shader& shader, const BspLoader* map, float timeSeconds) {
    float values[64];
    if (map != nullptr) {
        map->FillLightStyles(timeSeconds, values);
    } else {
        for (float& value : values) {
            value = 1.0F;
        }
    }
    shader.SetFloatArray("LightStyleValues", values, 64);
}

void DrawSkyPasses(
    const Shader& worldShader,
    const Shader& skyShader,
    BspLoader& map,
    GameSimulation* simulation,
    const glm::mat4& view,
    const glm::mat4& projection,
    float fovRadians,
    float aspect,
    const glm::vec3& cameraPosition,
    const glm::vec3& cameraFront,
    const glm::vec3& cameraUp,
    float timeSeconds
) {
    UploadLightStyles(worldShader, &map, timeSeconds);
    if (map.HasSkybox()) {
        const glm::mat4 skyView{glm::mat3(view)};
        skyShader.Use();
        skyShader.SetMat4("View", skyView);
        skyShader.SetMat4("Projection", projection);
        map.DrawSkybox(skyShader);
        worldShader.Use();
    }
    if (map.HasSkyCamera()) {
        const glm::vec3 skyOrigin = map.SkyViewOrigin(cameraPosition);
        const glm::mat4 skyView = glm::lookAt(skyOrigin, skyOrigin + cameraFront, cameraUp);
        const float skyFar = 28400.0F * kSourceToWorld;
        const glm::mat4 skyProjection = glm::perspective(fovRadians, std::max(aspect, 0.001F), 0.005F, skyFar);
        worldShader.SetMat4("View", skyView);
        worldShader.SetMat4("Projection", skyProjection);
        worldShader.SetMat4("Model", glm::mat4(1.0F));
        UploadFog(worldShader, map.SkyCamera().fog, skyOrigin);
        map.DrawSky(worldShader, skyProjection * skyView);
        if (simulation != nullptr) {
            simulation->DrawSky(worldShader);
            simulation->DrawSkyTransparent(worldShader);
        }
        glDisable(GL_BLEND);
        glDepthMask(GL_TRUE);
        glDepthFunc(GL_LESS);
        glClear(GL_DEPTH_BUFFER_BIT);
    }
    worldShader.SetMat4("Model", glm::mat4(1.0F));
    worldShader.SetMat4("View", view);
    worldShader.SetMat4("Projection", projection);
    UploadFog(worldShader, map.WorldFog(), cameraPosition);
}

// Query current process Resident Set Size (RSS) in Megabytes on Linux
float GetProcessRamUsageMb() {
#if defined(__linux__)
    std::ifstream statm("/proc/self/statm");
    if (statm.is_open()) {
        unsigned long size = 0;
        unsigned long resident = 0;
        if (statm >> size >> resident) {
            long pageSizeKb = sysconf(_SC_PAGE_SIZE) / 1024;
            return static_cast<float>(resident * pageSizeKb) / 1024.0F;
        }
    }
#endif
    return 0.0F;
}

// Query VRAM memory usage (dedicated memory info via NVX / ATI extensions if supported)
void GetVramUsageMb(float& usedMb, float& totalMb) {
    usedMb = 0.0F;
    totalMb = 0.0F;

    static int s_vramSupport = -1; // -1: unprobed, 0: unsupported, 1: NVX, 2: ATI
    if (s_vramSupport == 0) {
        return;
    }

    // NVIDIA: GL_NVX_gpu_memory_info
    #define GL_GPU_MEMORY_INFO_TOTAL_AVAILABLE_MEMORY_NVX 0x9048
    #define GL_GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX 0x9049
    // AMD: GL_ATI_meminfo
    #define GL_VBO_FREE_MEMORY_ATI 0x87FB
    #define GL_TEXTURE_FREE_MEMORY_ATI 0x87FC
    #define GL_RENDERBUFFER_FREE_MEMORY_ATI 0x87FD

    if (s_vramSupport == -1 || s_vramSupport == 1) {
        GLint totalKb = 0;
        GLint availKb = 0;
        glGetIntegerv(GL_GPU_MEMORY_INFO_TOTAL_AVAILABLE_MEMORY_NVX, &totalKb);
        if (glGetError() == GL_NO_ERROR && totalKb > 0) {
            glGetIntegerv(GL_GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX, &availKb);
            if (glGetError() == GL_NO_ERROR) {
                s_vramSupport = 1;
                totalMb = static_cast<float>(totalKb) / 1024.0F;
                usedMb = static_cast<float>(totalKb - availKb) / 1024.0F;
                return;
            }
        }
        while (glGetError() != GL_NO_ERROR) {}
    }

    if (s_vramSupport == -1 || s_vramSupport == 2) {
        GLint freeMem[4] = {0};
        glGetIntegerv(GL_VBO_FREE_MEMORY_ATI, freeMem);
        if (glGetError() == GL_NO_ERROR && freeMem[0] > 0) {
            s_vramSupport = 2;
            usedMb = static_cast<float>(freeMem[0]) / 1024.0F;
            return;
        }
        while (glGetError() != GL_NO_ERROR) {}
    }

    s_vramSupport = 0;
}

void ResizeWindow(GLFWwindow*, int width, int height) {
    glViewport(0, 0, width, height);
}

void MouseMove(GLFWwindow* window, double xPosition, double yPosition) {
    auto* camera = static_cast<Camera*>(glfwGetWindowUserPointer(window));
    if (!camera->mouseCaptured) {
        return;
    }
    if (camera->firstMouse) {
        camera->lastMouseX = static_cast<float>(xPosition);
        camera->lastMouseY = static_cast<float>(yPosition);
        camera->firstMouse = false;
    }

    const float xOffset = (static_cast<float>(xPosition) - camera->lastMouseX) * camera->sensitivity;
    const float yOffset = (camera->lastMouseY - static_cast<float>(yPosition)) * camera->sensitivity;
    camera->lastMouseX = static_cast<float>(xPosition);
    camera->lastMouseY = static_cast<float>(yPosition);
    camera->yaw += xOffset;
    camera->pitch = glm::clamp(camera->pitch + yOffset, -89.0F, 89.0F);
}

void ToggleFullscreen(GLFWwindow* window, WindowState& state) {
    if (state.fullscreen) {
        glfwSetWindowMonitor(
            window,
            nullptr,
            state.windowedX,
            state.windowedY,
            state.windowedWidth,
            state.windowedHeight,
            0
        );
        state.fullscreen = false;
        return;
    }

    glfwGetWindowPos(window, &state.windowedX, &state.windowedY);
    glfwGetWindowSize(window, &state.windowedWidth, &state.windowedHeight);
    GLFWmonitor* monitor = glfwGetPrimaryMonitor();
    const GLFWvidmode* mode = glfwGetVideoMode(monitor);
    if (mode != nullptr) {
        glfwSetWindowMonitor(window, monitor, 0, 0, mode->width, mode->height, mode->refreshRate);
        state.fullscreen = true;
    }
}

struct MenuKeyState {
    bool upWas = false;
    bool downWas = false;
    bool leftWas = false;
    bool rightWas = false;
    bool enterWas = false;
    bool escapeWas = false;
    bool gamepadConfirmWas = false;
    bool gamepadBackWas = false;
};

ui::MenuInput BuildMenuInput(GLFWwindow* window, MenuKeyState& keys, ui::IUIRenderBackend& backend) {
    ui::MenuInput input{};
    const auto edge = [&](int glfwKey, bool& wasPressed) {
        const bool pressed = glfwGetKey(window, glfwKey) == GLFW_PRESS;
        const bool edgeTriggered = pressed && !wasPressed;
        wasPressed = pressed;
        return edgeTriggered;
    };

    input.upPressed = edge(GLFW_KEY_UP, keys.upWas);
    input.downPressed = edge(GLFW_KEY_DOWN, keys.downWas);
    input.leftPressed = edge(GLFW_KEY_LEFT, keys.leftWas);
    input.rightPressed = edge(GLFW_KEY_RIGHT, keys.rightWas);
    const bool enterDown = glfwGetKey(window, GLFW_KEY_ENTER) == GLFW_PRESS
        || glfwGetKey(window, GLFW_KEY_KP_ENTER) == GLFW_PRESS;
    input.confirmPressed = enterDown && !keys.enterWas;
    keys.enterWas = enterDown;

    input.backPressed = edge(GLFW_KEY_ESCAPE, keys.escapeWas);

    if (glfwJoystickPresent(GLFW_JOYSTICK_1) == GLFW_TRUE) {
        int axisCount = 0;
        const float* axes = glfwGetJoystickAxes(GLFW_JOYSTICK_1, &axisCount);
        if (axes != nullptr && axisCount >= 2) {
            const float deadZone = 0.35F;
            if (axes[1] < -deadZone) {
                input.upPressed = true;
            }
            if (axes[1] > deadZone) {
                input.downPressed = true;
            }
            if (axes[0] < -deadZone) {
                input.leftPressed = true;
            }
            if (axes[0] > deadZone) {
                input.rightPressed = true;
            }
        }
        int buttonCount = 0;
        const unsigned char* buttons = glfwGetJoystickButtons(GLFW_JOYSTICK_1, &buttonCount);
        const bool gamepadConfirmDown = buttons != nullptr && buttonCount > 0 && buttons[0] == GLFW_PRESS;
        if (gamepadConfirmDown && !keys.gamepadConfirmWas) {
            input.confirmPressed = true;
        }
        keys.gamepadConfirmWas = gamepadConfirmDown;

        const bool gamepadBackDown = buttons != nullptr && buttonCount > 1 && buttons[1] == GLFW_PRESS;
        if (gamepadBackDown && !keys.gamepadBackWas) {
            input.backPressed = true;
        }
        keys.gamepadBackWas = gamepadBackDown;
    }

    input.mouseLeftPressed = backend.WasMouseButtonPressed(ui::MouseButton::Left);
    input.mouseLeftDown = backend.IsMouseButtonDown(ui::MouseButton::Left);
    input.mouseLeftReleased = backend.WasMouseButtonReleased(ui::MouseButton::Left);
    const ui::Vec2 mouse = backend.MousePosition();
    input.mousePosition = mouse;
    input.displaySize = backend.DisplaySize();
    input.mouseValid = mouse.x >= 0.0F && mouse.y >= 0.0F;
    return input;
}

class DebugTriangleMesh {
public:
    DebugTriangleMesh(const std::vector<glm::vec3>& vertices, const std::vector<std::uint32_t>& indices)
        : indexCount_(static_cast<GLsizei>(indices.size())) {
        glGenVertexArrays(1, &vao_);
        glGenBuffers(1, &vbo_);
        glGenBuffers(1, &ebo_);
        glBindVertexArray(vao_);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glBufferData(
            GL_ARRAY_BUFFER,
            static_cast<GLsizeiptr>(vertices.size() * sizeof(glm::vec3)),
            vertices.data(),
            GL_STATIC_DRAW
        );
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo_);
        glBufferData(
            GL_ELEMENT_ARRAY_BUFFER,
            static_cast<GLsizeiptr>(indices.size() * sizeof(std::uint32_t)),
            indices.data(),
            GL_STATIC_DRAW
        );
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), nullptr);
        glEnableVertexAttribArray(0);
        glBindVertexArray(0);
    }

    ~DebugTriangleMesh() {
        glDeleteBuffers(1, &ebo_);
        glDeleteBuffers(1, &vbo_);
        glDeleteVertexArrays(1, &vao_);
    }

    DebugTriangleMesh(const DebugTriangleMesh&) = delete;
    DebugTriangleMesh& operator=(const DebugTriangleMesh&) = delete;

    void Draw() const {
        glBindVertexArray(vao_);
        glDrawElements(GL_TRIANGLES, indexCount_, GL_UNSIGNED_INT, nullptr);
        glBindVertexArray(0);
    }

private:
    GLuint vao_ = 0;
    GLuint vbo_ = 0;
    GLuint ebo_ = 0;
    GLsizei indexCount_ = 0;
};

void DrawBlueWireframe(
    const Shader& shader,
    const DebugTriangleMesh& mesh,
    const glm::mat4& model,
    const glm::mat4& view,
    const glm::mat4& projection
) {
    shader.Use();
    shader.SetMat4("Model", model);
    shader.SetMat4("View", view);
    shader.SetMat4("Projection", projection);
    shader.SetVec3("Color", glm::vec3(0.15F, 0.45F, 1.0F));
    mesh.Draw();
}

void SetMouseCaptured(GLFWwindow* window, Camera& camera, bool captured) {
    camera.mouseCaptured = captured;
    camera.firstMouse = true;
    glfwSetInputMode(window, GLFW_CURSOR, captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
}

void ProcessInput(
    GLFWwindow* window,
    Camera& camera,
    WindowState& state,
    std::vector<PointLight>& pointLights,
    SpotLight& flashlight,
    const glm::vec3& playerPos,
    PhysicsWorld& physicsWorld,
    bool& showDebugOverlay,
    bool& showEntitySpawnLabels,
    bool& showCollisionMesh
) {
    const bool f12Pressed = glfwGetKey(window, GLFW_KEY_F12) == GLFW_PRESS;
    if (f12Pressed && !state.f12WasPressed) {
        camera.mouseCaptured = !camera.mouseCaptured;
        camera.firstMouse = true;
        glfwSetInputMode(
            window,
            GLFW_CURSOR,
            camera.mouseCaptured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL
        );
    }
    state.f12WasPressed = f12Pressed;

    const bool vPressed = glfwGetKey(window, GLFW_KEY_V) == GLFW_PRESS;
    if (vPressed && !state.vWasPressed) {
        state.noclip = !state.noclip;
    }
    state.vWasPressed = vPressed;

    const bool f2Pressed = glfwGetKey(window, GLFW_KEY_F2) == GLFW_PRESS;
    if (f2Pressed && !state.f2WasPressed) {
        showCollisionMesh = !showCollisionMesh;
    }
    state.f2WasPressed = f2Pressed;

    const bool f3Pressed = glfwGetKey(window, GLFW_KEY_F3) == GLFW_PRESS;
    if (f3Pressed && !state.f3WasPressed) {
        showDebugOverlay = !showDebugOverlay;
    }
    state.f3WasPressed = f3Pressed;

    const bool f6Pressed = glfwGetKey(window, GLFW_KEY_F6) == GLFW_PRESS;
    if (f6Pressed && !state.f6WasPressed) {
        showEntitySpawnLabels = !showEntitySpawnLabels;
    }
    state.f6WasPressed = f6Pressed;

    // Flashlight toggle with 'F'
    const bool fPressed = glfwGetKey(window, GLFW_KEY_F) == GLFW_PRESS;
    if (fPressed && !state.fWasPressed) {
        flashlight.enabled = !flashlight.enabled;
    }
    state.fWasPressed = fPressed;

    // Static point light spawn with 'G'
    const bool gPressed = glfwGetKey(window, GLFW_KEY_G) == GLFW_PRESS;
    if (gPressed && !state.gWasPressed) {
        if (pointLights.size() < kMaxPointLights) {
            PointLight newLight;
            newLight.position = playerPos;
            pointLights.push_back(newLight);
        } else {
            // Replace oldest light when max is reached
            PointLight newLight;
            newLight.position = playerPos;
            pointLights.erase(pointLights.begin());
            pointLights.push_back(newLight);
        }
    }
    state.gWasPressed = gPressed;

    const bool hPressed = glfwGetKey(window, GLFW_KEY_H) == GLFW_PRESS;
    if (hPressed && !state.hWasPressed) {
        // Spawn dynamic prop crate in front of player and throw forward
        const glm::vec3 spawnPosition = playerPos + camera.Front() * 1.5F;
        const glm::vec3 throwVelocity = camera.Front() * 6.0F + glm::vec3(0.0F, 1.5F, 0.0F);
        physicsWorld.SpawnDynamicBox(spawnPosition, throwVelocity);
    }
    state.hWasPressed = hPressed;
}

struct LaunchOptions {
    std::string game = "hl2";
    bool autoStart = false;
    std::filesystem::path mapPath;
};

LaunchOptions ParseLaunchOptions(int argc, char* argv[]) {
    LaunchOptions options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "-game" || argument == "--game") {
            if (index + 1 < argc) {
                options.game = argv[++index];
            }
        } else if (argument == "--autostart") {
            options.autoStart = true;
        } else if (argument == "+map") {
            if (index + 1 < argc) {
                options.mapPath = argv[++index];
            }
        } else if (options.mapPath.empty() && !argument.empty() && argument[0] != '-' && argument[0] != '+') {
            options.mapPath = argument;
        }
    }
    return options;
}

std::filesystem::path ExecutableDirectory() {
#if defined(_WIN32)
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length > 0 && length < buffer.size()) {
        buffer.resize(length);
        return std::filesystem::path(buffer).parent_path();
    }
#elif defined(__linux__)
    std::error_code error;
    const std::filesystem::path executable = std::filesystem::read_symlink("/proc/self/exe", error);
    if (!error) {
        return executable.parent_path();
    }
#endif
    return std::filesystem::current_path();
}
} // namespace

int main(int argc, char* argv[]) {
    if (glfwInit() == GLFW_FALSE) {
        std::cerr << "Failed to initialize GLFW\n";
        return EXIT_FAILURE;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#endif

    GLFWwindow* window = glfwCreateWindow(
        kWindowWidth,
        kWindowHeight,
        "Blueprint OpenGL",
        nullptr,
        nullptr
    );

    if (window == nullptr) {
        std::cerr << "Failed to create GLFW window\n";
        glfwTerminate();
        return EXIT_FAILURE;
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    glfwSetFramebufferSizeCallback(window, ResizeWindow);
    Camera camera;
    WindowState windowState;
    glfwSetWindowUserPointer(window, &camera);
    glfwSetCursorPosCallback(window, MouseMove);
    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    camera.mouseCaptured = false;

    if (gladLoadGL(glfwGetProcAddress) == 0) {
        std::cerr << "Failed to load OpenGL functions\n";
        glfwDestroyWindow(window);
        glfwTerminate();
        return EXIT_FAILURE;
    }

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    const std::vector<float> vertices = {
        -0.5F, -0.5F, -0.5F, 1.0F, 0.2F, 0.2F, 0.0F, 0.0F, 0.0F, 0.0F,
         0.5F, -0.5F, -0.5F, 0.2F, 1.0F, 0.2F, 1.0F, 0.0F, 1.0F, 0.0F,
         0.5F,  0.5F, -0.5F, 0.2F, 0.2F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F,
        -0.5F,  0.5F, -0.5F, 1.0F, 1.0F, 0.2F, 0.0F, 1.0F, 0.0F, 1.0F,
        -0.5F, -0.5F,  0.5F, 1.0F, 0.2F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F,
         0.5F, -0.5F,  0.5F, 0.2F, 1.0F, 1.0F, 1.0F, 0.0F, 1.0F, 0.0F,
         0.5F,  0.5F,  0.5F, 1.0F, 0.5F, 0.2F, 1.0F, 1.0F, 1.0F, 1.0F,
        -0.5F,  0.5F,  0.5F, 0.2F, 0.5F, 1.0F, 0.0F, 1.0F, 0.0F, 1.0F,
    };

    const std::vector<unsigned int> indices = {
        0, 1, 2, 2, 3, 0,
        4, 5, 6, 6, 7, 4,
        0, 4, 7, 7, 3, 0,
        1, 5, 6, 6, 2, 1,
        3, 2, 6, 6, 7, 3,
        0, 1, 5, 5, 4, 0,
    };

    // 24 vertices for crate mesh with distinct face colors (wooden crate look)
    const std::vector<float> crateVertices = {
        // Front face (Z = +0.5) - warm orange wood
        -0.5F, -0.5F,  0.5F,  0.88F, 0.54F, 0.22F,  0.0F, 0.0F,  0.0F, 0.0F,
         0.5F, -0.5F,  0.5F,  0.88F, 0.54F, 0.22F,  1.0F, 0.0F,  1.0F, 0.0F,
         0.5F,  0.5F,  0.5F,  0.88F, 0.54F, 0.22F,  1.0F, 1.0F,  1.0F, 1.0F,
        -0.5F,  0.5F,  0.5F,  0.88F, 0.54F, 0.22F,  0.0F, 1.0F,  0.0F, 1.0F,
        // Back face (Z = -0.5) - slightly darker wood
        -0.5F, -0.5F, -0.5F,  0.80F, 0.48F, 0.18F,  1.0F, 0.0F,  1.0F, 0.0F,
        -0.5F,  0.5F, -0.5F,  0.80F, 0.48F, 0.18F,  1.0F, 1.0F,  1.0F, 1.0F,
         0.5F,  0.5F, -0.5F,  0.80F, 0.48F, 0.18F,  0.0F, 1.0F,  0.0F, 1.0F,
         0.5F, -0.5F, -0.5F,  0.80F, 0.48F, 0.18F,  0.0F, 0.0F,  0.0F, 0.0F,
        // Top face (Y = +0.5) - lighter wood
        -0.5F,  0.5F, -0.5F,  0.95F, 0.62F, 0.28F,  0.0F, 1.0F,  0.0F, 1.0F,
        -0.5F,  0.5F,  0.5F,  0.95F, 0.62F, 0.28F,  0.0F, 0.0F,  0.0F, 0.0F,
         0.5F,  0.5F,  0.5F,  0.95F, 0.62F, 0.28F,  1.0F, 0.0F,  1.0F, 0.0F,
         0.5F,  0.5F, -0.5F,  0.95F, 0.62F, 0.28F,  1.0F, 1.0F,  1.0F, 1.0F,
        // Bottom face (Y = -0.5) - shadow wood
        -0.5F, -0.5F, -0.5F,  0.65F, 0.38F, 0.15F,  0.0F, 0.0F,  0.0F, 0.0F,
         0.5F, -0.5F, -0.5F,  0.65F, 0.38F, 0.15F,  1.0F, 0.0F,  1.0F, 0.0F,
         0.5F, -0.5F,  0.5F,  0.65F, 0.38F, 0.15F,  1.0F, 1.0F,  1.0F, 1.0F,
        -0.5F, -0.5F,  0.5F,  0.65F, 0.38F, 0.15F,  0.0F, 1.0F,  0.0F, 1.0F,
        // Right face (X = +0.5)
         0.5F, -0.5F, -0.5F,  0.84F, 0.50F, 0.20F,  1.0F, 0.0F,  1.0F, 0.0F,
         0.5F,  0.5F, -0.5F,  0.84F, 0.50F, 0.20F,  1.0F, 1.0F,  1.0F, 1.0F,
         0.5F,  0.5F,  0.5F,  0.84F, 0.50F, 0.20F,  0.0F, 1.0F,  0.0F, 1.0F,
         0.5F, -0.5F,  0.5F,  0.84F, 0.50F, 0.20F,  0.0F, 0.0F,  0.0F, 0.0F,
        // Left face (X = -0.5)
        -0.5F, -0.5F, -0.5F,  0.78F, 0.46F, 0.18F,  0.0F, 0.0F,  0.0F, 0.0F,
        -0.5F, -0.5F,  0.5F,  0.78F, 0.46F, 0.18F,  1.0F, 0.0F,  1.0F, 0.0F,
        -0.5F,  0.5F,  0.5F,  0.78F, 0.46F, 0.18F,  1.0F, 1.0F,  1.0F, 1.0F,
        -0.5F,  0.5F, -0.5F,  0.78F, 0.46F, 0.18F,  0.0F, 1.0F,  0.0F, 1.0F,
    };

    const std::vector<unsigned int> crateIndices = {
        0,  1,  2,  2,  3,  0,
        4,  5,  6,  6,  7,  4,
        8,  9, 10, 10, 11,  8,
       12, 13, 14, 14, 15, 12,
       16, 17, 18, 18, 19, 16,
       20, 21, 22, 22, 23, 20,
    };

    const std::vector<float> roomVertices = {
        // Floor (Y = 0.0) - neutral warm gray
        -14.0F, 0.0F, -14.0F,  0.50F, 0.52F, 0.55F,  0.0F,  0.0F,  0.0F, 0.0F,
         14.0F, 0.0F, -14.0F,  0.50F, 0.52F, 0.55F, 14.0F,  0.0F, 14.0F, 0.0F,
         14.0F, 0.0F,  14.0F,  0.50F, 0.52F, 0.55F, 14.0F, 14.0F, 14.0F, 14.0F,
        -14.0F, 0.0F,  14.0F,  0.50F, 0.52F, 0.55F,  0.0F, 14.0F,  0.0F, 14.0F,

        // Back Wall (Z = -14.0) - slate gray
        -14.0F, 0.0F, -14.0F,  0.38F, 0.40F, 0.44F,  0.0F, 0.0F,  0.0F, 0.0F,
        -14.0F, 7.0F, -14.0F,  0.38F, 0.40F, 0.44F,  0.0F, 7.0F,  0.0F, 7.0F,
         14.0F, 7.0F, -14.0F,  0.38F, 0.40F, 0.44F, 14.0F, 7.0F, 14.0F, 7.0F,
         14.0F, 0.0F, -14.0F,  0.38F, 0.40F, 0.44F, 14.0F, 0.0F, 14.0F, 0.0F,

        // Front Wall (Z = 14.0) - slate gray
         14.0F, 0.0F,  14.0F,  0.38F, 0.40F, 0.44F,  0.0F, 0.0F,  0.0F, 0.0F,
         14.0F, 7.0F,  14.0F,  0.38F, 0.40F, 0.44F,  0.0F, 7.0F,  0.0F, 7.0F,
        -14.0F, 7.0F,  14.0F,  0.38F, 0.40F, 0.44F, 14.0F, 7.0F, 14.0F, 7.0F,
        -14.0F, 0.0F,  14.0F,  0.38F, 0.40F, 0.44F, 14.0F, 0.0F, 14.0F, 0.0F,

        // Left Wall (X = -14.0) - dark slate
        -14.0F, 0.0F,  14.0F,  0.35F, 0.37F, 0.41F,  0.0F, 0.0F,  0.0F, 0.0F,
        -14.0F, 7.0F,  14.0F,  0.35F, 0.37F, 0.41F,  0.0F, 7.0F,  0.0F, 7.0F,
        -14.0F, 7.0F, -14.0F,  0.35F, 0.37F, 0.41F, 14.0F, 7.0F, 14.0F, 7.0F,
        -14.0F, 0.0F, -14.0F,  0.35F, 0.37F, 0.41F, 14.0F, 0.0F, 14.0F, 0.0F,

        // Right Wall (X = 14.0) - dark slate
         14.0F, 0.0F, -14.0F,  0.35F, 0.37F, 0.41F,  0.0F, 0.0F,  0.0F, 0.0F,
         14.0F, 7.0F, -14.0F,  0.35F, 0.37F, 0.41F,  0.0F, 7.0F,  0.0F, 7.0F,
         14.0F, 7.0F,  14.0F,  0.35F, 0.37F, 0.41F, 14.0F, 7.0F, 14.0F, 7.0F,
         14.0F, 0.0F,  14.0F,  0.35F, 0.37F, 0.41F, 14.0F, 0.0F, 14.0F, 0.0F,

        // Pillar / pedestal 1 at (-4.0, 1.0, -4.0), size 1.5 x 2.0 x 1.5
        // Top face
        -4.75F, 2.0F, -4.75F,  0.60F, 0.58F, 0.54F, 0.0F, 0.0F, 0.0F, 0.0F,
        -3.25F, 2.0F, -4.75F,  0.60F, 0.58F, 0.54F, 1.0F, 0.0F, 1.0F, 0.0F,
        -3.25F, 2.0F, -3.25F,  0.60F, 0.58F, 0.54F, 1.0F, 1.0F, 1.0F, 1.0F,
        -4.75F, 2.0F, -3.25F,  0.60F, 0.58F, 0.54F, 0.0F, 1.0F, 0.0F, 1.0F,
        // Front face
        -4.75F, 0.0F, -3.25F,  0.48F, 0.46F, 0.42F, 0.0F, 0.0F, 0.0F, 0.0F,
        -3.25F, 0.0F, -3.25F,  0.48F, 0.46F, 0.42F, 1.0F, 0.0F, 1.0F, 0.0F,
        -3.25F, 2.0F, -3.25F,  0.48F, 0.46F, 0.42F, 1.0F, 1.0F, 1.0F, 1.0F,
        -4.75F, 2.0F, -3.25F,  0.48F, 0.46F, 0.42F, 0.0F, 1.0F, 0.0F, 1.0F,
        // Right face
        -3.25F, 0.0F, -3.25F,  0.45F, 0.43F, 0.39F, 0.0F, 0.0F, 0.0F, 0.0F,
        -3.25F, 0.0F, -4.75F,  0.45F, 0.43F, 0.39F, 1.0F, 0.0F, 1.0F, 0.0F,
        -3.25F, 2.0F, -4.75F,  0.45F, 0.43F, 0.39F, 1.0F, 1.0F, 1.0F, 1.0F,
        -3.25F, 2.0F, -3.25F,  0.45F, 0.43F, 0.39F, 0.0F, 1.0F, 0.0F, 1.0F,

        // Pillar / pedestal 2 at (4.0, 0.75, 4.0), size 1.5 x 1.5 x 1.5
        // Top face
         3.25F, 1.5F,  3.25F,  0.60F, 0.58F, 0.54F, 0.0F, 0.0F, 0.0F, 0.0F,
         4.75F, 1.5F,  3.25F,  0.60F, 0.58F, 0.54F, 1.0F, 0.0F, 1.0F, 0.0F,
         4.75F, 1.5F,  4.75F,  0.60F, 0.58F, 0.54F, 1.0F, 1.0F, 1.0F, 1.0F,
         3.25F, 1.5F,  4.75F,  0.60F, 0.58F, 0.54F, 0.0F, 1.0F, 0.0F, 1.0F,
        // Front face
         3.25F, 0.0F,  4.75F,  0.48F, 0.46F, 0.42F, 0.0F, 0.0F, 0.0F, 0.0F,
         4.75F, 0.0F,  4.75F,  0.48F, 0.46F, 0.42F, 1.0F, 0.0F, 1.0F, 0.0F,
         4.75F, 1.5F,  4.75F,  0.48F, 0.46F, 0.42F, 1.0F, 1.0F, 1.0F, 1.0F,
         3.25F, 1.5F,  4.75F,  0.48F, 0.46F, 0.42F, 0.0F, 1.0F, 0.0F, 1.0F,
        // Left face
         3.25F, 0.0F,  3.25F,  0.45F, 0.43F, 0.39F, 0.0F, 0.0F, 0.0F, 0.0F,
         3.25F, 0.0F,  4.75F,  0.45F, 0.43F, 0.39F, 1.0F, 0.0F, 1.0F, 0.0F,
         3.25F, 1.5F,  4.75F,  0.45F, 0.43F, 0.39F, 1.0F, 1.0F, 1.0F, 1.0F,
         3.25F, 1.5F,  3.25F,  0.45F, 0.43F, 0.39F, 0.0F, 1.0F, 0.0F, 1.0F,
    };

    const std::vector<unsigned int> roomIndices = {
        0, 1, 2, 2, 3, 0,       // Floor
        4, 5, 6, 6, 7, 4,       // Back wall
        8, 9, 10, 10, 11, 8,    // Front wall
        12, 13, 14, 14, 15, 12, // Left wall
        16, 17, 18, 18, 19, 16, // Right wall
        20, 21, 22, 22, 23, 20, // Pillar 1 top
        24, 25, 26, 26, 27, 24, // Pillar 1 front
        28, 29, 30, 30, 31, 28, // Pillar 1 right
        32, 33, 34, 34, 35, 32, // Pillar 2 top
        36, 37, 38, 38, 39, 36, // Pillar 2 front
        40, 41, 42, 42, 43, 40, // Pillar 2 left
    };

    try {
        ui::OpenGLUIRenderBackend uiBackend;
        if (!uiBackend.Initialize(window, BLUEPRINT_SOURCE_DIR)) {
            std::cerr << "Failed to initialize OpenGL UI Backend\n";
            glfwDestroyWindow(window);
            glfwTerminate();
            return EXIT_FAILURE;
        }

        Shader shader(
            std::string(BLUEPRINT_SOURCE_DIR) + "/shaders/basic.vert",
            std::string(BLUEPRINT_SOURCE_DIR) + "/shaders/basic.frag"
        );
        shader.CacheLightUniformLocations();
        Shader skyShader(
            std::string(BLUEPRINT_SOURCE_DIR) + "/shaders/skybox.vert",
            std::string(BLUEPRINT_SOURCE_DIR) + "/shaders/skybox.frag"
        );
        Shader debugShader(
            std::string(BLUEPRINT_SOURCE_DIR) + "/shaders/debug_color.vert",
            std::string(BLUEPRINT_SOURCE_DIR) + "/shaders/debug_color.frag"
        );

        ShadowManager shadowManager;
        shadowManager.Init(BLUEPRINT_SOURCE_DIR);

        const LaunchOptions launchOptions = ParseLaunchOptions(argc, argv);
        auto gameFiles = std::make_unique<GameFileSystem>();
        const std::filesystem::path gameDirectory = std::filesystem::path(launchOptions.game).is_absolute()
            ? std::filesystem::path(launchOptions.game)
            : ExecutableDirectory() / "game" / launchOptions.game;
        const bool gameMounted = gameFiles->Mount(gameDirectory);
        if (!gameMounted) {
            gameFiles.reset();
        } else {
            std::cout << "Game: " << launchOptions.game << " (" << gameDirectory << ")\n";
        }

        const std::string gameLabel = gameFiles != nullptr ? gameFiles->GameLabel() : std::string{};
        glfwSetWindowTitle(window, gameLabel.empty() ? "BLUEPRINT ENGINE" : gameLabel.c_str());
        std::vector<ui::ChapterInfo> chapters;
        if (gameFiles != nullptr) {
            chapters = ui::LoadChapters(*gameFiles);
        }

        std::vector<std::filesystem::path> mapFiles;
        std::vector<std::filesystem::path> backgroundMaps;
        const auto addMapFile = [&](const std::filesystem::path& path) {
            if (ui::IsBackgroundMapFile(path)) {
                backgroundMaps.push_back(path);
            } else {
                mapFiles.push_back(path);
            }
        };
        if (gameFiles) {
            for (const std::string& relative : gameFiles->List("maps/", ".bsp")) {
                addMapFile(relative);
            }
        }
        if (mapFiles.empty() && backgroundMaps.empty()) {
            const std::filesystem::path mapsDirectory = std::filesystem::path(BLUEPRINT_SOURCE_DIR) / "maps";
            if (std::filesystem::exists(mapsDirectory)) {
                for (const auto& entry : std::filesystem::directory_iterator(mapsDirectory)) {
                    if (entry.is_regular_file() && entry.path().extension() == ".bsp") {
                        addMapFile(entry.path());
                    }
                }
            }
        }
        std::sort(mapFiles.begin(), mapFiles.end());
        std::sort(backgroundMaps.begin(), backgroundMaps.end());
        int selectedMap = 0;
        const bool autoStart = launchOptions.autoStart;
        if (!launchOptions.mapPath.empty()) {
            const std::filesystem::path requestedMap = launchOptions.mapPath;
            for (std::size_t index = 0; index < mapFiles.size(); ++index) {
                if (mapFiles[index].filename() == requestedMap
                    || mapFiles[index].stem() == requestedMap
                    || mapFiles[index] == requestedMap) {
                    selectedMap = static_cast<int>(index);
                    break;
                }
            }
        }

        std::unique_ptr<BspLoader> bspMap;
        std::unique_ptr<GameSimulation> gameSim;
        std::unique_ptr<Mesh> cube;
        std::unique_ptr<Mesh> demoRoom;
        std::unique_ptr<Mesh> propCube;
        PhysicsWorld physicsWorld;
        PlayerController player;
        player.SetWindow(window);
        std::vector<PointLight> pointLights;
        SpotLight flashlight;
        enum class Screen { MainMenu, Playing, Paused };
        Screen screen = Screen::MainMenu;
        bool startRequested = autoStart;
        bool mapTestRequested = false;
        bool returnToMenuRequested = false;
        std::filesystem::path chapterMapPath;

        AudioSystem audio;
        const std::filesystem::path contentRoot = BLUEPRINT_SOURCE_DIR;
        const bool audioReady = audio.Initialize(contentRoot, gameFiles.get());
        ui::NullUISoundBackend nullMenuSound;
        ui::WavUISoundBackend menuSound(&audio);
        ui::IUISoundBackend* menuSoundBackend = audioReady ? static_cast<ui::IUISoundBackend*>(&menuSound)
                                                           : static_cast<ui::IUISoundBackend*>(&nullMenuSound);
        ui::MainMenu mainMenu;
        ui::MenuRenderer menuRenderer;
        ui::VguiPauseMenu pauseMenu;
        ui::LoadingScreen loadingScreen;
        ui::ValveNetGraph netGraph;
        ui::EntityLabelRenderer entityLabelRenderer;
        MenuKeyState menuKeys;
        mainMenu.Initialize(menuSoundBackend);
        pauseMenu.Initialize(menuSoundBackend);
        mainMenu.SetChapterCount(static_cast<int>(chapters.size()));
        mainMenu.SetMapCount(static_cast<int>(mapFiles.size()) + 1);
        mainMenu.SyncOptionsFromEngine(windowState.fullscreen, camera.sensitivity, selectedMap);
        std::unique_ptr<Mesh> menuBackdrop = std::make_unique<Mesh>(WithStyleChannel(roomVertices), roomIndices);
        std::unique_ptr<BspLoader> menuMap;
        std::unique_ptr<PhysicsWorld> menuPhysics;
        std::unique_ptr<GameSimulation> menuSimulation;
        std::mt19937 menuRng{std::random_device{}()};
        const auto unloadMenuBackground = [&]() {
            menuSimulation.reset();
            menuPhysics.reset();
            menuMap.reset();
        };
        const auto loadMenuBackground = [&]() {
            unloadMenuBackground();
            if (backgroundMaps.empty()) {
                return;
            }
            std::uniform_int_distribution<std::size_t> distribution(0, backgroundMaps.size() - 1);
            const std::filesystem::path chosen = backgroundMaps[distribution(menuRng)];
            try {
                std::filesystem::path mapPath = chosen;
                if (gameFiles && !mapPath.is_absolute()) {
                    mapPath = gameFiles->Materialize(mapPath.generic_string());
                }
                menuMap = std::make_unique<BspLoader>(mapPath.string(), gameFiles.get());
                menuPhysics = std::make_unique<PhysicsWorld>();
                menuSimulation = std::make_unique<GameSimulation>(*menuMap, *menuPhysics, gameFiles.get(), &audio);
                std::cout << "Menu background: " << chosen.filename().string() << std::endl;
            } catch (const std::exception& error) {
                std::cerr << "Menu background failed: " << error.what() << std::endl;
                unloadMenuBackground();
            }
        };
        loadMenuBackground();
        std::vector<std::string> mapLabels;
        mapLabels.reserve(mapFiles.size() + 1);
        for (const auto& mapPath : mapFiles) {
            mapLabels.push_back(mapPath.filename().string());
        }
        mapLabels.emplace_back("Demo Cube");

        bool showDebugOverlay = true;
        bool showEntitySpawnLabels = false;
        bool showCollisionMesh = false;
        struct CachedCollisionMesh {
            std::uint32_t revision = 0;
            std::unique_ptr<DebugTriangleMesh> mesh;
        };
        std::unordered_map<std::uint64_t, CachedCollisionMesh> collisionDebugMeshes;
        float frameTimeAccumulator = 0.0F;
        int frameCountAccumulator = 0;
        float displayedFps = 0.0F;
        float displayedFrameTimeMs = 0.0F;
        float displayedRamMb = 0.0F;
        float displayedVramUsedMb = 0.0F;
        float displayedVramTotalMb = 0.0F;
        float ramCheckTimer = 0.0F;

        const char* glVendor = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
        const char* glRenderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
        const std::string gpuName = glRenderer ? std::string(glRenderer) : (glVendor ? std::string(glVendor) : "OpenGL GPU");

        float previousTime = static_cast<float>(glfwGetTime());
        glm::vec3 footstepPreviousPosition = player.Position();
        float footstepDistanceAccumulator = 0.0F;
        constexpr float kFootstepDistance = 1.55F;
        while (glfwWindowShouldClose(window) == GLFW_FALSE) {
            int framebufferWidth = 0;
            int framebufferHeight = 0;
            glfwGetFramebufferSize(window, &framebufferWidth, &framebufferHeight);
            const float aspect = framebufferHeight > 0
                ? static_cast<float>(framebufferWidth) / static_cast<float>(framebufferHeight)
                : 1.0F;

            uiBackend.UpdateInput(framebufferWidth, framebufferHeight);

            const bool f11Pressed = glfwGetKey(window, GLFW_KEY_F11) == GLFW_PRESS;
            if (f11Pressed && !windowState.f11WasPressed) {
                ToggleFullscreen(window, windowState);
            }
            windowState.f11WasPressed = f11Pressed;

            const float time = static_cast<float>(glfwGetTime());
            const float deltaTime = time - previousTime;
            previousTime = time;

            if (screen == Screen::MainMenu) {
                SetMouseCaptured(window, camera, false);
                mainMenu.Update(deltaTime);
                const ui::MenuInput menuInput = BuildMenuInput(window, menuKeys, uiBackend);
                const ui::MenuAction menuAction = mainMenu.HandleInput(menuInput);
                if (menuAction == ui::MenuAction::OpenOptions) {
                    mainMenu.SyncOptionsFromEngine(windowState.fullscreen, camera.sensitivity, selectedMap);
                }
                if (menuAction == ui::MenuAction::CloseOptions) {
                    const ui::OptionsSettings& applied = mainMenu.Options();
                    camera.sensitivity = applied.mouseSensitivity;
                    selectedMap = std::min(applied.selectedMapIndex, static_cast<int>(mapFiles.size()));
                    if (applied.fullscreen != windowState.fullscreen) {
                        ToggleFullscreen(window, windowState);
                    }
                }
                if (menuAction == ui::MenuAction::NewGame) {
                    selectedMap = std::min(mainMenu.Options().selectedMapIndex, static_cast<int>(mapFiles.size()));
                    chapterMapPath.clear();
                    mapTestRequested = false;
                    startRequested = true;
                }
                if (menuAction == ui::MenuAction::StartChapter) {
                    const int chapterIndex = mainMenu.ChapterIndex();
                    if (chapterIndex >= 0 && chapterIndex < static_cast<int>(chapters.size())
                        && !chapters[static_cast<std::size_t>(chapterIndex)].mapName.empty()) {
                        chapterMapPath = "maps/" + chapters[static_cast<std::size_t>(chapterIndex)].mapName + ".bsp";
                        mapTestRequested = false;
                        startRequested = true;
                    }
                }
                if (menuAction == ui::MenuAction::MapTest) {
                    selectedMap = std::min(mainMenu.Options().selectedMapIndex, static_cast<int>(mapFiles.size()));
                    mapTestRequested = true;
                    startRequested = true;
                }
                if (menuAction == ui::MenuAction::LoadGame) {
                    std::cout << "LOAD GAME: save system not implemented yet" << std::endl;
                }
                if (menuAction == ui::MenuAction::Quit) {
                    glfwSetWindowShouldClose(window, GLFW_TRUE);
                }
            } else {
                const bool escapePressed = glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS;
                if (escapePressed && !windowState.escapeWasPressed && screen == Screen::Playing) {
                    screen = Screen::Paused;
                    SetMouseCaptured(window, camera, false);
                    pauseMenu.Reset();
                    ui::PauseGameplaySettings pSettings{};
                    pSettings.fullscreen = windowState.fullscreen;
                    pSettings.mouseSensitivity = camera.sensitivity;
                    pSettings.showDebugOverlay = showDebugOverlay;
                    pSettings.showEntitySpawnLabels = showEntitySpawnLabels;
                    pSettings.flashlightEnabled = flashlight.enabled;
                    pSettings.shadowsEnabled = shadowManager.shadowsEnabled;
                    pSettings.flashlightShadows = shadowManager.flashlightShadows;
                    pSettings.pointLightShadows = shadowManager.pointLightShadows;
                    pSettings.pointLightCount = static_cast<int>(pointLights.size());
                    pSettings.maxPointLights = static_cast<int>(kMaxPointLights);
                    pauseMenu.SyncFromEngine(pSettings);
                }
                windowState.escapeWasPressed = escapePressed;

                if (screen == Screen::Paused) {
                    SetMouseCaptured(window, camera, false);
                    pauseMenu.Update(deltaTime);
                    ui::PauseGameplaySettings& ps = pauseMenu.Settings();
                    ps.pointLightCount = static_cast<int>(pointLights.size());
                    const ui::MenuInput pauseInput = BuildMenuInput(window, menuKeys, uiBackend);
                    const ui::PauseMenuAction pauseAction = pauseMenu.HandleInput(pauseInput);

                    const auto& s = pauseMenu.Settings();
                    camera.sensitivity = s.mouseSensitivity;
                    showDebugOverlay = s.showDebugOverlay;
                    showEntitySpawnLabels = s.showEntitySpawnLabels;
                    flashlight.enabled = s.flashlightEnabled;
                    shadowManager.shadowsEnabled = s.shadowsEnabled;
                    shadowManager.flashlightShadows = s.flashlightShadows;
                    shadowManager.pointLightShadows = s.pointLightShadows;
                    if (s.fullscreen != windowState.fullscreen) {
                        ToggleFullscreen(window, windowState);
                    }

                    if (pauseAction == ui::PauseMenuAction::ClearPointLights && !pointLights.empty()) {
                        pointLights.clear();
                        pauseMenu.Settings().pointLightCount = 0;
                    }
                    if (pauseAction == ui::PauseMenuAction::Continue) {
                        screen = Screen::Playing;
                        SetMouseCaptured(window, camera, true);
                    }
                    if (pauseAction == ui::PauseMenuAction::MainMenu) {
                        returnToMenuRequested = true;
                    }
                    if (pauseAction == ui::PauseMenuAction::Exit) {
                        glfwSetWindowShouldClose(window, GLFW_TRUE);
                    }
                }
            }

            if (startRequested) {
                unloadMenuBackground();
                const bool loadChapterMap = !chapterMapPath.empty();
                if (loadChapterMap || selectedMap < static_cast<int>(mapFiles.size())) {
                    windowState.noclip = mapTestRequested;
                    camera.pitch = -18.0F;
                    std::filesystem::path mapPath = loadChapterMap
                        ? chapterMapPath
                        : mapFiles[static_cast<std::size_t>(selectedMap)];
                    if (gameFiles && !mapPath.is_absolute()) {
                        mapPath = gameFiles->Materialize(mapPath.generic_string());
                    }
                    bspMap = std::make_unique<BspLoader>(mapPath.string(), gameFiles.get());
                    std::cout << "BSP assets ready" << std::endl;
                    physicsWorld.SetCollisionMesh(bspMap->CollisionVertices(), bspMap->CollisionIndices());
                    physicsWorld.SetDynamicBoxes(bspMap->PropPositions());
                    propCube = std::make_unique<Mesh>(WithStyleChannel(crateVertices), crateIndices);
                    std::cout << "Jolt world ready, loaded " << bspMap->PropPositions().size() << " props" << std::endl;
                    player.SetPhysicsWorld(&physicsWorld);
                    player.SetWorldBounds(bspMap->WorldMinimum(), bspMap->WorldMaximum());
                    if (bspMap->HasPlayerStartPosition()) {
                        player.SetPosition(bspMap->PlayerStartPosition() + glm::vec3(0.0F, 0.25F, 0.0F));
                    } else {
                        const glm::vec3 center = (bspMap->WorldMinimum() + bspMap->WorldMaximum()) * 0.5F;
                        player.SetPosition({center.x, bspMap->WorldMinimum().y + 1.0F, center.z});
                    }
                    camera.position = player.EyePosition();
                    gameSim = std::make_unique<GameSimulation>(*bspMap, physicsWorld, gameFiles.get(), &audio);
                    gameSim->SetCurrentMap(mapPath.stem().string());
                    cube.reset();
                    demoRoom.reset();
                } else {
                    windowState.noclip = mapTestRequested;
                    cube = std::make_unique<Mesh>(WithStyleChannel(vertices), indices);
                    demoRoom = std::make_unique<Mesh>(WithStyleChannel(roomVertices), roomIndices);
                    propCube = std::make_unique<Mesh>(WithStyleChannel(crateVertices), crateIndices);
                    bspMap.reset();
                    collisionDebugMeshes.clear();
                    gameSim.reset();
                    physicsWorld.SetCollisionMesh({}, {});
                    physicsWorld.SetDynamicBoxes({});
                    player.SetPhysicsWorld(&physicsWorld);
                    player.SetWorldBounds({-10.0F, -2.0F, -10.0F}, {10.0F, 10.0F, 10.0F});
                    player.SetPosition({0.0F, 0.0F, 3.0F});
                    camera.position = player.EyePosition();
                }
                screen = Screen::Playing;
                SetMouseCaptured(window, camera, true);
                footstepPreviousPosition = player.Position();
                footstepDistanceAccumulator = 0.0F;
                if (audioReady) {
                    audio.StartAmbientLoop();
                }
                startRequested = false;
                mapTestRequested = false;
                chapterMapPath.clear();
            }
            if (returnToMenuRequested) {
                bspMap.reset();
                collisionDebugMeshes.clear();
                gameSim.reset();
                cube.reset();
                demoRoom.reset();
                propCube.reset();
                physicsWorld.SetCollisionMesh({}, {});
                physicsWorld.SetDynamicBoxes({});
                pointLights.clear();
                windowState.noclip = false;
                screen = Screen::MainMenu;
                if (audioReady) {
                    audio.StopAmbientLoop();
                }
                footstepDistanceAccumulator = 0.0F;
                loadMenuBackground();
                returnToMenuRequested = false;
            }

            if (screen == Screen::Playing) {
                ProcessInput(
                    window,
                    camera,
                    windowState,
                    pointLights,
                    flashlight,
                    player.EyePosition(),
                    physicsWorld,
                    showDebugOverlay,
                    showEntitySpawnLabels,
                    showCollisionMesh
                );
                constexpr float kKeyboardYawSpeed = 140.0F;
                constexpr float kKeyboardPitchSpeed = 150.0F;
                if (glfwGetKey(window, GLFW_KEY_LEFT) == GLFW_PRESS) {
                    camera.yaw -= kKeyboardYawSpeed * deltaTime;
                }
                if (glfwGetKey(window, GLFW_KEY_RIGHT) == GLFW_PRESS) {
                    camera.yaw += kKeyboardYawSpeed * deltaTime;
                }
                if (glfwGetKey(window, GLFW_KEY_UP) == GLFW_PRESS) {
                    camera.pitch += kKeyboardPitchSpeed * deltaTime;
                }
                if (glfwGetKey(window, GLFW_KEY_DOWN) == GLFW_PRESS) {
                    camera.pitch -= kKeyboardPitchSpeed * deltaTime;
                }
                camera.pitch = glm::clamp(camera.pitch, -89.0F, 89.0F);
                glm::vec3 movementFront = camera.Front();
                movementFront.y = 0.0F;
                if (glm::length(movementFront) > 0.0F) {
                    movementFront = glm::normalize(movementFront);
                }
                const glm::vec3 movementRight = glm::normalize(glm::cross(
                    movementFront,
                    glm::vec3(0.0F, 1.0F, 0.0F)
                ));
                bool inWater = false;
                if (bspMap && !windowState.noclip) {
                    const int contents = bspMap->PointContents(player.Position())
                        | bspMap->PointContents(player.EyePosition());
                    inWater = (contents & (kBspContentsWater | kBspContentsSlime)) != 0;
                }
                player.SetInWater(inWater);
                player.Update(
                    deltaTime,
                    movementFront,
                    movementRight,
                    windowState.noclip,
                    glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS
                );
                camera.position = player.EyePosition();
                if (gameSim) {
                    gameSim->Update(deltaTime, player, player.EyePosition(), camera.Front(), window);
                    LevelChangeRequest levelChange;
                    if (gameSim->ConsumeLevelChange(levelChange)) {
                        const auto lowerCopy = [](std::string value) {
                            for (char& character : value) {
                                character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
                            }
                            return value;
                        };
                        const auto presentLoading = [&](float progress) {
                            int loadingWidth = 0;
                            int loadingHeight = 0;
                            glfwGetFramebufferSize(window, &loadingWidth, &loadingHeight);
                            glViewport(0, 0, std::max(loadingWidth, 1), std::max(loadingHeight, 1));
                            glClearColor(0.0F, 0.0F, 0.0F, 1.0F);
                            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
                            uiBackend.BeginRender(loadingWidth, loadingHeight);
                            loadingScreen.Render(uiBackend, levelChange.mapName, progress);
                            uiBackend.EndRender();
                            glfwSwapBuffers(window);
                            glfwPollEvents();
                        };

                        const std::string wantedMap = lowerCopy(levelChange.mapName);
                        std::filesystem::path mapPath;
                        int destinationMap = selectedMap;
                        for (std::size_t index = 0; index < mapFiles.size(); ++index) {
                            if (lowerCopy(mapFiles[index].stem().string()) == wantedMap) {
                                mapPath = mapFiles[index];
                                destinationMap = static_cast<int>(index);
                                break;
                            }
                        }
                        if (mapPath.empty()) {
                            std::cerr << "Map not found: " << levelChange.mapName << '\n';
                        } else {
                            if (gameFiles && !mapPath.is_absolute()) {
                                mapPath = gameFiles->Materialize(mapPath.generic_string());
                            }
                            presentLoading(0.08F);
                            std::unique_ptr<BspLoader> nextMap;
                            try {
                                nextMap = std::make_unique<BspLoader>(mapPath.string(), gameFiles.get());
                            } catch (const std::exception& error) {
                                std::cerr << error.what() << '\n';
                            }
                            if (nextMap) {
                                physicsWorld.SetCollisionMesh(nextMap->CollisionVertices(), nextMap->CollisionIndices());
                                physicsWorld.SetDynamicBoxes(nextMap->PropPositions());
                                if (!propCube) {
                                    propCube = std::make_unique<Mesh>(WithStyleChannel(crateVertices), crateIndices);
                                }
                                presentLoading(0.55F);
                                std::unique_ptr<GameSimulation> nextSim;
                                try {
                                    nextSim = std::make_unique<GameSimulation>(*nextMap, physicsWorld, gameFiles.get(), &audio);
                                } catch (const std::exception& error) {
                                    std::cerr << error.what() << '\n';
                                }
                                if (!nextSim) {
                                    if (bspMap) {
                                        physicsWorld.SetCollisionMesh(bspMap->CollisionVertices(), bspMap->CollisionIndices());
                                        physicsWorld.SetDynamicBoxes(bspMap->PropPositions());
                                        try {
                                            gameSim = std::make_unique<GameSimulation>(*bspMap, physicsWorld, gameFiles.get(), &audio);
                                            if (selectedMap >= 0 && static_cast<std::size_t>(selectedMap) < mapFiles.size()) {
                                                gameSim->SetCurrentMap(mapFiles[static_cast<std::size_t>(selectedMap)].stem().string());
                                            }
                                        } catch (const std::exception& error) {
                                            std::cerr << error.what() << '\n';
                                            gameSim.reset();
                                        }
                                    }
                                    player.SetPhysicsWorld(&physicsWorld);
                                    if (bspMap) {
                                        player.SetWorldBounds(bspMap->WorldMinimum(), bspMap->WorldMaximum());
                                    }
                                    player.SetPosition(levelChange.playerFeet);
                                    camera.position = player.EyePosition();
                                } else {
                                    gameSim.reset();
                                    bspMap = std::move(nextMap);
                                    gameSim = std::move(nextSim);
                                    gameSim->SetCurrentMap(levelChange.mapName);
                                    player.SetPhysicsWorld(&physicsWorld);
                                    player.SetWorldBounds(bspMap->WorldMinimum(), bspMap->WorldMaximum());
                                    gameSim->RestorePlayerState(levelChange.player);
                                    glm::vec3 destinationLandmark{0.0F};
                                    const glm::vec3 offset = levelChange.playerFeet - levelChange.landmarkOrigin;
                                    if (gameSim->FindLandmark(levelChange.landmarkName, destinationLandmark)) {
                                        player.SetPosition(destinationLandmark + offset);
                                        gameSim->SpawnCarriedEntities(levelChange.entities, destinationLandmark);
                                    } else {
                                        std::cerr << "Level transition landmark \"" << levelChange.landmarkName
                                                  << "\" not found in " << levelChange.mapName << '\n';
                                        if (bspMap->HasPlayerStartPosition()) {
                                            player.SetPosition(bspMap->PlayerStartPosition() + glm::vec3(0.0F, 0.25F, 0.0F));
                                        } else {
                                            const glm::vec3 center = (bspMap->WorldMinimum() + bspMap->WorldMaximum()) * 0.5F;
                                            player.SetPosition({center.x, bspMap->WorldMinimum().y + 1.0F, center.z});
                                        }
                                    }
                                    camera.position = player.EyePosition();
                                    selectedMap = destinationMap;
                                    footstepPreviousPosition = player.Position();
                                    footstepDistanceAccumulator = 0.0F;
                                    presentLoading(1.0F);
                                }
                            }
                        }
                    }
                }

                if (audioReady && player.IsGrounded() && !player.IsNoclip()) {
                    const glm::vec3 currentPosition = player.Position();
                    glm::vec3 horizontalDelta = currentPosition - footstepPreviousPosition;
                    horizontalDelta.y = 0.0F;
                    const float movedDistance = glm::length(horizontalDelta);
                    if (movedDistance > 0.001F && movedDistance < 5.0F) {
                        footstepDistanceAccumulator += movedDistance;
                        if (footstepDistanceAccumulator >= kFootstepDistance) {
                            footstepDistanceAccumulator = 0.0F;
                            audio.PlayFootstep();
                        }
                    } else if (movedDistance >= 5.0F) {
                        footstepDistanceAccumulator = 0.0F;
                    }
                    footstepPreviousPosition = currentPosition;
                } else {
                    footstepPreviousPosition = player.Position();
                    footstepDistanceAccumulator = 0.0F;
                }
                physicsWorld.StepSimulation(deltaTime);
            }

            // Frame time and FPS statistics
            frameTimeAccumulator += deltaTime;
            frameCountAccumulator++;
            if (frameTimeAccumulator >= 0.25F) {
                displayedFps = static_cast<float>(frameCountAccumulator) / frameTimeAccumulator;
                displayedFrameTimeMs = (frameTimeAccumulator / static_cast<float>(frameCountAccumulator)) * 1000.0F;
                frameTimeAccumulator = 0.0F;
                frameCountAccumulator = 0;
            }

            // Periodically refresh RAM/VRAM every 0.5s to avoid syscall/driver overhead
            ramCheckTimer += deltaTime;
            if (ramCheckTimer >= 0.5F || displayedRamMb <= 0.0F) {
                ramCheckTimer = 0.0F;
                displayedRamMb = GetProcessRamUsageMb();
                GetVramUsageMb(displayedVramUsedMb, displayedVramTotalMb);
            }

            ui::NetGraphMetrics netMetrics{};
            netMetrics.fps = displayedFps;
            netMetrics.frameTimeMs = displayedFrameTimeMs;
            netMetrics.ramMb = displayedRamMb;
            netMetrics.vramUsedMb = displayedVramUsedMb;
            netMetrics.vramTotalMb = displayedVramTotalMb;
            netMetrics.framebufferWidth = framebufferWidth;
            netMetrics.framebufferHeight = framebufferHeight;
            netMetrics.gpuName = gpuName;
            netMetrics.inGame = (screen == Screen::Playing);
            if (screen == Screen::Playing) {
                const glm::vec3 pos = player.Position();
                netMetrics.playerPosX = pos.x;
                netMetrics.playerPosY = pos.y;
                netMetrics.playerPosZ = pos.z;
                netMetrics.cameraYaw = camera.yaw;
                netMetrics.cameraPitch = camera.pitch;
                netMetrics.grounded = player.IsGrounded();
                netMetrics.noclip = player.IsNoclip();
                netMetrics.flashlightEnabled = flashlight.enabled;
                netMetrics.shadowsEnabled = shadowManager.shadowsEnabled;
                netMetrics.pointLightCount = static_cast<int>(pointLights.size());
                netMetrics.maxPointLights = static_cast<int>(kMaxPointLights);
                netMetrics.propCount = physicsWorld.DynamicBoxCount();
                if (bspMap) {
                    netMetrics.bspFaceCount = bspMap->FaceCount();
                    netMetrics.bspTextureCount = bspMap->TextureCount();
                    if (selectedMap < static_cast<int>(mapFiles.size())) {
                        netMetrics.mapName = mapFiles[static_cast<std::size_t>(selectedMap)].filename().string();
                    }
                }
            }

            const glm::mat4 model = bspMap
                ? glm::mat4(1.0F)
                : glm::translate(glm::mat4(1.0F), glm::vec3(0.0F, 1.8F, 0.0F)) * glm::rotate(glm::mat4(1.0F), time, glm::vec3(0.6F, 1.0F, 0.3F));
            const glm::mat4 view = glm::lookAt(camera.position, camera.position + camera.Front(), glm::vec3(0.0F, 1.0F, 0.0F));
            const glm::mat4 projection = glm::perspective(
                glm::radians(45.0F),
                aspect,
                0.005F,
                512.0F
            );

            if (screen != Screen::MainMenu && (bspMap || cube)) {
                // Sync flashlight with camera
                flashlight.position = camera.position;
                flashlight.direction = camera.Front();

                // Map lights fill the first slots. Player-spawned lights use whatever remains.
                const glm::vec3 camPos = camera.position;
                if (bspMap) {
                    std::vector<std::pair<std::string, bool>> doorStates;
                    if (gameSim) {
                        gameSim->CollectDoorStates(doorStates);
                    }
                    bspMap->UpdateVisibility(camPos, projection * view, doorStates);
                }
                if (gameSim) {
                    gameSim->SetViewPosition(camPos);
                }
                std::vector<PointLight> sortedPointLights = SelectFramePointLights(bspMap.get(), pointLights, camPos);
                const float shadowCullDistSq = ShadowManager::kShadowCameraCullDistance
                    * ShadowManager::kShadowCameraCullDistance;
                std::sort(
                    sortedPointLights.begin(),
                    sortedPointLights.end(),
                    [&camPos](const PointLight& a, const PointLight& b) {
                        const glm::vec3 da = a.position - camPos;
                        const glm::vec3 db = b.position - camPos;
                        return glm::dot(da, da) < glm::dot(db, db);
                    }
                );

                std::vector<PointLight> shadowPointLights;
                shadowPointLights.reserve(ShadowManager::kMaxShadowPointLights);
                for (const PointLight& light : sortedPointLights) {
                    const glm::vec3 offset = light.position - camPos;
                    if (glm::dot(offset, offset) > shadowCullDistSq) {
                        continue;
                    }
                    shadowPointLights.push_back(light);
                    if (shadowPointLights.size() >= ShadowManager::kMaxShadowPointLights) {
                        break;
                    }
                }

                // Shadow-casting lights must occupy uniforms [0 .. NumShadowPointLights) for the shader.
                std::vector<PointLight> lightsForShaders = shadowPointLights;
                lightsForShaders.reserve(std::min(sortedPointLights.size(), kMaxPointLights));
                for (const PointLight& light : sortedPointLights) {
                    if (lightsForShaders.size() >= kMaxPointLights) {
                        break;
                    }
                    bool alreadyListed = false;
                    for (const PointLight& listed : shadowPointLights) {
                        const glm::vec3 diff = listed.position - light.position;
                        if (glm::dot(diff, diff) < 1.0e-4F) {
                            alreadyListed = true;
                            break;
                        }
                    }
                    if (!alreadyListed) {
                        lightsForShaders.push_back(light);
                    }
                }

                const int activeShadowCount = static_cast<int>(shadowPointLights.size());

                // Compute dynamic box matrices ONCE per frame before shadow passes
                const std::vector<glm::mat4> propMatrices = physicsWorld.DynamicBoxMatrices();

                // Helper to render scene depth for shadow passes with spatial distance culling
                auto renderSceneDepth = [&](const Shader& depthShader, const glm::vec3& lightPos, float lightRadius) {
                    if (bspMap) {
                        bspMap->DrawDepth(depthShader, lightPos, lightRadius);
                    } else {
                        if (cube) {
                            depthShader.SetMat4("Model", model);
                            cube->Draw();
                        }
                        if (demoRoom) {
                            depthShader.SetMat4("Model", glm::mat4(1.0F));
                            demoRoom->Draw();
                        }
                    }
                    if (propCube && !propMatrices.empty()) {
                        const float radiusWithProp = (lightRadius > 0.0F) ? (lightRadius + 1.5F) : -1.0F;
                        const float maxPropDistSq = radiusWithProp * radiusWithProp;
                        for (const glm::mat4& propModel : propMatrices) {
                            if (radiusWithProp > 0.0F) {
                                const glm::vec3 propPos(propModel[3]);
                                const glm::vec3 diff = propPos - lightPos;
                                if (glm::dot(diff, diff) > maxPropDistSq) {
                                    continue;
                                }
                            }
                            depthShader.SetMat4("Model", propModel);
                            propCube->Draw();
                        }
                        glBindVertexArray(0);
                    }
                };

                // Ensure correct depth testing state for 3D passes
                glEnable(GL_DEPTH_TEST);
                glDepthFunc(GL_LESS);
                glDepthMask(GL_TRUE);

                // 1. Flashlight Shadow Map Pass (light is at camera — always within cull distance)
                if (shadowManager.shadowsEnabled && shadowManager.flashlightShadows && flashlight.enabled) {
                    shadowManager.BeginSpotlightPass(flashlight.position, flashlight.direction);
                    renderSceneDepth(shadowManager.GetSpotlightDepthShader(), flashlight.position, ShadowManager::kSpotlightFarPlane);
                    shadowManager.EndSpotlightPass();
                }

                // 2. Point Light Omnidirectional Shadow Cubemap Passes (camera-distance cull)
                if (shadowManager.shadowsEnabled && shadowManager.pointLightShadows && activeShadowCount > 0) {
                    for (int i = 0; i < activeShadowCount; ++i) {
                        const glm::vec3& lightPos = shadowPointLights[static_cast<std::size_t>(i)].position;
                        shadowManager.BeginPointLightPass(
                            i,
                            lightPos,
                            ShadowManager::kPointShadowFarPlane
                        );
                        renderSceneDepth(shadowManager.GetPointDepthShader(), lightPos, ShadowManager::kPointShadowFarPlane);
                        shadowManager.EndPointLightPass();
                    }
                }

                // 3. Main Scene Render Pass
                glViewport(0, 0, framebufferWidth, framebufferHeight);
                glClearColor(0.18F, 0.18F, 0.18F, 1.0F);
                glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

                shader.Use();
                shader.SetBool("DecalPass", false);
                shader.SetMat4("Model", model);
                shader.SetMat4("View", view);
                shader.SetMat4("Projection", projection);

                // Bind shadow maps to texture units 2..6 and pass shadow uniforms
                shadowManager.BindForRendering(shader, flashlight.enabled, activeShadowCount);

                // Point light uniforms: fast cached uploads avoiding string formatting/hash lookups
                const int numLights = static_cast<int>(std::min(lightsForShaders.size(), kMaxPointLights));
                shader.SetInt("NumPointLights", numLights);
                for (int i = 0; i < numLights; ++i) {
                    const auto& l = lightsForShaders[static_cast<std::size_t>(i)];
                    shader.SetPointLightUniform(i, l.position, l.color, l.intensity, l.constant, l.linear, l.quadratic);
                }

                // Update flashlight (spotlight) uniforms via fast cached setter
                shader.SetSpotlightUniform(
                    flashlight.position,
                    flashlight.direction,
                    flashlight.color,
                    flashlight.intensity,
                    flashlight.innerCutOff,
                    flashlight.outerCutOff,
                    flashlight.constant,
                    flashlight.linear,
                    flashlight.quadratic,
                    flashlight.enabled
                );
                UploadMapSpotsAndSun(shader, bspMap.get(), camPos);
                shader.SetBool("UseSpriteTint", false);
                shader.SetBool("FogEnabled", false);

                const glm::mat4 viewProj = projection * view;

                if (bspMap) {
                    DrawSkyPasses(
                        shader,
                        skyShader,
                        *bspMap,
                        gameSim.get(),
                        view,
                        projection,
                        glm::radians(45.0F),
                        aspect,
                        camPos,
                        camera.Front(),
                        glm::vec3(0.0F, 1.0F, 0.0F),
                        time
                    );
                    bspMap->PrepareFrame(shader, camPos, time);
                    bspMap->Draw(shader, viewProj);
                    if (propCube) {
                        shader.SetBool("UseTexture", false);
                        shader.SetBool("UseLightmap", false);
                        shader.SetInt("AlphaMode", 0);
                        shader.SetFloat("Opacity", 1.0F);
                        glDisable(GL_BLEND);
                        for (const glm::mat4& propModel : propMatrices) {
                            shader.SetMat4("Model", propModel);
                            propCube->Draw();
                        }
                        if (showEntitySpawnLabels) {
                            shader.SetFloat("Opacity", 0.85F);
                            shader.SetInt("AlphaMode", 0);
                            for (const BspMapEntity& entity : bspMap->MapEntities()) {
                                const glm::mat4 markerModel = glm::translate(glm::mat4(1.0F), entity.position)
                                    * glm::scale(glm::mat4(1.0F), glm::vec3(0.35F));
                                shader.SetMat4("Model", markerModel);
                                propCube->Draw();
                            }
                        }
                        glBindVertexArray(0);
                        shader.SetFloat("Opacity", 1.0F);
                        shader.SetMat4("Model", model);
                    }
                    if (gameSim) {
                        gameSim->Draw(shader);
                    }
                    bspMap->DrawTransparent(shader, viewProj);
                    bspMap->DrawDecals(shader, viewProj);
                    if (gameSim) {
                        gameSim->DrawTransparent(shader);
                    }
                    bspMap->DrawSprites(shader, view);
                } else {
                    shader.SetBool("UseTexture", false);
                    shader.SetBool("UseLightmap", false);
                    shader.SetFloat("Opacity", 1.0F);
                    if (cube) {
                        cube->Draw();
                    }
                    if (demoRoom) {
                        shader.SetMat4("Model", glm::mat4(1.0F));
                        demoRoom->Draw();
                        shader.SetMat4("Model", model);
                    }
                    if (propCube) {
                        for (const glm::mat4& propModel : propMatrices) {
                            shader.SetMat4("Model", propModel);
                            propCube->Draw();
                        }
                        glBindVertexArray(0);
                        shader.SetMat4("Model", model);
                    }
                }

                if (showCollisionMesh) {
                    std::vector<PhysicsDebugShape> debugShapes;
                    physicsWorld.CollectDebugShapes(debugShapes);
                    if (gameSim) {
                        gameSim->AppendDebugShapes(debugShapes);
                    }

                    std::unordered_set<std::uint64_t> liveShapes;
                    liveShapes.reserve(debugShapes.size());
                    for (const PhysicsDebugShape& shape : debugShapes) {
                        if (shape.vertices == nullptr || shape.indices == nullptr || shape.indices->size() < 3) {
                            continue;
                        }
                        liveShapes.insert(shape.key);
                        CachedCollisionMesh& cached = collisionDebugMeshes[shape.key];
                        if (cached.mesh == nullptr || cached.revision != shape.revision) {
                            cached.revision = shape.revision;
                            cached.mesh = std::make_unique<DebugTriangleMesh>(*shape.vertices, *shape.indices);
                        }
                    }
                    for (auto it = collisionDebugMeshes.begin(); it != collisionDebugMeshes.end();) {
                        if (!liveShapes.contains(it->first)) {
                            it = collisionDebugMeshes.erase(it);
                        } else {
                            ++it;
                        }
                    }

                    const GLboolean depthWasEnabled = glIsEnabled(GL_DEPTH_TEST);
                    const GLboolean cullWasEnabled = glIsEnabled(GL_CULL_FACE);
                    const GLboolean blendWasEnabled = glIsEnabled(GL_BLEND);
                    glDisable(GL_DEPTH_TEST);
                    glDisable(GL_CULL_FACE);
                    glDisable(GL_BLEND);
                    glDepthMask(GL_FALSE);
                    glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);

                    for (const PhysicsDebugShape& shape : debugShapes) {
                        const auto found = collisionDebugMeshes.find(shape.key);
                        if (found == collisionDebugMeshes.end() || found->second.mesh == nullptr) {
                            continue;
                        }
                        DrawBlueWireframe(debugShader, *found->second.mesh, shape.transform, view, projection);
                    }

                    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
                    glDepthMask(GL_TRUE);
                    if (depthWasEnabled == GL_TRUE) {
                        glEnable(GL_DEPTH_TEST);
                    }
                    if (blendWasEnabled == GL_TRUE) {
                        glEnable(GL_BLEND);
                    }
                    if (cullWasEnabled == GL_TRUE) {
                        glEnable(GL_CULL_FACE);
                    }
                    glBindVertexArray(0);
                }
            } else if (screen == Screen::MainMenu && (menuMap || menuBackdrop)) {
                glViewport(0, 0, framebufferWidth, framebufferHeight);
                glClearColor(0.0F, 0.0F, 0.0F, 1.0F);
                glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
                glEnable(GL_DEPTH_TEST);
                glDepthFunc(GL_LESS);
                glDepthMask(GL_TRUE);

                glm::mat4 menuView{1.0F};
                glm::mat4 menuProjection = projection;
                glm::vec3 menuEyePos{0.0F, 2.8F, 11.0F};
                glm::vec3 menuFront{0.0F, 0.0F, -1.0F};
                glm::vec3 menuUp{0.0F, 1.0F, 0.0F};
                float menuFovRadians = glm::radians(45.0F);
                if (menuMap) {
                    const ui::MenuCameraFrame menuCamera = ui::BuildMenuCameraFrame(*menuMap, time);
                    menuFovRadians = glm::radians(menuCamera.fovDegrees);
                    menuEyePos = menuCamera.eye;
                    menuFront = menuCamera.target - menuCamera.eye;
                    if (glm::length(menuFront) > 1.0e-4F) {
                        menuFront = glm::normalize(menuFront);
                    }
                    menuUp = menuCamera.up;
                    menuView = glm::lookAt(menuCamera.eye, menuCamera.target, menuCamera.up);
                    menuProjection = glm::perspective(
                        menuFovRadians,
                        aspect,
                        0.005F,
                        512.0F
                    );
                } else {
                    const float orbitRadius = 11.0F;
                    const glm::vec3 menuEye{
                        orbitRadius * std::cos(time * 0.12F),
                        2.8F,
                        orbitRadius * std::sin(time * 0.12F)
                    };
                    menuEyePos = menuEye;
                    const glm::vec3 menuTarget{0.0F, 1.6F, 0.0F};
                    menuView = glm::lookAt(menuEye, menuTarget, glm::vec3(0.0F, 1.0F, 0.0F));
                }

                shader.Use();
                shader.SetMat4("Model", glm::mat4(1.0F));
                shader.SetMat4("View", menuView);
                shader.SetMat4("Projection", menuProjection);
                shader.SetBool("Flashlight.enabled", false);
                shader.SetBool("UseSpriteTint", false);
                shader.SetBool("UseTexture", false);
                shader.SetBool("UseLightmap", false);
                shader.SetFloat("Opacity", 1.0F);
                shadowManager.BindForRendering(shader, false, 0);
                if (menuMap) {
                    const std::vector<PointLight> menuLights = SelectFramePointLights(menuMap.get(), {}, menuEyePos);
                    const int menuLightCount = static_cast<int>(menuLights.size());
                    shader.SetInt("NumPointLights", menuLightCount);
                    for (int lightIndex = 0; lightIndex < menuLightCount; ++lightIndex) {
                        const PointLight& light = menuLights[static_cast<std::size_t>(lightIndex)];
                        shader.SetPointLightUniform(
                            lightIndex,
                            light.position,
                            light.color,
                            light.intensity,
                            light.constant,
                            light.linear,
                            light.quadratic
                        );
                    }
                    UploadMapSpotsAndSun(shader, menuMap.get(), menuEyePos);
                    const glm::mat4 menuViewProj = menuProjection * menuView;
                    {
                        std::vector<std::pair<std::string, bool>> doorStates;
                        if (menuSimulation) {
                            menuSimulation->CollectDoorStates(doorStates);
                        }
                        menuMap->UpdateVisibility(menuEyePos, menuViewProj, doorStates);
                    }
                    if (menuSimulation) {
                        menuSimulation->SetViewPosition(menuEyePos);
                    }
                    DrawSkyPasses(
                        shader,
                        skyShader,
                        *menuMap,
                        menuSimulation.get(),
                        menuView,
                        menuProjection,
                        menuFovRadians,
                        aspect,
                        menuEyePos,
                        menuFront,
                        menuUp,
                        time
                    );
                    menuMap->PrepareFrame(shader, menuEyePos, time);
                    menuMap->Draw(shader, menuViewProj);
                    if (menuSimulation) {
                        menuSimulation->Draw(shader);
                    }
                    menuMap->DrawTransparent(shader, menuViewProj);
                    menuMap->DrawDecals(shader, menuViewProj);
                    if (menuSimulation) {
                        menuSimulation->DrawTransparent(shader);
                    }
                    menuMap->DrawSprites(shader, menuView);
                } else if (menuBackdrop) {
                    shader.SetInt("NumPointLights", 0);
                    shader.SetBool("FogEnabled", false);
                    UploadMapSpotsAndSun(shader, nullptr, menuEyePos);
                    menuBackdrop->Draw();
                }
            } else {
                glViewport(0, 0, framebufferWidth, framebufferHeight);
                glClearColor(0.18F, 0.18F, 0.18F, 1.0F);
                glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            }

            // 2D Valve VGUI Pass
            uiBackend.BeginRender(framebufferWidth, framebufferHeight);
            if (screen == Screen::MainMenu) {
                menuRenderer.Render(uiBackend, mainMenu, time, mapLabels, gameLabel, chapters);
            } else if (screen == Screen::Playing) {
                if (bspMap && showEntitySpawnLabels) {
                    const bool showEntityDetails = glfwGetKey(window, GLFW_KEY_F5) == GLFW_PRESS;
                    entityLabelRenderer.Render(
                        uiBackend,
                        bspMap->MapEntities(),
                        view,
                        projection,
                        camera.position,
                        showEntityDetails
                    );
                }
                if (showDebugOverlay) {
                    netGraph.Render(uiBackend, netMetrics);
                }
            } else if (screen == Screen::Paused) {
                pauseMenu.Render(uiBackend, time);
                if (showDebugOverlay) {
                    netGraph.Render(uiBackend, netMetrics);
                }
            }
            uiBackend.EndRender();

            glfwSwapBuffers(window);
            glfwPollEvents();
        }
        if (audioReady) {
            audio.StopAmbientLoop();
            audio.Shutdown();
        }
        ui::ReleaseChapterPreviews(chapters);
        uiBackend.Shutdown();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        glfwDestroyWindow(window);
        glfwTerminate();
        return EXIT_FAILURE;
    }

    glfwDestroyWindow(window);
    glfwTerminate();
    return EXIT_SUCCESS;
}
