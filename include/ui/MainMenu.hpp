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
    [[nodiscard]] bool IsChapterSelectOpen() const { return chapterSelectOpen_; }
    [[nodiscard]] int HighlightedIndex() const { return highlightedIndex_; }
    [[nodiscard]] int ActiveIndex() const { return activeIndex_; }
    [[nodiscard]] int ChapterIndex() const { return chapterIndex_; }
    [[nodiscard]] int ChapterCount() const { return chapterCount_; }
    [[nodiscard]] int DifficultyIndex() const { return difficultyIndex_; }
    [[nodiscard]] int ChapterScroll(int visibleRows) const;
    [[nodiscard]] OptionsSettings& Options() { return options_; }
    [[nodiscard]] const OptionsSettings& Options() const { return options_; }

    void SetOptionsOpen(bool open);
    void SyncOptionsFromEngine(bool fullscreen, float mouseSensitivity, int selectedMapIndex);
    void SetMapCount(int mapCount);
    void SetChapterCount(int chapterCount);
    [[nodiscard]] int OptionsFieldIndex() const { return optionsFieldIndex_; }

private:
    void MoveHighlight(int delta);
    MenuAction ActivateItem(MainMenuItem item);
    void UpdateHoverFromMouse(const MenuInput& input);
    void UpdateChapterHover(const MenuInput& input);
    void AdjustOptionsField(int direction);
    void MoveOptionsField(int delta);
    void MoveChapter(int delta);
    void AdjustDifficulty(int direction);
    MenuAction StartHighlightedChapter();
    void ArmConfirmCooldown();

    IUISoundBackend* sound_ = nullptr;
    int highlightedIndex_ = 0;
    int activeIndex_ = -1;
    bool optionsOpen_ = false;
    int optionsFieldIndex_ = 0;
    float activePulse_ = 0.0F;
    int lastHoveredIndex_ = -1;
    int mapCount_ = 1;
    int chapterCount_ = 0;
    int chapterIndex_ = 0;
    int difficultyIndex_ = 1;
    int hoverChapter_ = -1;
    int hoverDifficulty_ = -1;
    bool chapterSelectOpen_ = false;
    float confirmCooldown_ = 0.0F;
    OptionsSettings options_{};
};

} // namespace ui
