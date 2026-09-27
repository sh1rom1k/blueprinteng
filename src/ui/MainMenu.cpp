#include "ui/MainMenu.hpp"

#include <algorithm>

namespace ui {
namespace {

constexpr int kItemCount = static_cast<int>(MainMenuItem::Count);
constexpr float kConfirmRepeatDelaySeconds = 0.35F;

MainMenuItem IndexToItem(int index) {
    return static_cast<MainMenuItem>(std::clamp(index, 0, kItemCount - 1));
}

} // namespace

void MainMenu::Initialize(IUISoundBackend* soundBackend) {
    sound_ = soundBackend;
    highlightedIndex_ = 0;
    activeIndex_ = -1;
    optionsOpen_ = false;
    optionsFieldIndex_ = 0;
    activePulse_ = 0.0F;
    lastHoveredIndex_ = -1;
    mapCount_ = 1;
    chapterCount_ = 0;
    chapterIndex_ = 0;
    difficultyIndex_ = 1;
    hoverChapter_ = -1;
    hoverDifficulty_ = -1;
    chapterSelectOpen_ = false;
}

void MainMenu::SyncOptionsFromEngine(bool fullscreen, float mouseSensitivity, int selectedMapIndex) {
    options_.fullscreen = fullscreen;
    options_.mouseSensitivity = mouseSensitivity;
    options_.selectedMapIndex = selectedMapIndex;
}

void MainMenu::SetMapCount(int mapCount) {
    mapCount_ = std::max(mapCount, 1);
    options_.selectedMapIndex = std::clamp(options_.selectedMapIndex, 0, mapCount_ - 1);
}

void MainMenu::SetChapterCount(int chapterCount) {
    chapterCount_ = std::max(chapterCount, 0);
    if (chapterCount_ == 0) {
        chapterIndex_ = 0;
        chapterSelectOpen_ = false;
        return;
    }
    chapterIndex_ = std::clamp(chapterIndex_, 0, chapterCount_ - 1);
}

int MainMenu::ChapterScroll(int visibleRows) const {
    const int visible = std::max(visibleRows, 1);
    if (chapterIndex_ < visible) {
        return 0;
    }
    return chapterIndex_ - visible + 1;
}

void MainMenu::MoveChapter(int delta) {
    if (chapterCount_ <= 0 || delta == 0) {
        return;
    }
    const int previous = chapterIndex_;
    chapterIndex_ = (chapterIndex_ + delta + chapterCount_) % chapterCount_;
    if (chapterIndex_ != previous && sound_ != nullptr) {
        sound_->PlayHover();
        sound_->PlayRaw("ui/buttonrollover.wav");
    }
}

void MainMenu::AdjustDifficulty(int direction) {
    if (direction == 0) {
        return;
    }
    const int previous = difficultyIndex_;
    difficultyIndex_ = (difficultyIndex_ + direction + kDifficultyCount) % kDifficultyCount;
    if (difficultyIndex_ != previous && sound_ != nullptr) {
        sound_->PlayHover();
        sound_->PlayRaw("ui/buttonrollover.wav");
    }
}

MenuAction MainMenu::StartHighlightedChapter() {
    if (chapterCount_ <= 0) {
        return MenuAction::None;
    }
    if (sound_ != nullptr) {
        sound_->PlaySelect();
        sound_->PlayRaw("ui/buttonclick.wav");
    }
    chapterSelectOpen_ = false;
    return MenuAction::StartChapter;
}

void MainMenu::MoveOptionsField(int delta) {
    constexpr int fieldCount = 5;
    optionsFieldIndex_ = (optionsFieldIndex_ + delta + fieldCount) % fieldCount;
    if (sound_ != nullptr) {
        sound_->PlayHover();
    }
}

void MainMenu::AdjustOptionsField(int direction) {
    if (direction == 0) {
        return;
    }
    if (sound_ != nullptr) {
        sound_->PlayHover();
    }
    switch (optionsFieldIndex_) {
    case 0:
        options_.fullscreen = !options_.fullscreen;
        break;
    case 1:
        options_.mouseSensitivity = std::clamp(
            options_.mouseSensitivity + static_cast<float>(direction) * 0.02F,
            0.02F,
            0.4F
        );
        break;
    case 2:
        options_.masterVolume = std::clamp(
            options_.masterVolume + static_cast<float>(direction) * 0.05F,
            0.0F,
            1.0F
        );
        break;
    case 3:
        options_.sfxVolume = std::clamp(
            options_.sfxVolume + static_cast<float>(direction) * 0.05F,
            0.0F,
            1.0F
        );
        break;
    case 4:
        if (mapCount_ > 0) {
            options_.selectedMapIndex = (options_.selectedMapIndex + direction + mapCount_) % mapCount_;
        }
        break;
    default:
        break;
    }
}

void MainMenu::SetOptionsOpen(bool open) {
    if (optionsOpen_ == open) {
        return;
    }
    optionsOpen_ = open;
    if (!open && sound_ != nullptr) {
        sound_->PlayCancel();
    }
}

void MainMenu::Update(float deltaTime) {
    activePulse_ += deltaTime;
    if (activeIndex_ >= 0) {
        activePulse_ = 0.0F;
    }
    if (confirmCooldown_ > 0.0F) {
        confirmCooldown_ = std::max(0.0F, confirmCooldown_ - deltaTime);
    }
}

void MainMenu::ArmConfirmCooldown() {
    confirmCooldown_ = kConfirmRepeatDelaySeconds;
}

void MainMenu::MoveHighlight(int delta) {
    const int previous = highlightedIndex_;
    highlightedIndex_ = (highlightedIndex_ + delta + kItemCount) % kItemCount;
    if (highlightedIndex_ != previous && sound_ != nullptr) {
        sound_->PlayHover();
        sound_->PlayRaw("ui/buttonrollover.wav");
    }
}

MenuAction MainMenu::ActivateItem(MainMenuItem item) {
    if (sound_ != nullptr) {
        sound_->PlaySelect();
        sound_->PlayRaw("ui/buttonclick.wav");
    }
    activeIndex_ = static_cast<int>(item);

    switch (item) {
    case MainMenuItem::NewGame:
        if (chapterCount_ > 0) {
            chapterSelectOpen_ = true;
            chapterIndex_ = std::clamp(chapterIndex_, 0, chapterCount_ - 1);
            return MenuAction::None;
        }
        return MenuAction::NewGame;
    case MainMenuItem::LoadGame:
        return MenuAction::LoadGame;
    case MainMenuItem::MapTest:
        return MenuAction::MapTest;
    case MainMenuItem::Options:
        optionsOpen_ = true;
        return MenuAction::OpenOptions;
    case MainMenuItem::Quit:
        return MenuAction::Quit;
    default:
        return MenuAction::None;
    }
}

void MainMenu::UpdateChapterHover(const MenuInput& input) {
    hoverChapter_ = -1;
    hoverDifficulty_ = -1;
    if (!input.mouseValid) {
        return;
    }

    const int visible = VisibleChapterRows(input.displaySize.y);
    const int scroll = ChapterScroll(visible);
    for (int row = 0; row < visible; ++row) {
        const int index = scroll + row;
        if (index >= chapterCount_) {
            break;
        }
        const float y0 = kChapterListTop + static_cast<float>(row) * kChapterLineHeight;
        const float y1 = y0 + kChapterLineHeight;
        if (input.mousePosition.x >= kChapterListLeft
            && input.mousePosition.x <= kChapterListLeft + kChapterHitWidth
            && input.mousePosition.y >= y0
            && input.mousePosition.y <= y1) {
            hoverChapter_ = index;
            break;
        }
    }

    if (input.mousePosition.y >= kChapterDifficultyY
        && input.mousePosition.y <= kChapterDifficultyY + 32.0F) {
        for (int index = 0; index < kDifficultyCount; ++index) {
            const float x0 = kChapterPreviewX + static_cast<float>(index) * kChapterDifficultyGap;
            if (input.mousePosition.x >= x0 && input.mousePosition.x <= x0 + 110.0F) {
                hoverDifficulty_ = index;
                break;
            }
        }
    }

    if (hoverChapter_ >= 0 && hoverChapter_ != chapterIndex_) {
        chapterIndex_ = hoverChapter_;
        if (sound_ != nullptr) {
            sound_->PlayHover();
            sound_->PlayRaw("ui/buttonrollover.wav");
        }
    }
}

void MainMenu::UpdateHoverFromMouse(const MenuInput& input) {
    if (!input.mouseValid || optionsOpen_ || chapterSelectOpen_) {
        lastHoveredIndex_ = -1;
        return;
    }

    const float left = kMainMenuLeft;
    const float top = kMainMenuTop;
    const float lineHeight = kMainMenuLineHeight;
    const float hitWidth = kMainMenuHitWidth;

    int hoverIndex = -1;
    for (int i = 0; i < kItemCount; ++i) {
        const float y0 = top + static_cast<float>(i) * lineHeight;
        const float y1 = y0 + lineHeight;
        if (input.mousePosition.x >= left && input.mousePosition.x <= left + hitWidth
            && input.mousePosition.y >= y0 && input.mousePosition.y <= y1) {
            hoverIndex = i;
            break;
        }
    }

    if (hoverIndex >= 0 && hoverIndex != highlightedIndex_) {
        highlightedIndex_ = hoverIndex;
        if (hoverIndex != lastHoveredIndex_ && sound_ != nullptr) {
            sound_->PlayHover();
            sound_->PlayRaw("ui/buttonrollover.wav");
        }
    }
    lastHoveredIndex_ = hoverIndex;
}

MenuAction MainMenu::HandleInput(const MenuInput& input) {
    const bool confirmPressed = input.confirmPressed && confirmCooldown_ <= 0.0F;

    if (chapterSelectOpen_) {
        UpdateChapterHover(input);
        if (input.backPressed) {
            chapterSelectOpen_ = false;
            if (sound_ != nullptr) {
                sound_->PlayCancel();
            }
            ArmConfirmCooldown();
            return MenuAction::None;
        }
        if (input.upPressed) {
            MoveChapter(-1);
        }
        if (input.downPressed) {
            MoveChapter(1);
        }
        if (input.leftPressed) {
            AdjustDifficulty(-1);
        }
        if (input.rightPressed) {
            AdjustDifficulty(1);
        }
        if (confirmPressed) {
            ArmConfirmCooldown();
            return StartHighlightedChapter();
        }
        if (input.mouseLeftPressed && confirmCooldown_ <= 0.0F) {
            if (hoverDifficulty_ >= 0) {
                if (hoverDifficulty_ != difficultyIndex_ && sound_ != nullptr) {
                    sound_->PlayHover();
                    sound_->PlayRaw("ui/buttonrollover.wav");
                }
                difficultyIndex_ = hoverDifficulty_;
                ArmConfirmCooldown();
                return MenuAction::None;
            }
            if (hoverChapter_ >= 0) {
                chapterIndex_ = hoverChapter_;
                ArmConfirmCooldown();
                return StartHighlightedChapter();
            }
        }
        return MenuAction::None;
    }

    if (optionsOpen_) {
        if (input.backPressed) {
            SetOptionsOpen(false);
            ArmConfirmCooldown();
            return MenuAction::CloseOptions;
        }
        if (input.upPressed) {
            MoveOptionsField(-1);
        }
        if (input.downPressed) {
            MoveOptionsField(1);
        }
        if (input.leftPressed) {
            AdjustOptionsField(-1);
        }
        if (input.rightPressed) {
            AdjustOptionsField(1);
        }
        if (confirmPressed) {
            SetOptionsOpen(false);
            if (sound_ != nullptr) {
                sound_->PlaySelect();
            }
            ArmConfirmCooldown();
            return MenuAction::CloseOptions;
        }
        return MenuAction::None;
    }

    UpdateHoverFromMouse(input);

    if (input.upPressed) {
        MoveHighlight(-1);
    }
    if (input.downPressed) {
        MoveHighlight(1);
    }

    if (confirmPressed) {
        ArmConfirmCooldown();
        return ActivateItem(IndexToItem(highlightedIndex_));
    }

    if (input.mouseLeftPressed && lastHoveredIndex_ >= 0 && confirmCooldown_ <= 0.0F) {
        highlightedIndex_ = lastHoveredIndex_;
        ArmConfirmCooldown();
        return ActivateItem(IndexToItem(highlightedIndex_));
    }

    return MenuAction::None;
}

} // namespace ui
