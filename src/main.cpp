#include <cstdlib>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <memory>
#include <cmath>
#include <string>
#include <vector>

#include <glad/gl.h>
#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/vec4.hpp>

#include "AudioSystem.hpp"
#include "BspLoader.hpp"
#include "Mesh.hpp"
#include "Platform.hpp"
#include "PhysicsWorld.hpp"
#include "PlayerController.hpp"
#include "Shader.hpp"
#include "ShadowManager.hpp"
#include "ui/IUIRenderBackend.hpp"
#include "ui/IUISoundBackend.hpp"
#include "ui/MainMenu.hpp"
#include "ui/MenuRenderer.hpp"
#include "ui/OpenGLUIRenderBackend.hpp"
#include "ui/VguiPauseMenu.hpp"
#include "ui/ValveNetGraph.hpp"
#include "ui/EntityLabelRenderer.hpp"
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

bool HasBspExtension(const std::filesystem::path& path) {
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return extension == ".bsp";
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
    bool& showEntitySpawnLabels
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
} // namespace

void GlfwError(int code, const char* description) {
    std::cerr << "GLFW error " << code << ": " << (description != nullptr ? description : "") << '\n';
}

int main(int argc, char* argv[]) {
    blueprint::ConfigurePlatform();
    glfwSetErrorCallback(GlfwError);
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
        const std::filesystem::path contentRoot = blueprint::ContentRoot();

        if (!uiBackend.Initialize(window, contentRoot)) {
            std::cerr << "Failed to initialize OpenGL UI Backend\n";
            glfwDestroyWindow(window);
            glfwTerminate();
            return EXIT_FAILURE;
        }

        Shader shader(
            contentRoot / "shaders" / "basic.vert",
            contentRoot / "shaders" / "basic.frag"
        );
        shader.CacheLightUniformLocations();

        ShadowManager shadowManager;
        shadowManager.Init(contentRoot);

        std::vector<std::filesystem::path> mapFiles;
        const std::filesystem::path mapsDirectory = contentRoot / "maps";
        if (std::filesystem::exists(mapsDirectory)) {
            for (const auto& entry : std::filesystem::directory_iterator(
                     mapsDirectory,
                     std::filesystem::directory_options::skip_permission_denied)) {
                if (entry.is_regular_file() && HasBspExtension(entry.path())) {
                    mapFiles.push_back(entry.path());
                }
            }
        }
        std::sort(mapFiles.begin(), mapFiles.end());
        int selectedMap = 0;
        const bool autoStart = argc > 2 && std::string(argv[1]) == "--autostart";
        if (argc > 1) {
            const char* requestedPath = autoStart ? argv[2] : argv[1];
            for (std::size_t index = 0; index < mapFiles.size(); ++index) {
                std::error_code equivalentError;
                if (std::filesystem::equivalent(mapFiles[index], requestedPath, equivalentError)) {
                    selectedMap = static_cast<int>(index);
                    break;
                }
            }
        }

        std::unique_ptr<BspLoader> bspMap;
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

        AudioSystem audio;
        const bool audioReady = audio.Initialize(contentRoot);
        ui::NullUISoundBackend nullMenuSound;
        ui::WavUISoundBackend menuSound(&audio);
        ui::IUISoundBackend* menuSoundBackend = audioReady ? static_cast<ui::IUISoundBackend*>(&menuSound)
                                                           : static_cast<ui::IUISoundBackend*>(&nullMenuSound);
        ui::MainMenu mainMenu;
        ui::MenuRenderer menuRenderer;
        ui::VguiPauseMenu pauseMenu;
        ui::ValveNetGraph netGraph;
        ui::EntityLabelRenderer entityLabelRenderer;
        MenuKeyState menuKeys;
        mainMenu.Initialize(menuSoundBackend);
        pauseMenu.Initialize(menuSoundBackend);
        mainMenu.SetMapCount(static_cast<int>(mapFiles.size()) + 1);
        mainMenu.SyncOptionsFromEngine(windowState.fullscreen, camera.sensitivity, selectedMap);
        std::unique_ptr<Mesh> menuBackdrop = std::make_unique<Mesh>(roomVertices, roomIndices);
        std::vector<std::string> mapLabels;
        mapLabels.reserve(mapFiles.size() + 1);
        for (const auto& mapPath : mapFiles) {
            mapLabels.push_back(mapPath.filename().string());
        }
        mapLabels.emplace_back("Demo Cube");

        bool showDebugOverlay = true;
        bool showEntitySpawnLabels = false;
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
                    mapTestRequested = false;
                    startRequested = true;
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
                if (selectedMap < static_cast<int>(mapFiles.size())) {
                    windowState.noclip = mapTestRequested;
                    camera.pitch = -18.0F;
                    bspMap = std::make_unique<BspLoader>(mapFiles[static_cast<std::size_t>(selectedMap)]);
                    std::cout << "BSP assets ready" << std::endl;
                    physicsWorld.SetCollisionMesh(bspMap->CollisionVertices(), bspMap->CollisionIndices());
                    physicsWorld.SetDynamicBoxes(bspMap->PropPositions());
                    propCube = std::make_unique<Mesh>(crateVertices, crateIndices);
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
                    cube.reset();
                    demoRoom.reset();
                } else {
                    windowState.noclip = mapTestRequested;
                    cube = std::make_unique<Mesh>(vertices, indices);
                    demoRoom = std::make_unique<Mesh>(roomVertices, roomIndices);
                    propCube = std::make_unique<Mesh>(crateVertices, crateIndices);
                    bspMap.reset();
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
            }
            if (returnToMenuRequested) {
                bspMap.reset();
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
                returnToMenuRequested = false;
            }

            if (screen == Screen::Playing) {
                physicsWorld.StepSimulation(deltaTime);
                ProcessInput(
                    window,
                    camera,
                    windowState,
                    pointLights,
                    flashlight,
                    player.EyePosition(),
                    physicsWorld,
                    showDebugOverlay,
                    showEntitySpawnLabels
                );
                glm::vec3 movementFront = camera.Front();
                movementFront.y = 0.0F;
                if (glm::length(movementFront) > 0.0F) {
                    movementFront = glm::normalize(movementFront);
                }
                const glm::vec3 movementRight = glm::normalize(glm::cross(
                    movementFront,
                    glm::vec3(0.0F, 1.0F, 0.0F)
                ));
                player.Update(
                    deltaTime,
                    movementFront,
                    movementRight,
                    windowState.noclip,
                    glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS
                );
                camera.position = player.EyePosition();

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
                displayedRamMb = blueprint::ProcessRamUsageMb();
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

                // Sort point lights by distance to camera; only render shadows for lights within cull range.
                std::vector<PointLight> sortedPointLights = pointLights;
                const glm::vec3 camPos = camera.position;
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

                const glm::mat4 viewProj = projection * view;

                if (bspMap) {
                    bspMap->Draw(shader, viewProj);
                    if (propCube) {
                        shader.SetBool("UseTexture", false);
                        shader.SetBool("UseLightmap", false);
                        shader.SetFloat("Opacity", 1.0F);
                        for (const glm::mat4& propModel : propMatrices) {
                            shader.SetMat4("Model", propModel);
                            propCube->Draw();
                        }
                        if (showEntitySpawnLabels) {
                            for (const BspMapEntity& entity : bspMap->MapEntities()) {
                                shader.SetFloat("Opacity", 0.85F);
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
                    bspMap->DrawDecals(shader, viewProj);
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
            } else if (screen == Screen::MainMenu && menuBackdrop) {
                glViewport(0, 0, framebufferWidth, framebufferHeight);
                glClearColor(0.08F, 0.09F, 0.11F, 1.0F);
                glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
                glEnable(GL_DEPTH_TEST);
                glDepthFunc(GL_LESS);
                glDepthMask(GL_TRUE);

                const float orbitRadius = 11.0F;
                const glm::vec3 menuEye{
                    orbitRadius * std::cos(time * 0.12F),
                    2.8F,
                    orbitRadius * std::sin(time * 0.12F)
                };
                const glm::vec3 menuTarget{0.0F, 1.6F, 0.0F};
                const glm::mat4 menuView = glm::lookAt(menuEye, menuTarget, glm::vec3(0.0F, 1.0F, 0.0F));

                shader.Use();
                shader.SetMat4("Model", glm::mat4(1.0F));
                shader.SetMat4("View", menuView);
                shader.SetMat4("Projection", projection);
                shader.SetInt("NumPointLights", 0);
                shader.SetBool("Flashlight.enabled", false);
                shader.SetBool("UseTexture", false);
                shader.SetBool("UseLightmap", false);
                shader.SetFloat("Opacity", 1.0F);
                shadowManager.BindForRendering(shader, false, 0);
                menuBackdrop->Draw();
            } else {
                glViewport(0, 0, framebufferWidth, framebufferHeight);
                glClearColor(0.18F, 0.18F, 0.18F, 1.0F);
                glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            }

            // 2D Valve VGUI Pass
            uiBackend.BeginRender(framebufferWidth, framebufferHeight);
            if (screen == Screen::MainMenu) {
                menuRenderer.Render(uiBackend, mainMenu, time, mapLabels);
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
