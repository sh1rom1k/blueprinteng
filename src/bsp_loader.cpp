#include "BspLoader.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <regex>
#include <sstream>
#include <vector>

#include "Mesh.hpp"
#include "Platform.hpp"
#include "Shader.hpp"
#include "VtfTexture.hpp"

#include <glm/glm.hpp>
#include <glm/vec3.hpp>

namespace {
constexpr std::size_t kLumpCount = 64;
constexpr std::size_t kTexDataLump = 2;
constexpr std::size_t kVertexLump = 3;
constexpr std::size_t kTexInfoLump = 6;
constexpr std::size_t kFaceLump = 7;
constexpr std::size_t kLightingLump = 8;
constexpr std::size_t kEdgeLump = 12;
constexpr std::size_t kSurfEdgeLump = 13;
constexpr std::size_t kTexDataStringDataLump = 43;
constexpr std::size_t kTexDataStringTableLump = 44;
constexpr std::size_t kLightingHdrLump = 53;
constexpr std::size_t kEntitiesLump = 0;
constexpr std::uint32_t kSurfNolight = 0x0400U;
constexpr std::size_t kVertexSize = 12;
constexpr std::size_t kTexDataSize = 32;
constexpr std::size_t kTexInfoSize = 72;
constexpr std::size_t kFaceSize = 56;
constexpr std::size_t kEdgeSize = 4;
constexpr std::size_t kSurfEdgeSize = 4;

struct Lump {
    std::int32_t offset = 0;
    std::int32_t length = 0;
};

struct Point {
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
};

struct FaceLightmap {
    std::int32_t lightOffset = -1;
    std::int32_t minsS = 0;
    std::int32_t minsT = 0;
    std::int16_t texInfoIndex = -1;
    int luxelW = 0;
    int luxelH = 0;
    int atlasX = 0;
    int atlasY = 0;
    bool lit = false;
};

struct FacePolygon {
    std::vector<Point> points;
    std::vector<Point> sourcePoints;
    std::vector<std::array<float, 2>> uvs;
    std::vector<std::array<float, 2>> lightmapUvs;
    std::string material;
    FaceLightmap lightmap;
};

struct BatchGeometry {
    std::string material;
    std::vector<float> vertices;
    std::vector<unsigned int> indices;
    bool trigger = false;
};

struct LightmapAtlas {
    int width = 0;
    int height = 0;
    std::vector<float> rgb;
};

struct DecalSpawn {
    glm::vec3 position{0.0F};
    glm::vec3 normal{0.0F, 1.0F, 0.0F};
    glm::vec3 right{1.0F, 0.0F, 0.0F};
    glm::vec3 up{0.0F, 0.0F, 1.0F};
    float halfSize = 1.0F;
    std::string material;
    int faceIndex = -1;
    std::array<std::array<float, 2>, 4> lightmapUvs{{{0.0F, 0.0F}, {0.0F, 0.0F}, {0.0F, 0.0F}, {0.0F, 0.0F}}};
};

struct LoadedGeometry {
    std::vector<BatchGeometry> batches;
    LightmapAtlas lightmap;
    std::vector<glm::vec3> propPositions;
    std::vector<BspMapEntity> mapEntities;
    std::vector<DecalSpawn> decals;
    glm::vec3 playerStartPosition{0.0F};
    bool hasPlayerStartPosition = false;
};

struct MaterialTextureContext {
    const std::vector<std::filesystem::path>& textureRoots;
    const std::vector<std::filesystem::path>& vpkArchives;
    const std::vector<std::filesystem::path>& placeholderPaths;
    GLuint* placeholderTexture = nullptr;
};

struct LoadedMaterialTexture {
    GLuint texture = 0;
    bool ownsTexture = true;
};

bool IsPropClassname(const std::string& classname) {
    return classname.rfind("prop_", 0) == 0
        || classname.rfind("item_", 0) == 0
        || classname == "func_physbox";
}

bool IsDecalClassname(const std::string& classname) {
    return classname == "infodecal" || classname == "info_decals";
}

glm::vec3 ToOpenGlDirection(float sourceX, float sourceY, float sourceZ) {
    const glm::vec3 direction{sourceX, sourceZ, -sourceY};
    const float length = glm::length(direction);
    if (length <= 1.0e-6F) {
        return glm::vec3(0.0F, 1.0F, 0.0F);
    }
    return direction / length;
}

[[maybe_unused]] void SourceAngleBasis(
    float pitchDegrees,
    float yawDegrees,
    float rollDegrees,
    glm::vec3& forward,
    glm::vec3& right,
    glm::vec3& up
) {
    const float pitch = pitchDegrees * (3.14159265F / 180.0F);
    const float yaw = yawDegrees * (3.14159265F / 180.0F);
    const float roll = rollDegrees * (3.14159265F / 180.0F);
    const float sinPitch = std::sin(pitch);
    const float cosPitch = std::cos(pitch);
    const float sinYaw = std::sin(yaw);
    const float cosYaw = std::cos(yaw);
    const float sinRoll = std::sin(roll);
    const float cosRoll = std::cos(roll);
    const float sourceForwardX = cosPitch * cosYaw;
    const float sourceForwardY = cosPitch * sinYaw;
    const float sourceForwardZ = -sinPitch;
    const float sourceRightX = -sinRoll * sinPitch * cosYaw + cosRoll * sinYaw;
    const float sourceRightY = -sinRoll * sinPitch * sinYaw - cosRoll * cosYaw;
    const float sourceRightZ = -sinRoll * cosPitch;
    const float sourceUpX = cosRoll * sinPitch * cosYaw + sinRoll * sinYaw;
    const float sourceUpY = cosRoll * sinPitch * sinYaw - sinRoll * cosYaw;
    const float sourceUpZ = cosRoll * cosPitch;
    forward = ToOpenGlDirection(sourceForwardX, sourceForwardY, sourceForwardZ);
    right = ToOpenGlDirection(sourceRightX, sourceRightY, sourceRightZ);
    up = ToOpenGlDirection(sourceUpX, sourceUpY, sourceUpZ);
}

void AppendDecalVertex(
    std::vector<float>& vertices,
    const glm::vec3& position,
    float u,
    float v,
    float lmU = 0.0F,
    float lmV = 0.0F
) {
    vertices.insert(vertices.end(), {
        position.x,
        position.y,
        position.z,
        1.0F,
        1.0F,
        1.0F,
        u,
        v,
        lmU,
        lmV,
    });
}

BatchGeometry BuildDecalGeometry(const DecalSpawn& spawn) {
    const float surfaceBias = 0.001F;
    const glm::vec3 center = spawn.position + spawn.normal * surfaceBias;
    const glm::vec3 corner0 = center + (-spawn.right - spawn.up) * spawn.halfSize;
    const glm::vec3 corner1 = center + (spawn.right - spawn.up) * spawn.halfSize;
    const glm::vec3 corner2 = center + (spawn.right + spawn.up) * spawn.halfSize;
    const glm::vec3 corner3 = center + (-spawn.right + spawn.up) * spawn.halfSize;

    BatchGeometry geometry;
    geometry.material = spawn.material;
    AppendDecalVertex(geometry.vertices, corner0, 0.0F, 1.0F, spawn.lightmapUvs[0][0], spawn.lightmapUvs[0][1]);
    AppendDecalVertex(geometry.vertices, corner1, 1.0F, 1.0F, spawn.lightmapUvs[1][0], spawn.lightmapUvs[1][1]);
    AppendDecalVertex(geometry.vertices, corner2, 1.0F, 0.0F, spawn.lightmapUvs[2][0], spawn.lightmapUvs[2][1]);
    AppendDecalVertex(geometry.vertices, corner3, 0.0F, 0.0F, spawn.lightmapUvs[3][0], spawn.lightmapUvs[3][1]);
    geometry.indices = {0, 1, 2, 0, 2, 3};
    return geometry;
}

LoadedMaterialTexture LoadMaterialTexture(
    const std::string& material,
    MaterialTextureContext& context
) {
    LoadedMaterialTexture loaded;
    if (material.empty()) {
        return loaded;
    }
    std::filesystem::path texturePath;
    for (const auto& textureRoot : context.textureRoots) {
        const auto candidate = textureRoot / (material + ".vtf");
        if (std::filesystem::exists(candidate)) {
            texturePath = candidate;
            break;
        }
    }
    std::string internalTexturePath = material;
    if (internalTexturePath.rfind("materials/", 0) != 0) {
        internalTexturePath = "materials/" + internalTexturePath;
    }
    internalTexturePath += ".vtf";
    for (const auto& vpkArchive : context.vpkArchives) {
        try {
            loaded.texture = vtf::LoadVtfTextureFromVpk(vpkArchive.string(), internalTexturePath);
        } catch (const std::exception&) {
            loaded.texture = 0;
        }
        if (loaded.texture != 0) {
            loaded.ownsTexture = true;
            return loaded;
        }
    }
    if (std::filesystem::exists(texturePath)) {
        try {
            loaded.texture = vtf::LoadVtfTexture(texturePath.string());
            loaded.ownsTexture = true;
        } catch (const std::exception& error) {
            std::cerr << "Skipping texture " << texturePath << ": " << error.what() << '\n';
        }
    }
    if (loaded.texture == 0 && context.placeholderTexture != nullptr) {
        for (const auto& placeholderPath : context.placeholderPaths) {
            if (!std::filesystem::exists(placeholderPath)) {
                continue;
            }
            try {
                if (*context.placeholderTexture == 0) {
                    *context.placeholderTexture = vtf::LoadRasterTexture(placeholderPath.string());
                }
                loaded.texture = *context.placeholderTexture;
                loaded.ownsTexture = false;
            } catch (const std::exception&) {
                loaded.texture = 0;
            }
            if (loaded.texture != 0) {
                break;
            }
        }
    }
    return loaded;
}

std::string EntityKeyValue(
    const std::vector<std::pair<std::string, std::string>>& properties,
    const char* key
) {
    for (const auto& entry : properties) {
        if (entry.first == key) {
            return entry.second;
        }
    }
    return {};
}

std::vector<std::pair<std::string, std::string>> ParseEntityProperties(const std::string& block) {
    static const std::regex keyValueRegex(R"REGEX("\s*([^"]+)\s*"\s+"([^"]*)")REGEX");
    std::vector<std::pair<std::string, std::string>> properties;
    for (std::sregex_iterator it(block.begin(), block.end(), keyValueRegex), end; it != end; ++it) {
        properties.emplace_back((*it)[1].str(), (*it)[2].str());
    }
    return properties;
}

std::string ResolveEntityTexture(const std::vector<std::pair<std::string, std::string>>& properties) {
    for (const char* key : {"material", "OverlayMaterial", "texture", "vgui_screen_name"}) {
        const std::string value = EntityKeyValue(properties, key);
        if (!value.empty()) {
            return value;
        }
    }
    return {};
}

[[maybe_unused]] bool ParseEntityAngles(
    const std::vector<std::pair<std::string, std::string>>& properties,
    float& pitch,
    float& yaw,
    float& roll
) {
    const std::string anglesText = EntityKeyValue(properties, "angles");
    if (!anglesText.empty()) {
        std::istringstream stream(anglesText);
        stream >> pitch >> yaw >> roll;
        return !stream.fail();
    }
    const std::string angleText = EntityKeyValue(properties, "angle");
    if (!angleText.empty()) {
        pitch = 0.0F;
        roll = 0.0F;
        yaw = std::stof(angleText);
        return true;
    }
    return false;
}

[[noreturn]] void Fail(const std::string& message) {
    throw std::runtime_error("BSP loader: " + message);
}

std::uint32_t ReadU32(const std::vector<std::uint8_t>& data, std::size_t offset) {
    if (offset > data.size() || data.size() - offset < 4) {
        Fail("unexpected end of lump");
    }
    return static_cast<std::uint32_t>(data[offset])
        | (static_cast<std::uint32_t>(data[offset + 1]) << 8U)
        | (static_cast<std::uint32_t>(data[offset + 2]) << 16U)
        | (static_cast<std::uint32_t>(data[offset + 3]) << 24U);
}

std::int32_t ReadI32(const std::vector<std::uint8_t>& data, std::size_t offset) {
    return static_cast<std::int32_t>(ReadU32(data, offset));
}

std::uint16_t ReadU16(const std::vector<std::uint8_t>& data, std::size_t offset) {
    if (offset > data.size() || data.size() - offset < 2) {
        Fail("unexpected end of lump");
    }
    return static_cast<std::uint16_t>(data[offset])
        | (static_cast<std::uint16_t>(data[offset + 1]) << 8U);
}

std::int16_t ReadI16(const std::vector<std::uint8_t>& data, std::size_t offset) {
    return static_cast<std::int16_t>(ReadU16(data, offset));
}

float ReadFloat(const std::vector<std::uint8_t>& data, std::size_t offset) {
    const std::uint32_t bits = ReadU32(data, offset);
    float value = 0.0F;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

std::int32_t ReadFileI32(std::ifstream& file) {
    std::array<std::uint8_t, 4> bytes{};
    if (!file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
        Fail("truncated header");
    }
    return static_cast<std::int32_t>(bytes[0])
        | (static_cast<std::int32_t>(bytes[1]) << 8)
        | (static_cast<std::int32_t>(bytes[2]) << 16)
        | (static_cast<std::int32_t>(bytes[3]) << 24);
}

std::vector<std::uint8_t> ReadLump(
    std::ifstream& file,
    const Lump& lump,
    std::uintmax_t fileSize,
    const char* name
) {
    if (lump.offset < 0 || lump.length < 0) {
        Fail(std::string(name) + " has a negative offset or length");
    }
    const auto offset = static_cast<std::uintmax_t>(lump.offset);
    const auto length = static_cast<std::uintmax_t>(lump.length);
    if (offset > fileSize || length > fileSize - offset) {
        Fail(std::string(name) + " points outside the file");
    }
    std::vector<std::uint8_t> data(static_cast<std::size_t>(length));
    file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!data.empty() && !file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()))) {
        Fail(std::string("unable to read ") + name);
    }
    return data;
}

void ValidateStride(const std::vector<std::uint8_t>& data, std::size_t stride, const char* name) {
    if (data.size() % stride != 0) {
        Fail(std::string(name) + " has an invalid byte length");
    }
}

Point ReadPoint(const std::vector<std::uint8_t>& vertices, std::size_t index) {
    const std::size_t offset = index * kVertexSize;
    return {ReadFloat(vertices, offset), ReadFloat(vertices, offset + 4), ReadFloat(vertices, offset + 8)};
}

Point ToOpenGlPoint(const Point& sourcePoint) {
    return {sourcePoint.x, sourcePoint.z, -sourcePoint.y};
}

[[maybe_unused]] glm::vec3 SourcePointToWorld(const Point& sourcePoint, const Point& center, float scale) {
    const Point converted = ToOpenGlPoint(sourcePoint);
    return glm::vec3(
        (converted.x - center.x) * scale,
        (converted.y - center.y) * scale,
        (converted.z - center.z) * scale
    );
}

void ResolveDecalBasisFromBsp(
    const glm::vec3& worldPosition,
    const std::vector<FacePolygon>& polygons,
    const Point& center,
    float scale,
    glm::vec3& outNormal,
    glm::vec3& outRight,
    glm::vec3& outUp,
    int& outFaceIndex
) {
    outFaceIndex = -1;
    const float maxPlaneDistance = std::max(0.75F, 48.0F * scale);
    float bestScore = std::numeric_limits<float>::max();
    glm::vec3 bestNormal(0.0F, 1.0F, 0.0F);

    for (std::size_t polyIdx = 0; polyIdx < polygons.size(); ++polyIdx) {
        const FacePolygon& polygon = polygons[polyIdx];
        const std::size_t nVerts = polygon.points.size();
        if (nVerts < 3) {
            continue;
        }

        std::vector<glm::vec3> worldVerts(nVerts);
        glm::vec3 polyMin(1.0e9F);
        glm::vec3 polyMax(-1.0e9F);

        for (std::size_t i = 0; i < nVerts; ++i) {
            worldVerts[i] = glm::vec3(
                (polygon.points[i].x - center.x) * scale,
                (polygon.points[i].y - center.y) * scale,
                (polygon.points[i].z - center.z) * scale
            );
            polyMin = glm::min(polyMin, worldVerts[i]);
            polyMax = glm::max(polyMax, worldVerts[i]);
        }

        glm::vec3 faceNormal = glm::cross(worldVerts[1] - worldVerts[0], worldVerts[2] - worldVerts[0]);
        const float normalLength = glm::length(faceNormal);
        if (normalLength <= 1.0e-6F) {
            continue;
        }
        faceNormal /= normalLength;

        // Signed plane distance to face
        const float signedPlaneDist = glm::dot(worldPosition - worldVerts[0], faceNormal);
        const float absPlaneDist = std::abs(signedPlaneDist);
        if (absPlaneDist > maxPlaneDistance) {
            continue;
        }

        // Project point onto the plane of the face
        const glm::vec3 proj = worldPosition - signedPlaneDist * faceNormal;

        // Bounding box filter with a margin to exclude distant faces on the same plane
        constexpr float margin = 1.5F;
        if (proj.x < polyMin.x - margin || proj.x > polyMax.x + margin ||
            proj.y < polyMin.y - margin || proj.y > polyMax.y + margin ||
            proj.z < polyMin.z - margin || proj.z > polyMax.z + margin) {
            continue;
        }

        // Distance from polygon boundary
        float maxOutsideDist = 0.0F;
        for (std::size_t i = 0; i < nVerts; ++i) {
            const glm::vec3& v0 = worldVerts[i];
            const glm::vec3& v1 = worldVerts[(i + 1) % nVerts];
            const glm::vec3 edge = v1 - v0;
            const glm::vec3 inEdgeNormal = glm::cross(faceNormal, edge);
            const float inLen = glm::length(inEdgeNormal);
            if (inLen > 1.0e-6F) {
                const float distEdge = glm::dot(proj - v0, inEdgeNormal) / inLen;
                if (distEdge < 0.0F) {
                    maxOutsideDist = std::max(maxOutsideDist, -distEdge);
                }
            }
        }

        const float score = absPlaneDist * 5.0F + maxOutsideDist * 2.0F;
        if (score < bestScore) {
            bestScore = score;
            bestNormal = faceNormal;
            outFaceIndex = static_cast<int>(polyIdx);
        }
    }

    outNormal = bestNormal;

    // Build right and up tangents facing into the room
    if (std::abs(outNormal.y) < 0.8F) {
        // Vertical wall: decal up aligns with world up
        const glm::vec3 worldUp(0.0F, 1.0F, 0.0F);
        outRight = glm::normalize(glm::cross(worldUp, outNormal));
        outUp = glm::normalize(glm::cross(outNormal, outRight));
    } else if (outNormal.y >= 0.8F) {
        // Floor: decal up points North (-Z), right points East (+X)
        const glm::vec3 worldForward(0.0F, 0.0F, -1.0F);
        outRight = glm::normalize(glm::cross(worldForward, outNormal));
        outUp = glm::normalize(glm::cross(outNormal, outRight));
    } else {
        // Ceiling: decal up points South (+Z), right points East (+X)
        const glm::vec3 worldBack(0.0F, 0.0F, 1.0F);
        outRight = glm::normalize(glm::cross(worldBack, outNormal));
        outUp = glm::normalize(glm::cross(outNormal, outRight));
    }
}

std::string ReadMaterial(
    const std::vector<std::uint8_t>& texInfo,
    const std::vector<std::uint8_t>& texData,
    const std::vector<std::uint8_t>& stringTable,
    const std::vector<std::uint8_t>& stringData,
    std::int16_t texInfoIndex
) {
    if (texInfoIndex < 0) {
        return {};
    }
    const std::size_t infoOffset = static_cast<std::size_t>(texInfoIndex) * kTexInfoSize;
    if (infoOffset > texInfo.size() || texInfo.size() - infoOffset < kTexInfoSize) {
        Fail("FACES references an out-of-range TEXINFO");
    }
    const std::int32_t texDataIndex = ReadI32(texInfo, infoOffset + 68);
    if (texDataIndex < 0) {
        return {};
    }
    const std::size_t dataOffset = static_cast<std::size_t>(texDataIndex) * kTexDataSize;
    if (dataOffset > texData.size() || texData.size() - dataOffset < kTexDataSize) {
        Fail("TEXINFO references an out-of-range TEXDATA");
    }
    const std::int32_t stringTableIndex = ReadI32(texData, dataOffset + 12);
    if (stringTableIndex < 0) {
        return {};
    }
    const std::size_t tableOffset = static_cast<std::size_t>(stringTableIndex) * 4;
    if (tableOffset > stringTable.size() || stringTable.size() - tableOffset < 4) {
        Fail("TEXDATA references an out-of-range string table entry");
    }
    const std::int32_t stringOffset = ReadI32(stringTable, tableOffset);
    if (stringOffset < 0 || static_cast<std::size_t>(stringOffset) >= stringData.size()) {
        return {};
    }
    const std::size_t remaining = stringData.size() - static_cast<std::size_t>(stringOffset);
    const auto stringBegin = stringData.begin() + stringOffset;
    const auto stringEnd = std::find(stringBegin, stringData.end(), static_cast<std::uint8_t>(0));
    const std::size_t length = static_cast<std::size_t>(std::distance(stringBegin, stringEnd));
    if (length >= remaining) {
        Fail("unterminated material name");
    }
    return std::string(reinterpret_cast<const char*>(&*stringBegin), length);
}

std::array<float, 2> ReadUv(
    const std::vector<std::uint8_t>& texInfo,
    const std::vector<std::uint8_t>& texData,
    const Point& point,
    std::int16_t texInfoIndex
) {
    if (texInfoIndex < 0) {
        return {0.0F, 0.0F};
    }
    const std::size_t infoOffset = static_cast<std::size_t>(texInfoIndex) * kTexInfoSize;
    if (infoOffset > texInfo.size() || texInfo.size() - infoOffset < kTexInfoSize) {
        Fail("FACES references an out-of-range TEXINFO");
    }
    const std::int32_t texDataIndex = ReadI32(texInfo, infoOffset + 68);
    if (texDataIndex < 0) {
        return {0.0F, 0.0F};
    }
    const std::size_t dataOffset = static_cast<std::size_t>(texDataIndex) * kTexDataSize;
    if (dataOffset > texData.size() || texData.size() - dataOffset < kTexDataSize) {
        Fail("TEXINFO references an out-of-range TEXDATA");
    }
    const float s = ReadFloat(texInfo, infoOffset)
        * point.x + ReadFloat(texInfo, infoOffset + 4) * point.y
        + ReadFloat(texInfo, infoOffset + 8) * point.z + ReadFloat(texInfo, infoOffset + 12);
    const float t = ReadFloat(texInfo, infoOffset + 16)
        * point.x + ReadFloat(texInfo, infoOffset + 20) * point.y
        + ReadFloat(texInfo, infoOffset + 24) * point.z + ReadFloat(texInfo, infoOffset + 28);
    const float width = std::max(1.0F, static_cast<float>(ReadI32(texData, dataOffset + 16)));
    const float height = std::max(1.0F, static_cast<float>(ReadI32(texData, dataOffset + 20)));
    return {s / width, t / height};
}

std::array<float, 2> ReadLightmapLuxel(
    const std::vector<std::uint8_t>& texInfo,
    const Point& point,
    std::int16_t texInfoIndex,
    std::int32_t minsS,
    std::int32_t minsT
) {
    if (texInfoIndex < 0) {
        return {0.0F, 0.0F};
    }
    const std::size_t infoOffset = static_cast<std::size_t>(texInfoIndex) * kTexInfoSize;
    if (infoOffset > texInfo.size() || texInfo.size() - infoOffset < kTexInfoSize) {
        Fail("FACES references an out-of-range TEXINFO");
    }
    const float s = ReadFloat(texInfo, infoOffset + 32)
        * point.x + ReadFloat(texInfo, infoOffset + 36) * point.y
        + ReadFloat(texInfo, infoOffset + 40) * point.z + ReadFloat(texInfo, infoOffset + 44);
    const float t = ReadFloat(texInfo, infoOffset + 48)
        * point.x + ReadFloat(texInfo, infoOffset + 52) * point.y
        + ReadFloat(texInfo, infoOffset + 56) * point.z + ReadFloat(texInfo, infoOffset + 60);
    return {s - static_cast<float>(minsS), t - static_cast<float>(minsT)};
}

std::array<float, 3> DecodeRgbExp32(const std::vector<std::uint8_t>& lighting, std::size_t offset) {
    if (offset > lighting.size() || lighting.size() - offset < 4) {
        return {1.0F, 1.0F, 1.0F};
    }
    const float scale = std::ldexp(1.0F, static_cast<int>(static_cast<std::int8_t>(lighting[offset + 3]))) / 255.0F;
    return {
        static_cast<float>(lighting[offset]) * scale,
        static_cast<float>(lighting[offset + 1]) * scale,
        static_cast<float>(lighting[offset + 2]) * scale
    };
}

void WriteAtlasPixel(LightmapAtlas& atlas, int x, int y, const std::array<float, 3>& color) {
    if (x < 0 || y < 0 || x >= atlas.width || y >= atlas.height) {
        return;
    }
    const std::size_t index = (static_cast<std::size_t>(y) * static_cast<std::size_t>(atlas.width)
        + static_cast<std::size_t>(x)) * 3;
    atlas.rgb[index] = color[0];
    atlas.rgb[index + 1] = color[1];
    atlas.rgb[index + 2] = color[2];
}

bool PackLightmaps(std::vector<FacePolygon>& polygons, int atlasWidth, int atlasHeight) {
    int cursorX = 2;
    int cursorY = 0;
    int rowHeight = 2;
    for (FacePolygon& polygon : polygons) {
        if (!polygon.lightmap.lit) {
            continue;
        }
        const int packedWidth = polygon.lightmap.luxelW + 2;
        const int packedHeight = polygon.lightmap.luxelH + 2;
        if (packedWidth > atlasWidth || packedHeight > atlasHeight) {
            return false;
        }
        if (cursorX + packedWidth > atlasWidth) {
            cursorX = 0;
            cursorY += rowHeight;
            rowHeight = 0;
        }
        if (cursorY + packedHeight > atlasHeight) {
            return false;
        }
        polygon.lightmap.atlasX = cursorX + 1;
        polygon.lightmap.atlasY = cursorY + 1;
        cursorX += packedWidth;
        rowHeight = std::max(rowHeight, packedHeight);
    }
    return true;
}

void BlitFaceLightmap(
    LightmapAtlas& atlas,
    const FaceLightmap& lightmap,
    const std::vector<std::uint8_t>& lighting
) {
    const int width = lightmap.luxelW;
    const int height = lightmap.luxelH;
    const std::size_t base = static_cast<std::size_t>(lightmap.lightOffset);
    for (int y = -1; y <= height; ++y) {
        for (int x = -1; x <= width; ++x) {
            const int sampleX = std::clamp(x, 0, width - 1);
            const int sampleY = std::clamp(y, 0, height - 1);
            const std::size_t sampleOffset = base
                + (static_cast<std::size_t>(sampleY) * static_cast<std::size_t>(width)
                    + static_cast<std::size_t>(sampleX)) * 4;
            WriteAtlasPixel(
                atlas,
                lightmap.atlasX + x,
                lightmap.atlasY + y,
                DecodeRgbExp32(lighting, sampleOffset)
            );
        }
    }
}

GLuint UploadLightmapAtlas(const LightmapAtlas& atlas) {
    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(
        GL_TEXTURE_2D,
        0,
        GL_RGB16F,
        atlas.width,
        atlas.height,
        0,
        GL_RGB,
        GL_FLOAT,
        atlas.rgb.data()
    );
    glBindTexture(GL_TEXTURE_2D, 0);
    return texture;
}

LoadedGeometry LoadGeometry(
    const std::string& path,
    std::size_t& faceCount,
    glm::vec3& worldMinimum,
    glm::vec3& worldMaximum
) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        Fail("unable to open '" + path + "'");
    }
    file.seekg(0, std::ios::end);
    const auto fileSize = static_cast<std::uintmax_t>(file.tellg());
    file.seekg(0, std::ios::beg);

    if (ReadFileI32(file) != 0x50534256 || ReadFileI32(file) <= 0) {
        Fail("invalid BSP header");
    }
    std::array<Lump, kLumpCount> lumps{};
    for (Lump& lump : lumps) {
        lump.offset = ReadFileI32(file);
        lump.length = ReadFileI32(file);
        static_cast<void>(ReadFileI32(file));
        static_cast<void>(ReadFileI32(file));
    }
    static_cast<void>(ReadFileI32(file));

    const auto vertices = ReadLump(file, lumps[kVertexLump], fileSize, "VERTEXES");
    const auto entities = ReadLump(file, lumps[kEntitiesLump], fileSize, "ENTITIES");
    const auto texData = ReadLump(file, lumps[kTexDataLump], fileSize, "TEXDATA");
    const auto texInfo = ReadLump(file, lumps[kTexInfoLump], fileSize, "TEXINFO");
    const auto faces = ReadLump(file, lumps[kFaceLump], fileSize, "FACES");
    const auto edges = ReadLump(file, lumps[kEdgeLump], fileSize, "EDGES");
    const auto surfEdges = ReadLump(file, lumps[kSurfEdgeLump], fileSize, "SURFEDGES");
    const auto stringData = ReadLump(file, lumps[kTexDataStringDataLump], fileSize, "TEXDATA_STRING_DATA");
    const auto stringTable = ReadLump(file, lumps[kTexDataStringTableLump], fileSize, "TEXDATA_STRING_TABLE");
    const auto lightingLdr = ReadLump(file, lumps[kLightingLump], fileSize, "LIGHTING");
    const auto lightingHdr = ReadLump(file, lumps[kLightingHdrLump], fileSize, "LIGHTING_HDR");
    const std::vector<std::uint8_t>& lighting = lightingHdr.empty() ? lightingLdr : lightingHdr;

    ValidateStride(vertices, kVertexSize, "VERTEXES");
    ValidateStride(texData, kTexDataSize, "TEXDATA");
    ValidateStride(texInfo, kTexInfoSize, "TEXINFO");
    ValidateStride(faces, kFaceSize, "FACES");
    ValidateStride(edges, kEdgeSize, "EDGES");
    ValidateStride(surfEdges, kSurfEdgeSize, "SURFEDGES");
    ValidateStride(stringTable, 4, "TEXDATA_STRING_TABLE");

    const std::size_t vertexCount = vertices.size() / kVertexSize;
    const std::size_t totalEdgeCount = edges.size() / kEdgeSize;
    const std::size_t surfEdgeCount = surfEdges.size() / kSurfEdgeSize;
    const std::size_t totalFaceCount = faces.size() / kFaceSize;
    faceCount = totalFaceCount;

    std::vector<FacePolygon> polygons;
    std::vector<Point> allPoints;
    for (std::size_t faceIndex = 0; faceIndex < totalFaceCount; ++faceIndex) {
        const std::size_t faceOffset = faceIndex * kFaceSize;
        const std::int32_t firstEdge = ReadI32(faces, faceOffset + 4);
        const std::int16_t edgeCount = ReadI16(faces, faceOffset + 8);
        if (edgeCount < 3) {
            continue;
        }
        if (firstEdge < 0 || static_cast<std::size_t>(firstEdge) > surfEdgeCount
            || static_cast<std::size_t>(edgeCount) > surfEdgeCount - static_cast<std::size_t>(firstEdge)) {
            Fail("FACES contains an out-of-range edge span");
        }
        const std::int16_t texInfoIndex = ReadI16(faces, faceOffset + 10);
        FacePolygon polygon;
        polygon.material = ReadMaterial(texInfo, texData, stringTable, stringData, texInfoIndex);
        polygon.lightmap.texInfoIndex = texInfoIndex;
        polygon.lightmap.lightOffset = ReadI32(faces, faceOffset + 20);
        polygon.lightmap.minsS = ReadI32(faces, faceOffset + 28);
        polygon.lightmap.minsT = ReadI32(faces, faceOffset + 32);
        const std::int32_t sizeS = ReadI32(faces, faceOffset + 36);
        const std::int32_t sizeT = ReadI32(faces, faceOffset + 40);
        std::uint32_t texFlags = 0;
        if (texInfoIndex >= 0) {
            const std::size_t infoOffset = static_cast<std::size_t>(texInfoIndex) * kTexInfoSize;
            if (infoOffset <= texInfo.size() && texInfo.size() - infoOffset >= kTexInfoSize) {
                texFlags = static_cast<std::uint32_t>(ReadI32(texInfo, infoOffset + 64));
            }
        }
        const std::uint8_t style0 = faces[faceOffset + 16];
        const int luxelW = sizeS + 1;
        const int luxelH = sizeT + 1;
        if ((texFlags & kSurfNolight) == 0 && style0 != 255 && polygon.lightmap.lightOffset >= 0
            && luxelW > 0 && luxelH > 0 && luxelW <= 2048 && luxelH <= 2048) {
            const std::size_t sampleBytes = static_cast<std::size_t>(luxelW) * static_cast<std::size_t>(luxelH) * 4U;
            const auto lightOffset = static_cast<std::size_t>(polygon.lightmap.lightOffset);
            if (lightOffset <= lighting.size() && sampleBytes <= lighting.size() - lightOffset) {
                polygon.lightmap.lit = true;
                polygon.lightmap.luxelW = luxelW;
                polygon.lightmap.luxelH = luxelH;
            }
        }
        polygon.points.reserve(static_cast<std::size_t>(edgeCount));
        polygon.sourcePoints.reserve(static_cast<std::size_t>(edgeCount));
        polygon.uvs.reserve(static_cast<std::size_t>(edgeCount));
        for (std::size_t edgeNumber = 0; edgeNumber < static_cast<std::size_t>(edgeCount); ++edgeNumber) {
            const std::size_t surfEdgeOffset = (static_cast<std::size_t>(firstEdge) + edgeNumber) * kSurfEdgeSize;
            const std::int32_t surfEdge = ReadI32(surfEdges, surfEdgeOffset);
            if (surfEdge == std::numeric_limits<std::int32_t>::min()) {
                Fail("SURFEDGES contains an invalid minimum integer");
            }
            const bool forward = surfEdge >= 0;
            const std::size_t edgeIndex = static_cast<std::size_t>(forward ? surfEdge : -surfEdge);
            if (edgeIndex >= totalEdgeCount) {
                Fail("SURFEDGES references an out-of-range edge");
            }
            const std::size_t edgeOffset = edgeIndex * kEdgeSize;
            const std::uint16_t firstVertex = ReadU16(edges, edgeOffset);
            const std::uint16_t secondVertex = ReadU16(edges, edgeOffset + 2);
            const std::size_t vertexIndex = forward ? firstVertex : secondVertex;
            if (vertexIndex >= vertexCount) {
                Fail("EDGES references an out-of-range vertex");
            }
            const Point sourcePoint = ReadPoint(vertices, vertexIndex);
            polygon.sourcePoints.push_back(sourcePoint);
            polygon.points.push_back(ToOpenGlPoint(sourcePoint));
            polygon.uvs.push_back(ReadUv(texInfo, texData, sourcePoint, texInfoIndex));
            allPoints.push_back(polygon.points.back());
        }
        polygons.push_back(std::move(polygon));
    }

    if (allPoints.empty()) {
        Fail("FACES contains no drawable polygons");
    }
    Point minimum = allPoints.front();
    Point maximum = allPoints.front();
    for (const Point& point : allPoints) {
        minimum.x = std::min(minimum.x, point.x);
        minimum.y = std::min(minimum.y, point.y);
        minimum.z = std::min(minimum.z, point.z);
        maximum.x = std::max(maximum.x, point.x);
        maximum.y = std::max(maximum.y, point.y);
        maximum.z = std::max(maximum.z, point.z);
    }
    const Point center{
        (minimum.x + maximum.x) * 0.5F,
        (minimum.y + maximum.y) * 0.5F,
        (minimum.z + maximum.z) * 0.5F,
    };
    const float extent = std::max({maximum.x - minimum.x, maximum.y - minimum.y, maximum.z - minimum.z});
    constexpr float kBspWorldSize = 128.0F;
    const float scale = extent > 0.0F ? kBspWorldSize / extent : 1.0F;
    worldMinimum = glm::vec3(
        (minimum.x - center.x) * scale,
        (minimum.y - center.y) * scale,
        (minimum.z - center.z) * scale
    );
    worldMaximum = glm::vec3(
        (maximum.x - center.x) * scale,
        (maximum.y - center.y) * scale,
        (maximum.z - center.z) * scale
    );

    // Source BSP prop models and items are represented by dynamic
    // boxes with physics until custom model geometry lumps are loaded.
    std::vector<glm::vec3> propPositions;
    std::vector<BspMapEntity> mapEntities;
    std::vector<DecalSpawn> mapDecals;
    glm::vec3 playerStartPosition{0.0F};
    bool hasPlayerStartPosition = false;
    const std::string entityText(entities.begin(), entities.end());
    const std::regex entityRegex(R"(\{([^}]*)\})");
    const std::regex originRegex(R"REGEX("origin"\s+"([-+0-9.eE]+)\s+([-+0-9.eE]+)\s+([-+0-9.eE]+)")REGEX");
    const auto toWorldPosition = [&](const Point& sourcePoint) -> glm::vec3 {
        const Point converted = ToOpenGlPoint(sourcePoint);
        return glm::vec3(
            (converted.x - center.x) * scale,
            (converted.y - center.y) * scale,
            (converted.z - center.z) * scale
        );
    };
    for (std::sregex_iterator it(entityText.begin(), entityText.end(), entityRegex), end;
         it != end; ++it) {
        const std::string block = (*it)[1].str();
        const std::vector<std::pair<std::string, std::string>> properties = ParseEntityProperties(block);
        const std::string name = EntityKeyValue(properties, "classname");
        if (name.empty()) {
            continue;
        }
        std::smatch origin;
        if (!std::regex_search(block, origin, originRegex)) {
            continue;
        }
        const Point sourceOrigin{
            std::stof(origin[1].str()),
            std::stof(origin[2].str()),
            std::stof(origin[3].str()),
        };
        const glm::vec3 worldPosition = toWorldPosition(sourceOrigin);
        if (!hasPlayerStartPosition && (name == "info_player_start" || name == "info_player_deathmatch")) {
            playerStartPosition = worldPosition;
            hasPlayerStartPosition = true;
        }
        if (IsPropClassname(name)) {
            propPositions.push_back(worldPosition);
            if (propPositions.size() >= 512) {
                continue;
            }
            continue;
        }
        if (IsDecalClassname(name)) {
            const std::string material = ResolveEntityTexture(properties);
            if (!material.empty()) {
                DecalSpawn decal;
                decal.position = worldPosition;
                decal.material = material;
                decal.halfSize = 32.0F * scale;
                ResolveDecalBasisFromBsp(
                    worldPosition,
                    polygons,
                    center,
                    scale,
                    decal.normal,
                    decal.right,
                    decal.up,
                    decal.faceIndex
                );
                mapDecals.push_back(std::move(decal));
            }
            continue;
        }
        BspMapEntity entity;
        entity.position = worldPosition;
        entity.classname = name;
        entity.model = EntityKeyValue(properties, "model");
        entity.texture = ResolveEntityTexture(properties);
        entity.properties = properties;
        mapEntities.push_back(std::move(entity));
    }

    LightmapAtlas atlas;
    const bool anyLit = std::any_of(polygons.begin(), polygons.end(), [](const FacePolygon& polygon) {
        return polygon.lightmap.lit;
    });
    GLint maxTextureSize = 4096;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTextureSize);
    const int atlasCap = std::max(64, std::min(maxTextureSize, 8192));
    bool packed = false;
    if (anyLit) {
        for (int atlasSize = 256; atlasSize <= atlasCap; atlasSize *= 2) {
            if (PackLightmaps(polygons, atlasSize, atlasSize)) {
                atlas.width = atlasSize;
                atlas.height = atlasSize;
                packed = true;
                break;
            }
        }
    }
    if (packed) {
        atlas.rgb.assign(static_cast<std::size_t>(atlas.width) * static_cast<std::size_t>(atlas.height) * 3U, 0.0F);
        const std::array<float, 3> white{1.0F, 1.0F, 1.0F};
        WriteAtlasPixel(atlas, 0, 0, white);
        WriteAtlasPixel(atlas, 1, 0, white);
        WriteAtlasPixel(atlas, 0, 1, white);
        WriteAtlasPixel(atlas, 1, 1, white);
        for (const FacePolygon& polygon : polygons) {
            if (polygon.lightmap.lit) {
                BlitFaceLightmap(atlas, polygon.lightmap, lighting);
            }
        }
    }

    const float atlasWidth = atlas.width > 0 ? static_cast<float>(atlas.width) : 1.0F;
    const float atlasHeight = atlas.height > 0 ? static_cast<float>(atlas.height) : 1.0F;
    for (FacePolygon& polygon : polygons) {
        polygon.lightmapUvs.reserve(polygon.sourcePoints.size());
        for (const Point& sourcePoint : polygon.sourcePoints) {
            if (!packed || !polygon.lightmap.lit) {
                polygon.lightmapUvs.push_back({0.5F / atlasWidth, 0.5F / atlasHeight});
                continue;
            }
            const auto luxel = ReadLightmapLuxel(
                texInfo,
                sourcePoint,
                polygon.lightmap.texInfoIndex,
                polygon.lightmap.minsS,
                polygon.lightmap.minsT
            );
            polygon.lightmapUvs.push_back({
                (static_cast<float>(polygon.lightmap.atlasX) + luxel[0] + 0.5F) / atlasWidth,
                (static_cast<float>(polygon.lightmap.atlasY) + luxel[1] + 0.5F) / atlasHeight
            });
        }
    }

    for (DecalSpawn& decal : mapDecals) {
        if (decal.faceIndex >= 0 && decal.faceIndex < static_cast<int>(polygons.size())) {
            const FacePolygon& poly = polygons[static_cast<std::size_t>(decal.faceIndex)];
            if (packed && poly.lightmap.lit) {
                const glm::vec3 centerPos = decal.position + decal.normal * 0.001F;
                const glm::vec3 corners[4] = {
                    centerPos + (-decal.right - decal.up) * decal.halfSize,
                    centerPos + (decal.right - decal.up) * decal.halfSize,
                    centerPos + (decal.right + decal.up) * decal.halfSize,
                    centerPos + (-decal.right + decal.up) * decal.halfSize
                };
                for (int c = 0; c < 4; ++c) {
                    const Point openglPt{
                        corners[c].x / scale + center.x,
                        corners[c].y / scale + center.y,
                        corners[c].z / scale + center.z
                    };
                    const Point srcPt{openglPt.x, -openglPt.z, openglPt.y};
                    const auto luxel = ReadLightmapLuxel(
                        texInfo,
                        srcPt,
                        poly.lightmap.texInfoIndex,
                        poly.lightmap.minsS,
                        poly.lightmap.minsT
                    );
                    decal.lightmapUvs[c][0] = (static_cast<float>(poly.lightmap.atlasX) + luxel[0] + 0.5F) / atlasWidth;
                    decal.lightmapUvs[c][1] = (static_cast<float>(poly.lightmap.atlasY) + luxel[1] + 0.5F) / atlasHeight;
                }
            } else {
                for (int c = 0; c < 4; ++c) {
                    decal.lightmapUvs[c] = {0.5F / atlasWidth, 0.5F / atlasHeight};
                }
            }
        }
    }

    LoadedGeometry loaded;
    loaded.lightmap = std::move(atlas);
    loaded.propPositions = std::move(propPositions);
    loaded.mapEntities = std::move(mapEntities);
    loaded.decals = std::move(mapDecals);
    loaded.playerStartPosition = playerStartPosition;
    loaded.hasPlayerStartPosition = hasPlayerStartPosition;
    for (std::size_t polygonIndex = 0; polygonIndex < polygons.size(); ++polygonIndex) {
        const FacePolygon& polygon = polygons[polygonIndex];
        auto batch = std::find_if(loaded.batches.begin(), loaded.batches.end(), [&](const BatchGeometry& candidate) {
            return candidate.material == polygon.material;
        });
        if (batch == loaded.batches.end()) {
            std::string lowercaseMaterial = polygon.material;
            for (char& character : lowercaseMaterial) {
                character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
            }
            loaded.batches.push_back({polygon.material, {}, {}, lowercaseMaterial.find("trigger") != std::string::npos});
            batch = std::prev(loaded.batches.end());
        }
        const unsigned int baseVertex = static_cast<unsigned int>(
            batch->vertices.size() / Mesh::kFloatsPerVertex
        );
        const float red = 0.35F + static_cast<float>((polygonIndex * 37) % 55) / 100.0F;
        const float green = 0.35F + static_cast<float>((polygonIndex * 61) % 55) / 100.0F;
        const float blue = 0.35F + static_cast<float>((polygonIndex * 83) % 55) / 100.0F;
        for (std::size_t vertexIndex = 0; vertexIndex < polygon.points.size(); ++vertexIndex) {
            const Point& point = polygon.points[vertexIndex];
            batch->vertices.insert(batch->vertices.end(), {
                (point.x - center.x) * scale,
                (point.y - center.y) * scale,
                (point.z - center.z) * scale,
                red,
                green,
                blue,
                polygon.uvs[vertexIndex][0],
                polygon.uvs[vertexIndex][1],
                polygon.lightmapUvs[vertexIndex][0],
                polygon.lightmapUvs[vertexIndex][1],
            });
        }
        for (unsigned int vertexIndex = 1; vertexIndex + 1 < polygon.points.size(); ++vertexIndex) {
            batch->indices.insert(batch->indices.end(), {
                baseVertex,
                baseVertex + vertexIndex,
                baseVertex + vertexIndex + 1,
            });
        }
    }
    return loaded;
}
}

struct BspLoader::Batch {
    explicit Batch(BatchGeometry geometry)
        : mesh(std::make_unique<Mesh>(geometry.vertices, geometry.indices)),
          material(std::move(geometry.material)),
          trigger(geometry.trigger) {
        if (!geometry.vertices.empty()) {
            for (std::size_t i = 0; i + 2 < geometry.vertices.size(); i += Mesh::kFloatsPerVertex) {
                const glm::vec3 p(geometry.vertices[i], geometry.vertices[i + 1], geometry.vertices[i + 2]);
                aabbMin = glm::min(aabbMin, p);
                aabbMax = glm::max(aabbMax, p);
            }
        } else {
            aabbMin = glm::vec3(0.0F);
            aabbMax = glm::vec3(0.0F);
        }
    }

    ~Batch() {
        if (texture != 0 && ownsTexture) {
            glDeleteTextures(1, &texture);
        }
    }

    std::unique_ptr<Mesh> mesh;
    std::string material;
    GLuint texture = 0;
    bool ownsTexture = true;
    bool trigger = false;
    glm::vec3 aabbMin{1.0e9F};
    glm::vec3 aabbMax{-1.0e9F};
};

struct BspLoader::Decal {
    explicit Decal(BatchGeometry geometry, LoadedMaterialTexture loadedTexture)
        : mesh(std::make_unique<Mesh>(geometry.vertices, geometry.indices)),
          material(std::move(geometry.material)),
          texture(loadedTexture.texture),
          ownsTexture(loadedTexture.ownsTexture) {
        if (!geometry.vertices.empty()) {
            for (std::size_t i = 0; i + 2 < geometry.vertices.size(); i += Mesh::kFloatsPerVertex) {
                const glm::vec3 p(geometry.vertices[i], geometry.vertices[i + 1], geometry.vertices[i + 2]);
                aabbMin = glm::min(aabbMin, p);
                aabbMax = glm::max(aabbMax, p);
            }
        } else {
            aabbMin = glm::vec3(0.0F);
            aabbMax = glm::vec3(0.0F);
        }
    }

    ~Decal() {
        if (texture != 0 && ownsTexture) {
            glDeleteTextures(1, &texture);
        }
    }

    std::unique_ptr<Mesh> mesh;
    std::string material;
    GLuint texture = 0;
    bool ownsTexture = true;
    glm::vec3 aabbMin{1.0e9F};
    glm::vec3 aabbMax{-1.0e9F};
};

BspLoader::BspLoader(const std::string& path) {
    LoadedGeometry loaded = LoadGeometry(path, faceCount_, worldMinimum_, worldMaximum_);
    std::vector<BatchGeometry> geometry = std::move(loaded.batches);
    propPositions_ = std::move(loaded.propPositions);
    mapEntities_ = std::move(loaded.mapEntities);
    const std::vector<DecalSpawn> decalSpawns = std::move(loaded.decals);
    playerStartPosition_ = loaded.playerStartPosition;
    hasPlayerStartPosition_ = loaded.hasPlayerStartPosition;
    if (loaded.lightmap.width > 0 && !loaded.lightmap.rgb.empty()) {
        lightmapTexture_ = UploadLightmapAtlas(loaded.lightmap);
        std::cout << "Lightmap atlas " << loaded.lightmap.width << "x" << loaded.lightmap.height << std::endl;
    }
    std::cout << "BSP geometry loaded: " << faceCount_ << " faces, " << geometry.size() << " materials, "
              << mapEntities_.size() << " entity placeholders, " << decalSpawns.size() << " decals"
              << std::endl;
    for (const BatchGeometry& batchGeometry : geometry) {
        if (batchGeometry.trigger) {
            continue;
        }
        const std::uint32_t baseVertex = static_cast<std::uint32_t>(collisionVertices_.size());
        for (std::size_t offset = 0;
             offset + (Mesh::kFloatsPerVertex - 1) < batchGeometry.vertices.size();
             offset += Mesh::kFloatsPerVertex) {
            collisionVertices_.emplace_back(
                batchGeometry.vertices[offset],
                batchGeometry.vertices[offset + 1],
                batchGeometry.vertices[offset + 2]
            );
        }
        for (std::uint32_t index : batchGeometry.indices) {
            collisionIndices_.push_back(baseVertex + index);
        }
    }
    const std::filesystem::path mapDirectory = std::filesystem::path(path).parent_path();
    const std::filesystem::path gameDirectory = mapDirectory.filename() == "maps"
        ? mapDirectory.parent_path()
        : mapDirectory;
    const std::filesystem::path contentRoot = blueprint::ContentRoot();
    const std::vector<std::filesystem::path> textureRoots = {
        contentRoot / "textures",
        std::filesystem::current_path() / "textures",
        gameDirectory / "textures",
        gameDirectory / "materials",
        mapDirectory / "materials",
    };
    const std::vector<std::filesystem::path> placeholderPaths = {
        contentRoot / "textures" / "placeholder.jpg",
        std::filesystem::current_path() / "textures" / "placeholder.jpg",
    };
    const std::vector<std::filesystem::path> vpkRoots = {
        contentRoot / "textures",
        std::filesystem::current_path() / "textures",
        gameDirectory / "textures",
    };
    std::vector<std::filesystem::path> vpkArchives;
    for (const auto& vpkRoot : vpkRoots) {
        if (!std::filesystem::exists(vpkRoot) || !std::filesystem::is_directory(vpkRoot)) {
            continue;
        }
        for (const auto& entry : std::filesystem::directory_iterator(
                 vpkRoot,
                 std::filesystem::directory_options::skip_permission_denied)) {
            const std::string vpkName = entry.path().filename().string();
            const bool directoryArchive = vpkName.size() >= 8
                && std::equal(
                    vpkName.end() - 8,
                    vpkName.end(),
                    "_dir.vpk",
                    [](unsigned char left, unsigned char right) {
                        return std::tolower(left) == std::tolower(right);
                    });
            if (entry.is_regular_file() && directoryArchive
                && std::find(vpkArchives.begin(), vpkArchives.end(), entry.path()) == vpkArchives.end()) {
                vpkArchives.push_back(entry.path());
            }
        }
    }
    MaterialTextureContext textureContext{textureRoots, vpkArchives, placeholderPaths, &placeholderTexture_};
    std::unordered_map<std::string, LoadedMaterialTexture> materialCache;

    auto getOrLoadTexture = [&](const std::string& mat) -> LoadedMaterialTexture {
        if (mat.empty()) return {};
        const auto it = materialCache.find(mat);
        if (it != materialCache.end()) {
            return {it->second.texture, false};
        }
        const LoadedMaterialTexture loaded = LoadMaterialTexture(mat, textureContext);
        if (loaded.texture != 0) {
            materialCache[mat] = loaded;
        }
        return loaded;
    };

    for (BatchGeometry& batchGeometry : geometry) {
        auto batch = std::make_unique<Batch>(std::move(batchGeometry));
        if (!batch->material.empty()) {
            const LoadedMaterialTexture loadedTexture = getOrLoadTexture(batch->material);
            batch->texture = loadedTexture.texture;
            batch->ownsTexture = loadedTexture.ownsTexture;
        }
        if (batch->texture != 0) {
            ++textureCount_;
        }
        batches_.push_back(std::move(batch));
    }
    for (const DecalSpawn& spawn : decalSpawns) {
        BatchGeometry decalGeometry = BuildDecalGeometry(spawn);
        const LoadedMaterialTexture loadedTexture = getOrLoadTexture(spawn.material);
        if (loadedTexture.texture == 0) {
            std::cerr << "Skipping decal with missing texture: " << spawn.material << '\n';
            continue;
        }
        auto decal = std::make_unique<Decal>(std::move(decalGeometry), loadedTexture);
        ++textureCount_;
        decals_.push_back(std::move(decal));
    }

    for (const auto& batch : batches_) {
        if (batch->trigger) {
            triggerBatches_.push_back(batch.get());
        } else {
            solidBatches_.push_back(batch.get());
        }
    }
    // Sort solid batches by texture to minimize texture bind swaps during rendering
    std::stable_sort(solidBatches_.begin(), solidBatches_.end(), [](const Batch* a, const Batch* b) {
        return a->texture < b->texture;
    });
}

BspLoader::~BspLoader() {
    if (lightmapTexture_ != 0) {
        glDeleteTextures(1, &lightmapTexture_);
    }
    if (placeholderTexture_ != 0) {
        glDeleteTextures(1, &placeholderTexture_);
    }
}

namespace {

inline float DistSqPointAABB(const glm::vec3& p, const glm::vec3& bMin, const glm::vec3& bMax) {
    float distSq = 0.0F;
    for (int i = 0; i < 3; ++i) {
        const float v = p[i];
        if (v < bMin[i]) {
            const float d = bMin[i] - v;
            distSq += d * d;
        } else if (v > bMax[i]) {
            const float d = v - bMax[i];
            distSq += d * d;
        }
    }
    return distSq;
}

struct Frustum {
    glm::vec4 planes[6]{};

    static Frustum FromMatrix(const glm::mat4& m) {
        Frustum f{};
        const glm::vec4 row0(m[0][0], m[1][0], m[2][0], m[3][0]);
        const glm::vec4 row1(m[0][1], m[1][1], m[2][1], m[3][1]);
        const glm::vec4 row2(m[0][2], m[1][2], m[2][2], m[3][2]);
        const glm::vec4 row3(m[0][3], m[1][3], m[2][3], m[3][3]);

        f.planes[0] = row3 + row0; // Left
        f.planes[1] = row3 - row0; // Right
        f.planes[2] = row3 + row1; // Bottom
        f.planes[3] = row3 - row1; // Top
        f.planes[4] = row3 + row2; // Near
        f.planes[5] = row3 - row2; // Far

        for (int i = 0; i < 6; ++i) {
            const float len = glm::length(glm::vec3(f.planes[i]));
            if (len > 1.0e-6F) {
                f.planes[i] /= len;
            }
        }
        return f;
    }

    bool IsBoxVisible(const glm::vec3& bMin, const glm::vec3& bMax) const {
        for (int i = 0; i < 6; ++i) {
            const glm::vec3 p(
                planes[i].x > 0.0F ? bMax.x : bMin.x,
                planes[i].y > 0.0F ? bMax.y : bMin.y,
                planes[i].z > 0.0F ? bMax.z : bMin.z
            );
            if (glm::dot(glm::vec3(planes[i]), p) + planes[i].w < 0.0F) {
                return false;
            }
        }
        return true;
    }
};

} // namespace

void BspLoader::Draw(const Shader& shader, const glm::mat4& viewProjection) const {
    shader.SetInt("Texture0", 0);
    shader.SetInt("Lightmap0", 1);
    shader.SetBool("DecalPass", false);
    const bool useLightmap = lightmapTexture_ != 0;
    shader.SetBool("UseLightmap", useLightmap);
    if (useLightmap) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, lightmapTexture_);
    }

    glActiveTexture(GL_TEXTURE0);
    GLuint currentBoundTexture = 0xFFFFFFFF;
    int currentUseTexture = -1;

    const bool hasFrustum = (viewProjection != glm::mat4(0.0F));
    Frustum frustum{};
    if (hasFrustum) {
        frustum = Frustum::FromMatrix(viewProjection);
    }

    // 1. Non-trigger solid geometry (Opacity = 1.0F), pre-sorted by texture
    shader.SetFloat("Opacity", 1.0F);
    for (const Batch* batch : solidBatches_) {
        if (hasFrustum && !frustum.IsBoxVisible(batch->aabbMin, batch->aabbMax)) {
            continue;
        }
        if (batch->texture != currentBoundTexture) {
            glBindTexture(GL_TEXTURE_2D, batch->texture);
            currentBoundTexture = batch->texture;
            const int useTex = (batch->texture != 0) ? 1 : 0;
            if (useTex != currentUseTexture) {
                shader.SetBool("UseTexture", useTex != 0);
                currentUseTexture = useTex;
            }
        }
        batch->mesh->Draw();
    }

    // 2. Trigger geometry (translucent, Opacity = 0.1F)
    if (!triggerBatches_.empty()) {
        glDepthMask(GL_FALSE);
        shader.SetFloat("Opacity", 0.1F);
        for (const Batch* batch : triggerBatches_) {
            if (hasFrustum && !frustum.IsBoxVisible(batch->aabbMin, batch->aabbMax)) {
                continue;
            }
            if (batch->texture != currentBoundTexture) {
                glBindTexture(GL_TEXTURE_2D, batch->texture);
                currentBoundTexture = batch->texture;
                const int useTex = (batch->texture != 0) ? 1 : 0;
                if (useTex != currentUseTexture) {
                    shader.SetBool("UseTexture", useTex != 0);
                    currentUseTexture = useTex;
                }
            }
            batch->mesh->Draw();
        }
        glDepthMask(GL_TRUE);
    }

    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void BspLoader::Draw(const Shader& shader) const {
    Draw(shader, glm::mat4(0.0F));
}

void BspLoader::DrawDecals(const Shader& shader, const glm::mat4& /*viewProjection*/) const {
    if (decals_.empty()) {
        return;
    }

    const GLboolean blendWasEnabled = glIsEnabled(GL_BLEND);
    GLint depthMaskEnabled = GL_TRUE;
    glGetIntegerv(GL_DEPTH_WRITEMASK, &depthMaskEnabled);
    GLint depthFunc = GL_LESS;
    glGetIntegerv(GL_DEPTH_FUNC, &depthFunc);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    glDepthFunc(GL_LEQUAL);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(-1.0F, -1.0F);

    shader.SetBool("DecalPass", true);
    const bool useLightmap = lightmapTexture_ != 0;
    shader.SetBool("UseLightmap", useLightmap);
    if (useLightmap) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, lightmapTexture_);
    }
    shader.SetFloat("Opacity", 1.0F);

    GLuint currentBoundTexture = 0xFFFFFFFF;
    int currentUseTexture = -1;

    for (const auto& decal : decals_) {
        if (decal->texture != currentBoundTexture) {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, decal->texture);
            currentBoundTexture = decal->texture;
            const int useTex = (decal->texture != 0) ? 1 : 0;
            if (useTex != currentUseTexture) {
                shader.SetBool("UseTexture", useTex != 0);
                currentUseTexture = useTex;
            }
        }
        decal->mesh->Draw();
    }

    glBindVertexArray(0);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glDepthFunc(static_cast<GLenum>(depthFunc));
    glDepthMask(depthMaskEnabled ? GL_TRUE : GL_FALSE);
    if (!blendWasEnabled) {
        glDisable(GL_BLEND);
    }
    shader.SetBool("DecalPass", false);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    if (useLightmap) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
}

void BspLoader::DrawDecals(const Shader& shader) const {
    DrawDecals(shader, glm::mat4(0.0F));
}

void BspLoader::DrawDepth(const Shader& shader, const glm::vec3& lightPos, float lightRadius) const {
    shader.SetMat4("Model", glm::mat4(1.0F));
    const bool cullDistance = (lightRadius > 0.0F);
    const float maxDistSq = lightRadius * lightRadius;

    for (const Batch* batch : solidBatches_) {
        if (cullDistance && DistSqPointAABB(lightPos, batch->aabbMin, batch->aabbMax) > maxDistSq) {
            continue;
        }
        batch->mesh->Draw();
    }
    glBindVertexArray(0);
}

void BspLoader::DrawDepth(const Shader& shader) const {
    DrawDepth(shader, glm::vec3(0.0F), -1.0F);
}

std::size_t BspLoader::FaceCount() const {
    return faceCount_;
}

std::size_t BspLoader::TextureCount() const {
    return textureCount_;
}

const glm::vec3& BspLoader::WorldMinimum() const {
    return worldMinimum_;
}

const glm::vec3& BspLoader::WorldMaximum() const {
    return worldMaximum_;
}

const std::vector<glm::vec3>& BspLoader::CollisionVertices() const {
    return collisionVertices_;
}

const std::vector<std::uint32_t>& BspLoader::CollisionIndices() const {
    return collisionIndices_;
}

const std::vector<glm::vec3>& BspLoader::PropPositions() const {
    return propPositions_;
}

const std::vector<BspMapEntity>& BspLoader::MapEntities() const {
    return mapEntities_;
}

const glm::vec3& BspLoader::PlayerStartPosition() const {
    return playerStartPosition_;
}

bool BspLoader::HasPlayerStartPosition() const {
    return hasPlayerStartPosition_;
}
