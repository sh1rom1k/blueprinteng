#include "VtfTexture.hpp"

#include "VpkArchive.hpp"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <limits>
#include <memory>
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
constexpr std::uint32_t kFormatBgr888 = 3;
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

TextureFormat GetFormat(std::uint32_t format) {
    switch (format) {
    case kFormatRgba8888:
        return {false, 4, GL_RGBA, GL_RGBA8};
    case kFormatBgr888:
        return {false, 3, GL_RGB, GL_RGB8};
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
    if (versionMajor != 7 || versionMinor > 5) {
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
    std::vector<std::uint8_t> swapped;
    if (highResFormat == kFormatBgr888) {
        swapped.resize(imageSize);
        for (std::size_t index = 0; index + 2 < imageSize; index += 3) {
            swapped[index] = data[highResOffset + index + 2];
            swapped[index + 1] = data[highResOffset + index + 1];
            swapped[index + 2] = data[highResOffset + index];
        }
        pixels = swapped.data();
    }
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

GLuint LoadVtfCubemapData(const std::vector<std::uint8_t>& data, const std::string& source) {
    if (data.size() < kMinimumHeaderSize) {
        Fail("file is smaller than a VTF header");
    }
    if (ReadU32(data, 0) != kVtfSignature) {
        Fail("invalid signature in " + source + ", expected VTF\\0");
    }

    const std::uint32_t versionMajor = ReadU32(data, 4);
    const std::uint32_t versionMinor = ReadU32(data, 8);
    const std::uint32_t headerSize = ReadU32(data, 12);
    if (versionMajor != 7 || versionMinor > 5) {
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
    if ((flags & kEnvMapFlag) == 0 || width == 0 || height == 0) {
        Fail("cubemap flag is missing in " + source);
    }

    const std::size_t lowResSize = ImageSize(lowResFormat, lowResWidth, lowResHeight);
    std::size_t highResOffset = CheckedOffset(headerSize, lowResSize, data.size());
    const std::uint32_t storedFaces = versionMinor < 5 ? 7U : 6U;
    const TextureFormat textureFormat = GetFormat(highResFormat);
    for (std::uint32_t mip = mipmapCount - 1; mip > 0; --mip) {
        const std::uint32_t mipWidth = std::max<std::uint32_t>(1, width >> mip);
        const std::uint32_t mipHeight = std::max<std::uint32_t>(1, height >> mip);
        const std::size_t mipSize = ImageSize(highResFormat, mipWidth, mipHeight);
        const std::uintmax_t images = static_cast<std::uintmax_t>(frames) * storedFaces;
        const std::uintmax_t skip = static_cast<std::uintmax_t>(mipSize) * images;
        if (skip > std::numeric_limits<std::size_t>::max()) {
            Fail("mipmap data is too large");
        }
        highResOffset = CheckedOffset(highResOffset, static_cast<std::size_t>(skip), data.size());
    }

    const std::size_t imageSize = ImageSize(highResFormat, width, height);
    CheckedOffset(highResOffset, imageSize * 6U, data.size());

    static const GLenum kFaces[6] = {
        GL_TEXTURE_CUBE_MAP_POSITIVE_X,
        GL_TEXTURE_CUBE_MAP_NEGATIVE_X,
        GL_TEXTURE_CUBE_MAP_POSITIVE_Z,
        GL_TEXTURE_CUBE_MAP_NEGATIVE_Z,
        GL_TEXTURE_CUBE_MAP_POSITIVE_Y,
        GL_TEXTURE_CUBE_MAP_NEGATIVE_Y,
    };

    GLuint textureID = 0;
    glGenTextures(1, &textureID);
    glBindTexture(GL_TEXTURE_CUBE_MAP, textureID);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);

    for (int face = 0; face < 6; ++face) {
        const std::size_t faceOffset = highResOffset + imageSize * static_cast<std::size_t>(face);
        const void* pixels = data.data() + faceOffset;
        std::vector<std::uint8_t> swapped;
        if (highResFormat == kFormatBgr888) {
            swapped.resize(imageSize);
            for (std::size_t index = 0; index + 2 < imageSize; index += 3) {
                swapped[index] = data[faceOffset + index + 2];
                swapped[index + 1] = data[faceOffset + index + 1];
                swapped[index + 2] = data[faceOffset + index];
            }
            pixels = swapped.data();
        }
        if (textureFormat.compressed) {
            glCompressedTexImage2D(
                kFaces[face],
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
                kFaces[face],
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
    }

    glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
    return textureID;
}

GLuint LoadVtfTexture(const std::string& path) {
    return LoadVtfTextureData(ReadFile(path), path);
}

GLuint LoadVtfTextureFromVpk(const std::string& vpkPath, const std::string& internalPath) {
    static std::unordered_map<std::string, std::unique_ptr<VpkArchive>> archives;
    auto found = archives.find(vpkPath);
    if (found == archives.end()) {
        found = archives.emplace(vpkPath, std::make_unique<VpkArchive>(vpkPath)).first;
    }
    std::vector<std::uint8_t> bytes;
    if (!found->second->Read(internalPath, bytes)) {
        Fail("texture '" + internalPath + "' was not found in VPK");
    }
    return LoadVtfTextureData(bytes, vpkPath + ":" + internalPath);
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

GLuint CreateMissingTexture() {
    constexpr int kSize = 64;
    constexpr int kSquare = 8;
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(kSize * kSize * 4));
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            const bool purple = ((x / kSquare) + (y / kSquare)) % 2 == 0;
            const std::size_t index = static_cast<std::size_t>((y * kSize + x) * 4);
            pixels[index] = purple ? 255 : 0;
            pixels[index + 1] = 0;
            pixels[index + 2] = purple ? 255 : 0;
            pixels[index + 3] = 255;
        }
    }

    GLuint textureID = 0;
    glGenTextures(1, &textureID);
    glBindTexture(GL_TEXTURE_2D, textureID);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kSize, kSize, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    return textureID;
}

}
