#pragma once

#include <cstdint>
#include <string>

namespace ui {

struct Color {
    float r = 1.0F;
    float g = 1.0F;
    float b = 1.0F;
    float a = 1.0F;

    static constexpr Color White() { return {1.0F, 1.0F, 1.0F, 1.0F}; }
    static constexpr Color Black(float alpha = 1.0F) { return {0.0F, 0.0F, 0.0F, alpha}; }

    // Classic Half-Life 2 / Source VGUI palette
    static constexpr Color Hl2Default() { return {0.72F, 0.72F, 0.72F, 0.92F}; }
    static constexpr Color Hl2Hover() { return {0.92F, 0.58F, 0.12F, 1.0F}; }   // #EB941F Valve Orange
    static constexpr Color Hl2Active() { return {1.0F, 1.0F, 1.0F, 1.0F}; }
    static constexpr Color Hl2Dim() { return {0.45F, 0.45F, 0.45F, 0.75F}; }
    static constexpr Color Hl2PanelFill() { return {0.14F, 0.15F, 0.16F, 0.92F}; }
    static constexpr Color Hl2PanelBorder() { return {0.45F, 0.48F, 0.52F, 0.95F}; }
    static constexpr Color Hl2Overlay() { return {0.02F, 0.02F, 0.03F, 0.55F}; }

    // Valve VGUI2 window and widget styling
    static constexpr Color ValveOrange() { return {0.92F, 0.58F, 0.12F, 1.0F}; }
    static constexpr Color ValveYellow() { return {0.98F, 0.82F, 0.20F, 1.0F}; }
    static constexpr Color ValveGreen() { return {0.35F, 0.78F, 0.30F, 1.0F}; }
    static constexpr Color ValveRed() { return {0.88F, 0.28F, 0.28F, 1.0F}; }
    static constexpr Color ValveCyan() { return {0.35F, 0.75F, 0.90F, 1.0F}; }
    static constexpr Color ValveBg() { return {0.16F, 0.17F, 0.18F, 0.94F}; }
    static constexpr Color ValveTitleBar() { return {0.20F, 0.22F, 0.24F, 1.0F}; }
    static constexpr Color ValveSlotBg() { return {0.10F, 0.11F, 0.12F, 0.98F}; }
    static constexpr Color ValveBevelLight() { return {0.52F, 0.56F, 0.58F, 0.85F}; }
    static constexpr Color ValveBevelDark() { return {0.06F, 0.07F, 0.08F, 0.95F}; }
    static constexpr Color ValveButtonNormal() { return {0.24F, 0.26F, 0.28F, 0.95F}; }
    static constexpr Color ValveButtonHover() { return {0.34F, 0.38F, 0.41F, 1.0F}; }
    static constexpr Color ValveButtonActive() { return {0.18F, 0.20F, 0.21F, 1.0F}; }
    static constexpr Color ValveBorder() { return {0.38F, 0.42F, 0.45F, 1.0F}; }
    static constexpr Color ValveTextPrimary() { return {0.85F, 0.88F, 0.90F, 1.0F}; }
    static constexpr Color ValveTextDim() { return {0.55F, 0.58F, 0.60F, 0.85F}; }
};

struct Vec2 {
    float x = 0.0F;
    float y = 0.0F;
};

enum class MenuAction : std::uint8_t {
    None,
    NewGame,
    LoadGame,
    MapTest,
    OpenOptions,
    CloseOptions,
    Quit,
};

enum class PauseMenuAction : std::uint8_t {
    None,
    Continue,
    MainMenu,
    Exit,
    ClearPointLights,
};

struct PauseGameplaySettings {
    bool fullscreen = false;
    float mouseSensitivity = 0.1F;
    bool showDebugOverlay = true;
    bool showEntitySpawnLabels = false;
    bool flashlightEnabled = true;
    bool shadowsEnabled = true;
    bool flashlightShadows = true;
    bool pointLightShadows = true;
    int pointLightCount = 0;
    int maxPointLights = 32;
};

enum class MainMenuItem : std::int8_t {
    NewGame = 0,
    LoadGame,
    MapTest,
    Options,
    Quit,
    Count
};

struct MenuItemDef {
    MainMenuItem id;
    const char* label;
};

inline constexpr MenuItemDef kMainMenuItems[] = {
    {MainMenuItem::NewGame, "NEW GAME"},
    {MainMenuItem::LoadGame, "LOAD GAME"},
    {MainMenuItem::MapTest, "MAP TEST"},
    {MainMenuItem::Options, "OPTIONS"},
    {MainMenuItem::Quit, "QUIT"},
};

struct MenuInput {
    bool upPressed = false;
    bool downPressed = false;
    bool leftPressed = false;
    bool rightPressed = false;
    bool confirmPressed = false;
    bool backPressed = false;
    bool mouseLeftPressed = false;
    bool mouseLeftDown = false;
    bool mouseLeftReleased = false;
    Vec2 mousePosition{};
    Vec2 displaySize{};
    bool mouseValid = false;
};

struct OptionsSettings {
    bool fullscreen = false;
    float mouseSensitivity = 0.1F;
    float masterVolume = 1.0F;
    float sfxVolume = 1.0F;
    int selectedMapIndex = 0;
};

} // namespace ui
