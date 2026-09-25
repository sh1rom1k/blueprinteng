#pragma once

#include "ui/IUISoundBackend.hpp"

class AudioSystem;

namespace ui {

class WavUISoundBackend final : public IUISoundBackend {
public:
    explicit WavUISoundBackend(AudioSystem* audio);

    void PlayHover() override;
    void PlaySelect() override;
    void PlayCancel() override;
    void PlayRaw(std::string_view path) override;

private:
    AudioSystem* audio_ = nullptr;
};

} // namespace ui
