#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

class VpkArchive {
public:
    explicit VpkArchive(std::filesystem::path directoryArchive);

    [[nodiscard]] bool Contains(std::string relativePath) const;
    [[nodiscard]] bool Read(std::string relativePath, std::vector<std::uint8_t>& out) const;
    [[nodiscard]] std::vector<std::string> Entries() const;

private:
    struct Location {
        std::uint16_t archiveIndex = 0;
        std::uint32_t offset = 0;
        std::uint32_t length = 0;
        std::vector<std::uint8_t> preload;
    };

    std::filesystem::path directoryPath_;
    std::vector<std::uint8_t> directoryBytes_;
    std::unordered_map<std::string, Location> index_;
};
