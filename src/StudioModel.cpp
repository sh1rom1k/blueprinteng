#include "StudioModel.hpp"

#include "GameFileSystem.hpp"
#include "Mesh.hpp"
#include "SourceCoords.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

std::uint32_t ReadU32(const std::vector<std::uint8_t>& data, std::size_t offset) {
    if (offset + 4 > data.size()) {
        return 0;
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
    if (offset + 2 > data.size()) {
        return 0;
    }
    return static_cast<std::uint16_t>(data[offset])
        | (static_cast<std::uint16_t>(data[offset + 1]) << 8U);
}

float ReadFloat(const std::vector<std::uint8_t>& data, std::size_t offset) {
    const std::uint32_t bits = ReadU32(data, offset);
    float value = 0.0F;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

std::string ReadString(const std::vector<std::uint8_t>& data, std::size_t offset) {
    if (offset >= data.size()) {
        return {};
    }
    const std::size_t end = data.size();
    std::size_t cursor = offset;
    while (cursor < end && data[cursor] != 0) {
        ++cursor;
    }
    return std::string(reinterpret_cast<const char*>(data.data() + offset), cursor - offset);
}

void AppendBox(StudioModel& model) {
    const glm::vec3 minimum = SourcePointToWorld(model.hullMin.x, model.hullMin.y, model.hullMin.z);
    const glm::vec3 maximum = SourcePointToWorld(model.hullMax.x, model.hullMax.y, model.hullMax.z);
    const glm::vec3 corners[8] = {
        {minimum.x, minimum.y, minimum.z},
        {maximum.x, minimum.y, minimum.z},
        {maximum.x, maximum.y, minimum.z},
        {minimum.x, maximum.y, minimum.z},
        {minimum.x, minimum.y, maximum.z},
        {maximum.x, minimum.y, maximum.z},
        {maximum.x, maximum.y, maximum.z},
        {minimum.x, maximum.y, maximum.z},
    };
    const unsigned int faces[6][4] = {
        {0, 1, 2, 3},
        {5, 4, 7, 6},
        {4, 0, 3, 7},
        {1, 5, 6, 2},
        {3, 2, 6, 7},
        {4, 5, 1, 0},
    };
    StudioPrimitive primitive;
    primitive.material = "dev/dev_measuregeneric01";
    for (const auto& face : faces) {
        const unsigned int base = static_cast<unsigned int>(primitive.vertices.size() / Mesh::kFloatsPerVertex);
        for (int corner = 0; corner < 4; ++corner) {
            const glm::vec3& position = corners[face[corner]];
            const float u = corner == 0 || corner == 3 ? 0.0F : 1.0F;
            const float v = corner >= 2 ? 1.0F : 0.0F;
            primitive.vertices.insert(primitive.vertices.end(), {
                position.x, position.y, position.z,
                0.8F, 0.75F, 0.65F,
                u, v,
                0.0F, 0.0F,
                0.0F,
            });
        }
        primitive.indices.insert(primitive.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }
    model.parts.push_back(std::move(primitive));
}

bool AppendMeshes(
    StudioModel& model,
    const std::vector<std::uint8_t>& mdl,
    const std::vector<std::uint8_t>& vvd,
    const std::vector<std::uint8_t>& vtx
) {
    if (mdl.size() < 240 || vvd.size() < 64 || vtx.size() < 36) {
        return false;
    }
    if (ReadI32(mdl, 0) != 0x54534449 || ReadI32(vvd, 0) != 0x56534449) {
        return false;
    }
    constexpr std::size_t kVertexSize = 48;
    const int numLods = ReadI32(vvd, 12);
    const int lod0Count = ReadI32(vvd, 16);
    const int numFixups = ReadI32(vvd, 48);
    const int fixupStart = ReadI32(vvd, 52);
    const int vertexStart = ReadI32(vvd, 56);
    if (lod0Count <= 0 || vertexStart < 0) {
        return false;
    }

    std::vector<std::uint8_t> vertices(static_cast<std::size_t>(lod0Count) * kVertexSize);
    if (numFixups <= 0) {
        const std::size_t bytes = vertices.size();
        if (static_cast<std::size_t>(vertexStart) + bytes > vvd.size()) {
            return false;
        }
        std::memcpy(vertices.data(), vvd.data() + vertexStart, bytes);
    } else {
        std::size_t destination = 0;
        for (int fixupIndex = 0; fixupIndex < numFixups; ++fixupIndex) {
            const std::size_t fixupOffset = static_cast<std::size_t>(fixupStart) + static_cast<std::size_t>(fixupIndex) * 12;
            if (fixupOffset + 12 > vvd.size()) {
                return false;
            }
            const int lod = ReadI32(vvd, fixupOffset);
            const int sourceVertex = ReadI32(vvd, fixupOffset + 4);
            const int count = ReadI32(vvd, fixupOffset + 8);
            if (lod < 0 || count <= 0) {
                continue;
            }
            const std::size_t copyBytes = static_cast<std::size_t>(count) * kVertexSize;
            const std::size_t sourceOffset = static_cast<std::size_t>(vertexStart) + static_cast<std::size_t>(sourceVertex) * kVertexSize;
            if (sourceOffset + copyBytes > vvd.size() || destination + copyBytes > vertices.size()) {
                break;
            }
            std::memcpy(vertices.data() + destination, vvd.data() + sourceOffset, copyBytes);
            destination += copyBytes;
        }
        if (destination == 0) {
            return false;
        }
    }
    static_cast<void>(numLods);

    const int numTextures = ReadI32(mdl, 204);
    const int textureIndex = ReadI32(mdl, 208);
    const int numCd = ReadI32(mdl, 212);
    const int cdIndex = ReadI32(mdl, 216);
    const int numSkinRef = ReadI32(mdl, 220);
    const int skinIndex = ReadI32(mdl, 228);
    const int numBodyParts = ReadI32(mdl, 232);
    const int bodyPartIndex = ReadI32(mdl, 236);
    if (numBodyParts <= 0 || bodyPartIndex <= 0) {
        return false;
    }

    auto textureName = [&](int material) {
        int skin = material;
        if (numSkinRef > 0 && skinIndex > 0 && material >= 0 && material < numSkinRef) {
            skin = ReadU16(mdl, static_cast<std::size_t>(skinIndex) + static_cast<std::size_t>(material) * 2);
        }
        if (skin < 0 || skin >= numTextures || textureIndex <= 0) {
            return std::string("dev/dev_measuregeneric01");
        }
        const std::size_t textureOffset = static_cast<std::size_t>(textureIndex) + static_cast<std::size_t>(skin) * 64;
        const int nameOffset = ReadI32(mdl, textureOffset);
        std::string name = ReadString(mdl, textureOffset + static_cast<std::size_t>(nameOffset));
        for (char& character : name) {
            if (character == '\\') {
                character = '/';
            }
        }
        if (name.rfind("materials/", 0) == 0) {
            name = name.substr(10);
        }
        const std::string extension = name.size() >= 4 ? name.substr(name.size() - 4) : std::string();
        if (extension == ".vmt" || extension == ".vtf") {
            name.resize(name.size() - 4);
        }
        return name;
    };

    std::vector<std::string> folders;
    if (numCd > 0 && cdIndex > 0) {
        for (int folderIndex = 0; folderIndex < numCd; ++folderIndex) {
            const int folderOffset = ReadI32(mdl, static_cast<std::size_t>(cdIndex) + static_cast<std::size_t>(folderIndex) * 4);
            if (folderOffset <= 0) {
                continue;
            }
            std::string folder = ReadString(mdl, static_cast<std::size_t>(folderOffset));
            for (char& character : folder) {
                if (character == '\\') {
                    character = '/';
                }
            }
            while (!folder.empty() && folder.front() == '/') {
                folder.erase(folder.begin());
            }
            if (!folder.empty() && folder.back() != '/') {
                folder.push_back('/');
            }
            if (!folder.empty()) {
                folders.push_back(std::move(folder));
            }
        }
    }
    model.materialFolders = std::move(folders);

    const int vtxBodyParts = ReadI32(vtx, 28);
    const int vtxBodyOffset = ReadI32(vtx, 32);
    if (vtxBodyParts <= 0 || vtxBodyOffset <= 0) {
        return false;
    }
    const int bodyParts = std::min(numBodyParts, vtxBodyParts);
    bool anyTriangle = false;
    for (int bodyPart = 0; bodyPart < bodyParts; ++bodyPart) {
        const std::size_t mdlBody = static_cast<std::size_t>(bodyPartIndex) + static_cast<std::size_t>(bodyPart) * 16;
        const std::size_t vtxBody = static_cast<std::size_t>(vtxBodyOffset) + static_cast<std::size_t>(bodyPart) * 8;
        const int numModels = std::min(ReadI32(mdl, mdlBody + 4), ReadI32(vtx, vtxBody));
        const int mdlModelIndex = ReadI32(mdl, mdlBody + 12);
        const int vtxModelIndex = ReadI32(vtx, vtxBody + 4);
        if (numModels <= 0 || mdlModelIndex <= 0 || vtxModelIndex < 0) {
            continue;
        }
        const std::size_t mdlModel = mdlBody + static_cast<std::size_t>(mdlModelIndex);
        const std::size_t vtxModel = vtxBody + static_cast<std::size_t>(vtxModelIndex);
        const int numMeshes = ReadI32(mdl, mdlModel + 72);
        const int meshIndex = ReadI32(mdl, mdlModel + 76);
        const int modelVertexIndex = ReadI32(mdl, mdlModel + 84);
        const int vtxNumLods = ReadI32(vtx, vtxModel);
        const int lodOffset = ReadI32(vtx, vtxModel + 4);
        if (numMeshes <= 0 || meshIndex <= 0 || vtxNumLods <= 0 || lodOffset < 0) {
            continue;
        }
        const std::size_t lodHeader = vtxModel + static_cast<std::size_t>(lodOffset);
        const int lodMeshes = ReadI32(vtx, lodHeader);
        const int lodMeshOffset = ReadI32(vtx, lodHeader + 4);
        const int meshCount = std::min(numMeshes, lodMeshes);
        for (int meshNumber = 0; meshNumber < meshCount; ++meshNumber) {
            const std::size_t mdlMesh = mdlModel + static_cast<std::size_t>(meshIndex) + static_cast<std::size_t>(meshNumber) * 116;
            const std::size_t vtxMesh = lodHeader + static_cast<std::size_t>(lodMeshOffset) + static_cast<std::size_t>(meshNumber) * 9;
            const int material = ReadI32(mdl, mdlMesh);
            const int vertexOffset = ReadI32(mdl, mdlMesh + 12);
            const int numStripGroups = ReadI32(vtx, vtxMesh);
            const int stripGroupOffset = ReadI32(vtx, vtxMesh + 4);
            if (numStripGroups <= 0 || stripGroupOffset < 0) {
                continue;
            }
            const std::size_t firstGroup = vtxMesh + static_cast<std::size_t>(stripGroupOffset);
            auto plausibleGroup = [&](int stride) {
                if (numStripGroups < 2) {
                    return stride == 33;
                }
                const std::size_t next = firstGroup + static_cast<std::size_t>(stride);
                if (next + 16 > vtx.size()) {
                    return false;
                }
                const int nextVerts = ReadI32(vtx, next);
                const int nextIndices = ReadI32(vtx, next + 8);
                return nextVerts > 0 && nextVerts < 200000 && nextIndices >= 0 && nextIndices < 2000000;
            };
            const int stripGroupStride = plausibleGroup(33) || !plausibleGroup(25) ? 33 : 25;
            StudioPrimitive primitive;
            primitive.material = textureName(material);
            for (int groupIndex = 0; groupIndex < numStripGroups; ++groupIndex) {
                const std::size_t group = firstGroup + static_cast<std::size_t>(groupIndex) * static_cast<std::size_t>(stripGroupStride);
                const int numVerts = ReadI32(vtx, group);
                const int vertOffset = ReadI32(vtx, group + 4);
                const int numIndices = ReadI32(vtx, group + 8);
                const int indexOffset = ReadI32(vtx, group + 12);
                if (numVerts <= 0 || numIndices < 3 || vertOffset < 0 || indexOffset < 0) {
                    continue;
                }
                const unsigned int base = static_cast<unsigned int>(primitive.vertices.size() / Mesh::kFloatsPerVertex);
                for (int vertexNumber = 0; vertexNumber < numVerts; ++vertexNumber) {
                    const std::size_t vtxVertex = group + static_cast<std::size_t>(vertOffset) + static_cast<std::size_t>(vertexNumber) * 9;
                    const int originalId = ReadU16(vtx, vtxVertex + 4);
                    const int globalIndex = modelVertexIndex + vertexOffset + originalId;
                    if (globalIndex < 0 || static_cast<std::size_t>(globalIndex) * kVertexSize + 48 > vertices.size()) {
                        primitive.vertices.insert(primitive.vertices.end(), Mesh::kFloatsPerVertex, 0.0F);
                        primitive.skin.push_back(StudioSkinVertex{});
                        continue;
                    }
                    const std::size_t vertexBase = static_cast<std::size_t>(globalIndex) * kVertexSize;
                    StudioSkinVertex skinVertex;
                    skinVertex.position = glm::vec3(
                        ReadFloat(vertices, vertexBase + 16),
                        ReadFloat(vertices, vertexBase + 20),
                        ReadFloat(vertices, vertexBase + 24)
                    );
                    skinVertex.boneCount = vertices[vertexBase + 15];
                    if (skinVertex.boneCount > 3) {
                        skinVertex.boneCount = 3;
                    }
                    for (unsigned char weightIndex = 0; weightIndex < skinVertex.boneCount; ++weightIndex) {
                        skinVertex.weights[weightIndex] = ReadFloat(vertices, vertexBase + static_cast<std::size_t>(weightIndex) * 4);
                        skinVertex.bones[weightIndex] = vertices[vertexBase + 12 + weightIndex];
                    }
                    primitive.skin.push_back(skinVertex);
                    const glm::vec3 position = SourcePointToWorld(
                        skinVertex.position.x,
                        skinVertex.position.y,
                        skinVertex.position.z
                    );
                    primitive.vertices.insert(primitive.vertices.end(), {
                        position.x, position.y, position.z,
                        1.0F, 1.0F, 1.0F,
                        ReadFloat(vertices, vertexBase + 40),
                        ReadFloat(vertices, vertexBase + 44),
                        0.0F, 0.0F,
                        0.0F,
                    });
                }
                for (int indexNumber = 0; indexNumber + 2 < numIndices; indexNumber += 3) {
                    const std::size_t indexBase = group + static_cast<std::size_t>(indexOffset) + static_cast<std::size_t>(indexNumber) * 2;
                    const unsigned int i0 = ReadU16(vtx, indexBase);
                    const unsigned int i1 = ReadU16(vtx, indexBase + 2);
                    const unsigned int i2 = ReadU16(vtx, indexBase + 4);
                    if (i0 >= static_cast<unsigned int>(numVerts) || i1 >= static_cast<unsigned int>(numVerts)
                        || i2 >= static_cast<unsigned int>(numVerts)) {
                        continue;
                    }
                    primitive.indices.insert(primitive.indices.end(), {base + i0, base + i1, base + i2});
                    anyTriangle = true;
                }
            }
            if (!primitive.indices.empty()) {
                model.parts.push_back(std::move(primitive));
            }
        }
    }
    return anyTriangle;
}

std::string ResolveMaterial(
    const GameFileSystem* files,
    const std::string& name,
    const std::vector<std::string>& folders
) {
    std::vector<std::string> candidates;
    auto push = [&](std::string path) {
        for (char& character : path) {
            if (character == '\\') {
                character = '/';
            }
        }
        while (!path.empty() && path.front() == '/') {
            path.erase(path.begin());
        }
        if (path.size() >= 4) {
            std::string extension = path.substr(path.size() - 4);
            for (char& character : extension) {
                character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
            }
            if (extension == ".vmt" || extension == ".vtf") {
                path.resize(path.size() - 4);
            }
        }
        if (!path.empty()) {
            candidates.push_back(std::move(path));
        }
    };
    push(name);
    if (name.find('/') == std::string::npos && name.find('\\') == std::string::npos) {
        for (const std::string& folder : folders) {
            push(folder + name);
        }
    }
    auto exists = [&](const std::string& path) {
        return files != nullptr
            && (files->Exists("materials/" + path + ".vmt") || files->Exists("materials/" + path + ".vtf"));
    };
    for (const std::string& candidate : candidates) {
        if (exists(candidate)) {
            return candidate;
        }
    }
    for (const std::string& candidate : candidates) {
        if (candidate.find('/') != std::string::npos) {
            return candidate;
        }
    }
    return candidates.empty() ? name : candidates.front();
}

std::string LowerCopy(std::string value) {
    for (char& character : value) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    return value;
}

std::string QuotedValue(const std::string& block, const std::string& key) {
    const std::string lower = LowerCopy(block);
    const std::string quoted = "\"" + key + "\"";
    std::size_t found = lower.find(quoted);
    if (found == std::string::npos) {
        found = lower.find(key);
        if (found == std::string::npos) {
            return {};
        }
    }
    const std::size_t first = block.find('"', found + key.size());
    if (first == std::string::npos) {
        return {};
    }
    const std::size_t second = block.find('"', first + 1);
    if (second == std::string::npos) {
        return {};
    }
    return block.substr(first + 1, second - first - 1);
}

std::string NthBlock(const std::string& text, const std::string& name, int index) {
    const std::string lower = LowerCopy(text);
    const std::string needle = LowerCopy(name);
    std::size_t search = 0;
    int seen = 0;
    while (search < lower.size()) {
        const std::size_t found = lower.find(needle, search);
        if (found == std::string::npos) {
            return {};
        }
        const bool boundary = found == 0 || !std::isalnum(static_cast<unsigned char>(lower[found - 1]));
        if (!boundary) {
            search = found + needle.size();
            continue;
        }
        const std::size_t brace = lower.find('{', found + needle.size());
        if (brace == std::string::npos) {
            return {};
        }
        int depth = 1;
        std::size_t cursor = brace + 1;
        for (; cursor < text.size() && depth > 0; ++cursor) {
            if (text[cursor] == '{') {
                ++depth;
            } else if (text[cursor] == '}') {
                --depth;
            }
        }
        if (seen == index) {
            return text.substr(brace + 1, cursor > brace + 1 ? cursor - brace - 2 : 0);
        }
        ++seen;
        search = cursor;
    }
    return {};
}

float ParseFloatOr(const std::string& text, float fallback) {
    if (text.empty()) {
        return fallback;
    }
    try {
        return std::stof(text);
    } catch (const std::exception&) {
        return fallback;
    }
}

struct PointKey {
    int x = 0;
    int y = 0;
    int z = 0;

    bool operator==(const PointKey& other) const {
        return x == other.x && y == other.y && z == other.z;
    }
};

struct PointKeyHash {
    std::size_t operator()(const PointKey& key) const {
        return static_cast<std::size_t>(key.x) * 73856093U
            ^ static_cast<std::size_t>(key.y) * 19349663U
            ^ static_cast<std::size_t>(key.z) * 83492791U;
    }
};

constexpr float kIvpMetersToInches = 39.37008F;
constexpr std::size_t kCompactTriangleBytes = 16;

glm::vec3 IvpMetersToWorld(float x, float y, float z) {
    // IVP is Y-up. Source Z-up is (x, -z, -y), then world Y-up swaps Z with Y.
    return SourcePointToWorld(
        x * kIvpMetersToInches,
        -z * kIvpMetersToInches,
        -y * kIvpMetersToInches
    );
}

void AppendLedge(
    const std::vector<std::uint8_t>& data,
    std::size_t solidStart,
    std::size_t solidEnd,
    std::size_t ledge,
    std::vector<std::vector<glm::vec3>>& convexes
) {
    if (ledge + 16 > solidEnd || ledge < solidStart) {
        return;
    }
    const std::int32_t pointOffset = ReadI32(data, ledge);
    const std::uint32_t bits = ReadU32(data, ledge + 8);
    const int compactFlag = static_cast<int>((bits >> 2U) & 0x3U);
    const int triangleCount = static_cast<int>(static_cast<std::int16_t>(ReadU16(data, ledge + 12)));
    if (compactFlag != 1 || pointOffset <= 0 || triangleCount <= 0 || triangleCount > 8192) {
        return;
    }
    const std::size_t triangleStart = ledge + 16;
    const std::size_t triangleBytes = static_cast<std::size_t>(triangleCount) * kCompactTriangleBytes;
    if (triangleStart + triangleBytes > solidEnd) {
        return;
    }
    std::vector<int> indices;
    indices.reserve(static_cast<std::size_t>(triangleCount) * 3);
    int maxIndex = 0;
    for (int triangle = 0; triangle < triangleCount; ++triangle) {
        const std::size_t triangleOffset = triangleStart + static_cast<std::size_t>(triangle) * kCompactTriangleBytes;
        for (const std::size_t indexOffset : {4U, 8U, 12U}) {
            const int index = static_cast<std::int16_t>(ReadU16(data, triangleOffset + indexOffset));
            if (index < 0) {
                return;
            }
            indices.push_back(index);
            maxIndex = std::max(maxIndex, index);
        }
    }
    if (maxIndex > 4096) {
        return;
    }
    const std::size_t pointBase = static_cast<std::size_t>(static_cast<std::ptrdiff_t>(ledge) + pointOffset);
    if (pointBase < triangleStart + triangleBytes
        || pointBase + static_cast<std::size_t>(maxIndex + 1) * 16 > solidEnd) {
        return;
    }
    std::vector<glm::vec3> points;
    std::unordered_set<PointKey, PointKeyHash> seen;
    for (int index : indices) {
        const std::size_t offset = pointBase + static_cast<std::size_t>(index) * 16;
        const glm::vec3 point = IvpMetersToWorld(
            ReadFloat(data, offset),
            ReadFloat(data, offset + 4),
            ReadFloat(data, offset + 8)
        );
        if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z)) {
            continue;
        }
        if (std::abs(point.x) > 64.0F || std::abs(point.y) > 64.0F || std::abs(point.z) > 64.0F) {
            continue;
        }
        const PointKey key{
            static_cast<int>(std::lround(point.x * 500.0F)),
            static_cast<int>(std::lround(point.y * 500.0F)),
            static_cast<int>(std::lround(point.z * 500.0F)),
        };
        if (!seen.insert(key).second) {
            continue;
        }
        points.push_back(point);
    }
    if (points.size() >= 4) {
        convexes.push_back(std::move(points));
    }
}

void WalkLedgeTree(
    const std::vector<std::uint8_t>& data,
    std::size_t solidStart,
    std::size_t solidEnd,
    std::size_t node,
    int depth,
    int& visits,
    std::vector<std::vector<glm::vec3>>& convexes
) {
    if (depth > 8192 || visits > 8192 || node + 8 > solidEnd || node < solidStart) {
        return;
    }
    ++visits;
    const std::int32_t right = ReadI32(data, node);
    const std::int32_t ledge = ReadI32(data, node + 4);
    if (right == 0) {
        AppendLedge(data, solidStart, solidEnd, static_cast<std::size_t>(static_cast<std::ptrdiff_t>(node) + ledge), convexes);
        return;
    }
    if (right < 28) {
        return;
    }
    WalkLedgeTree(data, solidStart, solidEnd, node + 28, depth + 1, visits, convexes);
    if (right > 0) {
        WalkLedgeTree(
            data,
            solidStart,
            solidEnd,
            static_cast<std::size_t>(static_cast<std::ptrdiff_t>(node) + right),
            depth + 1,
            visits,
            convexes
        );
    }
}

void ParseSolidConvexes(
    const std::vector<std::uint8_t>& data,
    std::size_t solidStart,
    std::size_t solidEnd,
    std::vector<std::vector<glm::vec3>>& convexes
) {
    if (solidStart + 64 > solidEnd) {
        return;
    }
    const std::size_t surface = solidStart + 32;
    const int root = ReadI32(data, surface + 32);
    if (root <= 0) {
        return;
    }
    const std::size_t rootNode = surface + static_cast<std::size_t>(root);
    if (rootNode >= solidEnd) {
        return;
    }
    int visits = 0;
    WalkLedgeTree(data, solidStart, solidEnd, rootNode, 0, visits, convexes);
}

void ParseModelPhysics(const GameFileSystem* files, const std::string& stem, StudioModel& model) {
    if (files == nullptr) {
        return;
    }
    std::vector<std::uint8_t> data;
    if (!files->Read(stem + ".phy", data) || data.size() < 16) {
        return;
    }
    const int headerSize = ReadI32(data, 0);
    const int solidCount = ReadI32(data, 8);
    if (headerSize < 16 || headerSize > 256 || solidCount <= 0 || solidCount > 64) {
        return;
    }
    if (static_cast<std::size_t>(headerSize) >= data.size()) {
        return;
    }
    std::size_t cursor = static_cast<std::size_t>(headerSize);
    for (int solid = 0; solid < solidCount; ++solid) {
        if (cursor + 4 > data.size()) {
            break;
        }
        const int solidSize = ReadI32(data, cursor);
        if (solidSize < 32 || cursor + static_cast<std::size_t>(solidSize) > data.size()) {
            break;
        }
        ParseSolidConvexes(data, cursor, cursor + static_cast<std::size_t>(solidSize), model.physicsConvexes);
        cursor += static_cast<std::size_t>(solidSize);
    }
    if (cursor + 4 <= data.size()) {
        const int textSize = ReadI32(data, cursor);
        cursor += 4;
        if (textSize > 0 && cursor + static_cast<std::size_t>(textSize) <= data.size()) {
            const std::string text(reinterpret_cast<const char*>(data.data() + cursor), static_cast<std::size_t>(textSize));
            const std::string solid = NthBlock(text, "solid", 0);
            const std::string edit = NthBlock(text, "editparams", 0);
            model.physicsMass = ParseFloatOr(QuotedValue(solid, "mass"), 0.0F);
            if (model.physicsMass <= 0.0F) {
                model.physicsMass = ParseFloatOr(QuotedValue(edit, "totalmass"), 0.0F);
            }
            model.physicsDamping = std::max(0.0F, ParseFloatOr(QuotedValue(solid, "damping"), 0.0F));
            model.physicsRotDamping = std::max(0.0F, ParseFloatOr(QuotedValue(solid, "rotdamping"), 0.05F));
            model.physicsInertia = ParseFloatOr(QuotedValue(solid, "inertia"), 1.0F);
            if (model.physicsInertia <= 0.0F) {
                model.physicsInertia = 1.0F;
            }
            model.physicsSurface = LowerCopy(QuotedValue(solid, "surfaceprop"));
        }
    }
    model.hasPhysicsHull = !model.physicsConvexes.empty();
}

bool PlausibleAnimName(const std::string& name) {
    if (name.empty() || name.size() > 64) {
        return false;
    }
    for (const char character : name) {
        const unsigned char value = static_cast<unsigned char>(character);
        if (std::isalnum(value) == 0 && character != '_') {
            return false;
        }
    }
    return true;
}

std::string ReadCStringBounded(const std::vector<std::uint8_t>& data, std::size_t offset) {
    if (offset >= data.size()) {
        return {};
    }
    std::size_t end = offset;
    while (end < data.size() && data[end] != 0 && end - offset < 128) {
        ++end;
    }
    return std::string(reinterpret_cast<const char*>(data.data() + offset), end - offset);
}

float HalfToFloat(std::uint16_t bits) {
    const unsigned mantissa = bits & 0x3FFU;
    const unsigned exponent = (bits >> 10U) & 0x1FU;
    const unsigned sign = bits >> 15U;
    if (exponent == 0) {
        const float value = static_cast<float>(mantissa) / 1024.0F * (1.0F / 16384.0F);
        return sign != 0 ? -value : value;
    }
    if (exponent == 31) {
        return 0.0F;
    }
    const unsigned out = (mantissa << 13U) | ((exponent - 15U + 127U) << 23U) | (sign << 31U);
    float value = 0.0F;
    std::memcpy(&value, &out, sizeof(value));
    return value;
}

void UnpackQuaternion48(const std::vector<std::uint8_t>& data, std::size_t offset, float quat[4]) {
    const int x = ReadU16(data, offset);
    const int y = ReadU16(data, offset + 2);
    const int packed = ReadU16(data, offset + 4);
    const int z = packed & 0x7FFF;
    quat[0] = static_cast<float>(x - 32768) * (1.0F / 32768.0F);
    quat[1] = static_cast<float>(y - 32768) * (1.0F / 32768.0F);
    quat[2] = static_cast<float>(z - 16384) * (1.0F / 16384.0F);
    const float square = 1.0F - quat[0] * quat[0] - quat[1] * quat[1] - quat[2] * quat[2];
    quat[3] = square > 0.0F ? std::sqrt(square) : 0.0F;
    if ((packed & 0x8000) != 0) {
        quat[3] = -quat[3];
    }
}

void UnpackQuaternion64(const std::vector<std::uint8_t>& data, std::size_t offset, float quat[4]) {
    std::uint64_t packed = 0;
    for (int byte = 0; byte < 8 && offset + static_cast<std::size_t>(byte) < data.size(); ++byte) {
        packed |= static_cast<std::uint64_t>(data[offset + static_cast<std::size_t>(byte)]) << (8U * static_cast<unsigned>(byte));
    }
    const int x = static_cast<int>(packed & 0x1FFFFFULL);
    const int y = static_cast<int>((packed >> 21U) & 0x1FFFFFULL);
    const int z = static_cast<int>((packed >> 42U) & 0x1FFFFFULL);
    quat[0] = static_cast<float>(x - 1048576) * (1.0F / 1048576.5F);
    quat[1] = static_cast<float>(y - 1048576) * (1.0F / 1048576.5F);
    quat[2] = static_cast<float>(z - 1048576) * (1.0F / 1048576.5F);
    const float square = 1.0F - quat[0] * quat[0] - quat[1] * quat[1] - quat[2] * quat[2];
    quat[3] = square > 0.0F ? std::sqrt(square) : 0.0F;
    if ((packed >> 63U) != 0) {
        quat[3] = -quat[3];
    }
}

std::uint16_t ReadU16Raw(const std::uint8_t* data, std::size_t size, std::size_t offset) {
    if (data == nullptr || offset + 2 > size) {
        return 0;
    }
    return static_cast<std::uint16_t>(data[offset]) | (static_cast<std::uint16_t>(data[offset + 1]) << 8U);
}

float AnimValue(const std::uint8_t* data, std::size_t size, std::size_t valueOffset, int frame, float scale) {
    if (data == nullptr || valueOffset + 2 > size) {
        return 0.0F;
    }
    int remaining = frame;
    std::size_t cursor = valueOffset;
    for (int guard = 0; guard < 4096; ++guard) {
        if (cursor + 2 > size) {
            return 0.0F;
        }
        const int valid = data[cursor];
        const int total = data[cursor + 1];
        if (total == 0) {
            return 0.0F;
        }
        if (total > remaining) {
            float raw = 0.0F;
            if (valid > remaining) {
                const std::size_t sample = cursor + static_cast<std::size_t>(remaining + 1) * 2;
                if (sample + 2 <= size) {
                    raw = static_cast<float>(static_cast<std::int16_t>(ReadU16Raw(data, size, sample)));
                }
            } else if (valid > 0) {
                const std::size_t sample = cursor + static_cast<std::size_t>(valid) * 2;
                if (sample + 2 <= size) {
                    raw = static_cast<float>(static_cast<std::int16_t>(ReadU16Raw(data, size, sample)));
                }
            }
            return raw * scale;
        }
        remaining -= total;
        cursor += static_cast<std::size_t>(valid + 1) * 2;
    }
    return 0.0F;
}

void AngleQuaternion(const glm::vec3& euler, float quat[4]) {
    const float halfX = euler.x * 0.5F;
    const float halfY = euler.y * 0.5F;
    const float halfZ = euler.z * 0.5F;
    const float sx = std::sin(halfX);
    const float cx = std::cos(halfX);
    const float sy = std::sin(halfY);
    const float cy = std::cos(halfY);
    const float sz = std::sin(halfZ);
    const float cz = std::cos(halfZ);
    quat[0] = sx * cy * cz - cx * sy * sz;
    quat[1] = cx * sy * cz + sx * cy * sz;
    quat[2] = cx * cy * sz - sx * sy * cz;
    quat[3] = cx * cy * cz + sx * sy * sz;
}

void QuaternionMatrix(const float quat[4], const glm::vec3& position, float matrix[12]) {
    const float x = quat[0];
    const float y = quat[1];
    const float z = quat[2];
    const float w = quat[3];
    matrix[0] = 1.0F - 2.0F * y * y - 2.0F * z * z;
    matrix[4] = 2.0F * x * y + 2.0F * w * z;
    matrix[8] = 2.0F * x * z - 2.0F * w * y;
    matrix[1] = 2.0F * x * y - 2.0F * w * z;
    matrix[5] = 1.0F - 2.0F * x * x - 2.0F * z * z;
    matrix[9] = 2.0F * y * z + 2.0F * w * x;
    matrix[2] = 2.0F * x * z + 2.0F * w * y;
    matrix[6] = 2.0F * y * z - 2.0F * w * x;
    matrix[10] = 1.0F - 2.0F * x * x - 2.0F * y * y;
    matrix[3] = position.x;
    matrix[7] = position.y;
    matrix[11] = position.z;
}

void ConcatMatrix(const float parent[12], const float local[12], float out[12]) {
    for (int row = 0; row < 3; ++row) {
        const int base = row * 4;
        for (int column = 0; column < 3; ++column) {
            out[base + column] = parent[base] * local[column]
                + parent[base + 1] * local[4 + column]
                + parent[base + 2] * local[8 + column];
        }
        out[base + 3] = parent[base] * local[3] + parent[base + 1] * local[7] + parent[base + 2] * local[11] + parent[base + 3];
    }
}

int ChooseStride(
    const std::vector<std::uint8_t>& data,
    int count,
    int start,
    const int* strides,
    int strideCount,
    int nameOffset,
    bool anim
) {
    int bestStride = strides[0];
    int bestScore = -1;
    const int samples = std::min(count, 8);
    for (int strideIndex = 0; strideIndex < strideCount; ++strideIndex) {
        const int stride = strides[strideIndex];
        int score = 0;
        for (int index = 0; index < samples; ++index) {
            const std::size_t base = static_cast<std::size_t>(start) + static_cast<std::size_t>(index) * static_cast<std::size_t>(stride);
            if (base + 24 > data.size()) {
                break;
            }
            const int nameRel = ReadI32(data, base + static_cast<std::size_t>(nameOffset));
            if (nameRel <= 0) {
                continue;
            }
            const std::string name = ReadCStringBounded(data, base + static_cast<std::size_t>(nameRel));
            if (!PlausibleAnimName(name)) {
                continue;
            }
            if (anim) {
                const float fps = ReadFloat(data, base + 8);
                const int frames = ReadI32(data, base + 16);
                if (fps < 1.0F || fps > 200.0F || frames < 1 || frames > 8192) {
                    continue;
                }
            }
            ++score;
        }
        if (score > bestScore) {
            bestScore = score;
            bestStride = stride;
        }
    }
    return bestScore > 0 ? bestStride : 0;
}

void ParseBones(const std::vector<std::uint8_t>& mdl, StudioAnimation& animation) {
    if (mdl.size() < 168) {
        return;
    }
    const int count = ReadI32(mdl, 156);
    const int index = ReadI32(mdl, 160);
    if (count <= 0 || count > 256 || index <= 0) {
        return;
    }
    constexpr int kStride = 216;
    if (static_cast<std::size_t>(index) + static_cast<std::size_t>(count) * kStride > mdl.size()) {
        return;
    }
    std::vector<StudioBone> bones;
    bones.reserve(static_cast<std::size_t>(count));
    int named = 0;
    for (int boneIndex = 0; boneIndex < count; ++boneIndex) {
        const std::size_t base = static_cast<std::size_t>(index) + static_cast<std::size_t>(boneIndex) * kStride;
        StudioBone bone;
        bone.parent = ReadI32(mdl, base + 4);
        if (bone.parent < -1 || bone.parent >= count) {
            return;
        }
        const int nameRel = ReadI32(mdl, base);
        if (PlausibleAnimName(ReadCStringBounded(mdl, base + static_cast<std::size_t>(nameRel)))) {
            ++named;
        }
        bone.position = glm::vec3(ReadFloat(mdl, base + 32), ReadFloat(mdl, base + 36), ReadFloat(mdl, base + 40));
        bone.rotation[0] = ReadFloat(mdl, base + 44);
        bone.rotation[1] = ReadFloat(mdl, base + 48);
        bone.rotation[2] = ReadFloat(mdl, base + 52);
        bone.rotation[3] = ReadFloat(mdl, base + 56);
        bone.euler = glm::vec3(ReadFloat(mdl, base + 60), ReadFloat(mdl, base + 64), ReadFloat(mdl, base + 68));
        bone.positionScale = glm::vec3(ReadFloat(mdl, base + 72), ReadFloat(mdl, base + 76), ReadFloat(mdl, base + 80));
        bone.rotationScale = glm::vec3(ReadFloat(mdl, base + 84), ReadFloat(mdl, base + 88), ReadFloat(mdl, base + 92));
        for (int element = 0; element < 12; ++element) {
            bone.poseToBone[element] = ReadFloat(mdl, base + 96 + static_cast<std::size_t>(element) * 4);
        }
        bones.push_back(bone);
    }
    if (named == 0) {
        return;
    }
    animation.bones = std::move(bones);
}

void AddSequencesFromBuffer(const std::vector<std::uint8_t>& mdl, int sourceIndex, StudioAnimation& animation) {
    if (mdl.size() < 200) {
        return;
    }
    const int animCount = ReadI32(mdl, 180);
    const int animStart = ReadI32(mdl, 184);
    const int sequenceCount = ReadI32(mdl, 188);
    const int sequenceStart = ReadI32(mdl, 192);
    const int animStrides[] = {100, 96, 92, 104};
    const int sequenceStrides[] = {212, 204, 224, 196, 220, 208};
    const int animStride = ChooseStride(mdl, animCount, animStart, animStrides, 4, 4, true);
    const int sequenceStride = ChooseStride(mdl, sequenceCount, sequenceStart, sequenceStrides, 6, 4, false);
    if (animStride == 0 || sequenceStride == 0 || animCount <= 0 || sequenceCount <= 0) {
        return;
    }
    for (int sequenceIndex = 0; sequenceIndex < sequenceCount; ++sequenceIndex) {
        const std::size_t base = static_cast<std::size_t>(sequenceStart) + static_cast<std::size_t>(sequenceIndex) * static_cast<std::size_t>(sequenceStride);
        if (base + 64 > mdl.size()) {
            break;
        }
        const int nameRel = ReadI32(mdl, base + 4);
        const std::string name = ReadCStringBounded(mdl, base + static_cast<std::size_t>(nameRel));
        if (!PlausibleAnimName(name)) {
            continue;
        }
        const int animList = ReadI32(mdl, base + 60);
        if (animList <= 0 || base + static_cast<std::size_t>(animList) + 2 > mdl.size()) {
            continue;
        }
        const int anim = ReadU16(mdl, base + static_cast<std::size_t>(animList));
        if (anim < 0 || anim >= animCount) {
            continue;
        }
        const std::size_t desc = static_cast<std::size_t>(animStart) + static_cast<std::size_t>(anim) * static_cast<std::size_t>(animStride);
        if (desc + 88 > mdl.size()) {
            continue;
        }
        StudioSequence sequence;
        sequence.name = name;
        sequence.fps = ReadFloat(mdl, desc + 8);
        sequence.frameCount = std::max(1, ReadI32(mdl, desc + 16));
        sequence.loop = (ReadI32(mdl, base + 12) & 1) != 0;
        sequence.source = sourceIndex;
        sequence.descOffset = static_cast<int>(desc);
        sequence.animBlock = ReadI32(mdl, desc + 52);
        sequence.animIndex = ReadI32(mdl, desc + 56);
        const int sectionIndex = ReadI32(mdl, desc + 80);
        sequence.sectionFrames = ReadI32(mdl, desc + 84);
        if (sequence.sectionFrames > 0 && sectionIndex > 0) {
            sequence.sectionOffset = static_cast<int>(desc) + sectionIndex;
        }
        if (sequence.fps < 1.0F || sequence.fps > 200.0F) {
            sequence.fps = 30.0F;
        }
        animation.sequences.push_back(std::move(sequence));
    }
}

void CollectIncludePaths(const std::vector<std::uint8_t>& mdl, std::vector<std::string>& paths) {
    if (mdl.size() > 348) {
        const int count = ReadI32(mdl, 336);
        const int index = ReadI32(mdl, 340);
        if (count > 0 && count < 16 && index > 0) {
            for (int include = 0; include < count; ++include) {
                const std::size_t base = static_cast<std::size_t>(index) + static_cast<std::size_t>(include) * 8;
                if (base + 8 > mdl.size()) {
                    break;
                }
                const int nameRel = ReadI32(mdl, base + 4);
                if (nameRel <= 0) {
                    continue;
                }
                std::string path = ReadCStringBounded(mdl, base + static_cast<std::size_t>(nameRel));
                if (LowerCopy(path).find(".mdl") != std::string::npos) {
                    paths.push_back(std::move(path));
                }
            }
        }
    }
    std::size_t cursor = 0;
    while (cursor < mdl.size()) {
        if (mdl[cursor] < 32 || mdl[cursor] > 126) {
            ++cursor;
            continue;
        }
        const std::size_t start = cursor;
        while (cursor < mdl.size() && mdl[cursor] >= 32 && mdl[cursor] <= 126) {
            ++cursor;
        }
        if (cursor < mdl.size() && mdl[cursor] == 0 && cursor - start > 8) {
            std::string text(reinterpret_cast<const char*>(mdl.data() + start), cursor - start);
            const std::string lower = LowerCopy(text);
            if (lower.find("models/") != std::string::npos && lower.size() >= 4 && lower.substr(lower.size() - 4) == ".mdl") {
                paths.push_back(std::move(text));
            }
        }
        ++cursor;
    }
}

void AppendAnimationSource(
    const GameFileSystem* files,
    const std::string& path,
    const std::vector<std::uint8_t>& mdl,
    StudioAnimation& animation,
    int depth,
    std::unordered_set<std::string>& seen
) {
    const std::string key = LowerCopy(path);
    if (!seen.insert(key).second || depth > 2) {
        return;
    }
    StudioAnimBuffer source;
    source.bytes = mdl;
    if (mdl.size() > 360) {
        source.animBlockCount = ReadI32(mdl, 352);
        source.animBlockIndex = ReadI32(mdl, 356);
        const int nameRel = ReadI32(mdl, 348);
        if (files != nullptr && nameRel > 0) {
            const std::string aniName = ReadCStringBounded(mdl, static_cast<std::size_t>(nameRel));
            if (LowerCopy(aniName).find(".ani") != std::string::npos) {
                std::vector<std::uint8_t> ani;
                if (files->Read(aniName, ani) && !ani.empty()) {
                    source.ani = std::move(ani);
                }
            }
        }
    }
    if (source.ani.empty() && files != nullptr && path.size() > 4) {
        static_cast<void>(files->Read(path.substr(0, path.size() - 4) + ".ani", source.ani));
    }
    const int sourceIndex = static_cast<int>(animation.sources.size());
    animation.sources.push_back(std::move(source));
    AddSequencesFromBuffer(mdl, sourceIndex, animation);

    if (depth == 2) {
        return;
    }
    std::vector<std::string> includes;
    CollectIncludePaths(mdl, includes);
    for (const std::string& include : includes) {
        std::string normalized = include;
        for (char& character : normalized) {
            if (character == '\\') {
                character = '/';
            }
        }
        if (LowerCopy(normalized) == key || files == nullptr) {
            continue;
        }
        std::vector<std::uint8_t> included;
        if (!files->Read(normalized, included) || included.size() < 128 || ReadI32(included, 0) != 0x54534449) {
            continue;
        }
        AppendAnimationSource(files, normalized, included, animation, depth + 1, seen);
    }
}

const std::uint8_t* AnimationBytes(
    const StudioAnimation& animation,
    const StudioSequence& sequence,
    int frame,
    int& localFrame,
    std::size_t& cursor,
    std::size_t& extent
) {
    if (sequence.source < 0 || static_cast<std::size_t>(sequence.source) >= animation.sources.size()) {
        return nullptr;
    }
    const StudioAnimBuffer& source = animation.sources[static_cast<std::size_t>(sequence.source)];
    int block = sequence.animBlock;
    int index = sequence.animIndex;
    localFrame = frame;
    const std::vector<std::uint8_t>& mdl = source.bytes;
    const std::vector<std::uint8_t>& external = source.ani.empty() ? source.bytes : source.ani;
    if (sequence.sectionFrames > 0 && sequence.sectionOffset >= 0) {
        int section = frame / std::max(sequence.sectionFrames, 1);
        const int sectionCount = sequence.frameCount / std::max(sequence.sectionFrames, 1) + 2;
        if (section < 0) {
            section = 0;
        }
        if (section >= sectionCount) {
            section = sectionCount - 1;
        }
        const std::size_t at = static_cast<std::size_t>(sequence.sectionOffset) + static_cast<std::size_t>(section) * 8;
        if (at + 8 <= mdl.size()) {
            block = ReadI32(mdl, at);
            index = ReadI32(mdl, at + 4);
            localFrame = frame - section * sequence.sectionFrames;
        }
    }
    if (index < 0) {
        return nullptr;
    }
    if (block == 0) {
        cursor = static_cast<std::size_t>(sequence.descOffset) + static_cast<std::size_t>(index);
        extent = mdl.size();
        if (cursor >= mdl.size()) {
            return nullptr;
        }
        return mdl.data();
    }
    if (source.animBlockIndex <= 0 || block <= 0 || block >= source.animBlockCount) {
        return nullptr;
    }
    const std::size_t blockAt = static_cast<std::size_t>(source.animBlockIndex) + static_cast<std::size_t>(block) * 8;
    if (blockAt + 4 > mdl.size()) {
        return nullptr;
    }
    const int dataStart = ReadI32(mdl, blockAt);
    cursor = static_cast<std::size_t>(dataStart) + static_cast<std::size_t>(index);
    extent = external.size();
    if (cursor >= external.size()) {
        return nullptr;
    }
    return external.data();
}

void ParseStudioAnimation(const GameFileSystem* files, const std::string& path, const std::vector<std::uint8_t>& mdl, StudioAnimation& animation) {
    ParseBones(mdl, animation);
    if (animation.bones.empty()) {
        return;
    }
    std::unordered_set<std::string> seen;
    AppendAnimationSource(files, path, mdl, animation, 0, seen);
    animation.idle = -1;
    animation.walk = -1;
    animation.run = -1;
    animation.shoot = -1;
}

} // namespace

namespace {

std::string LowerHint(std::string value) {
    for (char& character : value) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    return value;
}

int ScoreSequence(const std::string& name, const std::string& hint) {
    if (hint.empty() || name.empty()) {
        return -1;
    }
    if (name == hint) {
        return 1000;
    }
    if (name.find(hint) == std::string::npos) {
        return -1;
    }
    int score = 220 - static_cast<int>(name.size());
    if (name.rfind("layer", 0) == 0 || name.find("_layer") != std::string::npos) {
        score -= 60;
    }
    if (hint == "idle" && (name.find("angry") != std::string::npos || name.find("combat") != std::string::npos
        || name.find("cower") != std::string::npos || name.find("crouch") != std::string::npos)) {
        score -= 40;
    }
    if ((hint == "walk" || hint == "run") && name.find("crouch") != std::string::npos) {
        score -= 30;
    }
    return score;
}

void BuildSkinMatrices(
    const std::vector<StudioBone>& bones,
    const std::vector<glm::vec3>& positions,
    const std::vector<std::array<float, 4>>& rotations,
    std::vector<glm::mat4>& skin
) {
    const std::size_t boneCount = bones.size();
    std::vector<std::array<float, 12>> world(boneCount);
    skin.resize(boneCount);
    for (std::size_t index = 0; index < boneCount; ++index) {
        float local[12];
        const glm::vec3 position = index < positions.size() ? positions[index] : bones[index].position;
        const float* rotation = index < rotations.size()
            ? rotations[index].data()
            : bones[index].rotation;
        QuaternionMatrix(rotation, position, local);
        const int parent = bones[index].parent;
        if (parent >= 0 && static_cast<std::size_t>(parent) < index) {
            ConcatMatrix(world[static_cast<std::size_t>(parent)].data(), local, world[index].data());
        } else {
            std::copy(local, local + 12, world[index].begin());
        }
        float posed[12];
        ConcatMatrix(world[index].data(), bones[index].poseToBone, posed);
        glm::mat4 matrix(1.0F);
        matrix[0] = glm::vec4(posed[0], posed[4], posed[8], 0.0F);
        matrix[1] = glm::vec4(posed[1], posed[5], posed[9], 0.0F);
        matrix[2] = glm::vec4(posed[2], posed[6], posed[10], 0.0F);
        matrix[3] = glm::vec4(posed[3], posed[7], posed[11], 1.0F);
        skin[index] = matrix;
    }
}

glm::vec3 SkinnedSourcePosition(const StudioSkinVertex& skin, const std::vector<glm::mat4>& bones) {
    if (skin.boneCount == 0 || bones.empty()) {
        return skin.position;
    }
    glm::vec3 sum(0.0F);
    float weight = 0.0F;
    const int influences = std::min(static_cast<int>(skin.boneCount), 3);
    for (int influence = 0; influence < influences; ++influence) {
        const unsigned int bone = skin.bones[influence];
        if (bone >= bones.size()) {
            continue;
        }
        sum += skin.weights[influence] * glm::vec3(bones[bone] * glm::vec4(skin.position, 1.0F));
        weight += skin.weights[influence];
    }
    if (weight <= 0.0F) {
        return skin.position;
    }
    return sum;
}

void BakeReferencePose(StudioModel& model) {
    const std::vector<StudioBone>& bones = model.animation.bones;
    if (bones.empty()) {
        return;
    }
    const std::size_t boneCount = bones.size();
    std::vector<glm::vec3> positions(boneCount);
    std::vector<std::array<float, 4>> rotations(boneCount);
    for (std::size_t index = 0; index < boneCount; ++index) {
        positions[index] = bones[index].position;
        for (int element = 0; element < 4; ++element) {
            rotations[index][static_cast<std::size_t>(element)] = bones[index].rotation[element];
        }
    }
    std::vector<glm::mat4> skin;
    BuildSkinMatrices(bones, positions, rotations, skin);

    constexpr float kMoveEpsilon = 0.05F;
    bool moved = false;
    for (const StudioPrimitive& part : model.parts) {
        for (const StudioSkinVertex& vertex : part.skin) {
            if (vertex.boneCount == 0) {
                continue;
            }
            const glm::vec3 posed = SkinnedSourcePosition(vertex, skin);
            const glm::vec3 delta = posed - vertex.position;
            if (delta.x * delta.x + delta.y * delta.y + delta.z * delta.z > kMoveEpsilon * kMoveEpsilon) {
                moved = true;
                break;
            }
        }
        if (moved) {
            break;
        }
    }
    if (!moved) {
        return;
    }

    for (StudioPrimitive& part : model.parts) {
        if (part.skin.size() * Mesh::kFloatsPerVertex != part.vertices.size()) {
            continue;
        }
        for (std::size_t index = 0; index < part.skin.size(); ++index) {
            const StudioSkinVertex& vertex = part.skin[index];
            if (vertex.boneCount == 0) {
                continue;
            }
            const glm::vec3 posed = SkinnedSourcePosition(vertex, skin);
            const glm::vec3 world = SourcePointToWorld(posed.x, posed.y, posed.z);
            const std::size_t offset = index * Mesh::kFloatsPerVertex;
            part.vertices[offset] = world.x;
            part.vertices[offset + 1] = world.y;
            part.vertices[offset + 2] = world.z;
        }
    }
}

} // namespace

int FindStudioSequence(const StudioAnimation& animation, std::string_view hint) {
    const std::string wanted = LowerHint(std::string(hint));
    int best = -1;
    int bestScore = -1;
    for (int index = 0; index < static_cast<int>(animation.sequences.size()); ++index) {
        const int score = ScoreSequence(LowerHint(animation.sequences[static_cast<std::size_t>(index)].name), wanted);
        if (score > bestScore) {
            bestScore = score;
            best = index;
        }
    }
    return bestScore > 0 ? best : -1;
}

bool SampleStudioPose(const StudioAnimation& animation, int sequence, float timeSeconds, std::vector<glm::mat4>& skin) {
    skin.clear();
    if (sequence < 0 || static_cast<std::size_t>(sequence) >= animation.sequences.size() || animation.bones.empty()) {
        return false;
    }
    const StudioSequence& desc = animation.sequences[static_cast<std::size_t>(sequence)];
    const int frameSpan = std::max(desc.frameCount - 1, 1);
    int frame = static_cast<int>(timeSeconds * desc.fps);
    if (desc.loop) {
        frame %= frameSpan;
    } else {
        frame = std::min(frame, frameSpan);
    }
    if (frame < 0) {
        frame = 0;
    }

    const std::size_t boneCount = animation.bones.size();
    std::vector<glm::vec3> positions(boneCount);
    std::vector<std::array<float, 4>> rotations(boneCount);
    for (std::size_t index = 0; index < boneCount; ++index) {
        positions[index] = animation.bones[index].position;
        for (int element = 0; element < 4; ++element) {
            rotations[index][static_cast<std::size_t>(element)] = animation.bones[index].rotation[element];
        }
    }

    int localFrame = 0;
    std::size_t cursor = 0;
    std::size_t extent = 0;
    const std::uint8_t* bytes = AnimationBytes(animation, desc, frame, localFrame, cursor, extent);
    if (bytes != nullptr) {
        for (int guard = 0; guard < 256 && cursor + 4 <= extent; ++guard) {
            const int bone = bytes[cursor];
            const int flags = bytes[cursor + 1];
            const int next = ReadU16Raw(bytes, extent, cursor + 2);
            std::size_t data = cursor + 4;
            const bool delta = (flags & 0x10) != 0;
            if (!delta && bone >= 0 && static_cast<std::size_t>(bone) < boneCount) {
                const StudioBone& rest = animation.bones[static_cast<std::size_t>(bone)];
                if ((flags & 0x20) != 0 && data + 8 <= extent) {
                    std::vector<std::uint8_t> packed(bytes + data, bytes + data + 8);
                    UnpackQuaternion64(packed, 0, rotations[static_cast<std::size_t>(bone)].data());
                    data += 8;
                } else if ((flags & 0x02) != 0 && data + 6 <= extent) {
                    std::vector<std::uint8_t> packed(bytes + data, bytes + data + 6);
                    UnpackQuaternion48(packed, 0, rotations[static_cast<std::size_t>(bone)].data());
                    data += 6;
                }
                if ((flags & 0x01) != 0 && data + 6 <= extent) {
                    positions[static_cast<std::size_t>(bone)] = glm::vec3(
                        HalfToFloat(ReadU16Raw(bytes, extent, data)),
                        HalfToFloat(ReadU16Raw(bytes, extent, data + 2)),
                        HalfToFloat(ReadU16Raw(bytes, extent, data + 4))
                    );
                    data += 6;
                }
                const bool animRot = (flags & 0x08) != 0;
                const bool animPos = (flags & 0x04) != 0;
                if (animRot || animPos) {
                    const std::size_t rotPointer = data;
                    const std::size_t posPointer = data + (animRot ? 6 : 0);
                    if (animRot && rotPointer + 6 <= extent) {
                        glm::vec3 euler = rest.euler;
                        for (int axis = 0; axis < 3; ++axis) {
                            const int offset = static_cast<std::int16_t>(ReadU16Raw(bytes, extent, rotPointer + static_cast<std::size_t>(axis) * 2));
                            if (offset <= 0) {
                                continue;
                            }
                            const float scale = axis == 0 ? rest.rotationScale.x : axis == 1 ? rest.rotationScale.y : rest.rotationScale.z;
                            const float base = axis == 0 ? rest.euler.x : axis == 1 ? rest.euler.y : rest.euler.z;
                            const float value = AnimValue(bytes, extent, rotPointer + static_cast<std::size_t>(offset), localFrame, scale);
                            if (axis == 0) {
                                euler.x = base + value;
                            } else if (axis == 1) {
                                euler.y = base + value;
                            } else {
                                euler.z = base + value;
                            }
                        }
                        AngleQuaternion(euler, rotations[static_cast<std::size_t>(bone)].data());
                    }
                    if (animPos && posPointer + 6 <= extent) {
                        glm::vec3 position = rest.position;
                        for (int axis = 0; axis < 3; ++axis) {
                            const int offset = static_cast<std::int16_t>(ReadU16Raw(bytes, extent, posPointer + static_cast<std::size_t>(axis) * 2));
                            if (offset <= 0) {
                                continue;
                            }
                            const float scale = axis == 0 ? rest.positionScale.x : axis == 1 ? rest.positionScale.y : rest.positionScale.z;
                            const float base = axis == 0 ? rest.position.x : axis == 1 ? rest.position.y : rest.position.z;
                            const float value = AnimValue(bytes, extent, posPointer + static_cast<std::size_t>(offset), localFrame, scale);
                            if (axis == 0) {
                                position.x = base + value;
                            } else if (axis == 1) {
                                position.y = base + value;
                            } else {
                                position.z = base + value;
                            }
                        }
                        positions[static_cast<std::size_t>(bone)] = position;
                    }
                }
            }
            if (next == 0) {
                break;
            }
            cursor += static_cast<std::size_t>(next);
        }
    }

    BuildSkinMatrices(animation.bones, positions, rotations, skin);
    return true;
}

StudioModel LoadStudioModel(const GameFileSystem* files, const std::string& modelPath) {
    StudioModel model;
    if (files == nullptr || modelPath.empty()) {
        AppendBox(model);
        return model;
    }
    std::string path = modelPath;
    for (char& character : path) {
        if (character == '\\') {
            character = '/';
        }
    }
    std::vector<std::uint8_t> mdl;
    if (!files->Read(path, mdl) || mdl.size() < 128 || ReadI32(mdl, 0) != 0x54534449) {
        AppendBox(model);
        return model;
    }
    model.hullMin = glm::vec3(ReadFloat(mdl, 104), ReadFloat(mdl, 108), ReadFloat(mdl, 112));
    model.hullMax = glm::vec3(ReadFloat(mdl, 116), ReadFloat(mdl, 120), ReadFloat(mdl, 124));
    if (model.hullMax.x < model.hullMin.x) {
        model.hullMin = glm::vec3(-16.0F);
        model.hullMax = glm::vec3(16.0F);
    }

    const std::string stem = path.size() >= 4 ? path.substr(0, path.size() - 4) : path;
    ParseModelPhysics(files, stem, model);
    std::vector<std::uint8_t> vvd;
    std::vector<std::uint8_t> vtx;
    static_cast<void>(files->Read(stem + ".vvd", vvd));
    if (!files->Read(stem + ".dx90.vtx", vtx) && !files->Read(stem + ".vtx", vtx)) {
        static_cast<void>(files->Read(stem + ".dx80.vtx", vtx));
    }
    if (!AppendMeshes(model, mdl, vvd, vtx)) {
        model.parts.clear();
        AppendBox(model);
        model.hasMesh = false;
        return model;
    }
    const std::size_t slash = path.find_last_of('/');
    if (slash != std::string::npos) {
        model.materialFolders.push_back(path.substr(0, slash + 1));
    }
    for (StudioPrimitive& part : model.parts) {
        part.material = ResolveMaterial(files, part.material, model.materialFolders);
    }
    ParseStudioAnimation(files, path, mdl, model.animation);
    model.animation.idle = FindStudioSequence(model.animation, "idle");
    model.animation.walk = FindStudioSequence(model.animation, "walk");
    model.animation.run = FindStudioSequence(model.animation, "run");
    model.animation.shoot = FindStudioSequence(model.animation, "shoot");
    if (model.animation.shoot < 0) {
        model.animation.shoot = FindStudioSequence(model.animation, "fire");
    }
    if (model.animation.shoot < 0) {
        model.animation.shoot = FindStudioSequence(model.animation, "attack");
    }
    BakeReferencePose(model);
    model.hasMesh = true;
    return model;
}
