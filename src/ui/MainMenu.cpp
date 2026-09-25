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

void MainMenu::UpdateHoverFromMouse(const MenuInput& input) {
    if (!input.mouseValid || optionsOpen_) {
        lastHoveredIndex_ = -1;
        return;
    }

    const float left = 72.0F;
    const float top = 220.0F;
    const float lineHeight = 44.0F;
    const float hitWidth = 420.0F;

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
