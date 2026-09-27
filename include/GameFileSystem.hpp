#pragma once

#include "VpkArchive.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

class GameFileSystem {
public:
    bool Mount(const std::filesystem::path& gameDirectory);

    [[nodiscard]] const std::filesystem::path& GameDirectory() const;
    [[nodiscard]] const std::string& GameLabel() const;
    [[nodiscard]] bool Exists(std::string_view relativePath) const;
    [[nodiscard]] bool Read(std::string_view relativePath, std::vector<std::uint8_t>& out) const;
    [[nodiscard]] std::vector<std::string> List(std::string_view directory, std::string_view extension) const;
    [[nodiscard]] std::filesystem::path Materialize(std::string_view relativePath) const;

private:
    struct MountPoint {
        std::filesystem::path loose;
        std::unique_ptr<VpkArchive> archive;
    };

    void MountDirectory(const std::filesystem::path& directory);
    void MountVpk(const std::filesystem::path& directoryArchive);
    void MountSearchPath(const std::filesystem::path& path);
    void MountCustom(const std::filesystem::path& customDirectory);
    [[nodiscard]] std::string Expand(std::string value) const;
    [[nodiscard]] static std::string Normalize(std::string_view relativePath);

    std::filesystem::path gameDirectory_;
    std::filesystem::path cacheDirectory_;
    std::string gameLabel_;
    std::vector<MountPoint> mounts_;
};
