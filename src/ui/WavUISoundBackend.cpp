#include "ui/WavUISoundBackend.hpp"

#include "AudioSystem.hpp"

namespace ui {

WavUISoundBackend::WavUISoundBackend(AudioSystem* audio) : audio_(audio) {}

void WavUISoundBackend::PlayHover() {}

void WavUISoundBackend::PlaySelect() {}

void WavUISoundBackend::PlayCancel() {}

void WavUISoundBackend::PlayRaw(std::string_view path) {
    if (audio_ != nullptr) {
        audio_->PlayOneShot(path, 1.0F);
    }
}

} // namespace ui
