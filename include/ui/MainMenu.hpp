#pragma once

#include "ui/IUISoundBackend.hpp"
#include "ui/MenuTypes.hpp"

namespace ui {

class MainMenu {
public:
    void Initialize(IUISoundBackend* soundBackend);

    void Update(float deltaTime);
    MenuAction HandleInput(const MenuInput& input);

    [[nodiscard]] bool IsOptionsOpen() const { return optionsOpen_; }
    [[nodiscard]] int HighlightedIndex() const { return highlightedIndex_; }
    [[nodiscard]] int ActiveIndex() const { return activeIndex_; }
    [[nodiscard]] OptionsSettings& Options() { return options_; }
    [[nodiscard]] const OptionsSettings& Options() const { return options_; }

    void SetOptionsOpen(bool open);
    void SyncOptionsFromEngine(bool fullscreen, float mouseSensitivity, int selectedMapIndex);
    void SetMapCount(int mapCount);
    [[nodiscard]] int OptionsFieldIndex() const { return optionsFieldIndex_; }

private:
    void MoveHighlight(int delta);
    MenuAction ActivateItem(MainMenuItem item);
    void UpdateHoverFromMouse(const MenuInput& input);
    void AdjustOptionsField(int direction);
    void MoveOptionsField(int delta);
    void ArmConfirmCooldown();

    IUISoundBackend* sound_ = nullptr;
    int highlightedIndex_ = 0;
    int activeIndex_ = -1;
    bool optionsOpen_ = false;
    int optionsFieldIndex_ = 0;
    float activePulse_ = 0.0F;
    int lastHoveredIndex_ = -1;
    int mapCount_ = 1;
    float confirmCooldown_ = 0.0F;
    OptionsSettings options_{};
};

} // namespace ui
