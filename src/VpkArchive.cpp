#include "VpkArchive.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <stdexcept>

namespace {

[[noreturn]] void Fail(const std::string& message) {
    throw std::runtime_error("VPK: " + message);
}

std::uint16_t ReadU16(const std::vector<std::uint8_t>& data, std::size_t offset) {
    if (offset > data.size() || data.size() - offset < 2) {
        Fail("unexpected end of file");
    }
    return static_cast<std::uint16_t>(data[offset])
        | (static_cast<std::uint16_t>(data[offset + 1]) << 8U);
}

std::uint32_t ReadU32(const std::vector<std::uint8_t>& data, std::size_t offset) {
    if (offset > data.size() || data.size() - offset < 4) {
        Fail("unexpected end of file");
    }
    return static_cast<std::uint32_t>(data[offset])
        | (static_cast<std::uint32_t>(data[offset + 1]) << 8U)
        | (static_cast<std::uint32_t>(data[offset + 2]) << 16U)
        | (static_cast<std::uint32_t>(data[offset + 3]) << 24U);
}

std::uint16_t ReadU16Raw(const std::vector<std::uint8_t>& data, std::size_t& cursor) {
    const std::uint16_t value = ReadU16(data, cursor);
    cursor += 2;
    return value;
}

std::uint32_t ReadU32Raw(const std::vector<std::uint8_t>& data, std::size_t& cursor) {
    const std::uint32_t value = ReadU32(data, cursor);
    cursor += 4;
    return value;
}

std::string ReadCString(const std::vector<std::uint8_t>& data, std::size_t& cursor, std::size_t end) {
    const std::size_t start = cursor;
    while (cursor < end && data[cursor] != 0) {
        ++cursor;
    }
    if (cursor >= end) {
        Fail("truncated VPK directory tree");
    }
    const std::string value(reinterpret_cast<const char*>(data.data() + start), cursor - start);
    ++cursor;
    return value;
}

std::string Lowercase(std::string value) {
    for (char& character : value) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    std::replace(value.begin(), value.end(), '\\', '/');
    return value;
}

std::vector<std::uint8_t> ReadFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        Fail("unable to open '" + path.string() + "'");
    }
    file.seekg(0, std::ios::end);
    const std::streamoff size = file.tellg();
    if (size < 0) {
        Fail("unable to determine file size");
    }
    file.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
    if (!data.empty() && !file.read(reinterpret_cast<char*>(data.data()), size)) {
        Fail("unable to read '" + path.string() + "'");
    }
    return data;
}

std::vector<std::uint8_t> ReadFileRange(const std::filesystem::path& path, std::size_t offset, std::size_t length) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        Fail("unable to open '" + path.string() + "'");
    }
    file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    std::vector<std::uint8_t> data(length);
    if (!data.empty() && !file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(length))) {
        Fail("unable to read VPK data from '" + path.string() + "'");
    }
    return data;
}

} // namespace

VpkArchive::VpkArchive(std::filesystem::path directoryArchive)
    : directoryPath_(std::move(directoryArchive)) {
    directoryBytes_ = ReadFile(directoryPath_);
    if (directoryBytes_.size() < 12 || ReadU32(directoryBytes_, 0) != 0x55AA1234) {
        Fail("invalid VPK archive");
    }
    const std::uint32_t version = ReadU32(directoryBytes_, 4);
    const std::uint32_t treeSize = ReadU32(directoryBytes_, 8);
    const std::size_t headerSize = version == 1 ? 12 : 28;
    if ((version != 1 && version != 2) || headerSize + treeSize > directoryBytes_.size()) {
        Fail("unsupported or truncated VPK header");
    }

    std::size_t cursor = headerSize;
    const std::size_t treeEnd = headerSize + treeSize;
    while (cursor < treeEnd) {
        const std::string extension = ReadCString(directoryBytes_, cursor, treeEnd);
        if (extension.empty()) {
            break;
        }
        while (cursor < treeEnd) {
            const std::string path = ReadCString(directoryBytes_, cursor, treeEnd);
            if (path.empty()) {
                break;
            }
            while (cursor < treeEnd) {
                const std::string filename = ReadCString(directoryBytes_, cursor, treeEnd);
                if (filename.empty()) {
                    break;
                }
                if (cursor + 18 > treeEnd) {
                    Fail("truncated VPK entry");
                }
                static_cast<void>(ReadU32Raw(directoryBytes_, cursor));
                const std::uint16_t preloadSize = ReadU16Raw(directoryBytes_, cursor);
                const std::uint16_t archiveIndex = ReadU16Raw(directoryBytes_, cursor);
                const std::uint32_t entryOffset = ReadU32Raw(directoryBytes_, cursor);
                const std::uint32_t entryLength = ReadU32Raw(directoryBytes_, cursor);
                if (ReadU16Raw(directoryBytes_, cursor) != 0xFFFF) {
                    Fail("invalid VPK entry terminator");
                }
                const std::string entryPath = Lowercase(
                    path == " " ? filename + "." + extension : path + "/" + filename + "." + extension
                );
                const std::size_t preloadOffset = cursor;
                if (preloadSize > treeEnd - cursor) {
                    Fail("truncated VPK preload");
                }
                cursor += preloadSize;
                Location location;
                location.archiveIndex = archiveIndex;
                location.offset = entryOffset;
                location.length = entryLength;
                location.preload.assign(
                    directoryBytes_.begin() + static_cast<std::ptrdiff_t>(preloadOffset),
                    directoryBytes_.begin() + static_cast<std::ptrdiff_t>(preloadOffset + preloadSize)
                );
                index_.emplace(entryPath, std::move(location));
            }
        }
    }
}

bool VpkArchive::Contains(std::string relativePath) const {
    return index_.find(Lowercase(std::move(relativePath))) != index_.end();
}

bool VpkArchive::Read(std::string relativePath, std::vector<std::uint8_t>& out) const {
    const auto entry = index_.find(Lowercase(std::move(relativePath)));
    if (entry == index_.end()) {
        return false;
    }
    const Location& location = entry->second;
    out = location.preload;
    if (location.length == 0) {
        return true;
    }
    if (location.archiveIndex == 0x7FFF) {
        const std::uint32_t treeSize = ReadU32(directoryBytes_, 8);
        const std::size_t headerSize = ReadU32(directoryBytes_, 4) == 1 ? 12 : 28;
        const std::size_t dataOffset = headerSize + treeSize + location.offset;
        if (dataOffset > directoryBytes_.size() || location.length > directoryBytes_.size() - dataOffset) {
            Fail("embedded VPK entry points outside the directory");
        }
        out.insert(
            out.end(),
            directoryBytes_.begin() + static_cast<std::ptrdiff_t>(dataOffset),
            directoryBytes_.begin() + static_cast<std::ptrdiff_t>(dataOffset + location.length)
        );
        return true;
    }

    const std::string filename = directoryPath_.filename().string();
    const std::string suffix = "_dir.vpk";
    if (filename.size() <= suffix.size() || filename.substr(filename.size() - suffix.size()) != suffix) {
        Fail("VPK archive is not a directory archive");
    }
    const std::string indexText = (location.archiveIndex < 10 ? "00" : location.archiveIndex < 100 ? "0" : "")
        + std::to_string(location.archiveIndex);
    std::filesystem::path archivePath = directoryPath_;
    archivePath.replace_filename(
        filename.substr(0, filename.size() - suffix.size()) + "_" + indexText + ".vpk"
    );
    const std::vector<std::uint8_t> archiveData = ReadFileRange(archivePath, location.offset, location.length);
    out.insert(out.end(), archiveData.begin(), archiveData.end());
    return true;
}

std::vector<std::string> VpkArchive::Entries() const {
    std::vector<std::string> entries;
    entries.reserve(index_.size());
    for (const auto& entry : index_) {
        entries.push_back(entry.first);
    }
    return entries;
}
