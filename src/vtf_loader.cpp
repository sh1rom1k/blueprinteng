#include "VtfTexture.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <cctype>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

namespace {
constexpr std::uint32_t kVtfSignature = 0x00465456;
constexpr std::uint32_t kEnvMapFlag = 0x00004000;
constexpr std::size_t kHighResFormatOffset = 52;
constexpr std::size_t kMipmapCountOffset = 56;
constexpr std::size_t kLowResFormatOffset = 57;
constexpr std::size_t kLowResWidthOffset = 61;
constexpr std::size_t kLowResHeightOffset = 62;
constexpr std::size_t kMinimumHeaderSize = 63;

constexpr std::uint32_t kFormatRgba8888 = 0;
constexpr std::uint32_t kFormatBgra8888 = 12;
constexpr std::uint32_t kFormatDxt1 = 13;
constexpr std::uint32_t kFormatDxt3 = 14;
constexpr std::uint32_t kFormatDxt5 = 15;
constexpr std::uint32_t kFormatNone = 0xFFFFFFFF;

struct TextureFormat {
    bool compressed = false;
    std::size_t bytesPerPixel = 0;
    GLenum externalFormat = GL_RGBA;
    GLenum internalFormat = GL_RGBA8;
};

[[noreturn]] void Fail(const std::string& message) {
    throw std::runtime_error("VTF loader: " + message);
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

std::vector<std::uint8_t> ReadFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        Fail("unable to open '" + path + "'");
    }

    file.seekg(0, std::ios::end);
    const std::streamoff size = file.tellg();
    if (size < 0) {
        Fail("unable to determine file size");
    }
    file.seekg(0, std::ios::beg);

    std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
    if (!data.empty() && !file.read(reinterpret_cast<char*>(data.data()), size)) {
        Fail("unable to read '" + path + "'");
    }
    return data;
}

std::vector<std::uint8_t> ReadFileRange(const std::string& path, std::size_t offset, std::size_t length) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        Fail("unable to open '" + path + "'");
    }
    file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!file) {
        Fail("invalid data offset in '" + path + "'");
    }
    std::vector<std::uint8_t> data(length);
    if (!data.empty() && !file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(length))) {
        Fail("unable to read VPK data from '" + path + "'");
    }
    return data;
}

const std::vector<std::uint8_t>& ReadCachedFile(const std::string& path) {
    static std::unordered_map<std::string, std::vector<std::uint8_t>> cache;
    const auto cached = cache.find(path);
    if (cached != cache.end()) {
        return cached->second;
    }
    return cache.emplace(path, ReadFile(path)).first->second;
}

TextureFormat GetFormat(std::uint32_t format) {
    switch (format) {
    case kFormatRgba8888:
        return {false, 4, GL_RGBA, GL_RGBA8};
    case kFormatBgra8888:
        return {false, 4, GL_BGRA, GL_RGBA8};
    case kFormatDxt1:
        return {true, 8, GL_RGBA, GL_COMPRESSED_RGBA_S3TC_DXT1_EXT};
    case kFormatDxt3:
        return {true, 16, GL_RGBA, GL_COMPRESSED_RGBA_S3TC_DXT3_EXT};
    case kFormatDxt5:
        return {true, 16, GL_RGBA, GL_COMPRESSED_RGBA_S3TC_DXT5_EXT};
    default:
        Fail("unsupported image format " + std::to_string(format));
    }
}

std::size_t ImageSize(std::uint32_t format, std::uint32_t width, std::uint32_t height) {
    if (format == kFormatNone) {
        return 0;
    }
    const TextureFormat textureFormat = GetFormat(format);
    if (!textureFormat.compressed) {
        const std::uintmax_t size = static_cast<std::uintmax_t>(width)
            * static_cast<std::uintmax_t>(height)
            * textureFormat.bytesPerPixel;
        if (size > std::numeric_limits<std::size_t>::max()) {
            Fail("image is too large");
        }
        return static_cast<std::size_t>(size);
    }

    const std::uintmax_t blocksWide = std::max<std::uint32_t>(1, (width + 3) / 4);
    const std::uintmax_t blocksHigh = std::max<std::uint32_t>(1, (height + 3) / 4);
    const std::uintmax_t size = blocksWide * blocksHigh * textureFormat.bytesPerPixel;
    if (size > std::numeric_limits<std::size_t>::max()) {
        Fail("compressed image is too large");
    }
    return static_cast<std::size_t>(size);
}

std::size_t CheckedOffset(std::size_t offset, std::size_t amount, std::size_t fileSize) {
    if (offset > fileSize || amount > fileSize - offset) {
        Fail("texture data points outside the file");
    }
    return offset + amount;
}

std::uint16_t ReadU16Raw(const std::vector<std::uint8_t>& data, std::size_t& cursor) {
    const std::uint16_t value = static_cast<std::uint16_t>(data.at(cursor))
        | (static_cast<std::uint16_t>(data.at(cursor + 1)) << 8U);
    cursor += 2;
    return value;
}

std::uint32_t ReadU32Raw(const std::vector<std::uint8_t>& data, std::size_t& cursor) {
    const std::uint32_t value = static_cast<std::uint32_t>(data.at(cursor))
        | (static_cast<std::uint32_t>(data.at(cursor + 1)) << 8U)
        | (static_cast<std::uint32_t>(data.at(cursor + 2)) << 16U)
        | (static_cast<std::uint32_t>(data.at(cursor + 3)) << 24U);
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
    return value;
}

struct VpkLocation {
    std::uint16_t archiveIndex = 0;
    std::uint32_t offset = 0;
    std::uint32_t length = 0;
    std::vector<std::uint8_t> preload;
};

using VpkIndex = std::unordered_map<std::string, VpkLocation>;

VpkIndex BuildVpkIndex(const std::vector<std::uint8_t>& directory) {
    if (directory.size() < 12 || ReadU32(directory, 0) != 0x55AA1234) {
        Fail("invalid VPK archive");
    }
    const std::uint32_t version = ReadU32(directory, 4);
    const std::uint32_t treeSize = ReadU32(directory, 8);
    const std::size_t headerSize = version == 1 ? 12 : 28;
    if ((version != 1 && version != 2) || headerSize + treeSize > directory.size()) {
        Fail("unsupported or truncated VPK header");
    }

    VpkIndex index;
    std::size_t cursor = headerSize;
    const std::size_t treeEnd = headerSize + treeSize;
    while (cursor < treeEnd) {
        const std::string extension = ReadCString(directory, cursor, treeEnd);
        if (extension.empty()) {
            break;
        }
        while (cursor < treeEnd) {
            const std::string path = ReadCString(directory, cursor, treeEnd);
            if (path.empty()) {
                break;
            }
            while (cursor < treeEnd) {
                const std::string filename = ReadCString(directory, cursor, treeEnd);
                if (filename.empty()) {
                    break;
                }
                if (cursor + 18 > treeEnd) {
                    Fail("truncated VPK entry");
                }
                static_cast<void>(ReadU32Raw(directory, cursor));
                const std::uint16_t preloadSize = ReadU16Raw(directory, cursor);
                const std::uint16_t archiveIndex = ReadU16Raw(directory, cursor);
                const std::uint32_t entryOffset = ReadU32Raw(directory, cursor);
                const std::uint32_t entryLength = ReadU32Raw(directory, cursor);
                if (ReadU16Raw(directory, cursor) != 0xFFFF) {
                    Fail("invalid VPK entry terminator");
                }
                const std::string entryPath = Lowercase(
                    path == " " ? filename + "." + extension : path + "/" + filename + "." + extension
                );
                const std::size_t preloadOffset = cursor;
                cursor = CheckedOffset(cursor, preloadSize, treeEnd);
                VpkLocation location;
                location.archiveIndex = archiveIndex;
                location.offset = entryOffset;
                location.length = entryLength;
                location.preload.assign(
                    directory.begin() + static_cast<std::ptrdiff_t>(preloadOffset),
                    directory.begin() + static_cast<std::ptrdiff_t>(preloadOffset + preloadSize)
                );
                index.emplace(entryPath, std::move(location));
            }
        }
    }
    return index;
}

std::vector<std::uint8_t> ReadVpkEntry(const std::string& vpkPath, const std::string& requestedPath) {
    static std::unordered_map<std::string, VpkIndex> indexCache;
    auto cachedIndex = indexCache.find(vpkPath);
    if (cachedIndex == indexCache.end()) {
        cachedIndex = indexCache.emplace(vpkPath, BuildVpkIndex(ReadCachedFile(vpkPath))).first;
    }
    const auto entry = cachedIndex->second.find(Lowercase(requestedPath));
    if (entry == cachedIndex->second.end()) {
        Fail("texture '" + requestedPath + "' was not found in VPK");
    }

    const VpkLocation& location = entry->second;
    std::vector<std::uint8_t> result = location.preload;
    if (location.archiveIndex == 0x7FFF) {
        const std::vector<std::uint8_t>& directory = ReadCachedFile(vpkPath);
        const std::uint32_t treeSize = ReadU32(directory, 8);
        const std::size_t headerSize = ReadU32(directory, 4) == 1 ? 12 : 28;
        const std::size_t dataOffset = CheckedOffset(headerSize + treeSize, location.offset, directory.size());
        CheckedOffset(dataOffset, location.length, directory.size());
        result.insert(result.end(), directory.begin() + static_cast<std::ptrdiff_t>(dataOffset),
            directory.begin() + static_cast<std::ptrdiff_t>(dataOffset + location.length));
    } else {
        std::filesystem::path archivePath(vpkPath);
        const std::string filenameWithoutDir = archivePath.filename().string();
        const std::string suffix = "_dir.vpk";
        if (filenameWithoutDir.size() <= suffix.size()
            || filenameWithoutDir.substr(filenameWithoutDir.size() - suffix.size()) != suffix) {
            Fail("VPK archive is not a directory archive");
        }
        archivePath.replace_filename(
            filenameWithoutDir.substr(0, filenameWithoutDir.size() - suffix.size())
            + "_" + (location.archiveIndex < 10 ? "00" : location.archiveIndex < 100 ? "0" : "")
            + std::to_string(location.archiveIndex) + ".vpk"
        );
        const std::vector<std::uint8_t> archiveData = ReadFileRange(
            archivePath.string(), location.offset, location.length
        );
        result.insert(result.end(), archiveData.begin(), archiveData.end());
    }
    return result;
}
}

namespace vtf {

GLuint LoadVtfTextureData(const std::vector<std::uint8_t>& data, const std::string& source) {
    if (data.size() < kMinimumHeaderSize) {
        Fail("file is smaller than a VTF header");
    }
    if (ReadU32(data, 0) != kVtfSignature) {
        Fail("invalid signature in " + source + ", expected VTF\\0");
    }

    const std::uint32_t versionMajor = ReadU32(data, 4);
    const std::uint32_t versionMinor = ReadU32(data, 8);
    const std::uint32_t headerSize = ReadU32(data, 12);
    if (versionMajor != 7 || versionMinor < 1 || versionMinor > 2) {
        Fail("unsupported VTF version " + std::to_string(versionMajor) + "." + std::to_string(versionMinor));
    }
    if (headerSize < kMinimumHeaderSize || headerSize > data.size()) {
        Fail("invalid header size");
    }

    const std::uint32_t width = ReadU16(data, 16);
    const std::uint32_t height = ReadU16(data, 18);
    const std::uint32_t flags = ReadU32(data, 20);
    const std::uint32_t frames = std::max<std::uint16_t>(1, ReadU16(data, 24));
    const std::uint32_t highResFormat = ReadU32(data, kHighResFormatOffset);
    const std::uint32_t lowResFormat = ReadU32(data, kLowResFormatOffset);
    const std::uint32_t lowResWidth = data[kLowResWidthOffset];
    const std::uint32_t lowResHeight = data[kLowResHeightOffset];
    const std::uint32_t mipmapCount = std::max<std::uint8_t>(1, data[kMipmapCountOffset]);
    if (width == 0 || height == 0) {
        Fail("texture has zero dimensions");
    }

    const std::size_t lowResSize = ImageSize(lowResFormat, lowResWidth, lowResHeight);
    std::size_t highResOffset = headerSize;
    highResOffset = CheckedOffset(highResOffset, lowResSize, data.size());

    const TextureFormat textureFormat = GetFormat(highResFormat);
    const std::uint32_t faceCount = (flags & kEnvMapFlag) != 0 ? 6 : 1;
    for (std::uint32_t mip = mipmapCount - 1; mip > 0; --mip) {
        const std::uint32_t mipWidth = std::max<std::uint32_t>(1, width >> mip);
        const std::uint32_t mipHeight = std::max<std::uint32_t>(1, height >> mip);
        const std::size_t mipSize = ImageSize(highResFormat, mipWidth, mipHeight);
        const std::uintmax_t images = static_cast<std::uintmax_t>(frames) * faceCount;
        const std::uintmax_t skip = static_cast<std::uintmax_t>(mipSize) * images;
        if (skip > std::numeric_limits<std::size_t>::max()) {
            Fail("mipmap data is too large");
        }
        highResOffset = CheckedOffset(highResOffset, static_cast<std::size_t>(skip), data.size());
    }

    const std::size_t imageSize = ImageSize(highResFormat, width, height);
    CheckedOffset(highResOffset, imageSize, data.size());

    GLuint textureID = 0;
    glGenTextures(1, &textureID);
    glBindTexture(GL_TEXTURE_2D, textureID);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

    const void* pixels = data.data() + highResOffset;
    if (textureFormat.compressed) {
        glCompressedTexImage2D(
            GL_TEXTURE_2D,
            0,
            textureFormat.internalFormat,
            static_cast<GLsizei>(width),
            static_cast<GLsizei>(height),
            0,
            static_cast<GLsizei>(imageSize),
            pixels
        );
    } else {
        glTexImage2D(
            GL_TEXTURE_2D,
            0,
            textureFormat.internalFormat,
            static_cast<GLsizei>(width),
            static_cast<GLsizei>(height),
            0,
            textureFormat.externalFormat,
            GL_UNSIGNED_BYTE,
            pixels
        );
    }

    glBindTexture(GL_TEXTURE_2D, 0);
    return textureID;
}

GLuint LoadVtfTexture(const std::string& path) {
    return LoadVtfTextureData(ReadFile(path), path);
}

GLuint LoadVtfTextureFromVpk(const std::string& vpkPath, const std::string& internalPath) {
    return LoadVtfTextureData(ReadVpkEntry(vpkPath, internalPath), vpkPath + ":" + internalPath);
}

GLuint LoadRasterTexture(const std::string& path) {
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_set_flip_vertically_on_load(1);
    unsigned char* pixels = stbi_load(path.c_str(), &width, &height, &channels, STBI_rgb_alpha);
    if (pixels == nullptr) {
        Fail("unable to decode raster texture '" + path + "'");
    }

    GLuint textureID = 0;
    glGenTextures(1, &textureID);
    glBindTexture(GL_TEXTURE_2D, textureID);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glGenerateMipmap(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, 0);
    stbi_image_free(pixels);
    return textureID;
}

}
