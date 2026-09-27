#pragma once

#include <filesystem>
#include <memory>
#include <string_view>

class GameFileSystem;

class AudioSystem {
public:
    AudioSystem();
    ~AudioSystem();

    AudioSystem(const AudioSystem&) = delete;
    AudioSystem& operator=(const AudioSystem&) = delete;

    bool Initialize(const std::filesystem::path& contentRoot, const GameFileSystem* files = nullptr);
    void Shutdown();

    void PlayOneShot(std::string_view relativePath, float volume = 1.0F);
    void PlayFootstep();
    void StartAmbientLoop(float volume = 0.45F);
    void StopAmbientLoop();

private:
    [[nodiscard]] std::filesystem::path Resolve(std::string_view relativePath) const;

    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::filesystem::path contentRoot_;
    const GameFileSystem* files_ = nullptr;
};
