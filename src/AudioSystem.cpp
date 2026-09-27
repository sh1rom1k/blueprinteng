#include "AudioSystem.hpp"

#include "GameFileSystem.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <string>

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

namespace {

constexpr ma_uint32 kFootstepSoundFlags =
    MA_SOUND_FLAG_DECODE | MA_SOUND_FLAG_NO_PITCH | MA_SOUND_FLAG_NO_SPATIALIZATION;

constexpr ma_uint32 kAmbientSoundFlags =
    MA_SOUND_FLAG_DECODE | MA_SOUND_FLAG_LOOPING | MA_SOUND_FLAG_NO_PITCH | MA_SOUND_FLAG_NO_SPATIALIZATION;

} // namespace

struct AudioSystem::Impl {
    ma_engine engine{};
    ma_sound ambient{};
    std::array<ma_sound, 4> footsteps{};
    bool engineReady = false;
    bool ambientReady = false;
    bool ambientPlaying = false;
    std::array<bool, 4> footstepReady{};
};

AudioSystem::AudioSystem() : impl_(std::make_unique<Impl>()) {}

AudioSystem::~AudioSystem() {
    Shutdown();
}

bool AudioSystem::Initialize(const std::filesystem::path& contentRoot, const GameFileSystem* files) {
    if (impl_->engineReady) {
        return true;
    }

    contentRoot_ = contentRoot;
    files_ = files;
    ma_engine_config engineConfig = ma_engine_config_init();
    engineConfig.channels = 2;
    engineConfig.sampleRate = 44100;
    engineConfig.periodSizeInMilliseconds = 20;

    const ma_result result = ma_engine_init(&engineConfig, &impl_->engine);
    if (result != MA_SUCCESS) {
        std::cerr << "Audio engine init failed (code " << static_cast<int>(result) << ")\n";
        return false;
    }

    impl_->engineReady = true;

    const std::filesystem::path ambientPath = Resolve("sound/ambient_loop.wav");
    const ma_result ambientResult = ma_sound_init_from_file(
        &impl_->engine,
        ambientPath.string().c_str(),
        kAmbientSoundFlags,
        nullptr,
        nullptr,
        &impl_->ambient
    );
    if (ambientResult != MA_SUCCESS) {
        std::cerr << "Failed to load ambient loop: " << ambientPath << '\n';
    } else {
        impl_->ambientReady = true;
        ma_sound_set_volume(&impl_->ambient, 0.45F);
    }

    for (std::size_t index = 0; index < impl_->footsteps.size(); ++index) {
        const std::string footstepPath =
            Resolve("sound/concrete" + std::to_string(index + 1) + ".wav").string();
        const ma_result footstepResult = ma_sound_init_from_file(
            &impl_->engine,
            footstepPath.c_str(),
            kFootstepSoundFlags,
            nullptr,
            nullptr,
            &impl_->footsteps[index]
        );
        if (footstepResult != MA_SUCCESS) {
            std::cerr << "Failed to load footstep: " << footstepPath << '\n';
            impl_->footstepReady[index] = false;
        } else {
            impl_->footstepReady[index] = true;
            ma_sound_set_volume(&impl_->footsteps[index], 0.9F);
        }
    }

    return true;
}

void AudioSystem::Shutdown() {
    if (!impl_->engineReady) {
        return;
    }

    if (impl_->ambientReady) {
        ma_sound_stop(&impl_->ambient);
        ma_sound_uninit(&impl_->ambient);
        impl_->ambientReady = false;
        impl_->ambientPlaying = false;
    }

    for (std::size_t index = 0; index < impl_->footsteps.size(); ++index) {
        if (!impl_->footstepReady[index]) {
            continue;
        }
        ma_sound_stop(&impl_->footsteps[index]);
        ma_sound_uninit(&impl_->footsteps[index]);
        impl_->footstepReady[index] = false;
    }

    ma_engine_uninit(&impl_->engine);
    impl_->engineReady = false;
}

std::filesystem::path AudioSystem::Resolve(std::string_view relativePath) const {
    if (files_ != nullptr && files_->Exists(relativePath)) {
        const std::filesystem::path materialized = files_->Materialize(relativePath);
        if (!materialized.empty()) {
            return materialized;
        }
    }
    return contentRoot_ / relativePath;
}

void AudioSystem::PlayOneShot(std::string_view relativePath, float volume) {
    if (!impl_->engineReady) {
        return;
    }

    const std::filesystem::path absolutePath = Resolve(relativePath);
    const ma_result playResult = ma_engine_play_sound(
        &impl_->engine,
        absolutePath.string().c_str(),
        nullptr
    );
    if (playResult != MA_SUCCESS) {
        return;
    }

    (void)volume;
}

void AudioSystem::PlayFootstep() {
    if (!impl_->engineReady) {
        return;
    }

    const std::size_t index = static_cast<std::size_t>(std::rand() % 4);
    if (!impl_->footstepReady[index]) {
        return;
    }

    ma_sound* footstep = &impl_->footsteps[index];
    ma_sound_stop(footstep);
    ma_sound_seek_to_pcm_frame(footstep, 0);
    ma_sound_start(footstep);
}

void AudioSystem::StartAmbientLoop(float volume) {
    if (!impl_->engineReady || !impl_->ambientReady) {
        return;
    }

    ma_sound_set_volume(&impl_->ambient, volume);
    if (!impl_->ambientPlaying) {
        ma_sound_start(&impl_->ambient);
        impl_->ambientPlaying = true;
        return;
    }

    if (!ma_sound_is_playing(&impl_->ambient)) {
        ma_sound_start(&impl_->ambient);
    }
}

void AudioSystem::StopAmbientLoop() {
    if (!impl_->ambientReady || !impl_->ambientPlaying) {
        return;
    }
    ma_sound_stop(&impl_->ambient);
    impl_->ambientPlaying = false;
}
