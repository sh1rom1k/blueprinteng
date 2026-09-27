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
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "GameFileSystem.hpp"
#include "LightStyle.hpp"
#include "Mesh.hpp"
#include "Shader.hpp"
#include "SourceCoords.hpp"
#include "VtfTexture.hpp"

#include <glm/glm.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

struct BspVisibility {
    struct Plane {
        float x = 0.0F;
        float y = 0.0F;
        float z = 0.0F;
        float distance = 0.0F;
    };
    struct Node {
        int plane = 0;
        int child[2] = {0, 0};
    };
    struct Leaf {
        int contents = 0;
        int cluster = -1;
        int area = -1;
        int water = -1;
    };
    struct Area {
        int firstPortal = 0;
        int portalCount = 0;
    };
    struct Portal {
        int number = 0;
        int otherArea = 0;
        int firstVert = 0;
        int vertCount = 0;
        bool startOpen = true;
        std::string target;
    };
    struct PortalEntity {
        int number = 0;
        bool startOpen = true;
        std::string target;
    };

    std::vector<Plane> planes;
    std::vector<Node> nodes;
    std::vector<Leaf> leaves;
    std::vector<std::uint8_t> clusterPvs;
    int clusterCount = 0;
    int pvsRowBytes = 0;
    std::vector<Area> areas;
    std::vector<Portal> portals;
    std::vector<glm::vec3> portalVertices;
    std::vector<int> faceLeafOffset;
    std::vector<int> faceLeafIndices;
    std::vector<PortalEntity> portalEntities;
    bool cullWorld = false;
    std::vector<std::uint8_t> visibleLeaves;
    std::vector<std::uint8_t> visibleFaces;
    std::vector<float> waterSurfaceY;
};

struct CubemapSample {
    glm::vec3 position{0.0F};
    int x = 0;
    int y = 0;
    int z = 0;
};

struct FillLight {
    glm::vec3 position{0.0F};
    glm::vec3 direction{0.0F, -1.0F, 0.0F};
    glm::vec3 color{0.0F};
    float radius = 0.0F;
    float constant = 0.0F;
    float linear = 0.0F;
    float quadratic = 1.0F;
    float stopDot = 1.0F;
    float stopDot2 = 0.0F;
    bool ambient = false;
    bool spot = false;
};

glm::vec3 ShadeFill(const glm::vec3& world, const glm::vec3& normal, const std::vector<FillLight>& lights) {
    glm::vec3 color(0.0F);
    glm::vec3 ambient(0.0F);
    const bool useNormal = glm::length(normal) > 1.0e-4F;
    for (const FillLight& light : lights) {
        if (light.ambient) {
            glm::vec3 sky = light.color;
            const float peak = std::max(sky.r, std::max(sky.g, sky.b));
            if (peak > 1.0F) {
                sky /= peak;
            }
            ambient += sky * 0.22F;
            continue;
        }
        const glm::vec3 delta = light.position - world;
        const float dist = glm::length(delta);
        if (light.radius > 0.0F && dist > light.radius) {
            continue;
        }
        float constant = light.constant;
        float linear = light.linear;
        float quadratic = light.quadratic;
        if (constant <= 1.0e-4F && linear <= 1.0e-4F && quadratic <= 1.0e-4F) {
            const float inches = 1.0F / kSourceToWorld;
            quadratic = inches * inches;
        }
        const float denom = constant + linear * dist + quadratic * dist * dist;
        float atten = denom > 1.0e-4F ? 1.0F / denom : 0.0F;
        if (dist <= 1.0e-4F) {
            atten = 1.0F;
        }
        const glm::vec3 toward = dist > 1.0e-4F ? delta / dist : glm::vec3(0.0F, 1.0F, 0.0F);
        if (useNormal) {
            atten *= std::max(0.0F, glm::dot(normal, toward));
        }
        if (light.spot) {
            const float cone = glm::dot(-toward, light.direction);
            const float outer = std::min(light.stopDot, light.stopDot2);
            const float inner = std::max(light.stopDot, light.stopDot2);
            if (cone <= outer) {
                continue;
            }
            if (inner > outer) {
                atten *= std::clamp((cone - outer) / (inner - outer), 0.0F, 1.0F);
            }
        }
        glm::vec3 contribution = light.color * std::min(atten, 4.0F);
        const float peak = std::max(contribution.r, std::max(contribution.g, contribution.b));
        if (peak > 1.25F) {
            contribution *= 1.25F / peak;
        }
        color += contribution;
    }
    color += ambient;
    return glm::clamp(color, glm::vec3(0.0F), glm::vec3(1.0F));
}

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
constexpr std::size_t kVisibilityLump = 4;
constexpr std::size_t kNodeLump = 5;
constexpr std::size_t kNodeSize = 32;
constexpr std::size_t kLeafLump = 10;
constexpr std::size_t kLeafFaceLump = 16;
constexpr std::size_t kAreaLump = 20;
constexpr std::size_t kAreaPortalLump = 21;
constexpr std::size_t kClipPortalVertLump = 41;
constexpr std::size_t kCubemapLump = 42;
constexpr std::size_t kOverlayLump = 45;
constexpr std::size_t kWorldLightLump = 15;
constexpr std::size_t kLeafWaterLump = 36;
constexpr std::size_t kWorldLightSize = 88;
constexpr std::size_t kOverlayLumpSize = 352;
constexpr std::size_t kDispInfoLump = 26;
constexpr std::size_t kDispVertLump = 33;
constexpr std::size_t kDispTriLump = 48;
constexpr std::size_t kEntitiesLump = 0;
constexpr std::size_t kPlaneLump = 1;
constexpr std::size_t kPlaneSize = 20;
constexpr std::uint32_t kSurfNodraw = 0x0080U;
constexpr std::uint32_t kSurfNodecal = 0x2000U;
constexpr std::size_t kModelLump = 14;
constexpr std::size_t kModelSize = 48;
constexpr std::size_t kDispVertSize = 20;
constexpr std::uint32_t kSurfSky2D = 0x0002U;
constexpr std::uint32_t kSurfSky = 0x0004U;
constexpr std::uint32_t kSurfWarp = 0x0008U;
constexpr std::uint32_t kSurfTrigger = 0x0040U;
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
    std::int32_t version = 0;
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
    int stylePages = 1;
    bool lit = false;
};

struct FacePolygon {
    std::vector<Point> points;
    std::vector<Point> sourcePoints;
    std::vector<std::array<float, 2>> uvs;
    std::vector<std::array<float, 2>> lightmapUvs;
    std::vector<unsigned int> gridIndices;
    std::string material;
    FaceLightmap lightmap;
    int modelIndex = 0;
    int faceIndex = -1;
    float styleIndex = 0.0F;
    std::uint32_t texFlags = 0;
    bool sky = false;
    bool skyPortal = false;
    glm::vec3 planeNormal{0.0F, 1.0F, 0.0F};
    Point planeAnchor{};
};

struct FaceSpan {
    int faceIndex = -1;
    std::uint32_t firstIndex = 0;
    std::uint32_t indexCount = 0;
};

struct BatchGeometry {
    std::string material;
    std::vector<float> vertices;
    std::vector<unsigned int> indices;
    std::vector<FaceSpan> faceSpans;
    bool trigger = false;
    bool hidden = false;
    bool collide = true;
    bool warp = false;
};

float PointComponent(const Point& point, int axis) {
    if (axis == 0) {
        return point.x;
    }
    if (axis == 1) {
        return point.y;
    }
    return point.z;
}

float Cross2D(float ax, float ay, float bx, float by, float cx, float cy) {
    return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
}

bool PointInTriangle2D(
    float px,
    float py,
    float ax,
    float ay,
    float bx,
    float by,
    float cx,
    float cy
) {
    constexpr float kEdge = -1.0e-5F;
    const float ab = Cross2D(ax, ay, bx, by, px, py);
    const float bc = Cross2D(bx, by, cx, cy, px, py);
    const float ca = Cross2D(cx, cy, ax, ay, px, py);
    return ab >= kEdge && bc >= kEdge && ca >= kEdge;
}

std::vector<unsigned int> EarClipPolygon(const std::vector<Point>& points) {
    const std::size_t count = points.size();
    if (count < 3 || count > 256) {
        return {};
    }
    glm::vec3 normal(0.0F);
    for (std::size_t index = 0; index < count; ++index) {
        const Point& current = points[index];
        const Point& next = points[(index + 1) % count];
        normal.x += (current.y - next.y) * (current.z + next.z);
        normal.y += (current.z - next.z) * (current.x + next.x);
        normal.z += (current.x - next.x) * (current.y + next.y);
    }
    if (glm::dot(normal, normal) < 1.0e-12F) {
        return {};
    }
    int axis = 0;
    if (std::abs(normal.y) > std::abs(normal[axis])) {
        axis = 1;
    }
    if (std::abs(normal.z) > std::abs(normal[axis])) {
        axis = 2;
    }
    const int uAxis = (axis + 1) % 3;
    const int vAxis = (axis + 2) % 3;
    const float flip = normal[axis] >= 0.0F ? 1.0F : -1.0F;

    struct Vertex2D {
        float x = 0.0F;
        float y = 0.0F;
        unsigned int index = 0;
    };
    std::vector<Vertex2D> ring;
    ring.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const float x = PointComponent(points[index], uAxis) * flip;
        const float y = PointComponent(points[index], vAxis);
        if (!ring.empty()) {
            const float dx = x - ring.back().x;
            const float dy = y - ring.back().y;
            if (dx * dx + dy * dy < 1.0e-10F) {
                continue;
            }
        }
        ring.push_back(Vertex2D{x, y, static_cast<unsigned int>(index)});
    }
    if (ring.size() >= 2) {
        const float dx = ring.front().x - ring.back().x;
        const float dy = ring.front().y - ring.back().y;
        if (dx * dx + dy * dy < 1.0e-10F) {
            ring.pop_back();
        }
    }
    if (ring.size() < 3) {
        return {};
    }

    bool removed = true;
    while (removed && ring.size() > 3) {
        removed = false;
        for (std::size_t index = 0; index < ring.size(); ++index) {
            const Vertex2D& previous = ring[(index + ring.size() - 1) % ring.size()];
            const Vertex2D& current = ring[index];
            const Vertex2D& next = ring[(index + 1) % ring.size()];
            const float turn = Cross2D(previous.x, previous.y, current.x, current.y, next.x, next.y);
            if (std::abs(turn) > 1.0e-6F) {
                continue;
            }
            ring.erase(ring.begin() + static_cast<std::ptrdiff_t>(index));
            removed = true;
            break;
        }
    }
    if (ring.size() < 3) {
        return {};
    }

    std::vector<unsigned int> indices;
    indices.reserve((ring.size() - 2) * 3);
    int guard = static_cast<int>(ring.size() * ring.size());
    while (ring.size() > 3 && guard-- > 0) {
        bool clipped = false;
        for (std::size_t index = 0; index < ring.size(); ++index) {
            const std::size_t previousIndex = (index + ring.size() - 1) % ring.size();
            const std::size_t nextIndex = (index + 1) % ring.size();
            const Vertex2D& previous = ring[previousIndex];
            const Vertex2D& current = ring[index];
            const Vertex2D& next = ring[nextIndex];
            const float turn = Cross2D(previous.x, previous.y, current.x, current.y, next.x, next.y);
            if (turn <= 1.0e-6F) {
                continue;
            }
            bool blocked = false;
            for (std::size_t other = 0; other < ring.size(); ++other) {
                if (other == previousIndex || other == index || other == nextIndex) {
                    continue;
                }
                const Vertex2D& candidate = ring[other];
                if (PointInTriangle2D(
                        candidate.x,
                        candidate.y,
                        previous.x,
                        previous.y,
                        current.x,
                        current.y,
                        next.x,
                        next.y
                    )) {
                    blocked = true;
                    break;
                }
            }
            if (blocked) {
                continue;
            }
            indices.push_back(previous.index);
            indices.push_back(current.index);
            indices.push_back(next.index);
            ring.erase(ring.begin() + static_cast<std::ptrdiff_t>(index));
            clipped = true;
            break;
        }
        if (!clipped) {
            return {};
        }
    }
    if (ring.size() != 3) {
        return {};
    }
    indices.push_back(ring[0].index);
    indices.push_back(ring[1].index);
    indices.push_back(ring[2].index);
    return indices;
}

bool FaceCollides(const std::string& lowercaseMaterial, std::uint32_t texFlags) {
    if ((texFlags & (kSurfSky | kSurfSky2D | kSurfTrigger | kSurfWarp)) != 0) {
        return false;
    }
    static constexpr std::array<const char*, 7> blocked = {
        "trigger",
        "toolshint",
        "toolsskip",
        "toolsblocklight",
        "toolsorigin",
        "toolsnpcclip",
        "invisibleladder",
    };
    for (const char* token : blocked) {
        if (lowercaseMaterial.find(token) != std::string::npos) {
            return false;
        }
    }
    return true;
}

struct LightmapAtlas {
    int width = 0;
    int height = 0;
    std::vector<float> rgb;
    std::vector<float> rgbStyle;
};

struct DecalSpawn {
    glm::vec3 position{0.0F};
    glm::vec3 normal{0.0F, 1.0F, 0.0F};
    glm::vec3 sAxis{1.0F, 0.0F, 0.0F};
    glm::vec3 tAxis{0.0F, 0.0F, 1.0F};
    float halfWidth = 1.0F;
    float halfHeight = 1.0F;
    std::string material;
    int faceIndex = -1;
    bool explicitQuad = false;
    glm::vec3 basisOrigin{0.0F};
    glm::vec3 basisU{1.0F, 0.0F, 0.0F};
    glm::vec3 basisV{0.0F, 1.0F, 0.0F};
    std::array<std::array<float, 2>, 4> uvs{{{0.0F, 0.0F}, {1.0F, 0.0F}, {1.0F, 1.0F}, {0.0F, 1.0F}}};
    std::vector<int> sideFaces;
    std::vector<float> meshVertices;
    std::vector<unsigned int> meshIndices;
};

struct SpriteSpawn {
    glm::vec3 position{0.0F};
    glm::vec3 color{1.0F};
    float alpha = 1.0F;
    float halfSize = 0.25F;
    bool additive = false;
    std::string material;
    int leaf = -1;
};

struct LoadedBrush {
    std::vector<BatchGeometry> batches;
    glm::vec3 aabbMin{1.0e9F};
    glm::vec3 aabbMax{-1.0e9F};
};

struct LoadedGeometry {
    std::vector<BatchGeometry> batches;
    std::vector<LoadedBrush> brushes;
    LightmapAtlas lightmap;
    std::vector<glm::vec3> propPositions;
    std::vector<BspMapEntity> mapEntities;
    std::vector<BspMapLight> mapLights;
    BspMapLight environmentLight{};
    bool hasEnvironmentLight = false;
    std::string skyName;
    BspSkyCamera skyCamera{};
    BspFog worldFog{};
    std::array<std::string, 64> lightStyles{};
    std::vector<BatchGeometry> skyBatches;
    int displacementCount = 0;
    std::vector<DecalSpawn> decals;
    std::vector<SpriteSpawn> sprites;
    int staticPropCount = 0;
    glm::vec3 playerStartPosition{0.0F};
    bool hasPlayerStartPosition = false;
    BspVisibility visibility;
    std::vector<CubemapSample> cubemaps;
    std::vector<FillLight> fillLights;
};

struct MaterialTextureContext {
    const std::vector<std::filesystem::path>& textureRoots;
    const std::vector<std::filesystem::path>& vpkArchives;
    GLuint* placeholderTexture = nullptr;
    const GameFileSystem* files = nullptr;
};

struct LoadedMaterialTexture {
    GLuint texture = 0;
    bool ownsTexture = true;
    int alphaMode = 0;
    bool envmap = false;
    glm::vec3 envmapTint{1.0F};
};

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
        0.0F,
    });
}

BatchGeometry BuildDecalGeometry(const DecalSpawn& spawn) {
    BatchGeometry geometry;
    geometry.material = spawn.material;
    geometry.vertices = spawn.meshVertices;
    geometry.indices = spawn.meshIndices;
    return geometry;
}

std::string MaterialToken(const std::string& text, const std::string& lowered, std::string_view key) {
    std::size_t search = 0;
    while (search < lowered.size()) {
        const std::size_t found = lowered.find(key, search);
        if (found == std::string::npos) {
            return {};
        }
        std::size_t cursor = found + key.size();
        if (cursor < lowered.size()) {
            const unsigned char next = static_cast<unsigned char>(lowered[cursor]);
            if (std::isalnum(next) != 0 || lowered[cursor] == '_') {
                search = cursor;
                continue;
            }
        }
        if (cursor < text.size() && text[cursor] == '"') {
            ++cursor;
        }
        while (cursor < text.size() && std::isspace(static_cast<unsigned char>(text[cursor])) != 0) {
            ++cursor;
        }
        if (cursor >= text.size()) {
            return {};
        }
        if (text[cursor] == '"') {
            ++cursor;
            const std::size_t close = text.find('"', cursor);
            if (close == std::string::npos) {
                return {};
            }
            return text.substr(cursor, close - cursor);
        }
        const std::size_t start = cursor;
        while (cursor < text.size()) {
            const unsigned char character = static_cast<unsigned char>(text[cursor]);
            if (std::isspace(character) != 0 || text[cursor] == '"' || text[cursor] == '{' || text[cursor] == '}') {
                break;
            }
            ++cursor;
        }
        if (cursor == start) {
            search = cursor + 1;
            continue;
        }
        return text.substr(start, cursor - start);
    }
    return {};
}

bool MaterialFlagEnabled(const std::string& text, const std::string& lowered, const char* key) {
    const std::string value = MaterialToken(text, lowered, key);
    return value == "1" || value == "1.0" || value == "true";
}

std::string MaterialKeyValue(const std::string& text, const std::string& lowered, std::string_view key) {
    return MaterialToken(text, lowered, key);
}

std::string StripMaterialComments(std::string text) {
    std::string clean;
    clean.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] == '/' && index + 1 < text.size() && text[index + 1] == '/') {
            while (index < text.size() && text[index] != '\n') {
                ++index;
            }
            continue;
        }
        clean.push_back(text[index]);
    }
    return clean;
}

std::string CubemapBaseMaterial(const std::string& material) {
    if (material.rfind("maps/", 0) != 0) {
        return {};
    }
    const std::size_t mapSlash = material.find('/', 5);
    if (mapSlash == std::string::npos || mapSlash + 1 >= material.size()) {
        return {};
    }
    std::string rest = material.substr(mapSlash + 1);
    for (int group = 0; group < 3; ++group) {
        const std::size_t underscore = rest.rfind('_');
        if (underscore == std::string::npos || underscore + 1 >= rest.size()) {
            return {};
        }
        const std::string token = rest.substr(underscore + 1);
        std::size_t index = token.front() == '-' ? 1 : 0;
        if (index >= token.size()) {
            return {};
        }
        for (; index < token.size(); ++index) {
            if (std::isdigit(static_cast<unsigned char>(token[index])) == 0) {
                return {};
            }
        }
        rest.resize(underscore);
    }
    return rest;
}

std::string NormalizeTexturePath(std::string path) {
    for (char& character : path) {
        if (character == '\\') {
            character = '/';
        }
    }
    while (!path.empty() && path.front() == '/') {
        path.erase(path.begin());
    }
    if (path.rfind("materials/", 0) == 0) {
        path = path.substr(10);
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
    return path;
}

std::string BaseTextureFromMaterial(
    const GameFileSystem* files,
    const std::string& material,
    int depth
) {
    if (files == nullptr || material.empty() || depth > 4) {
        return NormalizeTexturePath(material);
    }
    std::string path = NormalizeTexturePath(material);
    std::vector<std::uint8_t> vmt;
    if (!files->Read("materials/" + path + ".vmt", vmt) && !files->Read(path + ".vmt", vmt)) {
        const std::string cubemapBase = CubemapBaseMaterial(path);
        if (!cubemapBase.empty() && cubemapBase != path) {
            return BaseTextureFromMaterial(files, cubemapBase, depth + 1);
        }
        return path;
    }
    const std::string text = StripMaterialComments(std::string(vmt.begin(), vmt.end()));
    std::string lowered = text;
    for (char& character : lowered) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    const auto usableTexture = [](const std::string& value) {
        if (value.empty() || value == "0" || value == "env_cubemap") {
            return false;
        }
        std::string lowered = value;
        for (char& character : lowered) {
            character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
        }
        return lowered.rfind("_rt_", 0) != 0;
    };
    for (const char* key : {"$basetexture", "$basetexture2", "$normalmap", "$bumpmap", "$tooltexture", "$bottommaterial"}) {
        const std::string value = MaterialKeyValue(text, lowered, key);
        if (usableTexture(value)) {
            return NormalizeTexturePath(value);
        }
    }
    const std::string include = MaterialKeyValue(text, lowered, "include");
    if (!include.empty()) {
        return BaseTextureFromMaterial(files, include, depth + 1);
    }
    return path;
}

float DecalScaleFromMaterial(const GameFileSystem* files, const std::string& material, int depth) {
    if (files == nullptr || material.empty() || depth > 4) {
        return 1.0F;
    }
    const std::string path = NormalizeTexturePath(material);
    std::vector<std::uint8_t> vmt;
    if (!files->Read("materials/" + path + ".vmt", vmt) && !files->Read(path + ".vmt", vmt)) {
        return 1.0F;
    }
    const std::string text = StripMaterialComments(std::string(vmt.begin(), vmt.end()));
    std::string lowered = text;
    for (char& character : lowered) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    const std::string scaleText = MaterialToken(text, lowered, "$decalscale");
    if (!scaleText.empty()) {
        try {
            const float scale = std::stof(scaleText);
            if (scale > 0.0F) {
                return scale;
            }
        } catch (const std::exception&) {
        }
    }
    const std::string include = MaterialToken(text, lowered, "include");
    if (!include.empty()) {
        return DecalScaleFromMaterial(files, include, depth + 1);
    }
    return 1.0F;
}

bool VtfPixelSize(const std::vector<std::uint8_t>& data, int& width, int& height) {
    if (data.size() < 20 || data[0] != 'V' || data[1] != 'T' || data[2] != 'F' || data[3] != '\0') {
        return false;
    }
    width = static_cast<int>(data[16]) | (static_cast<int>(data[17]) << 8);
    height = static_cast<int>(data[18]) | (static_cast<int>(data[19]) << 8);
    return width > 0 && height > 0 && width <= 8192 && height <= 8192;
}

void DecalHalfExtents(
    const GameFileSystem* files,
    const std::string& material,
    float& halfWidth,
    float& halfHeight
) {
    const float decalScale = DecalScaleFromMaterial(files, material, 0);
    int width = 64;
    int height = 64;
    if (files != nullptr) {
        std::string vtf = BaseTextureFromMaterial(files, material, 0);
        if (vtf.rfind("materials/", 0) != 0) {
            vtf = "materials/" + vtf;
        }
        if (vtf.size() < 4 || vtf.substr(vtf.size() - 4) != ".vtf") {
            std::string extension = vtf.size() >= 4 ? vtf.substr(vtf.size() - 4) : std::string();
            for (char& character : extension) {
                character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
            }
            if (extension != ".vtf") {
                vtf += ".vtf";
            }
        }
        std::vector<std::uint8_t> bytes;
        int textureWidth = 0;
        int textureHeight = 0;
        if (files->Read(vtf, bytes) && VtfPixelSize(bytes, textureWidth, textureHeight)) {
            width = textureWidth;
            height = textureHeight;
        }
    }
    halfWidth = std::max(static_cast<float>(width) * decalScale * kSourceToWorld * 0.5F, 0.01F);
    halfHeight = std::max(static_cast<float>(height) * decalScale * kSourceToWorld * 0.5F, 0.01F);
}

int MaterialAlphaMode(const std::string& text) {
    std::string lowered = text;
    for (char& character : lowered) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    if (MaterialFlagEnabled(text, lowered, "$additive")) {
        return 3;
    }
    if (MaterialFlagEnabled(text, lowered, "$translucent")) {
        return 1;
    }
    if (MaterialFlagEnabled(text, lowered, "$alphatest")) {
        return 2;
    }
    const std::string alpha = MaterialToken(text, lowered, "$alpha");
    if (!alpha.empty()) {
        try {
            if (std::stof(alpha) < 0.99F) {
                return 1;
            }
        } catch (const std::exception&) {
        }
    }
    return 0;
}

glm::vec3 ParseTintColor(const std::string& text) {
    std::string cleaned;
    cleaned.reserve(text.size());
    for (const char character : text) {
        const bool keep = (character >= '0' && character <= '9') || character == '.' || character == '-'
            || std::isspace(static_cast<unsigned char>(character)) != 0;
        if (keep) {
            cleaned.push_back(character);
        }
    }
    std::istringstream stream(cleaned);
    float red = 1.0F;
    float green = 1.0F;
    float blue = 1.0F;
    if (!(stream >> red >> green >> blue)) {
        return glm::vec3(1.0F);
    }
    glm::vec3 color(red, green, blue);
    if (color.r > 1.0F || color.g > 1.0F || color.b > 1.0F) {
        color /= 255.0F;
    }
    return glm::clamp(color, glm::vec3(0.0F), glm::vec3(4.0F));
}

void ReadMaterialShading(const GameFileSystem* files, const std::string& material, LoadedMaterialTexture& loaded, int depth) {
    if (files == nullptr || material.empty() || depth > 4) {
        return;
    }
    const std::string path = NormalizeTexturePath(material);
    std::vector<std::uint8_t> vmt;
    if (!files->Read("materials/" + path + ".vmt", vmt) && !files->Read(path + ".vmt", vmt)) {
        return;
    }
    const std::string text = StripMaterialComments(std::string(vmt.begin(), vmt.end()));
    std::string lowered = text;
    for (char& character : lowered) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    const std::string envmap = MaterialToken(text, lowered, "$envmap");
    if (!envmap.empty()) {
        std::string envLower = envmap;
        for (char& character : envLower) {
            character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
        }
        if (envLower != "0" && envLower != "off") {
            loaded.envmap = true;
        }
    }
    const std::string tint = MaterialToken(text, lowered, "$envmaptint");
    if (!tint.empty()) {
        loaded.envmapTint = ParseTintColor(tint);
    }
    if (!loaded.envmap) {
        const std::string include = MaterialToken(text, lowered, "include");
        if (!include.empty()) {
            ReadMaterialShading(files, include, loaded, depth + 1);
        }
    }
}

int ClassifyMaterial(const GameFileSystem* files, const std::string& material, int depth) {
    if (files == nullptr || material.empty() || depth > 4) {
        return 0;
    }
    const std::string path = NormalizeTexturePath(material);
    std::string loweredPath = path;
    for (char& character : loweredPath) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    if (loweredPath.find("glass") != std::string::npos
        || (loweredPath.find("window") != std::string::npos && loweredPath.find("frame") == std::string::npos)) {
        return 1;
    }
    std::vector<std::uint8_t> vmt;
    if (!files->Read("materials/" + path + ".vmt", vmt) && !files->Read(path + ".vmt", vmt)) {
        return 0;
    }
    const std::string text = StripMaterialComments(std::string(vmt.begin(), vmt.end()));
    std::string lowered = text;
    for (char& character : lowered) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    const std::size_t shaderStart = text.find('"');
    if (shaderStart != std::string::npos) {
        const std::size_t shaderEnd = text.find('"', shaderStart + 1);
        if (shaderEnd != std::string::npos) {
            const std::string shader = lowered.substr(shaderStart + 1, shaderEnd - shaderStart - 1);
            if (shader == "decalmodulate") {
                return 4;
            }
            if (shader == "refract" || shader == "water") {
                return 1;
            }
        }
    }
    const std::string surface = MaterialToken(text, lowered, "$surfaceprop");
    std::string loweredSurface = surface;
    for (char& character : loweredSurface) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    if (loweredSurface == "glass") {
        return 1;
    }
    const int mode = MaterialAlphaMode(text);
    if (mode != 0) {
        return mode;
    }
    if (MaterialFlagEnabled(text, lowered, "$decal")) {
        return 1;
    }
    const std::string include = MaterialToken(text, lowered, "include");
    if (!include.empty()) {
        return ClassifyMaterial(files, include, depth + 1);
    }
    return 0;
}

LoadedMaterialTexture LoadMaterialTexture(
    const std::string& material,
    MaterialTextureContext& context
) {
    LoadedMaterialTexture loaded;
    loaded.alphaMode = ClassifyMaterial(context.files, material, 0);
    ReadMaterialShading(context.files, material, loaded, 0);
    if (material.empty()) {
        return loaded;
    }
    if (context.files != nullptr) {
        const std::string base = BaseTextureFromMaterial(context.files, material, 0);
        std::string vtf = base;
        if (vtf.rfind("materials/", 0) != 0) {
            vtf = "materials/" + vtf;
        }
        if (vtf.size() < 4 || vtf.substr(vtf.size() - 4) != ".vtf") {
            std::string extension = vtf.size() >= 4 ? vtf.substr(vtf.size() - 4) : std::string();
            for (char& character : extension) {
                character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
            }
            if (extension != ".vtf") {
                vtf += ".vtf";
            }
        }
        std::vector<std::uint8_t> bytes;
        if (context.files->Read(vtf, bytes)) {
            try {
                loaded.texture = vtf::LoadVtfTextureData(bytes, vtf);
                loaded.ownsTexture = true;
                return loaded;
            } catch (const std::exception& error) {
                std::cerr << "Skipping texture " << vtf << ": " << error.what() << '\n';
            }
        }
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
        if (*context.placeholderTexture == 0) {
            *context.placeholderTexture = vtf::CreateMissingTexture();
        }
        loaded.texture = *context.placeholderTexture;
        loaded.ownsTexture = false;
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

std::string EntityKeyValueCi(
    const std::vector<std::pair<std::string, std::string>>& properties,
    const char* key
) {
    const std::string wanted(key);
    for (const auto& entry : properties) {
        if (entry.first.size() != wanted.size()) {
            continue;
        }
        bool match = true;
        for (std::size_t index = 0; index < wanted.size(); ++index) {
            const auto left = static_cast<unsigned char>(entry.first[index]);
            const auto right = static_cast<unsigned char>(wanted[index]);
            if (std::tolower(left) != std::tolower(right)) {
                match = false;
                break;
            }
        }
        if (match) {
            return entry.second;
        }
    }
    return {};
}

bool SameEntityName(const std::string& left, const std::string& right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto a = static_cast<unsigned char>(left[index]);
        const auto b = static_cast<unsigned char>(right[index]);
        if (std::tolower(a) != std::tolower(b)) {
            return false;
        }
    }
    return true;
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

float PropertyFloat(
    const std::vector<std::pair<std::string, std::string>>& properties,
    const char* key,
    float fallback
) {
    const std::string value = EntityKeyValue(properties, key);
    if (value.empty()) {
        return fallback;
    }
    try {
        return std::stof(value);
    } catch (const std::exception&) {
        return fallback;
    }
}

bool ParseLightColor(const std::string& text, glm::vec3& color, float& brightness) {
    if (text.empty()) {
        return false;
    }
    std::istringstream stream(text);
    float red = 0.0F;
    float green = 0.0F;
    float blue = 0.0F;
    float intensity = -1.0F;
    stream >> red >> green >> blue;
    if (stream.fail()) {
        return false;
    }
    if (!(stream >> intensity)) {
        intensity = std::max(red, std::max(green, blue));
    }
    color = glm::vec3(red, green, blue) / 255.0F;
    const float maxChannel = std::max(color.r, std::max(color.g, color.b));
    if (maxChannel > 1.0F) {
        color /= maxChannel;
        intensity *= maxChannel;
    }
    color = glm::clamp(color, glm::vec3(0.0F), glm::vec3(1.0F));
    brightness = std::max(intensity, 0.0F);
    return true;
}

bool PropertyVec3(
    const std::vector<std::pair<std::string, std::string>>& properties,
    const char* key,
    glm::vec3& out,
    bool asPoint
) {
    const std::string value = EntityKeyValue(properties, key);
    if (value.empty()) {
        return false;
    }
    std::istringstream stream(value);
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
    stream >> x >> y >> z;
    if (stream.fail()) {
        return false;
    }
    out = asPoint ? SourcePointToWorld(x, y, z) : SourceDirectionToWorld(x, y, z);
    return true;
}

bool PropertyUv(const std::string& text, float& u, float& v) {
    if (text.empty()) {
        return false;
    }
    std::istringstream stream(text);
    stream >> u >> v;
    return !stream.fail();
}

void ApplyLightRange(BspMapLight& light, float rangeInches) {
    const float range = std::max(rangeInches * kSourceToWorld, 0.05F);
    light.constant = 1.0F;
    light.linear = 0.0F;
    light.quadratic = 1.0F / (range * range);
}

float LightRangeInches(
    const std::vector<std::pair<std::string, std::string>>& properties,
    float brightness,
    bool dynamic
) {
    const float distance = PropertyFloat(properties, "distance", 0.0F);
    const float zero = PropertyFloat(properties, "_zero_percent_distance", 0.0F);
    const float fifty = PropertyFloat(properties, "_fifty_percent_distance", 0.0F);
    if (dynamic && distance > 0.0F) {
        return distance;
    }
    if (zero > 0.0F) {
        return zero;
    }
    if (fifty > 0.0F) {
        return fifty * 2.0F;
    }
    if (distance > 0.0F) {
        return distance;
    }
    const float estimated = std::sqrt(std::max(brightness, 1.0F)) * 32.0F;
    return std::clamp(estimated, 64.0F, 1536.0F);
}

bool TryBuildMapLight(
    const std::string& name,
    const std::vector<std::pair<std::string, std::string>>& properties,
    const glm::vec3& position,
    bool hasOrigin,
    BspMapLight& out
) {
    const bool environment = name == "light_environment";
    const bool dynamic = name == "light_dynamic";
    const bool spotClass = name == "light_spot";
    if (!environment && !dynamic && !spotClass && name != "light") {
        return false;
    }
    if (!environment && !hasOrigin) {
        return false;
    }

    glm::vec3 color(1.0F);
    float brightness = 0.0F;
    const bool hasLight = ParseLightColor(EntityKeyValue(properties, "_light"), color, brightness);
    if (!hasLight) {
        brightness = PropertyFloat(properties, "brightness", 0.0F);
        color = glm::vec3(1.0F);
    } else if (dynamic) {
        const float brightnessKey = PropertyFloat(properties, "brightness", -1.0F);
        if (brightnessKey >= 0.0F) {
            brightness = brightnessKey;
        }
    }

    glm::vec3 ambient(0.0F);
    float ambientBrightness = 0.0F;
    if (environment) {
        glm::vec3 ambientColor(0.0F);
        if (ParseLightColor(EntityKeyValue(properties, "_ambient"), ambientColor, ambientBrightness)) {
            ambient = ambientColor * (ambientBrightness / 100.0F);
        }
    }
    const float intensity = std::clamp(brightness / 100.0F, 0.0F, 64.0F);
    if (intensity <= 0.001F && glm::dot(ambient, ambient) <= 1.0e-8F) {
        return false;
    }

    float pitch = 0.0F;
    float yaw = 0.0F;
    float roll = 0.0F;
    static_cast<void>(ParseEntityAngles(properties, pitch, yaw, roll));
    if (environment) {
        const std::string pitchText = EntityKeyValue(properties, "pitch");
        if (!pitchText.empty()) {
            try {
                pitch = -std::stof(pitchText);
            } catch (const std::exception&) {
            }
        }
    }
    glm::vec3 forward(0.0F, -1.0F, 0.0F);
    glm::vec3 right(1.0F, 0.0F, 0.0F);
    glm::vec3 up(0.0F, 0.0F, 1.0F);
    SourceAngleBasis(pitch, yaw, roll, forward, right, up);

    const float spotRadius = PropertyFloat(properties, "spotlight_radius", 0.0F);
    const bool spot = spotClass || spotRadius > 0.0F;
    const float rangeInches = LightRangeInches(properties, brightness, dynamic);

    out = {};
    out.position = position;
    out.color = color;
    out.ambient = ambient;
    out.intensity = intensity;
    out.dynamic = dynamic;
    ApplyLightRange(out, rangeInches);
    if (environment) {
        out.kind = BspLightKind::Sun;
        const glm::vec3 towardSun = -forward;
        out.direction = glm::length(towardSun) > 1.0e-6F ? glm::normalize(towardSun) : glm::vec3(0.0F, 1.0F, 0.0F);
        return true;
    }
    if (spot) {
        out.kind = BspLightKind::Spot;
        out.direction = glm::length(forward) > 1.0e-6F ? glm::normalize(forward) : glm::vec3(0.0F, -1.0F, 0.0F);
        float outerDegrees = PropertyFloat(properties, "_cone", 0.0F);
        if (outerDegrees <= 0.0F && spotRadius > 0.0F && rangeInches > 1.0F) {
            outerDegrees = std::atan(spotRadius / rangeInches) * (180.0F / 3.14159265F);
        }
        if (outerDegrees <= 0.0F) {
            outerDegrees = 45.0F;
        }
        outerDegrees = std::clamp(outerDegrees, 1.0F, 89.0F);
        float innerDegrees = PropertyFloat(properties, "_inner_cone", outerDegrees * 0.6F);
        innerDegrees = std::clamp(innerDegrees, 0.0F, outerDegrees - 0.5F);
        out.outerCutOff = std::cos(outerDegrees * (3.14159265F / 180.0F));
        out.innerCutOff = std::cos(innerDegrees * (3.14159265F / 180.0F));
        return true;
    }
    out.kind = BspLightKind::Point;
    return true;
}

bool TryBuildOverlay(
    const std::vector<std::pair<std::string, std::string>>& properties,
    const glm::vec3& fallbackPosition,
    DecalSpawn& decal
) {
    glm::vec3 basisOrigin(0.0F);
    glm::vec3 basisU(0.0F);
    glm::vec3 basisV(0.0F);
    glm::vec3 basisNormal(0.0F);
    if (!PropertyVec3(properties, "BasisOrigin", basisOrigin, true)) {
        basisOrigin = fallbackPosition;
    }
    if (!PropertyVec3(properties, "BasisU", basisU, true) || !PropertyVec3(properties, "BasisV", basisV, true)) {
        return false;
    }
    const bool hasNormal = PropertyVec3(properties, "BasisNormal", basisNormal, false);
    const std::string material = ResolveEntityTexture(properties);
    if (material.empty()) {
        return false;
    }
    glm::vec3 normal = basisNormal;
    if (!hasNormal || glm::length(normal) <= 1.0e-6F) {
        normal = glm::cross(basisU, basisV);
    }
    if (glm::length(normal) <= 1.0e-6F) {
        normal = glm::vec3(0.0F, 1.0F, 0.0F);
    } else {
        normal = glm::normalize(normal);
    }

    float startU = PropertyFloat(properties, "StartU", 0.0F);
    float endU = PropertyFloat(properties, "EndU", 1.0F);
    float startV = PropertyFloat(properties, "StartV", 0.0F);
    float endV = PropertyFloat(properties, "EndV", 1.0F);
    decal = {};
    decal.explicitQuad = true;
    decal.material = material;
    decal.normal = normal;
    decal.basisOrigin = basisOrigin;
    decal.basisU = basisU;
    decal.basisV = basisV;
    decal.position = basisOrigin + (basisU + basisV) * 0.5F;
    const float uLength = glm::length(basisU);
    const float vLength = glm::length(basisV);
    decal.halfWidth = std::max(uLength * 0.5F, 0.01F);
    decal.halfHeight = std::max(vLength * 0.5F, 0.01F);
    if (uLength > 1.0e-6F) {
        decal.sAxis = basisU / uLength;
    }
    if (vLength > 1.0e-6F) {
        decal.tAxis = basisV / vLength;
    }
    decal.uvs = {{{startU, startV}, {endU, startV}, {endU, endV}, {startU, endV}}};
    for (int corner = 0; corner < 4; ++corner) {
        float u = 0.0F;
        float v = 0.0F;
        const std::string key = "uv" + std::to_string(corner);
        if (PropertyUv(EntityKeyValue(properties, key.c_str()), u, v)) {
            decal.uvs[static_cast<std::size_t>(corner)] = {u, v};
        }
    }
    return true;
}

bool TryBuildSprite(
    const std::string& name,
    const std::vector<std::pair<std::string, std::string>>& properties,
    const glm::vec3& position,
    bool hasOrigin,
    SpriteSpawn& sprite
) {
    if (!hasOrigin || (name != "env_sprite" && name != "env_lightglow")) {
        return false;
    }
    std::string material = EntityKeyValue(properties, "model");
    if (material.empty()) {
        material = ResolveEntityTexture(properties);
    }
    if (material.empty()) {
        return false;
    }
    glm::vec3 color(1.0F);
    float ignored = 0.0F;
    static_cast<void>(ParseLightColor(EntityKeyValue(properties, "rendercolor"), color, ignored));
    const float renderAmount = std::clamp(PropertyFloat(properties, "renderamt", 255.0F), 0.0F, 255.0F);
    const int renderMode = static_cast<int>(PropertyFloat(properties, "rendermode", name == "env_lightglow" ? 5.0F : 0.0F));
    const bool alphaBlend = renderMode == 1 || renderMode == 2 || renderMode == 4;
    float halfSource = 16.0F;
    if (name == "env_lightglow") {
        const float glow = std::max(
            PropertyFloat(properties, "HorizontalGlowSize", 0.0F),
            PropertyFloat(properties, "VerticalGlowSize", 0.0F)
        );
        if (glow > 0.0F) {
            halfSource = glow * 0.5F;
        }
    }
    float scale = PropertyFloat(properties, "scale", PropertyFloat(properties, "Scale", 1.0F));
    if (scale <= 0.0F) {
        scale = 1.0F;
    }
    sprite.position = position;
    sprite.color = color;
    sprite.alpha = renderAmount / 255.0F;
    sprite.halfSize = std::max(halfSource * scale * kSourceToWorld, 0.02F);
    sprite.additive = !alphaBlend && (
        name == "env_lightglow"
        || renderMode == 0
        || renderMode == 3
        || renderMode == 5
        || renderMode == 7
        || renderMode == 8
        || renderMode == 9
    );
    sprite.material = material;
    return sprite.alpha > 0.001F;
}

constexpr std::size_t kGameLump = 35;
constexpr std::uint32_t kStaticPropLumpId = 0x73707270;
constexpr std::uint32_t kDetailPropLumpId = 0x70727064;

glm::vec3 DecodeColorExp(std::uint8_t red, std::uint8_t green, std::uint8_t blue, std::uint8_t exponent) {
    const float scale = std::ldexp(1.0F, static_cast<int>(static_cast<std::int8_t>(exponent))) / 255.0F;
    return glm::vec3(red, green, blue) * scale;
}

std::uint32_t ReadU32(const std::vector<std::uint8_t>& data, std::size_t offset);
std::int32_t ReadI32(const std::vector<std::uint8_t>& data, std::size_t offset);
std::uint16_t ReadU16(const std::vector<std::uint8_t>& data, std::size_t offset);
float ReadFloat(const std::vector<std::uint8_t>& data, std::size_t offset);
std::vector<std::uint8_t> ReadLump(
    std::ifstream& file,
    const Lump& lump,
    std::uintmax_t fileSize,
    const char* name
);

int AppendDetailProps(
    const std::vector<std::uint8_t>& gameLump,
    std::ifstream& file,
    std::uintmax_t fileSize,
    std::vector<BspMapEntity>& entities,
    std::vector<SpriteSpawn>& sprites
) {
    if (gameLump.size() < 4) {
        return 0;
    }
    const int lumpCount = ReadI32(gameLump, 0);
    if (lumpCount <= 0 || lumpCount > 64) {
        return 0;
    }
    const std::size_t directoryBytes = static_cast<std::size_t>(lumpCount) * 16;
    if (gameLump.size() < 4 + directoryBytes) {
        return 0;
    }
    int added = 0;
    for (int lumpIndex = 0; lumpIndex < lumpCount; ++lumpIndex) {
        const std::size_t entry = 4 + static_cast<std::size_t>(lumpIndex) * 16;
        const std::uint32_t id = ReadU32(gameLump, entry);
        const std::int32_t fileOffset = ReadI32(gameLump, entry + 8);
        const std::int32_t fileLength = ReadI32(gameLump, entry + 12);
        if (id != kDetailPropLumpId || fileOffset < 0 || fileLength <= 0) {
            continue;
        }
        Lump detailLump;
        detailLump.offset = fileOffset;
        detailLump.length = fileLength;
        std::vector<std::uint8_t> detailData;
        try {
            detailData = ReadLump(file, detailLump, fileSize, "DETAIL_PROPS");
        } catch (const std::exception&) {
            continue;
        }
        if (detailData.size() < 4) {
            continue;
        }
        std::size_t cursor = 0;
        const auto need = [&](std::size_t bytes) {
            return bytes <= detailData.size() && cursor <= detailData.size() - bytes;
        };
        if (!need(4)) {
            continue;
        }
        const int dictCount = ReadI32(detailData, cursor);
        cursor += 4;
        if (dictCount < 0 || dictCount > 4096 || !need(static_cast<std::size_t>(dictCount) * 128)) {
            continue;
        }
        std::vector<std::string> names;
        names.reserve(static_cast<std::size_t>(dictCount));
        for (int dictIndex = 0; dictIndex < dictCount; ++dictIndex) {
            const char* text = reinterpret_cast<const char*>(detailData.data() + cursor);
            std::size_t length = 0;
            while (length < 128 && text[length] != '\0') {
                ++length;
            }
            names.emplace_back(text, length);
            cursor += 128;
        }
        if (!need(4)) {
            continue;
        }
        const int spriteCount = ReadI32(detailData, cursor);
        cursor += 4;
        if (spriteCount < 0 || !need(static_cast<std::size_t>(spriteCount) * 32 + 4)) {
            continue;
        }
        cursor += static_cast<std::size_t>(spriteCount) * 32;
        const int objectCount = ReadI32(detailData, cursor);
        cursor += 4;
        if (objectCount <= 0 || cursor >= detailData.size()) {
            continue;
        }
        const std::size_t remaining = detailData.size() - cursor;
        const int entrySize = static_cast<int>(remaining / static_cast<std::size_t>(objectCount));
        if (entrySize < 28) {
            continue;
        }
        const int count = std::min(objectCount, static_cast<int>(remaining / static_cast<std::size_t>(entrySize)));
        for (int objectIndex = 0; objectIndex < count; ++objectIndex) {
            const std::size_t objectOffset = cursor + static_cast<std::size_t>(objectIndex) * static_cast<std::size_t>(entrySize);
            const int model = static_cast<int>(ReadU16(detailData, objectOffset + 24));
            if (model < 0 || model >= dictCount || names[static_cast<std::size_t>(model)].empty()) {
                continue;
            }
            const std::string& name = names[static_cast<std::size_t>(model)];
            const float originX = ReadFloat(detailData, objectOffset);
            const float originY = ReadFloat(detailData, objectOffset + 4);
            const float originZ = ReadFloat(detailData, objectOffset + 8);
            const float pitch = ReadFloat(detailData, objectOffset + 12);
            const float yaw = ReadFloat(detailData, objectOffset + 16);
            const float roll = ReadFloat(detailData, objectOffset + 20);
            const int leaf = static_cast<int>(ReadU16(detailData, objectOffset + 26));
            const unsigned char orientation = entrySize > 40 ? detailData[objectOffset + 40] : 0;
            float scale = 1.0F;
            if (entrySize >= 48) {
                scale = ReadFloat(detailData, objectOffset + 44);
                if (scale <= 0.0F) {
                    scale = 1.0F;
                }
            }
            glm::vec3 detailLight(0.35F);
            bool hasDetailLight = false;
            if (entrySize >= 32) {
                detailLight = DecodeColorExp(
                    detailData[objectOffset + 28],
                    detailData[objectOffset + 29],
                    detailData[objectOffset + 30],
                    detailData[objectOffset + 31]
                );
                const float peak = std::max(detailLight.r, std::max(detailLight.g, detailLight.b));
                if (peak > 1.0F) {
                    detailLight /= peak;
                }
                hasDetailLight = peak > 0.02F;
            }
            std::string lowered = name;
            for (char& character : lowered) {
                character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
            }
            const bool modelFile = lowered.find(".mdl") != std::string::npos || lowered.rfind("models/", 0) == 0;
            const glm::vec3 position = SourcePointToWorld(originX, originY, originZ);
            if (modelFile && orientation == 0) {
                BspMapEntity entity;
                entity.classname = "prop_static";
                entity.model = name;
                entity.position = position;
                entity.visibilityLeaves.push_back(leaf);
                entity.properties.emplace_back("model", name);
                std::ostringstream angles;
                angles << pitch << ' ' << yaw << ' ' << roll;
                entity.properties.emplace_back("angles", angles.str());
                entity.properties.emplace_back("solid", "0");
                if (hasDetailLight) {
                    entity.lightTint = detailLight;
                    entity.hasLightingOrigin = true;
                }
                entities.push_back(std::move(entity));
                ++added;
                continue;
            }
            SpriteSpawn sprite;
            sprite.position = position;
            sprite.material = name;
            sprite.halfSize = std::max(8.0F * scale * kSourceToWorld, 0.03F);
            sprite.leaf = leaf;
            sprite.additive = false;
            if (hasDetailLight) {
                sprite.color = detailLight;
                const float peak = std::max(sprite.color.r, std::max(sprite.color.g, sprite.color.b));
                if (peak > 1.0F) {
                    sprite.color /= peak;
                }
            }
            sprites.push_back(std::move(sprite));
            ++added;
        }
    }
    return added;
}

int StaticPropEntrySize(int version) {
    switch (version) {
        case 4: return 56;
        case 5: return 60;
        case 6: return 64;
        case 7: return 68;
        case 8: return 72;
        case 9: return 76;
        case 10: return 80;
        case 11: return 84;
        default: return 0;
    }
}

std::uint32_t ReadU32(const std::vector<std::uint8_t>& data, std::size_t offset);
std::int32_t ReadI32(const std::vector<std::uint8_t>& data, std::size_t offset);
std::uint16_t ReadU16(const std::vector<std::uint8_t>& data, std::size_t offset);
float ReadFloat(const std::vector<std::uint8_t>& data, std::size_t offset);
std::vector<std::uint8_t> ReadLump(
    std::ifstream& file,
    const Lump& lump,
    std::uintmax_t fileSize,
    const char* name
);

int AppendStaticProps(
    const std::vector<std::uint8_t>& gameLump,
    std::ifstream& file,
    std::uintmax_t fileSize,
    std::vector<BspMapEntity>& entities
) {
    if (gameLump.size() < 4) {
        return 0;
    }
    const int lumpCount = ReadI32(gameLump, 0);
    if (lumpCount <= 0 || lumpCount > 64) {
        return 0;
    }
    const std::size_t directoryBytes = static_cast<std::size_t>(lumpCount) * 16;
    if (gameLump.size() < 4 + directoryBytes) {
        return 0;
    }
    int added = 0;
    for (int lumpIndex = 0; lumpIndex < lumpCount; ++lumpIndex) {
        const std::size_t entry = 4 + static_cast<std::size_t>(lumpIndex) * 16;
        const std::uint32_t id = ReadU32(gameLump, entry);
        const int version = static_cast<int>(ReadU16(gameLump, entry + 6));
        const std::int32_t fileOffset = ReadI32(gameLump, entry + 8);
        const std::int32_t fileLength = ReadI32(gameLump, entry + 12);
        if (id != kStaticPropLumpId || fileOffset < 0 || fileLength <= 0) {
            continue;
        }
        Lump propLump;
        propLump.offset = fileOffset;
        propLump.length = fileLength;
        std::vector<std::uint8_t> propData;
        try {
            propData = ReadLump(file, propLump, fileSize, "STATIC_PROPS");
        } catch (const std::exception&) {
            continue;
        }
        if (propData.size() < 4) {
            continue;
        }
        std::size_t cursor = 0;
        const auto need = [&](std::size_t bytes) {
            return bytes <= propData.size() && cursor <= propData.size() - bytes;
        };
        if (!need(4)) {
            continue;
        }
        const int dictCount = ReadI32(propData, cursor);
        cursor += 4;
        if (dictCount < 0 || dictCount > 4096 || !need(static_cast<std::size_t>(dictCount) * 128)) {
            continue;
        }
        std::vector<std::string> names;
        names.reserve(static_cast<std::size_t>(dictCount));
        for (int dictIndex = 0; dictIndex < dictCount; ++dictIndex) {
            const char* text = reinterpret_cast<const char*>(propData.data() + cursor);
            std::size_t length = 0;
            while (length < 128 && text[length] != '\0') {
                ++length;
            }
            names.emplace_back(text, length);
            cursor += 128;
        }
        if (!need(4)) {
            continue;
        }
        const int leafCount = ReadI32(propData, cursor);
        cursor += 4;
        if (leafCount < 0 || !need(static_cast<std::size_t>(leafCount) * 2 + 4)) {
            continue;
        }
        std::vector<int> propLeafTable;
        propLeafTable.reserve(static_cast<std::size_t>(leafCount));
        for (int leafIndex = 0; leafIndex < leafCount; ++leafIndex) {
            propLeafTable.push_back(static_cast<int>(ReadU16(propData, cursor)));
            cursor += 2;
        }
        int propCount = ReadI32(propData, cursor);
        cursor += 4;
        if (propCount <= 0) {
            continue;
        }
        int entrySize = StaticPropEntrySize(version);
        if (entrySize <= 0) {
            if (cursor >= propData.size()) {
                continue;
            }
            const std::size_t remaining = propData.size() - cursor;
            entrySize = static_cast<int>(remaining / static_cast<std::size_t>(propCount));
        }
        if (entrySize < 56) {
            continue;
        }
        const int available = static_cast<int>((propData.size() - cursor) / static_cast<std::size_t>(entrySize));
        propCount = std::min(propCount, available);
        for (int propIndex = 0; propIndex < propCount; ++propIndex) {
            const std::size_t propOffset = cursor + static_cast<std::size_t>(propIndex) * static_cast<std::size_t>(entrySize);
            const float originX = ReadFloat(propData, propOffset);
            const float originY = ReadFloat(propData, propOffset + 4);
            const float originZ = ReadFloat(propData, propOffset + 8);
            const float pitch = ReadFloat(propData, propOffset + 12);
            const float yaw = ReadFloat(propData, propOffset + 16);
            const float roll = ReadFloat(propData, propOffset + 20);
            const int propType = static_cast<int>(ReadU16(propData, propOffset + 24));
            const int firstLeaf = static_cast<int>(ReadU16(propData, propOffset + 26));
            const int propLeafCount = static_cast<int>(ReadU16(propData, propOffset + 28));
            const int solid = static_cast<int>(propData[propOffset + 30]);
            if (propType < 0 || propType >= dictCount || names[static_cast<std::size_t>(propType)].empty()) {
                continue;
            }
            BspMapEntity entity;
            entity.classname = "prop_static";
            entity.model = names[static_cast<std::size_t>(propType)];
            entity.position = SourcePointToWorld(originX, originY, originZ);
            if (firstLeaf >= 0 && propLeafCount > 0
                && static_cast<std::size_t>(firstLeaf) < propLeafTable.size()) {
                const int available = static_cast<int>(propLeafTable.size() - static_cast<std::size_t>(firstLeaf));
                const int count = std::min(propLeafCount, available);
                entity.visibilityLeaves.reserve(static_cast<std::size_t>(count));
                for (int leaf = 0; leaf < count; ++leaf) {
                    entity.visibilityLeaves.push_back(propLeafTable[static_cast<std::size_t>(firstLeaf + leaf)]);
                }
            }
            entity.properties.emplace_back("model", entity.model);
            std::ostringstream angles;
            angles << pitch << ' ' << yaw << ' ' << roll;
            entity.properties.emplace_back("angles", angles.str());
            entity.properties.emplace_back("solid", std::to_string(solid));
            if (entrySize >= 56) {
                entity.fadeMin = std::max(0.0F, ReadFloat(propData, propOffset + 36) * kSourceToWorld);
                entity.fadeMax = std::max(0.0F, ReadFloat(propData, propOffset + 40) * kSourceToWorld);
                entity.lightingOrigin = SourcePointToWorld(
                    ReadFloat(propData, propOffset + 44),
                    ReadFloat(propData, propOffset + 48),
                    ReadFloat(propData, propOffset + 52)
                );
                entity.hasLightingOrigin = true;
            }
            entities.push_back(std::move(entity));
            ++added;
        }
    }
    return added;
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

bool FaceAcceptsDecal(const FacePolygon& polygon) {
    if (polygon.sky || polygon.skyPortal || polygon.points.size() < 3) {
        return false;
    }
    if ((polygon.texFlags & (kSurfSky | kSurfSky2D | kSurfTrigger | kSurfNodraw | kSurfNodecal)) != 0) {
        return false;
    }
    std::string lowered = polygon.material;
    for (char& character : lowered) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    static constexpr std::array<const char*, 8> blocked = {
        "trigger",
        "nodraw",
        "toolshint",
        "toolsskip",
        "toolsblocklight",
        "toolsorigin",
        "toolsnpcclip",
        "invisibleladder",
    };
    for (const char* token : blocked) {
        if (lowered.find(token) != std::string::npos) {
            return false;
        }
    }
    return true;
}

glm::vec3 FaceVertexWorld(const Point& point, float scale) {
    return glm::vec3(point.x * scale, point.y * scale, point.z * scale);
}

float PolygonOutsideDistance(
    const glm::vec3* vertices,
    std::size_t count,
    const glm::vec3& normal,
    const glm::vec3& point
) {
    if (count < 3) {
        return 1.0e6F;
    }
    glm::vec3 centroid(0.0F);
    for (std::size_t index = 0; index < count; ++index) {
        centroid += vertices[index];
    }
    centroid /= static_cast<float>(count);
    float maxOutside = 0.0F;
    for (std::size_t index = 0; index < count; ++index) {
        const glm::vec3& v0 = vertices[index];
        const glm::vec3& v1 = vertices[(index + 1) % count];
        const glm::vec3 edge = v1 - v0;
        glm::vec3 inward = glm::cross(normal, edge);
        if (glm::dot(inward, centroid - v0) < 0.0F) {
            inward = -inward;
        }
        const float inwardLength = glm::length(inward);
        if (inwardLength <= 1.0e-6F) {
            continue;
        }
        const float edgeDistance = glm::dot(point - v0, inward) / inwardLength;
        if (edgeDistance < 0.0F) {
            maxOutside = std::max(maxOutside, -edgeDistance);
        }
    }
    return maxOutside;
}

void ComputeSourceDecalBasis(const glm::vec3& normal, glm::vec3& outS, glm::vec3& outT) {
    glm::vec3 safeNormal = normal;
    if (glm::dot(safeNormal, safeNormal) <= 1.0e-8F) {
        safeNormal = glm::vec3(0.0F, 1.0F, 0.0F);
    } else {
        safeNormal = glm::normalize(safeNormal);
    }
    glm::vec3 sAxis(1.0F, 0.0F, 0.0F);
    glm::vec3 tAxis(0.0F, 1.0F, 0.0F);
    if (std::abs(safeNormal.x) < 1.0e-3F && std::abs(safeNormal.z) < 1.0e-3F) {
        tAxis = glm::cross(sAxis, safeNormal);
    } else {
        // World-down as the initial T, matching Source so U runs left-to-right and V runs top-to-bottom.
        sAxis = glm::cross(glm::vec3(0.0F, 1.0F, 0.0F), safeNormal);
        if (glm::dot(sAxis, sAxis) <= 1.0e-8F) {
            sAxis = glm::vec3(1.0F, 0.0F, 0.0F);
        } else {
            sAxis = glm::normalize(sAxis);
        }
        tAxis = glm::cross(sAxis, safeNormal);
    }
    if (glm::dot(sAxis, sAxis) <= 1.0e-8F) {
        sAxis = glm::vec3(1.0F, 0.0F, 0.0F);
    } else {
        sAxis = glm::normalize(sAxis);
    }
    if (glm::dot(tAxis, tAxis) <= 1.0e-8F) {
        tAxis = glm::vec3(0.0F, 1.0F, 0.0F);
    } else {
        tAxis = glm::normalize(tAxis);
    }
    outS = sAxis;
    outT = tAxis;
}

void ResolveDecalBasisFromBsp(
    glm::vec3& position,
    const std::vector<FacePolygon>& polygons,
    float scale,
    glm::vec3& outNormal,
    glm::vec3& outS,
    glm::vec3& outT,
    int& outFaceIndex
) {
    outFaceIndex = -1;
    const float margin = 2.0F * scale;
    const auto search = [&](float maxPlaneDistance, float& bestScore, float& bestOutside, glm::vec3& bestNormal, glm::vec3& bestSnap) {
        for (std::size_t polyIdx = 0; polyIdx < polygons.size(); ++polyIdx) {
            const FacePolygon& polygon = polygons[polyIdx];
            if (!FaceAcceptsDecal(polygon)) {
                continue;
            }
            glm::vec3 polyMin(1.0e9F);
            glm::vec3 polyMax(-1.0e9F);
            for (const Point& point : polygon.points) {
                const glm::vec3 world = FaceVertexWorld(point, scale);
                polyMin = glm::min(polyMin, world);
                polyMax = glm::max(polyMax, world);
            }
            if (position.x < polyMin.x - maxPlaneDistance || position.x > polyMax.x + maxPlaneDistance
                || position.y < polyMin.y - maxPlaneDistance || position.y > polyMax.y + maxPlaneDistance
                || position.z < polyMin.z - maxPlaneDistance || position.z > polyMax.z + maxPlaneDistance) {
                continue;
            }

            const auto consider = [&](const glm::vec3& a, const glm::vec3& b, const glm::vec3& c, const glm::vec3& hintNormal) {
                glm::vec3 faceNormal = glm::cross(b - a, c - a);
                const float normalLength = glm::length(faceNormal);
                if (normalLength <= 1.0e-6F) {
                    faceNormal = hintNormal;
                } else {
                    faceNormal /= normalLength;
                    if (glm::dot(faceNormal, hintNormal) < 0.0F) {
                        faceNormal = -faceNormal;
                    }
                }
                if (glm::dot(faceNormal, faceNormal) <= 1.0e-8F) {
                    return;
                }
                const float signedPlaneDist = glm::dot(position - a, faceNormal);
                const float absPlaneDist = std::abs(signedPlaneDist);
                if (absPlaneDist > maxPlaneDistance) {
                    return;
                }
                const glm::vec3 projected = position - signedPlaneDist * faceNormal;
                const glm::vec3 triangle[3] = {a, b, c};
                const float outside = PolygonOutsideDistance(triangle, 3, faceNormal, projected);
                if (outside > margin && absPlaneDist > margin) {
                    return;
                }
                const float outsidePenalty = outside > (0.25F * scale) ? 1000.0F : 0.0F;
                const float score = outsidePenalty + absPlaneDist * 5.0F + outside * 2.0F;
                if (score < bestScore) {
                    bestScore = score;
                    bestOutside = outside;
                    bestNormal = faceNormal;
                    bestSnap = projected;
                    outFaceIndex = polygon.faceIndex;
                }
            };

            const glm::vec3 hint = polygon.planeNormal;
            if (!polygon.gridIndices.empty()) {
                for (std::size_t index = 0; index + 2 < polygon.gridIndices.size(); index += 3) {
                    const unsigned int i0 = polygon.gridIndices[index];
                    const unsigned int i1 = polygon.gridIndices[index + 1];
                    const unsigned int i2 = polygon.gridIndices[index + 2];
                    if (i0 >= polygon.points.size() || i1 >= polygon.points.size() || i2 >= polygon.points.size()) {
                        continue;
                    }
                    consider(
                        FaceVertexWorld(polygon.points[i0], scale),
                        FaceVertexWorld(polygon.points[i1], scale),
                        FaceVertexWorld(polygon.points[i2], scale),
                        hint
                    );
                }
            } else {
                const glm::vec3 origin = FaceVertexWorld(polygon.planeAnchor, scale);
                const float signedPlaneDist = glm::dot(position - origin, hint);
                const float absPlaneDist = std::abs(signedPlaneDist);
                if (absPlaneDist > maxPlaneDistance || glm::dot(hint, hint) <= 1.0e-8F) {
                    continue;
                }
                const glm::vec3 projected = position - signedPlaneDist * hint;
                std::vector<glm::vec3> worldVerts(polygon.points.size());
                for (std::size_t index = 0; index < polygon.points.size(); ++index) {
                    worldVerts[index] = FaceVertexWorld(polygon.points[index], scale);
                }
                const float outside = PolygonOutsideDistance(worldVerts.data(), worldVerts.size(), hint, projected);
                const float outsidePenalty = outside > (0.25F * scale) ? 1000.0F : 0.0F;
                const float score = outsidePenalty + absPlaneDist * 5.0F + outside * 2.0F;
                if (score < bestScore) {
                    bestScore = score;
                    bestOutside = outside;
                    bestNormal = hint;
                    bestSnap = projected;
                    outFaceIndex = polygon.faceIndex;
                }
            }
        }
    };

    float bestScore = std::numeric_limits<float>::max();
    float bestOutside = std::numeric_limits<float>::max();
    glm::vec3 bestNormal(0.0F, 1.0F, 0.0F);
    glm::vec3 bestSnap = position;
    search(8.0F * scale, bestScore, bestOutside, bestNormal, bestSnap);
    if (outFaceIndex < 0 || bestOutside > scale) {
        outFaceIndex = -1;
        bestScore = std::numeric_limits<float>::max();
        bestOutside = std::numeric_limits<float>::max();
        search(std::max(0.75F, 48.0F * scale), bestScore, bestOutside, bestNormal, bestSnap);
    }
    if (outFaceIndex >= 0) {
        position = bestSnap;
        outNormal = glm::dot(bestNormal, bestNormal) > 1.0e-8F ? glm::normalize(bestNormal) : glm::vec3(0.0F, 1.0F, 0.0F);
    } else {
        outNormal = glm::vec3(0.0F, 1.0F, 0.0F);
    }
    ComputeSourceDecalBasis(outNormal, outS, outT);
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

std::vector<glm::vec3> ClipPolygonToPlane(
    const std::vector<glm::vec3>& input,
    const glm::vec3& planePoint,
    const glm::vec3& planeNormal
) {
    std::vector<glm::vec3> output;
    if (input.empty()) {
        return output;
    }
    constexpr float kEpsilon = 1.0e-6F;
    const auto sideOf = [&](const glm::vec3& point) {
        return glm::dot(point - planePoint, planeNormal);
    };
    glm::vec3 previous = input.back();
    float previousSide = sideOf(previous);
    output.reserve(input.size() + 1U);
    for (const glm::vec3& current : input) {
        const float currentSide = sideOf(current);
        const bool previousInside = previousSide >= -kEpsilon;
        const bool currentInside = currentSide >= -kEpsilon;
        if (previousInside != currentInside) {
            const float span = previousSide - currentSide;
            float t = 0.0F;
            if (std::abs(span) > 1.0e-12F) {
                t = previousSide / span;
            }
            t = std::clamp(t, 0.0F, 1.0F);
            output.push_back(previous + (current - previous) * t);
        }
        if (currentInside) {
            output.push_back(current);
        }
        previous = current;
        previousSide = currentSide;
    }
    return output;
}

Point WorldToSourcePoint(const glm::vec3& world, float scale) {
    const float inverse = scale > 1.0e-8F ? 1.0F / scale : 1.0F;
    return Point{world.x * inverse, -world.z * inverse, world.y * inverse};
}

std::array<glm::vec3, 4> DecalQuadCorners(const DecalSpawn& decal) {
    if (decal.explicitQuad) {
        return {
            decal.basisOrigin,
            decal.basisOrigin + decal.basisU,
            decal.basisOrigin + decal.basisU + decal.basisV,
            decal.basisOrigin + decal.basisV,
        };
    }
    const glm::vec3 alongS = decal.sAxis * decal.halfWidth;
    const glm::vec3 alongT = decal.tAxis * decal.halfHeight;
    return {
        decal.position - alongS - alongT,
        decal.position + alongS - alongT,
        decal.position + alongS + alongT,
        decal.position - alongS + alongT,
    };
}

void DecalTextureCoord(const DecalSpawn& decal, const glm::vec3& world, float& u, float& v) {
    if (decal.explicitQuad) {
        const glm::vec3 delta = world - decal.basisOrigin;
        const float uu = glm::dot(decal.basisU, decal.basisU);
        const float vv = glm::dot(decal.basisV, decal.basisV);
        const float uv = glm::dot(decal.basisU, decal.basisV);
        const float wu = glm::dot(delta, decal.basisU);
        const float wv = glm::dot(delta, decal.basisV);
        const float determinant = uu * vv - uv * uv;
        float alongU = 0.0F;
        float alongV = 0.0F;
        if (std::abs(determinant) > 1.0e-8F) {
            alongU = (wu * vv - wv * uv) / determinant;
            alongV = (uu * wv - uv * wu) / determinant;
        } else {
            alongU = uu > 1.0e-8F ? wu / uu : 0.0F;
            alongV = vv > 1.0e-8F ? wv / vv : 0.0F;
        }
        const float left = 1.0F - alongU;
        const float bottom = 1.0F - alongV;
        u = left * bottom * decal.uvs[0][0] + alongU * bottom * decal.uvs[1][0]
            + alongU * alongV * decal.uvs[2][0] + left * alongV * decal.uvs[3][0];
        v = left * bottom * decal.uvs[0][1] + alongU * bottom * decal.uvs[1][1]
            + alongU * alongV * decal.uvs[2][1] + left * alongV * decal.uvs[3][1];
        return;
    }
    const float width = std::max(decal.halfWidth * 2.0F, 1.0e-4F);
    const float height = std::max(decal.halfHeight * 2.0F, 1.0e-4F);
    const glm::vec3 delta = world - decal.position;
    u = glm::dot(delta, decal.sAxis) / width + 0.5F;
    v = glm::dot(delta, decal.tAxis) / height + 0.5F;
}

void AppendClippedDecal(
    DecalSpawn& decal,
    const std::vector<glm::vec3>& polygon,
    const glm::vec3& faceNormal,
    const FacePolygon& face,
    const std::vector<std::uint8_t>& texInfo,
    float scale,
    bool packed,
    float atlasWidth,
    float atlasHeight
) {
    if (polygon.size() < 3) {
        return;
    }
    std::vector<glm::vec3> wound(polygon);
    glm::vec3 winding(0.0F);
    for (std::size_t index = 0; index < wound.size(); ++index) {
        const glm::vec3& current = wound[index];
        const glm::vec3& next = wound[(index + 1) % wound.size()];
        winding += glm::cross(current, next);
    }
    if (glm::dot(winding, faceNormal) < 0.0F) {
        std::reverse(wound.begin(), wound.end());
    }
    const unsigned int base = static_cast<unsigned int>(decal.meshVertices.size() / Mesh::kFloatsPerVertex);
        const glm::vec3 bias = faceNormal * 0.0015F;
    for (const glm::vec3& point : wound) {
        float u = 0.0F;
        float v = 0.0F;
        DecalTextureCoord(decal, point, u, v);
        float lightmapU = 0.5F / atlasWidth;
        float lightmapV = 0.5F / atlasHeight;
        if (packed && face.lightmap.lit) {
            const auto luxel = ReadLightmapLuxel(
                texInfo,
                WorldToSourcePoint(point, scale),
                face.lightmap.texInfoIndex,
                face.lightmap.minsS,
                face.lightmap.minsT
            );
            lightmapU = (static_cast<float>(face.lightmap.atlasX) + luxel[0] + 0.5F) / atlasWidth;
            lightmapV = (static_cast<float>(face.lightmap.atlasY) + luxel[1] + 0.5F) / atlasHeight;
        }
        AppendDecalVertex(decal.meshVertices, point + bias, u, v, lightmapU, lightmapV);
    }
    for (std::size_t index = 1; index + 1 < wound.size(); ++index) {
        decal.meshIndices.push_back(base);
        decal.meshIndices.push_back(base + static_cast<unsigned int>(index));
        decal.meshIndices.push_back(base + static_cast<unsigned int>(index + 1));
    }
}

void ProjectDecalsOntoFaces(
    std::vector<DecalSpawn>& decals,
    const std::vector<FacePolygon>& polygons,
    const std::vector<int>& polygonByFace,
    const std::vector<std::uint8_t>& texInfo,
    float scale,
    bool packed,
    float atlasWidth,
    float atlasHeight
) {
    for (DecalSpawn& decal : decals) {
        const std::array<glm::vec3, 4> corners = DecalQuadCorners(decal);
        glm::vec3 center(0.0F);
        for (const glm::vec3& corner : corners) {
            center += corner;
        }
        center *= 0.25F;
        const float halfDiagonal = std::max(glm::length(corners[2] - corners[0]) * 0.5F, 0.01F);
        std::array<glm::vec3, 4> planePoints{};
        std::array<glm::vec3, 4> planeNormals{};
        bool planesReady = true;
        for (int edge = 0; edge < 4; ++edge) {
            const glm::vec3& start = corners[static_cast<std::size_t>(edge)];
            const glm::vec3& end = corners[static_cast<std::size_t>((edge + 1) % 4)];
            glm::vec3 inward = glm::cross(decal.normal, end - start);
            if (glm::dot(inward, center - start) < 0.0F) {
                inward = -inward;
            }
            const float inwardLength = glm::length(inward);
            if (inwardLength <= 1.0e-6F) {
                planesReady = false;
                break;
            }
            planePoints[static_cast<std::size_t>(edge)] = start;
            planeNormals[static_cast<std::size_t>(edge)] = inward / inwardLength;
        }
        if (!planesReady) {
            continue;
        }

        const auto clipFragment = [&](const std::vector<glm::vec3>& source) {
            std::vector<glm::vec3> clipped = source;
            for (int plane = 0; plane < 4; ++plane) {
                clipped = ClipPolygonToPlane(
                    clipped,
                    planePoints[static_cast<std::size_t>(plane)],
                    planeNormals[static_cast<std::size_t>(plane)]
                );
                if (clipped.size() < 3) {
                    clipped.clear();
                    break;
                }
            }
            return clipped;
        };

        const auto projectOntoFace = [&](const FacePolygon& face, bool listedSide) {
            if (!FaceAcceptsDecal(face)) {
                return;
            }
            glm::vec3 faceMin(1.0e9F);
            glm::vec3 faceMax(-1.0e9F);
            for (const Point& point : face.points) {
                const glm::vec3 world = FaceVertexWorld(point, scale);
                faceMin = glm::min(faceMin, world);
                faceMax = glm::max(faceMax, world);
            }
            const float reach = listedSide ? halfDiagonal + 8.0F * scale : halfDiagonal;
            if (center.x < faceMin.x - reach || center.x > faceMax.x + reach
                || center.y < faceMin.y - reach || center.y > faceMax.y + reach
                || center.z < faceMin.z - reach || center.z > faceMax.z + reach) {
                return;
            }

            const auto emit = [&](const std::vector<glm::vec3>& fragment, const glm::vec3& fragmentNormal) {
                if (fragment.size() < 3) {
                    return;
                }
                float fragmentArea = 0.0F;
                for (std::size_t index = 1; index + 1 < fragment.size(); ++index) {
                    fragmentArea += glm::length(glm::cross(
                        fragment[index] - fragment[0],
                        fragment[index + 1] - fragment[0]
                    ));
                }
                fragmentArea *= 0.5F;
                if (fragmentArea < 0.0008F) {
                    return;
                }
                if (!listedSide && glm::dot(fragmentNormal, decal.normal) <= 0.01F) {
                    return;
                }
                if (listedSide && glm::dot(fragmentNormal, decal.normal) <= 0.0F) {
                    return;
                }
                const float planeDistance = std::abs(glm::dot(center - fragment.front(), fragmentNormal));
                if (planeDistance > reach) {
                    return;
                }
                AppendClippedDecal(
                    decal,
                    clipFragment(fragment),
                    fragmentNormal,
                    face,
                    texInfo,
                    scale,
                    packed,
                    atlasWidth,
                    atlasHeight
                );
            };

            if (!face.gridIndices.empty()) {
                for (std::size_t index = 0; index + 2 < face.gridIndices.size(); index += 3) {
                    const unsigned int i0 = face.gridIndices[index];
                    const unsigned int i1 = face.gridIndices[index + 1];
                    const unsigned int i2 = face.gridIndices[index + 2];
                    if (i0 >= face.points.size() || i1 >= face.points.size() || i2 >= face.points.size()) {
                        continue;
                    }
                    const glm::vec3 a = FaceVertexWorld(face.points[i0], scale);
                    const glm::vec3 b = FaceVertexWorld(face.points[i1], scale);
                    const glm::vec3 c = FaceVertexWorld(face.points[i2], scale);
                    glm::vec3 triangleNormal = glm::cross(b - a, c - a);
                    const float normalLength = glm::length(triangleNormal);
                    if (normalLength <= 1.0e-6F) {
                        continue;
                    }
                    triangleNormal /= normalLength;
                    if (glm::dot(triangleNormal, face.planeNormal) < 0.0F) {
                        triangleNormal = -triangleNormal;
                    }
                    emit({a, b, c}, triangleNormal);
                }
                return;
            }

            std::vector<glm::vec3> worldVerts;
            worldVerts.reserve(face.points.size());
            for (const Point& point : face.points) {
                worldVerts.push_back(FaceVertexWorld(point, scale));
            }
            emit(worldVerts, face.planeNormal);
        };

        if (decal.sideFaces.empty()) {
            if (decal.faceIndex < 0 || static_cast<std::size_t>(decal.faceIndex) >= polygonByFace.size()) {
                continue;
            }
            const int hitPolygon = polygonByFace[static_cast<std::size_t>(decal.faceIndex)];
            if (hitPolygon < 0 || static_cast<std::size_t>(hitPolygon) >= polygons.size()) {
                continue;
            }
            const FacePolygon& hit = polygons[static_cast<std::size_t>(hitPolygon)];
            const glm::vec3 hitAnchor = FaceVertexWorld(hit.planeAnchor, scale);
            for (const FacePolygon& face : polygons) {
                if (glm::dot(face.planeNormal, hit.planeNormal) < 0.92F) {
                    continue;
                }
                const glm::vec3 faceAnchor = FaceVertexWorld(face.planeAnchor, scale);
                if (std::abs(glm::dot(faceAnchor - hitAnchor, hit.planeNormal)) > 0.04F) {
                    continue;
                }
                projectOntoFace(face, true);
            }
            continue;
        }
        for (const int faceId : decal.sideFaces) {
            if (faceId < 0 || static_cast<std::size_t>(faceId) >= polygonByFace.size()) {
                continue;
            }
            const int polygonIndex = polygonByFace[static_cast<std::size_t>(faceId)];
            if (polygonIndex < 0 || static_cast<std::size_t>(polygonIndex) >= polygons.size()) {
                continue;
            }
            projectOntoFace(polygons[static_cast<std::size_t>(polygonIndex)], true);
        }
    }
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

void WriteAtlasPixel(
    std::vector<float>& pixels,
    int width,
    int height,
    int x,
    int y,
    const std::array<float, 3>& color
) {
    if (x < 0 || y < 0 || x >= width || y >= height) {
        return;
    }
    const std::size_t index = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width)
        + static_cast<std::size_t>(x)) * 3;
    if (index + 2 >= pixels.size()) {
        return;
    }
    pixels[index] = color[0];
    pixels[index + 1] = color[1];
    pixels[index + 2] = color[2];
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
    std::vector<float>& pixels,
    int atlasWidth,
    int atlasHeight,
    const FaceLightmap& lightmap,
    const std::vector<std::uint8_t>& lighting,
    int page
) {
    const int width = lightmap.luxelW;
    const int height = lightmap.luxelH;
    if (width <= 0 || height <= 0 || page < 0) {
        return;
    }
    const std::size_t pageBytes = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U;
    const std::size_t base = static_cast<std::size_t>(lightmap.lightOffset) + pageBytes * static_cast<std::size_t>(page);
    if (base > lighting.size() || pageBytes > lighting.size() - base) {
        return;
    }
    for (int y = -1; y <= height; ++y) {
        for (int x = -1; x <= width; ++x) {
            const int sampleX = std::clamp(x, 0, width - 1);
            const int sampleY = std::clamp(y, 0, height - 1);
            const std::size_t sampleOffset = base
                + (static_cast<std::size_t>(sampleY) * static_cast<std::size_t>(width)
                    + static_cast<std::size_t>(sampleX)) * 4;
            WriteAtlasPixel(
                pixels,
                atlasWidth,
                atlasHeight,
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
    glBindTexture(GL_TEXTURE_2D_ARRAY, texture);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage3D(
        GL_TEXTURE_2D_ARRAY,
        0,
        GL_RGB16F,
        atlas.width,
        atlas.height,
        2,
        0,
        GL_RGB,
        GL_FLOAT,
        nullptr
    );
    glTexSubImage3D(
        GL_TEXTURE_2D_ARRAY,
        0,
        0,
        0,
        0,
        atlas.width,
        atlas.height,
        1,
        GL_RGB,
        GL_FLOAT,
        atlas.rgb.data()
    );
    const float* stylePixels = atlas.rgbStyle.empty() ? atlas.rgb.data() : atlas.rgbStyle.data();
    glTexSubImage3D(
        GL_TEXTURE_2D_ARRAY,
        0,
        0,
        0,
        1,
        atlas.width,
        atlas.height,
        1,
        GL_RGB,
        GL_FLOAT,
        stylePixels
    );
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    return texture;
}

std::size_t DispInfoStride(const std::vector<std::uint8_t>& dispInfo) {
    if (dispInfo.empty()) {
        return 0;
    }
    if (dispInfo.size() % 176U == 0) {
        return 176U;
    }
    if (dispInfo.size() % 184U == 0) {
        return 184U;
    }
    if (dispInfo.size() % 232U == 0) {
        return 232U;
    }
    return 0;
}

bool BuildDisplacement(
    FacePolygon& polygon,
    const std::vector<std::uint8_t>& dispInfo,
    const std::vector<std::uint8_t>& dispVerts,
    const std::vector<std::uint8_t>& dispTris,
    int dispIndex,
    const std::vector<std::uint8_t>& texInfo,
    const std::vector<std::uint8_t>& texData
) {
    const std::size_t stride = DispInfoStride(dispInfo);
    if (stride == 0 || dispIndex < 0 || polygon.sourcePoints.size() != 4) {
        return false;
    }
    if (static_cast<std::size_t>(dispIndex) >= dispInfo.size() / stride) {
        return false;
    }
    const std::size_t base = static_cast<std::size_t>(dispIndex) * stride;
    const Point start{
        ReadFloat(dispInfo, base),
        ReadFloat(dispInfo, base + 4),
        ReadFloat(dispInfo, base + 8),
    };
    const int vertStart = ReadI32(dispInfo, base + 12);
    const int power = ReadI32(dispInfo, base + 20);
    static_cast<void>(dispTris);
    if (power < 2 || power > 4 || vertStart < 0) {
        return false;
    }
    const int size = (1 << power) + 1;
    const int count = size * size;
    const std::size_t vertBytes = static_cast<std::size_t>(vertStart + count) * kDispVertSize;
    if (vertBytes > dispVerts.size()) {
        return false;
    }

    int startCorner = 0;
    float bestDistance = std::numeric_limits<float>::max();
    for (int corner = 0; corner < 4; ++corner) {
        const Point& point = polygon.sourcePoints[static_cast<std::size_t>(corner)];
        const float dx = point.x - start.x;
        const float dy = point.y - start.y;
        const float dz = point.z - start.z;
        const float distance = dx * dx + dy * dy + dz * dz;
        if (distance < bestDistance) {
            bestDistance = distance;
            startCorner = corner;
        }
    }
    std::array<Point, 4> corners{};
    for (int corner = 0; corner < 4; ++corner) {
        corners[static_cast<std::size_t>(corner)] = polygon.sourcePoints[static_cast<std::size_t>((startCorner + corner) % 4)];
    }

    std::vector<Point> sourcePoints;
    std::vector<Point> points;
    std::vector<std::array<float, 2>> uvs;
    sourcePoints.reserve(static_cast<std::size_t>(count));
    points.reserve(static_cast<std::size_t>(count));
    uvs.reserve(static_cast<std::size_t>(count));
    const float denom = static_cast<float>(size - 1);
    for (int y = 0; y < size; ++y) {
        const float fy = static_cast<float>(y) / denom;
        for (int x = 0; x < size; ++x) {
            const float fx = static_cast<float>(x) / denom;
            const float w0 = (1.0F - fx) * (1.0F - fy);
            const float w1 = fx * (1.0F - fy);
            const float w2 = fx * fy;
            const float w3 = (1.0F - fx) * fy;
            Point sample{
                corners[0].x * w0 + corners[1].x * w1 + corners[2].x * w2 + corners[3].x * w3,
                corners[0].y * w0 + corners[1].y * w1 + corners[2].y * w2 + corners[3].y * w3,
                corners[0].z * w0 + corners[1].z * w1 + corners[2].z * w2 + corners[3].z * w3,
            };
            const std::size_t vertOffset = (static_cast<std::size_t>(vertStart) + static_cast<std::size_t>(y * size + x)) * kDispVertSize;
            const float distance = ReadFloat(dispVerts, vertOffset + 12);
            sample.x += ReadFloat(dispVerts, vertOffset) * distance;
            sample.y += ReadFloat(dispVerts, vertOffset + 4) * distance;
            sample.z += ReadFloat(dispVerts, vertOffset + 8) * distance;
            sourcePoints.push_back(sample);
            points.push_back(ToOpenGlPoint(sample));
            uvs.push_back(ReadUv(texInfo, texData, sample, polygon.lightmap.texInfoIndex));
        }
    }

    std::vector<unsigned int> indices;
    const int quadCount = (size - 1) * (size - 1);
    indices.reserve(static_cast<std::size_t>(quadCount) * 6U);
    // Face edges are already in the same order as brush faces. Keep that winding
    // for every quad so the surface faces the same way as the rest of the map.
    for (int y = 0; y < size - 1; ++y) {
        for (int x = 0; x < size - 1; ++x) {
            const unsigned int i0 = static_cast<unsigned int>(y * size + x);
            const unsigned int i1 = i0 + 1U;
            const unsigned int i2 = i0 + static_cast<unsigned int>(size);
            const unsigned int i3 = i2 + 1U;
            indices.insert(indices.end(), {i0, i1, i3, i0, i3, i2});
        }
    }

    polygon.sourcePoints = std::move(sourcePoints);
    polygon.points = std::move(points);
    polygon.uvs = std::move(uvs);
    polygon.gridIndices = std::move(indices);
    return true;
}

BspFog ParseFog(const std::vector<std::pair<std::string, std::string>>& properties) {
    BspFog fog;
    const std::string colorText = EntityKeyValue(properties, "fogcolor");
    if (!colorText.empty()) {
        std::istringstream stream(colorText);
        float red = 0.0F;
        float green = 0.0F;
        float blue = 0.0F;
        if (stream >> red >> green >> blue) {
            fog.color = glm::clamp(glm::vec3(red, green, blue) / 255.0F, glm::vec3(0.0F), glm::vec3(1.0F));
        }
    }
    fog.start = std::max(0.0F, PropertyFloat(properties, "fogstart", 0.0F) * kSourceToWorld);
    fog.end = std::max(0.0F, PropertyFloat(properties, "fogend", 0.0F) * kSourceToWorld);
    fog.maxDensity = std::clamp(PropertyFloat(properties, "fogmaxdensity", 1.0F), 0.0F, 1.0F);
    const float enabled = PropertyFloat(properties, "fogenable", 1.0F);
    fog.enabled = enabled > 0.5F && fog.end > fog.start + 0.001F;
    return fog;
}

std::size_t LeafStrideForVersion(int bspVersion, int lumpVersion, std::size_t lumpSize) {
    const auto divides = [&](std::size_t stride) {
        return lumpSize >= stride && lumpSize % stride == 0;
    };
    if (lumpVersion <= 0 && divides(56U)) {
        return 56U;
    }
    if (lumpVersion >= 1 && divides(32U)) {
        return 32U;
    }
    const std::size_t preferred = bspVersion >= 20 ? 32U : 56U;
    if (divides(preferred)) {
        return preferred;
    }
    if (divides(32U)) {
        return 32U;
    }
    if (divides(56U)) {
        return 56U;
    }
    return 0;
}

bool SkySetUsable(const std::unordered_set<int>& faces, std::size_t totalFaceCount) {
    return !faces.empty() && faces.size() * 2U <= totalFaceCount;
}

constexpr float kSkyRadius = 12288.0F;

Point WorldToSourcePoint(const glm::vec3& world) {
    const float inverse = 1.0F / kSourceToWorld;
    return {world.x * inverse, -world.z * inverse, world.y * inverse};
}

int LeafAreaValue(const std::vector<std::uint8_t>& leafLump, std::size_t leafStride, int leaf) {
    if (leaf < 0 || leafStride == 0) {
        return -1;
    }
    const std::size_t offset = static_cast<std::size_t>(leaf) * leafStride;
    if (offset + 8 > leafLump.size()) {
        return -1;
    }
    return static_cast<int>(ReadU16(leafLump, offset + 6) & 0x1FFU);
}

int PointLeafIndex(
    const Point& point,
    const std::vector<std::uint8_t>& nodes,
    const std::vector<std::uint8_t>& planes,
    std::size_t planeCount,
    std::size_t leafCount
) {
    if (nodes.size() < kNodeSize || nodes.size() % kNodeSize != 0 || planeCount == 0 || leafCount == 0) {
        return -1;
    }
    const std::size_t nodeCount = nodes.size() / kNodeSize;
    int nodeIndex = 0;
    for (int step = 0; step < 1024 && nodeIndex >= 0; ++step) {
        if (static_cast<std::size_t>(nodeIndex) >= nodeCount) {
            return -1;
        }
        const std::size_t offset = static_cast<std::size_t>(nodeIndex) * kNodeSize;
        const int planeIndex = ReadI32(nodes, offset);
        if (planeIndex < 0 || static_cast<std::size_t>(planeIndex) >= planeCount) {
            return -1;
        }
        const std::size_t planeOffset = static_cast<std::size_t>(planeIndex) * kPlaneSize;
        const float side = ReadFloat(planes, planeOffset) * point.x
            + ReadFloat(planes, planeOffset + 4) * point.y
            + ReadFloat(planes, planeOffset + 8) * point.z
            - ReadFloat(planes, planeOffset + 12);
        nodeIndex = ReadI32(nodes, offset + (side >= 0.0F ? 4U : 8U));
    }
    if (nodeIndex >= 0) {
        return -1;
    }
    const int leaf = -1 - nodeIndex;
    if (leaf < 0 || static_cast<std::size_t>(leaf) >= leafCount) {
        return -1;
    }
    return leaf;
}

struct SkySelection {
    std::unordered_set<int> faces;
    int area = -1;
    bool areaAccepted = false;
};

std::unordered_set<int> FacesInArea(
    const std::vector<std::uint8_t>& leafLump,
    const std::vector<std::uint8_t>& leafFaces,
    std::size_t leafStride,
    int skyArea,
    std::size_t totalFaceCount
) {
    std::unordered_set<int> faces;
    if (skyArea < 0 || leafStride == 0 || leafFaces.size() % 2U != 0) {
        return faces;
    }
    const std::size_t leafFaceCount = leafFaces.size() / 2U;
    const std::size_t leafCount = leafLump.size() / leafStride;
    for (std::size_t leafIndex = 0; leafIndex < leafCount; ++leafIndex) {
        const std::size_t offset = leafIndex * leafStride;
        if (LeafAreaValue(leafLump, leafStride, static_cast<int>(leafIndex)) != skyArea) {
            continue;
        }
        const unsigned int first = ReadU16(leafLump, offset + 20);
        const unsigned int count = ReadU16(leafLump, offset + 22);
        if (static_cast<std::size_t>(first) > leafFaceCount || count > leafFaceCount - first) {
            continue;
        }
        for (unsigned int index = 0; index < count; ++index) {
            const int face = static_cast<int>(ReadU16(leafFaces, (static_cast<std::size_t>(first) + index) * 2U));
            if (face >= 0 && static_cast<std::size_t>(face) < totalFaceCount) {
                faces.insert(face);
            }
        }
    }
    return faces;
}

std::unordered_set<int> FacesNearSkyOrigin(
    const std::vector<FacePolygon>& polygons,
    const Point& skyOrigin,
    std::size_t totalFaceCount
) {
    std::unordered_set<int> faces;
    const float radiusSquared = kSkyRadius * kSkyRadius;
    for (const FacePolygon& polygon : polygons) {
        if (polygon.modelIndex != 0 || polygon.faceIndex < 0 || polygon.sourcePoints.empty() || polygon.skyPortal) {
            continue;
        }
        Point centroid{};
        for (const Point& point : polygon.sourcePoints) {
            centroid.x += point.x;
            centroid.y += point.y;
            centroid.z += point.z;
        }
        const float count = static_cast<float>(polygon.sourcePoints.size());
        centroid.x /= count;
        centroid.y /= count;
        centroid.z /= count;
        const float dx = centroid.x - skyOrigin.x;
        const float dy = centroid.y - skyOrigin.y;
        const float dz = centroid.z - skyOrigin.z;
        if (dx * dx + dy * dy + dz * dz <= radiusSquared) {
            faces.insert(polygon.faceIndex);
        }
    }
    if (!SkySetUsable(faces, totalFaceCount)) {
        faces.clear();
    }
    return faces;
}

SkySelection CollectSkyFaces(
    const std::vector<std::uint8_t>& nodes,
    const std::vector<std::uint8_t>& planes,
    std::size_t planeCount,
    const std::vector<std::uint8_t>& leafLump,
    const std::vector<std::uint8_t>& leafFaces,
    const std::vector<FacePolygon>& polygons,
    const Point& skyOrigin,
    int bspVersion,
    int leafLumpVersion,
    std::size_t totalFaceCount
) {
    SkySelection selection;
    const std::size_t leafStride = LeafStrideForVersion(bspVersion, leafLumpVersion, leafLump.size());
    const std::size_t leafCount = leafStride == 0 ? 0 : leafLump.size() / leafStride;
    const int leaf = PointLeafIndex(skyOrigin, nodes, planes, planeCount, leafCount);
    selection.area = LeafAreaValue(leafLump, leafStride, leaf);
    if (selection.area >= 0) {
        selection.faces = FacesInArea(leafLump, leafFaces, leafStride, selection.area, totalFaceCount);
        if (SkySetUsable(selection.faces, totalFaceCount)) {
            selection.areaAccepted = true;
            return selection;
        }
        selection.faces.clear();
    }
    selection.faces = FacesNearSkyOrigin(polygons, skyOrigin, totalFaceCount);
    return selection;
}

std::vector<float> SkyQuad(const std::array<std::array<float, 5>, 4>& corners);

std::vector<float> SourceSkyFaceVertices(int axis, float extent) {
    static const int kSourceToTex[6][3] = {
        {3, -1, 2},
        {-3, 1, 2},
        {1, 3, 2},
        {-1, -3, 2},
        {-2, -1, 3},
        {2, -1, -3},
    };
    const float samples[4][2] = {
        {-1.0F, -1.0F},
        {1.0F, -1.0F},
        {1.0F, 1.0F},
        {-1.0F, 1.0F},
    };
    std::array<std::array<float, 5>, 4> corners{};
    for (int corner = 0; corner < 4; ++corner) {
        const float s = samples[corner][0];
        const float t = samples[corner][1];
        const float basis[3] = {s * extent, t * extent, extent};
        float source[3] = {0.0F, 0.0F, 0.0F};
        for (int axisIndex = 0; axisIndex < 3; ++axisIndex) {
            const int code = kSourceToTex[axis][axisIndex];
            const int component = std::abs(code) - 1;
            source[axisIndex] = code < 0 ? -basis[component] : basis[component];
        }
        corners[static_cast<std::size_t>(corner)] = {
            source[0],
            source[2],
            -source[1],
            (s + 1.0F) * 0.5F,
            1.0F - (t + 1.0F) * 0.5F,
        };
    }
    return SkyQuad(corners);
}

std::vector<float> SkyQuad(const std::array<std::array<float, 5>, 4>& corners) {
    std::vector<float> vertices;
    vertices.reserve(4U * Mesh::kFloatsPerVertex);
    for (const auto& corner : corners) {
        vertices.insert(vertices.end(), {
            corner[0], corner[1], corner[2],
            1.0F, 1.0F, 1.0F,
            corner[3], corner[4],
            0.0F, 0.0F,
            0.0F,
        });
    }
    return vertices;
}

void DecompressClusterPvs(const std::uint8_t* source, std::size_t sourceSize, std::uint8_t* destination, int rowBytes) {
    int written = 0;
    std::size_t cursor = 0;
    while (written < rowBytes) {
        if (cursor >= sourceSize) {
            std::memset(destination, 0xFF, static_cast<std::size_t>(rowBytes));
            return;
        }
        const std::uint8_t value = source[cursor++];
        if (value != 0) {
            destination[written++] = value;
            continue;
        }
        if (cursor >= sourceSize) {
            std::memset(destination, 0xFF, static_cast<std::size_t>(rowBytes));
            return;
        }
        int run = static_cast<int>(source[cursor++]);
        if (run <= 0) {
            std::memset(destination, 0xFF, static_cast<std::size_t>(rowBytes));
            return;
        }
        if (written + run > rowBytes) {
            run = rowBytes - written;
        }
        std::memset(destination + written, 0, static_cast<std::size_t>(run));
        written += run;
    }
}

BspVisibility BuildVisibility(
    const std::vector<std::uint8_t>& nodeLump,
    const std::vector<std::uint8_t>& planeLump,
    const std::vector<std::uint8_t>& leafLump,
    std::size_t leafStride,
    const std::vector<std::uint8_t>& leafFaces,
    const std::vector<std::uint8_t>& visLump,
    const std::vector<std::uint8_t>& areaLump,
    const std::vector<std::uint8_t>& portalLump,
    const std::vector<std::uint8_t>& clipVertLump,
    const std::vector<std::uint8_t>& waterLump,
    std::size_t faceCount
) {
    BspVisibility visibility;
    if (planeLump.size() >= kPlaneSize && planeLump.size() % kPlaneSize == 0) {
        const std::size_t planeCount = planeLump.size() / kPlaneSize;
        visibility.planes.reserve(planeCount);
        for (std::size_t planeIndex = 0; planeIndex < planeCount; ++planeIndex) {
            const std::size_t offset = planeIndex * kPlaneSize;
            BspVisibility::Plane plane;
            plane.x = ReadFloat(planeLump, offset);
            plane.y = ReadFloat(planeLump, offset + 4);
            plane.z = ReadFloat(planeLump, offset + 8);
            plane.distance = ReadFloat(planeLump, offset + 12);
            visibility.planes.push_back(plane);
        }
    }
    if (nodeLump.size() >= kNodeSize && nodeLump.size() % kNodeSize == 0) {
        const std::size_t nodeCount = nodeLump.size() / kNodeSize;
        visibility.nodes.reserve(nodeCount);
        for (std::size_t nodeIndex = 0; nodeIndex < nodeCount; ++nodeIndex) {
            const std::size_t offset = nodeIndex * kNodeSize;
            BspVisibility::Node node;
            node.plane = ReadI32(nodeLump, offset);
            node.child[0] = ReadI32(nodeLump, offset + 4);
            node.child[1] = ReadI32(nodeLump, offset + 8);
            visibility.nodes.push_back(node);
        }
    }

    const std::size_t leafFaceCount = leafFaces.size() / 2U;
    const bool leafFacesAligned = leafFaces.size() % 2U == 0;
    std::vector<std::vector<int>> faceLeaves(faceCount);
    if (leafStride != 0 && leafLump.size() % leafStride == 0) {
        const std::size_t leafCount = leafLump.size() / leafStride;
        visibility.leaves.reserve(leafCount);
        for (std::size_t leafIndex = 0; leafIndex < leafCount; ++leafIndex) {
            const std::size_t offset = leafIndex * leafStride;
            BspVisibility::Leaf leaf;
            leaf.contents = ReadI32(leafLump, offset);
            leaf.cluster = static_cast<int>(ReadI16(leafLump, offset + 4));
            leaf.area = static_cast<int>(ReadU16(leafLump, offset + 6) & 0x1FFU);
            if (leafStride >= 30 && offset + 30 <= leafLump.size()) {
                leaf.water = static_cast<int>(ReadI16(leafLump, offset + 28));
            }
            visibility.leaves.push_back(leaf);
            if (!leafFacesAligned || offset + 24 > leafLump.size()) {
                continue;
            }
            const unsigned int first = ReadU16(leafLump, offset + 20);
            const unsigned int count = ReadU16(leafLump, offset + 22);
            if (static_cast<std::size_t>(first) > leafFaceCount || count > leafFaceCount - first) {
                continue;
            }
            for (unsigned int index = 0; index < count; ++index) {
                const int face = static_cast<int>(ReadU16(leafFaces, (static_cast<std::size_t>(first) + index) * 2U));
                if (face >= 0 && static_cast<std::size_t>(face) < faceCount) {
                    faceLeaves[static_cast<std::size_t>(face)].push_back(static_cast<int>(leafIndex));
                }
            }
        }
    }

    visibility.faceLeafOffset.assign(faceCount + 1, 0);
    int cursor = 0;
    for (std::size_t faceIndex = 0; faceIndex < faceCount; ++faceIndex) {
        visibility.faceLeafOffset[faceIndex] = cursor;
        for (const int leaf : faceLeaves[faceIndex]) {
            visibility.faceLeafIndices.push_back(leaf);
            ++cursor;
        }
    }
    if (!visibility.faceLeafOffset.empty()) {
        visibility.faceLeafOffset.back() = cursor;
    }

    if (visLump.size() >= 4) {
        const int clusterCount = ReadI32(visLump, 0);
        const int rowBytes = (clusterCount + 7) >> 3;
        const std::size_t headerBytes = 4U + static_cast<std::size_t>(clusterCount) * 8U;
        if (clusterCount > 0 && clusterCount <= 65536 && rowBytes > 0
            && visLump.size() >= headerBytes) {
            visibility.clusterCount = clusterCount;
            visibility.pvsRowBytes = rowBytes;
            visibility.clusterPvs.assign(static_cast<std::size_t>(clusterCount) * static_cast<std::size_t>(rowBytes), 0xFF);
            for (int cluster = 0; cluster < clusterCount; ++cluster) {
                const std::size_t row = static_cast<std::size_t>(cluster) * static_cast<std::size_t>(rowBytes);
                const int offset = ReadI32(visLump, 4 + static_cast<std::size_t>(cluster) * 8U);
                if (offset < 0 || static_cast<std::size_t>(offset) >= visLump.size()) {
                    continue;
                }
                DecompressClusterPvs(
                    visLump.data() + offset,
                    visLump.size() - static_cast<std::size_t>(offset),
                    visibility.clusterPvs.data() + row,
                    rowBytes
                );
            }
        }
    }

    if (areaLump.size() >= 8 && areaLump.size() % 8U == 0) {
        const std::size_t areaCount = areaLump.size() / 8U;
        visibility.areas.reserve(areaCount);
        for (std::size_t areaIndex = 0; areaIndex < areaCount; ++areaIndex) {
            const std::size_t offset = areaIndex * 8U;
            BspVisibility::Area area;
            area.portalCount = ReadI32(areaLump, offset);
            area.firstPortal = ReadI32(areaLump, offset + 4);
            visibility.areas.push_back(area);
        }
    }
    if (portalLump.size() >= 12 && portalLump.size() % 12U == 0) {
        const std::size_t portalCount = portalLump.size() / 12U;
        visibility.portals.reserve(portalCount);
        for (std::size_t portalIndex = 0; portalIndex < portalCount; ++portalIndex) {
            const std::size_t offset = portalIndex * 12U;
            BspVisibility::Portal portal;
            portal.number = static_cast<int>(ReadU16(portalLump, offset));
            portal.otherArea = static_cast<int>(ReadU16(portalLump, offset + 2));
            portal.firstVert = static_cast<int>(ReadU16(portalLump, offset + 4));
            portal.vertCount = static_cast<int>(ReadU16(portalLump, offset + 6));
            visibility.portals.push_back(portal);
        }
    }
    if (clipVertLump.size() >= 12 && clipVertLump.size() % 12U == 0) {
        const std::size_t vertCount = clipVertLump.size() / 12U;
        visibility.portalVertices.reserve(vertCount);
        for (std::size_t vertIndex = 0; vertIndex < vertCount; ++vertIndex) {
            const std::size_t offset = vertIndex * 12U;
            visibility.portalVertices.push_back(::SourcePointToWorld(
                ReadFloat(clipVertLump, offset),
                ReadFloat(clipVertLump, offset + 4),
                ReadFloat(clipVertLump, offset + 8)
            ));
        }
    }
    if (waterLump.size() >= 12 && waterLump.size() % 12U == 0) {
        const std::size_t waterCount = waterLump.size() / 12U;
        visibility.waterSurfaceY.reserve(waterCount);
        for (std::size_t waterIndex = 0; waterIndex < waterCount; ++waterIndex) {
            const float surfaceZ = ReadFloat(waterLump, waterIndex * 12U);
            visibility.waterSurfaceY.push_back(surfaceZ * kSourceToWorld);
        }
    }
    return visibility;
}

void ApplyPortalEntities(BspVisibility& visibility) {
    for (BspVisibility::Portal& portal : visibility.portals) {
        const BspVisibility::PortalEntity* match = nullptr;
        for (const BspVisibility::PortalEntity& entity : visibility.portalEntities) {
            if (entity.number == portal.number) {
                match = &entity;
                break;
            }
        }
        if (match == nullptr) {
            portal.startOpen = true;
            portal.target.clear();
            continue;
        }
        portal.startOpen = match->startOpen;
        portal.target = match->target;
    }
}

LoadedGeometry LoadGeometry(
    const std::string& path,
    std::size_t& faceCount,
    glm::vec3& worldMinimum,
    glm::vec3& worldMaximum,
    const GameFileSystem* files
) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        Fail("unable to open '" + path + "'");
    }
    file.seekg(0, std::ios::end);
    const auto fileSize = static_cast<std::uintmax_t>(file.tellg());
    file.seekg(0, std::ios::beg);

    if (ReadFileI32(file) != 0x50534256) {
        Fail("invalid BSP header");
    }
    const int bspVersion = ReadFileI32(file);
    if (bspVersion <= 0) {
        Fail("invalid BSP header");
    }
    std::array<Lump, kLumpCount> lumps{};
    for (Lump& lump : lumps) {
        lump.offset = ReadFileI32(file);
        lump.length = ReadFileI32(file);
        lump.version = ReadFileI32(file);
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
    const auto modelLump = ReadLump(file, lumps[kModelLump], fileSize, "MODELS");
    const auto lightingLdr = ReadLump(file, lumps[kLightingLump], fileSize, "LIGHTING");
    const auto lightingHdr = ReadLump(file, lumps[kLightingHdrLump], fileSize, "LIGHTING_HDR");
    const std::vector<std::uint8_t>& lighting = !lightingLdr.empty() ? lightingLdr : lightingHdr;
    const auto nodeLump = ReadLump(file, lumps[kNodeLump], fileSize, "NODES");
    const auto leafLump = ReadLump(file, lumps[kLeafLump], fileSize, "LEAFS");
    const auto leafFaces = ReadLump(file, lumps[kLeafFaceLump], fileSize, "LEAFFACES");
    const auto dispInfo = ReadLump(file, lumps[kDispInfoLump], fileSize, "DISPINFO");
    const auto dispVerts = ReadLump(file, lumps[kDispVertLump], fileSize, "DISP_VERTS");
    const auto dispTris = ReadLump(file, lumps[kDispTriLump], fileSize, "DISP_TRIS");
    const auto planeLump = ReadLump(file, lumps[kPlaneLump], fileSize, "PLANES");
    const std::size_t planeCount = planeLump.size() >= kPlaneSize && planeLump.size() % kPlaneSize == 0
        ? planeLump.size() / kPlaneSize
        : 0;

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

    std::vector<int> faceModels(totalFaceCount, 0);
    if (!modelLump.empty() && modelLump.size() % kModelSize == 0) {
        const std::size_t modelCount = modelLump.size() / kModelSize;
        for (std::size_t modelIndex = 0; modelIndex < modelCount; ++modelIndex) {
            const std::size_t modelOffset = modelIndex * kModelSize;
            const int firstFace = ReadI32(modelLump, modelOffset + 40);
            const int faceSpan = ReadI32(modelLump, modelOffset + 44);
            if (firstFace < 0 || faceSpan <= 0) {
                continue;
            }
            for (int face = firstFace; face < firstFace + faceSpan; ++face) {
                if (face >= 0 && static_cast<std::size_t>(face) < faceModels.size()) {
                    faceModels[static_cast<std::size_t>(face)] = static_cast<int>(modelIndex);
                }
            }
        }
    }

    std::vector<FacePolygon> polygons;
    std::vector<int> polygonByFace(totalFaceCount, -1);
    std::vector<Point> allPoints;
    int displacementCount = 0;
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
        const std::int16_t dispInfoIndex = ReadI16(faces, faceOffset + 12);
        FacePolygon polygon;
        polygon.faceIndex = static_cast<int>(faceIndex);
        const int planeIndex = static_cast<int>(ReadU16(faces, faceOffset));
        const bool flipPlane = faces[faceOffset + 2] != 0;
        bool hasPlaneNormal = false;
        if (planeIndex >= 0 && static_cast<std::size_t>(planeIndex) < planeCount) {
            const std::size_t planeOffset = static_cast<std::size_t>(planeIndex) * kPlaneSize;
            glm::vec3 normal = SourceDirectionToWorld(
                ReadFloat(planeLump, planeOffset),
                ReadFloat(planeLump, planeOffset + 4),
                ReadFloat(planeLump, planeOffset + 8)
            );
            if (flipPlane) {
                normal = -normal;
            }
            if (glm::dot(normal, normal) > 1.0e-8F) {
                polygon.planeNormal = glm::normalize(normal);
                hasPlaneNormal = true;
            }
        }
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
        const std::uint8_t style1 = faces[faceOffset + 17];
        const std::uint8_t style2 = faces[faceOffset + 18];
        const std::uint8_t style3 = faces[faceOffset + 19];
        const std::uint8_t styleBytes[4] = {style0, style1, style2, style3};
        int stylePages = 0;
        float extraStyle = 0.0F;
        for (const std::uint8_t style : styleBytes) {
            if (style == 255) {
                continue;
            }
            if (stylePages == 1) {
                extraStyle = static_cast<float>(style);
            }
            ++stylePages;
        }
        polygon.styleIndex = stylePages >= 2 ? extraStyle : 0.0F;
        polygon.texFlags = texFlags;
        polygon.skyPortal = (texFlags & (kSurfSky | kSurfSky2D)) != 0;
        const int luxelW = sizeS + 1;
        const int luxelH = sizeT + 1;
        if ((texFlags & kSurfNolight) == 0 && style0 != 255 && stylePages > 0 && polygon.lightmap.lightOffset >= 0
            && luxelW > 0 && luxelH > 0 && luxelW <= 2048 && luxelH <= 2048) {
            const std::size_t sampleBytes = static_cast<std::size_t>(luxelW) * static_cast<std::size_t>(luxelH) * 4U;
            const std::size_t allBytes = sampleBytes * static_cast<std::size_t>(stylePages);
            const auto lightOffset = static_cast<std::size_t>(polygon.lightmap.lightOffset);
            if (lightOffset <= lighting.size() && allBytes <= lighting.size() - lightOffset) {
                polygon.lightmap.lit = true;
                polygon.lightmap.luxelW = luxelW;
                polygon.lightmap.luxelH = luxelH;
                polygon.lightmap.stylePages = stylePages;
            }
        }
        polygon.modelIndex = faceModels[faceIndex];
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
        }
        if (!polygon.points.empty()) {
            polygon.planeAnchor = polygon.points.front();
        }
        if (!hasPlaneNormal && polygon.points.size() >= 3) {
            glm::vec3 winding(0.0F);
            for (std::size_t pointIndex = 0; pointIndex < polygon.points.size(); ++pointIndex) {
                const Point& current = polygon.points[pointIndex];
                const Point& next = polygon.points[(pointIndex + 1) % polygon.points.size()];
                winding.x += (current.y - next.y) * (current.z + next.z);
                winding.y += (current.z - next.z) * (current.x + next.x);
                winding.z += (current.x - next.x) * (current.y + next.y);
            }
            if (glm::dot(winding, winding) > 1.0e-8F) {
                polygon.planeNormal = -glm::normalize(winding);
            }
        }
        if (dispInfoIndex >= 0 && edgeCount == 4
            && BuildDisplacement(polygon, dispInfo, dispVerts, dispTris, dispInfoIndex, texInfo, texData)) {
            ++displacementCount;
        }
        allPoints.insert(allPoints.end(), polygon.points.begin(), polygon.points.end());
        polygonByFace[faceIndex] = static_cast<int>(polygons.size());
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
    const Point center{0.0F, 0.0F, 0.0F};
    const float scale = kSourceToWorld;
    static_cast<void>(minimum);
    static_cast<void>(maximum);
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

    std::vector<glm::vec3> propPositions;
    std::vector<BspMapEntity> mapEntities;
    std::vector<BspVisibility::PortalEntity> portalEntities;
    std::vector<BspMapLight> mapLights;
    BspMapLight environmentLight{};
    bool hasEnvironmentLight = false;
    std::vector<DecalSpawn> mapDecals;
    std::vector<DecalSpawn> entityOverlays;
    std::unordered_map<std::string, std::pair<float, float>> decalExtentCache;
    std::vector<SpriteSpawn> mapSprites;
    glm::vec3 playerStartPosition{0.0F};
    bool hasPlayerStartPosition = false;
    std::string skyName;
    BspSkyCamera skyCamera{};
    BspFog worldFog{};
    bool worldFogSet = false;
    Point skySourceOrigin{};
    bool hasSkySource = false;
    std::array<std::string, 64> lightStyles{};
    FillDefaultLightStyles(lightStyles);
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
        if (name == "worldspawn") {
            const std::string sky = EntityKeyValue(properties, "skyname");
            if (!sky.empty()) {
                skyName = sky;
            }
            continue;
        }
        {
            const int style = static_cast<int>(PropertyFloat(properties, "style", 0.0F));
            const std::string pattern = EntityKeyValue(properties, "pattern");
            if (!pattern.empty() && style > 0 && style < 64) {
                lightStyles[static_cast<std::size_t>(style)] = pattern;
            }
        }
        if (name == "env_fog_controller") {
            const BspFog fog = ParseFog(properties);
            if (!worldFogSet || (fog.enabled && !worldFog.enabled)) {
                worldFog = fog;
                worldFogSet = true;
            }
        }
        glm::vec3 worldPosition{0.0F};
        std::smatch origin;
        const bool hasOrigin = std::regex_search(block, origin, originRegex);
        if (hasOrigin) {
            const Point sourceOrigin{
                std::stof(origin[1].str()),
                std::stof(origin[2].str()),
                std::stof(origin[3].str()),
            };
            worldPosition = toWorldPosition(sourceOrigin);
            if (name == "sky_camera") {
                skySourceOrigin = Point{
                    std::stof(origin[1].str()),
                    std::stof(origin[2].str()),
                    std::stof(origin[3].str()),
                };
                hasSkySource = true;
            }
        }
        if (name == "sky_camera") {
            skyCamera.present = hasOrigin;
            skyCamera.origin = worldPosition;
            float pitch = 0.0F;
            float yaw = 0.0F;
            float roll = 0.0F;
            static_cast<void>(ParseEntityAngles(properties, pitch, yaw, roll));
            skyCamera.pitch = pitch;
            skyCamera.yaw = yaw;
            skyCamera.roll = roll;
            const float scale = PropertyFloat(properties, "scale", 16.0F);
            skyCamera.scale = scale >= 1.0F ? scale : 16.0F;
            skyCamera.fog = ParseFog(properties);
        }
        if (!hasPlayerStartPosition && (name == "info_player_start" || name == "info_player_deathmatch")) {
            playerStartPosition = worldPosition;
            hasPlayerStartPosition = true;
        }
        if (IsDecalClassname(name)) {
            const std::string material = ResolveEntityTexture(properties);
            if (!material.empty()) {
                DecalSpawn decal;
                decal.position = worldPosition;
                decal.material = material;
                const auto cachedExtent = decalExtentCache.find(material);
                if (cachedExtent != decalExtentCache.end()) {
                    decal.halfWidth = cachedExtent->second.first;
                    decal.halfHeight = cachedExtent->second.second;
                } else {
                    DecalHalfExtents(files, material, decal.halfWidth, decal.halfHeight);
                    decalExtentCache.emplace(material, std::make_pair(decal.halfWidth, decal.halfHeight));
                }
                mapDecals.push_back(std::move(decal));
            }
            continue;
        }
        BspMapLight builtLight;
        if (TryBuildMapLight(name, properties, worldPosition, hasOrigin, builtLight)) {
            if (builtLight.kind == BspLightKind::Sun) {
                if (!hasEnvironmentLight || builtLight.intensity > environmentLight.intensity) {
                    environmentLight = builtLight;
                    hasEnvironmentLight = true;
                }
            } else {
                mapLights.push_back(builtLight);
            }
        }
        if (name == "info_overlay") {
            DecalSpawn overlay;
            if (TryBuildOverlay(properties, worldPosition, overlay)) {
                const std::string sides = EntityKeyValue(properties, "sides");
                std::string cleaned;
                cleaned.reserve(sides.size());
                for (const char character : sides) {
                    const bool keep = (character >= '0' && character <= '9') || character == '-'
                        || std::isspace(static_cast<unsigned char>(character)) != 0;
                    cleaned.push_back(keep ? character : ' ');
                }
                std::istringstream sideStream(cleaned);
                int sideFace = 0;
                while (sideStream >> sideFace) {
                    if (sideFace >= 0) {
                        overlay.sideFaces.push_back(sideFace);
                    }
                }
                entityOverlays.push_back(std::move(overlay));
            }
        }
        SpriteSpawn sprite;
        if (TryBuildSprite(name, properties, worldPosition, hasOrigin, sprite)) {
            mapSprites.push_back(std::move(sprite));
        }
        if (SameEntityName(name, "func_areaportal") || SameEntityName(name, "func_areaportalwindow")) {
            BspVisibility::PortalEntity portalEntity;
            portalEntity.number = static_cast<int>(PropertyFloat(properties, "portalnumber", 0.0F));
            const std::string portalNumber = EntityKeyValueCi(properties, "portalnumber");
            if (!portalNumber.empty()) {
                try {
                    portalEntity.number = std::stoi(portalNumber);
                } catch (const std::exception&) {
                    portalEntity.number = 0;
                }
            }
            portalEntity.target = EntityKeyValueCi(properties, "target");
            const std::string startOpen = EntityKeyValueCi(properties, "StartOpen");
            if (!startOpen.empty()) {
                portalEntity.startOpen = startOpen != "0";
            } else {
                const int spawnFlags = static_cast<int>(PropertyFloat(properties, "spawnflags", 0.0F));
                portalEntity.startOpen = (spawnFlags & 1) != 0;
            }
            portalEntities.push_back(std::move(portalEntity));
        }
        BspMapEntity entity;
        entity.position = worldPosition;
        entity.classname = name;
        entity.model = EntityKeyValue(properties, "model");
        entity.texture = ResolveEntityTexture(properties);
        entity.properties = properties;
        if (!entity.model.empty() && entity.model[0] == '*') {
            try {
                entity.brushModel = std::stoi(entity.model.substr(1));
            } catch (const std::exception&) {
                entity.brushModel = -1;
            }
        }
        mapEntities.push_back(std::move(entity));
    }

    int staticPropCount = 0;
    try {
        const auto gameLump = ReadLump(file, lumps[kGameLump], fileSize, "GAME_LUMP");
        staticPropCount = AppendStaticProps(gameLump, file, fileSize, mapEntities);
        const int detailCount = AppendDetailProps(gameLump, file, fileSize, mapEntities, mapSprites);
        if (detailCount > 0) {
            std::cout << "Detail props " << detailCount << std::endl;
        }
    } catch (const std::exception& error) {
        std::cerr << "Skipping static props: " << error.what() << '\n';
    }

    if (hasSkySource) {
        const SkySelection skySelection = CollectSkyFaces(
            nodeLump,
            planeLump,
            planeCount,
            leafLump,
            leafFaces,
            polygons,
            skySourceOrigin,
            bspVersion,
            lumps[kLeafLump].version,
            totalFaceCount
        );
        const std::unordered_set<int>& skyFaces = skySelection.faces;
        std::cout << "3D sky faces " << skyFaces.size() << std::endl;
        for (FacePolygon& polygon : polygons) {
            if (polygon.modelIndex == 0 && skyFaces.find(polygon.faceIndex) != skyFaces.end()) {
                polygon.sky = true;
            }
        }
        const std::size_t leafStride = LeafStrideForVersion(bspVersion, lumps[kLeafLump].version, leafLump.size());
        const std::size_t leafCount = leafStride == 0 ? 0 : leafLump.size() / leafStride;
        const float skyRadiusSquared = kSkyRadius * kSkyRadius;
        int skyEntityCount = 0;
        for (BspMapEntity& entity : mapEntities) {
            if (skySelection.areaAccepted) {
                const int leaf = PointLeafIndex(
                    WorldToSourcePoint(entity.position),
                    nodeLump,
                    planeLump,
                    planeCount,
                    leafCount
                );
                entity.inSkybox = LeafAreaValue(leafLump, leafStride, leaf) == skySelection.area;
            } else if (!skyFaces.empty()) {
                const Point source = WorldToSourcePoint(entity.position);
                const float dx = source.x - skySourceOrigin.x;
                const float dy = source.y - skySourceOrigin.y;
                const float dz = source.z - skySourceOrigin.z;
                entity.inSkybox = dx * dx + dy * dy + dz * dz <= skyRadiusSquared;
            }
            if (entity.inSkybox) {
                ++skyEntityCount;
            }
        }
        std::cout << "3D sky entities " << skyEntityCount << std::endl;
        bool anyWorldPoint = false;
        Point worldMin{};
        Point worldMax{};
        for (const FacePolygon& polygon : polygons) {
            if (polygon.sky) {
                continue;
            }
            for (const Point& point : polygon.points) {
                if (!anyWorldPoint) {
                    worldMin = point;
                    worldMax = point;
                    anyWorldPoint = true;
                    continue;
                }
                worldMin.x = std::min(worldMin.x, point.x);
                worldMin.y = std::min(worldMin.y, point.y);
                worldMin.z = std::min(worldMin.z, point.z);
                worldMax.x = std::max(worldMax.x, point.x);
                worldMax.y = std::max(worldMax.y, point.y);
                worldMax.z = std::max(worldMax.z, point.z);
            }
        }
        if (anyWorldPoint) {
            worldMinimum = glm::vec3(
                (worldMin.x - center.x) * scale,
                (worldMin.y - center.y) * scale,
                (worldMin.z - center.z) * scale
            );
            worldMaximum = glm::vec3(
                (worldMax.x - center.x) * scale,
                (worldMax.y - center.y) * scale,
                (worldMax.z - center.z) * scale
            );
        }
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
        const std::size_t texelCount = static_cast<std::size_t>(atlas.width) * static_cast<std::size_t>(atlas.height) * 3U;
        atlas.rgb.assign(texelCount, 0.0F);
        atlas.rgbStyle.assign(texelCount, 0.0F);
        const std::array<float, 3> white{1.0F, 1.0F, 1.0F};
        WriteAtlasPixel(atlas.rgb, atlas.width, atlas.height, 0, 0, white);
        WriteAtlasPixel(atlas.rgb, atlas.width, atlas.height, 1, 0, white);
        WriteAtlasPixel(atlas.rgb, atlas.width, atlas.height, 0, 1, white);
        WriteAtlasPixel(atlas.rgb, atlas.width, atlas.height, 1, 1, white);
        for (const FacePolygon& polygon : polygons) {
            if (!polygon.lightmap.lit) {
                continue;
            }
            BlitFaceLightmap(atlas.rgb, atlas.width, atlas.height, polygon.lightmap, lighting, 0);
            if (polygon.styleIndex > 0.0F) {
                BlitFaceLightmap(atlas.rgbStyle, atlas.width, atlas.height, polygon.lightmap, lighting, 1);
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

    try {
        const auto overlayLump = ReadLump(file, lumps[kOverlayLump], fileSize, "OVERLAYS");
        if (overlayLump.size() >= kOverlayLumpSize && overlayLump.size() % kOverlayLumpSize == 0) {
            const std::size_t overlayCount = overlayLump.size() / kOverlayLumpSize;
            int addedOverlays = 0;
            for (std::size_t overlayIndex = 0; overlayIndex < overlayCount; ++overlayIndex) {
                const std::size_t base = overlayIndex * kOverlayLumpSize;
                const std::int16_t texInfoIndex = ReadI16(overlayLump, base + 4);
                const int faceCount = static_cast<int>(ReadU16(overlayLump, base + 6) & 0x3FFFU);
                const std::string material = ReadMaterial(texInfo, texData, stringTable, stringData, texInfoIndex);
                if (material.empty() || faceCount <= 0) {
                    continue;
                }
                glm::vec3 uv[4];
                for (int corner = 0; corner < 4; ++corner) {
                    const std::size_t uvOffset = base + 280 + static_cast<std::size_t>(corner) * 12;
                    uv[corner] = ::SourcePointToWorld(
                        ReadFloat(overlayLump, uvOffset),
                        ReadFloat(overlayLump, uvOffset + 4),
                        ReadFloat(overlayLump, uvOffset + 8)
                    );
                }
                const glm::vec3 basisU = uv[1] - uv[0];
                const glm::vec3 basisV = uv[3] - uv[0];
                if (glm::length(basisU) <= 1.0e-5F || glm::length(basisV) <= 1.0e-5F) {
                    continue;
                }
                DecalSpawn decal;
                decal.explicitQuad = true;
                decal.material = material;
                decal.basisOrigin = uv[0];
                decal.basisU = basisU;
                decal.basisV = basisV;
                decal.position = uv[0] + (basisU + basisV) * 0.5F;
                glm::vec3 normal = glm::cross(basisU, basisV);
                const float storedNx = ReadFloat(overlayLump, base + 340);
                const float storedNy = ReadFloat(overlayLump, base + 344);
                const float storedNz = ReadFloat(overlayLump, base + 348);
                if (std::abs(storedNx) + std::abs(storedNy) + std::abs(storedNz) > 1.0e-4F) {
                    normal = ::SourceDirectionToWorld(storedNx, storedNy, storedNz);
                }
                if (glm::length(normal) <= 1.0e-6F) {
                    normal = glm::vec3(0.0F, 1.0F, 0.0F);
                } else {
                    normal = glm::normalize(normal);
                }
                decal.normal = normal;
                const float uLength = glm::length(basisU);
                const float vLength = glm::length(basisV);
                decal.halfWidth = std::max(uLength * 0.5F, 0.01F);
                decal.halfHeight = std::max(vLength * 0.5F, 0.01F);
                decal.sAxis = basisU / uLength;
                decal.tAxis = basisV / vLength;
                const float startU = ReadFloat(overlayLump, base + 264);
                const float endU = ReadFloat(overlayLump, base + 268);
                const float startV = ReadFloat(overlayLump, base + 272);
                const float endV = ReadFloat(overlayLump, base + 276);
                decal.uvs = {{{startU, startV}, {endU, startV}, {endU, endV}, {startU, endV}}};
                const int listed = std::min(faceCount, 64);
                for (int faceSlot = 0; faceSlot < listed; ++faceSlot) {
                    const int faceId = ReadI32(overlayLump, base + 8 + static_cast<std::size_t>(faceSlot) * 4);
                    if (faceId >= 0) {
                        decal.sideFaces.push_back(faceId);
                    }
                }
                if (decal.sideFaces.empty()) {
                    continue;
                }
                decal.faceIndex = decal.sideFaces.front();
                mapDecals.push_back(std::move(decal));
                ++addedOverlays;
            }
            if (addedOverlays > 0) {
                std::cout << "BSP overlays " << addedOverlays << std::endl;
            }
        }
    } catch (const std::exception& error) {
        std::cerr << "Skipping overlays: " << error.what() << '\n';
    }
    if (entityOverlays.empty() == false) {
        const bool lumpHasOverlays = std::any_of(mapDecals.begin(), mapDecals.end(), [](const DecalSpawn& decal) {
            return decal.explicitQuad && !decal.sideFaces.empty();
        });
        if (!lumpHasOverlays) {
            mapDecals.insert(mapDecals.end(), std::make_move_iterator(entityOverlays.begin()), std::make_move_iterator(entityOverlays.end()));
        }
    }

    for (DecalSpawn& decal : mapDecals) {
        if (decal.explicitQuad) {
            continue;
        }
        ResolveDecalBasisFromBsp(
            decal.position,
            polygons,
            scale,
            decal.normal,
            decal.sAxis,
            decal.tAxis,
            decal.faceIndex
        );
    }
    ProjectDecalsOntoFaces(
        mapDecals,
        polygons,
        polygonByFace,
        texInfo,
        scale,
        packed,
        atlasWidth,
        atlasHeight
    );

    if (packed) {
        mapLights.erase(
            std::remove_if(mapLights.begin(), mapLights.end(), [](const BspMapLight& light) {
                return !light.dynamic;
            }),
            mapLights.end()
        );
    }

    LoadedGeometry loaded;
    loaded.lightmap = std::move(atlas);
    loaded.propPositions = std::move(propPositions);
    loaded.mapEntities = std::move(mapEntities);
    loaded.mapLights = std::move(mapLights);
    loaded.environmentLight = environmentLight;
    loaded.hasEnvironmentLight = hasEnvironmentLight;
    loaded.skyName = std::move(skyName);
    loaded.skyCamera = skyCamera;
    loaded.worldFog = worldFog;
    loaded.lightStyles = std::move(lightStyles);
    loaded.displacementCount = displacementCount;
    loaded.decals = std::move(mapDecals);
    loaded.sprites = std::move(mapSprites);
    loaded.staticPropCount = staticPropCount;
    loaded.playerStartPosition = playerStartPosition;
    {
        const auto visLump = ReadLump(file, lumps[kVisibilityLump], fileSize, "VISIBILITY");
        const auto areaLump = ReadLump(file, lumps[kAreaLump], fileSize, "AREAS");
        const auto portalLump = ReadLump(file, lumps[kAreaPortalLump], fileSize, "AREAPORTALS");
        const auto clipVertLump = ReadLump(file, lumps[kClipPortalVertLump], fileSize, "CLIPPORTALVERTS");
        const std::size_t leafStride = LeafStrideForVersion(bspVersion, lumps[kLeafLump].version, leafLump.size());
        loaded.visibility = BuildVisibility(
            nodeLump,
            planeLump,
            leafLump,
            leafStride,
            leafFaces,
            visLump,
            areaLump,
            portalLump,
            clipVertLump,
            ReadLump(file, lumps[kLeafWaterLump], fileSize, "LEAFWATERDATA"),
            totalFaceCount
        );
        loaded.visibility.portalEntities = std::move(portalEntities);
        ApplyPortalEntities(loaded.visibility);
    }
    loaded.hasPlayerStartPosition = hasPlayerStartPosition;
    try {
        const auto cubemapLump = ReadLump(file, lumps[kCubemapLump], fileSize, "CUBEMAPS");
        if (cubemapLump.size() >= 16 && cubemapLump.size() % 16U == 0) {
            const std::size_t sampleCount = cubemapLump.size() / 16U;
            loaded.cubemaps.reserve(sampleCount);
            for (std::size_t sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex) {
                const std::size_t base = sampleIndex * 16U;
                CubemapSample sample;
                sample.x = ReadI32(cubemapLump, base);
                sample.y = ReadI32(cubemapLump, base + 4);
                sample.z = ReadI32(cubemapLump, base + 8);
                sample.position = ::SourcePointToWorld(
                    static_cast<float>(sample.x),
                    static_cast<float>(sample.y),
                    static_cast<float>(sample.z)
                );
                loaded.cubemaps.push_back(sample);
            }
        }
    } catch (const std::exception& error) {
        std::cerr << "Skipping cubemaps: " << error.what() << '\n';
    }
    try {
        const auto lightLump = ReadLump(file, lumps[kWorldLightLump], fileSize, "WORLDLIGHTS");
        if (lightLump.size() >= kWorldLightSize && lightLump.size() % kWorldLightSize == 0) {
            const std::size_t lightCount = lightLump.size() / kWorldLightSize;
            const float inchesPerWorld = 1.0F / kSourceToWorld;
            for (std::size_t lightIndex = 0; lightIndex < lightCount; ++lightIndex) {
                const std::size_t base = lightIndex * kWorldLightSize;
                if (ReadI32(lightLump, base + 44) != 0) {
                    continue;
                }
                const int type = ReadI32(lightLump, base + 40);
                FillLight light;
                light.position = ::SourcePointToWorld(
                    ReadFloat(lightLump, base),
                    ReadFloat(lightLump, base + 4),
                    ReadFloat(lightLump, base + 8)
                );
                light.color = glm::vec3(
                    ReadFloat(lightLump, base + 12),
                    ReadFloat(lightLump, base + 16),
                    ReadFloat(lightLump, base + 20)
                );
                light.direction = ::SourceDirectionToWorld(
                    ReadFloat(lightLump, base + 24),
                    ReadFloat(lightLump, base + 28),
                    ReadFloat(lightLump, base + 32)
                );
                if (glm::length(light.direction) > 1.0e-4F) {
                    light.direction = glm::normalize(light.direction);
                }
                const float radius = ReadFloat(lightLump, base + 60);
                light.radius = radius > 0.0F ? radius * kSourceToWorld : 0.0F;
                light.constant = ReadFloat(lightLump, base + 64);
                light.linear = ReadFloat(lightLump, base + 68) * inchesPerWorld;
                light.quadratic = ReadFloat(lightLump, base + 72) * inchesPerWorld * inchesPerWorld;
                light.stopDot = ReadFloat(lightLump, base + 48);
                light.stopDot2 = ReadFloat(lightLump, base + 52);
                if (type == 3 || type == 5) {
                    light.ambient = true;
                } else if (type == 2) {
                    light.spot = true;
                }
                if (glm::length(light.color) <= 1.0e-4F) {
                    continue;
                }
                loaded.fillLights.push_back(light);
            }
        }
    } catch (const std::exception& error) {
        std::cerr << "Skipping world lights: " << error.what() << '\n';
    }
    int brushCount = 0;
    for (const FacePolygon& polygon : polygons) {
        brushCount = std::max(brushCount, polygon.modelIndex + 1);
    }
    loaded.brushes.resize(static_cast<std::size_t>(std::max(brushCount, 1)));
    for (std::size_t polygonIndex = 0; polygonIndex < polygons.size(); ++polygonIndex) {
        const FacePolygon& polygon = polygons[polygonIndex];
        const bool hidden = polygon.skyPortal;
        const bool drawSky = polygon.sky && !hidden && polygon.modelIndex == 0;
        std::string lowercaseMaterial = polygon.material;
        for (char& character : lowercaseMaterial) {
            character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
        }
        const bool faceTrigger = lowercaseMaterial.find("trigger") != std::string::npos
            || (polygon.texFlags & kSurfTrigger) != 0;
        const bool faceCollides = FaceCollides(lowercaseMaterial, polygon.texFlags);
        std::vector<BatchGeometry>& destination = polygon.modelIndex > 0
            ? loaded.brushes[static_cast<std::size_t>(polygon.modelIndex)].batches
            : (drawSky ? loaded.skyBatches : loaded.batches);
        auto batch = std::find_if(destination.begin(), destination.end(), [&](const BatchGeometry& candidate) {
            const bool faceWarp = (polygon.texFlags & kSurfWarp) != 0;
            return candidate.material == polygon.material
                && candidate.hidden == hidden
                && candidate.trigger == faceTrigger
                && candidate.collide == faceCollides
                && candidate.warp == faceWarp;
        });
        if (batch == destination.end()) {
            BatchGeometry created;
            created.material = polygon.material;
            created.trigger = faceTrigger;
            created.hidden = hidden;
            created.collide = faceCollides;
            created.warp = (polygon.texFlags & kSurfWarp) != 0;
            destination.push_back(std::move(created));
            batch = std::prev(destination.end());
        }
        const unsigned int baseVertex = static_cast<unsigned int>(
            batch->vertices.size() / Mesh::kFloatsPerVertex
        );
        const bool fillUnlit = !polygon.lightmap.lit && !loaded.fillLights.empty();
        const float styleValue = fillUnlit ? -1.0F : polygon.styleIndex;
        const float red = 0.35F + static_cast<float>((polygonIndex * 37) % 55) / 100.0F;
        const float green = 0.35F + static_cast<float>((polygonIndex * 61) % 55) / 100.0F;
        const float blue = 0.35F + static_cast<float>((polygonIndex * 83) % 55) / 100.0F;
        for (std::size_t vertexIndex = 0; vertexIndex < polygon.points.size(); ++vertexIndex) {
            const Point& point = polygon.points[vertexIndex];
            glm::vec3 tint(red, green, blue);
            if (fillUnlit) {
                tint = ShadeFill(FaceVertexWorld(point, scale), polygon.planeNormal, loaded.fillLights);
            }
            batch->vertices.insert(batch->vertices.end(), {
                (point.x - center.x) * scale,
                (point.y - center.y) * scale,
                (point.z - center.z) * scale,
                tint.r,
                tint.g,
                tint.b,
                polygon.uvs[vertexIndex][0],
                polygon.uvs[vertexIndex][1],
                polygon.lightmapUvs[vertexIndex][0],
                polygon.lightmapUvs[vertexIndex][1],
                styleValue,
            });
        }
        std::vector<unsigned int> faceIndices = polygon.gridIndices;
        if (faceIndices.empty()) {
            faceIndices = EarClipPolygon(polygon.points);
        }
        if (faceIndices.empty()) {
            for (unsigned int vertexIndex = 1; vertexIndex + 1 < polygon.points.size(); ++vertexIndex) {
                faceIndices.insert(faceIndices.end(), {0U, vertexIndex, vertexIndex + 1U});
            }
        }
        const std::uint32_t firstIndex = static_cast<std::uint32_t>(batch->indices.size());
        for (const unsigned int index : faceIndices) {
            batch->indices.push_back(baseVertex + index);
        }
        const std::uint32_t indexCount = static_cast<std::uint32_t>(batch->indices.size()) - firstIndex;
        if (indexCount > 0 && &destination == &loaded.batches && polygon.modelIndex == 0) {
            batch->faceSpans.push_back(FaceSpan{polygon.faceIndex, firstIndex, indexCount});
        }
    }
    if (!loaded.skyName.empty()) {
        std::cout << "Sky name " << loaded.skyName << std::endl;
    }
    if (loaded.skyCamera.present) {
        std::cout << "3D skybox scale " << loaded.skyCamera.scale << std::endl;
    }
    if (displacementCount > 0) {
        std::cout << "Displacements " << displacementCount << std::endl;
    }
    return loaded;
}
}

struct BspLoader::Batch {
    explicit Batch(BatchGeometry geometry)
        : mesh(std::make_unique<Mesh>(geometry.vertices, geometry.indices)),
          faceSpans(std::move(geometry.faceSpans)),
          material(std::move(geometry.material)),
          trigger(geometry.trigger),
          hidden(geometry.hidden),
          warp(geometry.warp) {
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
    std::vector<FaceSpan> faceSpans;
    std::string material;
    GLuint texture = 0;
    bool ownsTexture = true;
    bool trigger = false;
    bool hidden = false;
    bool warp = false;
    bool useEnvmap = false;
    glm::vec3 envmapTint{1.0F};
    int alphaMode = 0;
    glm::vec3 aabbMin{1.0e9F};
    glm::vec3 aabbMax{-1.0e9F};
};

struct BspLoader::SkyboxFace {
    std::unique_ptr<Mesh> mesh;
    GLuint texture = 0;
    bool ownsTexture = true;

    ~SkyboxFace() {
        if (ownsTexture && texture != 0) {
            glDeleteTextures(1, &texture);
        }
    }
};

struct BspLoader::Decal {
    explicit Decal(BatchGeometry geometry, LoadedMaterialTexture loadedTexture)
        : mesh(std::make_unique<Mesh>(geometry.vertices, geometry.indices)),
          material(std::move(geometry.material)),
          texture(loadedTexture.texture),
          ownsTexture(loadedTexture.ownsTexture),
          alphaMode(loadedTexture.alphaMode == 0 ? 1 : loadedTexture.alphaMode) {
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
    int alphaMode = 1;
    std::vector<int> faces;
    glm::vec3 aabbMin{1.0e9F};
    glm::vec3 aabbMax{-1.0e9F};
};

struct BspLoader::BrushModel {
    struct Part {
        std::unique_ptr<Mesh> mesh;
        GLuint texture = 0;
        bool ownsTexture = true;
        int alphaMode = 0;
        bool warp = false;
        bool useEnvmap = false;
        bool glass = false;
        glm::vec3 envmapTint{1.0F};
    };

    std::vector<Part> parts;
    std::vector<glm::vec3> collisionVertices;
    std::vector<std::uint32_t> collisionIndices;
    glm::vec3 aabbMin{1.0e9F};
    glm::vec3 aabbMax{-1.0e9F};

    ~BrushModel() {
        for (Part& part : parts) {
            if (part.ownsTexture && part.texture != 0) {
                glDeleteTextures(1, &part.texture);
            }
        }
    }
};

BspLoader::BspLoader(const std::string& path, const GameFileSystem* files) {
    LoadedGeometry loaded = LoadGeometry(path, faceCount_, worldMinimum_, worldMaximum_, files);
    std::vector<BatchGeometry> geometry = std::move(loaded.batches);
    propPositions_ = std::move(loaded.propPositions);
    mapEntities_ = std::move(loaded.mapEntities);
    for (BspMapEntity& entity : mapEntities_) {
        if (!entity.hasLightingOrigin || loaded.fillLights.empty()) {
            continue;
        }
        if (glm::length(entity.lightTint - glm::vec3(0.35F)) > 0.02F) {
            continue;
        }
        const glm::vec3 shade = ShadeFill(entity.lightingOrigin, glm::vec3(0.0F), loaded.fillLights);
        if (glm::length(shade) > 0.05F) {
            entity.lightTint = shade;
        }
    }
    mapLights_ = std::move(loaded.mapLights);
    environmentLight_ = loaded.environmentLight;
    hasEnvironmentLight_ = loaded.hasEnvironmentLight;
    skyName_ = std::move(loaded.skyName);
    skyCamera_ = loaded.skyCamera;
    worldFog_ = loaded.worldFog;
    lightStyles_ = std::move(loaded.lightStyles);
    const std::vector<DecalSpawn> decalSpawns = std::move(loaded.decals);
    const std::vector<SpriteSpawn> spriteSpawns = std::move(loaded.sprites);
    const int staticPropCount = loaded.staticPropCount;
    playerStartPosition_ = loaded.playerStartPosition;
    hasPlayerStartPosition_ = loaded.hasPlayerStartPosition;
    visibility_ = std::make_unique<BspVisibility>(std::move(loaded.visibility));
    std::cout << "BSP visibility clusters " << visibility_->clusterCount
              << ", areas " << visibility_->areas.size()
              << ", portals " << visibility_->portals.size() << std::endl;
    if (files != nullptr) {
        std::string stem = std::filesystem::path(path).stem().string();
        for (char& character : stem) {
            character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
        }
        for (const CubemapSample& sample : loaded.cubemaps) {
            const std::string relative = "materials/maps/" + stem + "/c" + std::to_string(sample.x) + "_"
                + std::to_string(sample.y) + "_" + std::to_string(sample.z) + ".vtf";
            std::vector<std::uint8_t> bytes;
            if (!files->Read(relative, bytes)) {
                continue;
            }
            const GLuint texture = vtf::LoadVtfCubemapData(bytes, relative);
            if (texture == 0) {
                continue;
            }
            cubemaps_.push_back(CubemapProbe{sample.position, texture});
        }
        if (!loaded.cubemaps.empty()) {
            std::cout << "Cubemap probes " << cubemaps_.size() << "/" << loaded.cubemaps.size() << std::endl;
        }
    }
    if (loaded.lightmap.width > 0 && !loaded.lightmap.rgb.empty()) {
        lightmapTexture_ = UploadLightmapAtlas(loaded.lightmap);
        std::cout << "Lightmap atlas " << loaded.lightmap.width << "x" << loaded.lightmap.height << std::endl;
    }
    std::cout << "BSP geometry loaded: " << faceCount_ << " faces, " << geometry.size() << " materials, "
              << mapEntities_.size() << " entities, " << staticPropCount << " static props, "
              << mapLights_.size() << " lights, " << decalSpawns.size() << " decals, "
              << spriteSpawns.size() << " sprites"
              << std::endl;
    for (const BatchGeometry& batchGeometry : geometry) {
        if (!batchGeometry.collide) {
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
    const std::vector<std::filesystem::path> textureRoots = {
        std::filesystem::path(BLUEPRINT_SOURCE_DIR) / "textures",
        std::filesystem::current_path() / "textures",
        gameDirectory / "textures",
        gameDirectory / "materials",
        mapDirectory / "materials",
    };
    const std::vector<std::filesystem::path> vpkRoots = {
        std::filesystem::path(BLUEPRINT_SOURCE_DIR) / "textures",
        std::filesystem::current_path() / "textures",
        gameDirectory,
        gameDirectory / "textures",
    };
    std::vector<std::filesystem::path> vpkArchives;
    for (const auto& vpkRoot : vpkRoots) {
        if (!std::filesystem::exists(vpkRoot) || !std::filesystem::is_directory(vpkRoot)) {
            continue;
        }
        for (const auto& entry : std::filesystem::directory_iterator(vpkRoot)) {
            const std::string vpkName = entry.path().filename().string();
            if (entry.is_regular_file() && vpkName.size() >= 8
                && vpkName.substr(vpkName.size() - 8) == "_dir.vpk"
                && std::find(vpkArchives.begin(), vpkArchives.end(), entry.path()) == vpkArchives.end()) {
                vpkArchives.push_back(entry.path());
            }
        }
    }
    MaterialTextureContext textureContext{textureRoots, vpkArchives, &placeholderTexture_, files};
    std::unordered_map<std::string, LoadedMaterialTexture> materialCache;

    auto getOrLoadTexture = [&](const std::string& mat) -> LoadedMaterialTexture {
        if (mat.empty()) return {};
        const auto it = materialCache.find(mat);
        if (it != materialCache.end()) {
            LoadedMaterialTexture cached = it->second;
            cached.ownsTexture = false;
            return cached;
        }
        const LoadedMaterialTexture loaded = LoadMaterialTexture(mat, textureContext);
        if (loaded.texture != 0) {
            materialCache[mat] = loaded;
        }
        return loaded;
    };

    brushModels_.resize(loaded.brushes.size());
    for (std::size_t modelIndex = 0; modelIndex < loaded.brushes.size(); ++modelIndex) {
        auto brush = std::make_unique<BrushModel>();
        for (BatchGeometry& batchGeometry : loaded.brushes[modelIndex].batches) {
            if (batchGeometry.vertices.empty() || batchGeometry.indices.empty()) {
                continue;
            }
            BrushModel::Part part;
            part.mesh = std::make_unique<Mesh>(batchGeometry.vertices, batchGeometry.indices);
            part.warp = batchGeometry.warp;
            {
                std::string lowered = batchGeometry.material;
                for (char& character : lowered) {
                    character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
                }
                part.glass = lowered.find("glass") != std::string::npos
                    || (lowered.find("window") != std::string::npos && lowered.find("frame") == std::string::npos);
            }
            if (!batchGeometry.material.empty()) {
                const LoadedMaterialTexture loadedTexture = getOrLoadTexture(batchGeometry.material);
                part.texture = loadedTexture.texture;
                part.ownsTexture = loadedTexture.ownsTexture;
                part.alphaMode = loadedTexture.alphaMode;
                part.useEnvmap = loadedTexture.envmap;
                part.envmapTint = loadedTexture.envmapTint;
                if (part.texture != 0) {
                    ++textureCount_;
                }
            }
            const bool solid = batchGeometry.collide;
            const std::uint32_t baseVertex = static_cast<std::uint32_t>(brush->collisionVertices.size());
            for (std::size_t offset = 0;
                 offset + (Mesh::kFloatsPerVertex - 1) < batchGeometry.vertices.size();
                 offset += Mesh::kFloatsPerVertex) {
                const glm::vec3 point(
                    batchGeometry.vertices[offset],
                    batchGeometry.vertices[offset + 1],
                    batchGeometry.vertices[offset + 2]
                );
                brush->aabbMin = glm::min(brush->aabbMin, point);
                brush->aabbMax = glm::max(brush->aabbMax, point);
                if (solid) {
                    brush->collisionVertices.push_back(point);
                }
            }
            if (solid) {
                for (unsigned int index : batchGeometry.indices) {
                    brush->collisionIndices.push_back(baseVertex + index);
                }
            }
            brush->parts.push_back(std::move(part));
        }
        brushModels_[modelIndex] = std::move(brush);
    }

    for (BatchGeometry& batchGeometry : geometry) {
        auto batch = std::make_unique<Batch>(std::move(batchGeometry));
        if (!batch->material.empty()) {
            const LoadedMaterialTexture loadedTexture = getOrLoadTexture(batch->material);
            batch->texture = loadedTexture.texture;
            batch->ownsTexture = loadedTexture.ownsTexture;
            batch->alphaMode = loadedTexture.alphaMode;
            batch->useEnvmap = loadedTexture.envmap;
            batch->envmapTint = loadedTexture.envmapTint;
        }
        if (batch->texture != 0) {
            ++textureCount_;
        }
        batches_.push_back(std::move(batch));
    }
    for (const DecalSpawn& spawn : decalSpawns) {
        BatchGeometry decalGeometry = BuildDecalGeometry(spawn);
        if (decalGeometry.vertices.empty() || decalGeometry.indices.empty()) {
            continue;
        }
        const LoadedMaterialTexture loadedTexture = getOrLoadTexture(spawn.material);
        if (loadedTexture.texture == 0) {
            std::cerr << "Skipping decal with missing texture: " << spawn.material << '\n';
            continue;
        }
        auto decal = std::make_unique<Decal>(std::move(decalGeometry), loadedTexture);
        decal->faces = spawn.sideFaces;
        if (spawn.faceIndex >= 0) {
            decal->faces.push_back(spawn.faceIndex);
        }
        ++textureCount_;
        decals_.push_back(std::move(decal));
    }

    {
        const std::vector<float> quadVertices = {
            -1.0F, -1.0F, 0.0F, 1.0F, 1.0F, 1.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F,
             1.0F, -1.0F, 0.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 0.0F, 0.0F, 0.0F,
             1.0F,  1.0F, 0.0F, 1.0F, 1.0F, 1.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F,
            -1.0F,  1.0F, 0.0F, 1.0F, 1.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F,
        };
        spriteQuad_ = std::make_unique<Mesh>(quadVertices, std::vector<unsigned int>{0, 1, 2, 0, 2, 3});
    }
    for (const SpriteSpawn& spawn : spriteSpawns) {
        const LoadedMaterialTexture loadedTexture = getOrLoadTexture(spawn.material);
        if (loadedTexture.texture == 0 || loadedTexture.texture == placeholderTexture_) {
            continue;
        }
        SpriteInstance sprite;
        sprite.position = spawn.position;
        sprite.color = spawn.color;
        sprite.alpha = spawn.alpha;
        sprite.halfSize = spawn.halfSize;
        sprite.additive = spawn.additive || loadedTexture.alphaMode == 3;
        sprite.alphaMode = sprite.additive ? 3 : (loadedTexture.alphaMode == 2 ? 2 : 1);
        sprite.leaf = spawn.leaf;
        sprite.texture = loadedTexture.texture;
        sprite.ownsTexture = loadedTexture.ownsTexture;
        sprites_.push_back(sprite);
    }

    for (const auto& batch : batches_) {
        if (batch->trigger) {
            triggerBatches_.push_back(batch.get());
        } else if (batch->alphaMode == 1 || batch->alphaMode == 3) {
            transparentBatches_.push_back(batch.get());
        } else {
            solidBatches_.push_back(batch.get());
        }
    }

    for (BatchGeometry& batchGeometry : loaded.skyBatches) {
        if (batchGeometry.vertices.empty() || batchGeometry.indices.empty()) {
            continue;
        }
        auto batch = std::make_unique<Batch>(std::move(batchGeometry));
        if (!batch->material.empty()) {
            const LoadedMaterialTexture loadedTexture = getOrLoadTexture(batch->material);
            batch->texture = loadedTexture.texture;
            batch->ownsTexture = loadedTexture.ownsTexture;
            batch->alphaMode = loadedTexture.alphaMode;
            batch->useEnvmap = loadedTexture.envmap;
            batch->envmapTint = loadedTexture.envmapTint;
        }
        if (batch->texture != 0) {
            ++textureCount_;
        }
        skyBatches_.push_back(std::move(batch));
    }
    for (const auto& batch : skyBatches_) {
        if (batch->hidden) {
            continue;
        }
        if (batch->alphaMode == 1 || batch->alphaMode == 3) {
            skyTransparentBatches_.push_back(batch.get());
        } else {
            skySolidBatches_.push_back(batch.get());
        }
    }

    if (!skyName_.empty()) {
        static const char* kSkySuffixes[6] = {"rt", "lf", "bk", "ft", "up", "dn"};
        std::string skyBase = skyName_;
        for (char& character : skyBase) {
            character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
        }
        const std::string prefix = skyBase.find('/') == std::string::npos ? "skybox/" + skyBase : skyBase;
        for (int side = 0; side < 6; ++side) {
            const LoadedMaterialTexture loadedTexture = getOrLoadTexture(prefix + kSkySuffixes[side]);
            if (loadedTexture.texture == 0 || loadedTexture.texture == placeholderTexture_) {
                continue;
            }
            auto face = std::make_unique<SkyboxFace>();
            face->mesh = std::make_unique<Mesh>(
                SourceSkyFaceVertices(side, 10.0F),
                std::vector<unsigned int>{0, 1, 2, 0, 2, 3}
            );
            face->texture = loadedTexture.texture;
            face->ownsTexture = loadedTexture.ownsTexture;
            skyboxFaces_.push_back(std::move(face));
            ++textureCount_;
        }
        if (!skyboxFaces_.empty()) {
            std::cout << "Skybox textures " << skyboxFaces_.size() << "/6" << std::endl;
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
    for (const SpriteInstance& sprite : sprites_) {
        if (sprite.ownsTexture && sprite.texture != 0) {
            glDeleteTextures(1, &sprite.texture);
        }
    }
    for (const CubemapProbe& probe : cubemaps_) {
        if (probe.texture != 0) {
            glDeleteTextures(1, &probe.texture);
        }
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
    shader.SetBool("UseSpriteTint", false);
    shader.SetInt("AlphaMode", 0);
    shader.SetInt("Texture0", 0);
    shader.SetInt("Lightmap0", 1);
    shader.SetBool("DecalPass", false);
    const bool useLightmap = lightmapTexture_ != 0;
    shader.SetBool("UseLightmap", useLightmap);
    if (useLightmap) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D_ARRAY, lightmapTexture_);
    }

    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    glActiveTexture(GL_TEXTURE0);
    GLuint currentBoundTexture = 0xFFFFFFFF;
    int currentUseTexture = -1;
    int currentAlphaMode = -1;

    const bool hasFrustum = (viewProjection != glm::mat4(0.0F));
    Frustum frustum{};
    if (hasFrustum) {
        frustum = Frustum::FromMatrix(viewProjection);
    }

    shader.SetFloat("Opacity", 1.0F);
    for (const Batch* batch : solidBatches_) {
        if (batch->hidden) {
            continue;
        }
        if (hasFrustum && !frustum.IsBoxVisible(batch->aabbMin, batch->aabbMax)) {
            continue;
        }
        if (batch->alphaMode != currentAlphaMode) {
            shader.SetInt("AlphaMode", batch->alphaMode);
            currentAlphaMode = batch->alphaMode;
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
        ApplySurface(shader, batch);
        DrawCulledMesh(batch);
    }

    shader.SetBool("WarpSurface", false);
    shader.SetBool("UseEnvmap", false);
    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
}

void BspLoader::Draw(const Shader& shader) const {
    Draw(shader, glm::mat4(0.0F));
}

void BspLoader::DrawSkybox(const Shader& shader) const {
    if (skyboxFaces_.empty()) {
        return;
    }
    const GLboolean cullEnabled = glIsEnabled(GL_CULL_FACE);
    const GLboolean depthEnabled = glIsEnabled(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDepthMask(GL_FALSE);
    shader.SetInt("Texture0", 0);
    glActiveTexture(GL_TEXTURE0);
    for (const std::unique_ptr<SkyboxFace>& face : skyboxFaces_) {
        if (face == nullptr || face->mesh == nullptr || face->texture == 0) {
            continue;
        }
        glBindTexture(GL_TEXTURE_2D, face->texture);
        face->mesh->Draw();
    }
    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glDepthMask(GL_TRUE);
    if (depthEnabled) {
        glEnable(GL_DEPTH_TEST);
    }
    if (cullEnabled) {
        glEnable(GL_CULL_FACE);
    }
}

void BspLoader::DrawSky(const Shader& shader, const glm::mat4& viewProjection) const {
    if (skySolidBatches_.empty() && skyTransparentBatches_.empty()) {
        return;
    }
    shader.SetBool("UseSpriteTint", false);
    shader.SetInt("AlphaMode", 0);
    shader.SetInt("Texture0", 0);
    shader.SetInt("Lightmap0", 1);
    shader.SetBool("DecalPass", false);
    shader.SetFloat("Opacity", 1.0F);
    const bool useLightmap = lightmapTexture_ != 0;
    shader.SetBool("UseLightmap", useLightmap);
    if (useLightmap) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D_ARRAY, lightmapTexture_);
    }
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    glActiveTexture(GL_TEXTURE0);

    const bool hasFrustum = viewProjection != glm::mat4(0.0F);
    Frustum frustum{};
    if (hasFrustum) {
        frustum = Frustum::FromMatrix(viewProjection);
    }
    for (const Batch* batch : skySolidBatches_) {
        if (hasFrustum && !frustum.IsBoxVisible(batch->aabbMin, batch->aabbMax)) {
            continue;
        }
        shader.SetInt("AlphaMode", batch->alphaMode);
        glBindTexture(GL_TEXTURE_2D, batch->texture);
        shader.SetBool("UseTexture", batch->texture != 0);
        ApplySurface(shader, batch);
        batch->mesh->Draw();
    }
    if (!skyTransparentBatches_.empty()) {
        glEnable(GL_BLEND);
        glDepthMask(GL_FALSE);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        for (const Batch* batch : skyTransparentBatches_) {
            if (hasFrustum && !frustum.IsBoxVisible(batch->aabbMin, batch->aabbMax)) {
                continue;
            }
            if (batch->alphaMode == 3) {
                glBlendFunc(GL_SRC_ALPHA, GL_ONE);
            } else {
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            }
            shader.SetInt("AlphaMode", batch->alphaMode);
            glBindTexture(GL_TEXTURE_2D, batch->texture);
            shader.SetBool("UseTexture", batch->texture != 0);
            ApplySurface(shader, batch);
            batch->mesh->Draw();
        }
        glDepthMask(GL_TRUE);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }
    shader.SetBool("WarpSurface", false);
    shader.SetBool("UseEnvmap", false);
    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    if (useLightmap) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    }
}

void BspLoader::DrawTransparent(const Shader& shader, const glm::mat4& viewProjection) const {
    if (transparentBatches_.empty() && triggerBatches_.empty()) {
        return;
    }

    const bool hasFrustum = (viewProjection != glm::mat4(0.0F));
    Frustum frustum{};
    if (hasFrustum) {
        frustum = Frustum::FromMatrix(viewProjection);
    }

    const GLboolean blendWasEnabled = glIsEnabled(GL_BLEND);
    GLint depthMaskEnabled = GL_TRUE;
    glGetIntegerv(GL_DEPTH_WRITEMASK, &depthMaskEnabled);

    glEnable(GL_BLEND);
    glDepthMask(GL_FALSE);
    glDepthFunc(GL_LEQUAL);
    shader.SetBool("DecalPass", false);
    shader.SetBool("UseSpriteTint", false);
    const bool useLightmap = lightmapTexture_ != 0;
    shader.SetBool("UseLightmap", useLightmap);
    if (useLightmap) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D_ARRAY, lightmapTexture_);
    }
    glActiveTexture(GL_TEXTURE0);

    auto drawBatch = [&](const Batch* batch, float opacity) {
        if (batch->hidden) {
            return;
        }
        std::string lowered = batch->material;
        for (char& character : lowered) {
            character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
        }
        if (opacity > 0.9F
            && (lowered.find("glass") != std::string::npos
                || (lowered.find("window") != std::string::npos && lowered.find("frame") == std::string::npos))) {
            opacity = 0.45F;
        }
        if (hasFrustum && !frustum.IsBoxVisible(batch->aabbMin, batch->aabbMax)) {
            return;
        }
        const int mode = batch->trigger ? 1 : batch->alphaMode;
        shader.SetInt("AlphaMode", mode);
        shader.SetFloat("Opacity", opacity);
        if (mode == 4) {
            glBlendFunc(GL_DST_COLOR, GL_SRC_COLOR);
        } else if (mode == 3) {
            glBlendFunc(GL_SRC_ALPHA, GL_ONE);
        } else {
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        }
        glBindTexture(GL_TEXTURE_2D, batch->texture);
        shader.SetBool("UseTexture", batch->texture != 0);
        ApplySurface(shader, batch);
        DrawCulledMesh(batch);
    };

    for (const Batch* batch : transparentBatches_) {
        drawBatch(batch, 1.0F);
    }
    for (const Batch* batch : triggerBatches_) {
        drawBatch(batch, 0.1F);
    }

    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glDepthFunc(GL_LESS);
    glDepthMask(depthMaskEnabled ? GL_TRUE : GL_FALSE);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    if (!blendWasEnabled) {
        glDisable(GL_BLEND);
    }
    shader.SetInt("AlphaMode", 0);
    shader.SetFloat("Opacity", 1.0F);
    shader.SetBool("WarpSurface", false);
    shader.SetBool("UseEnvmap", false);
    if (useLightmap) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    }
}

void BspLoader::DrawDecals(const Shader& shader, const glm::mat4& /*viewProjection*/) const {
    if (decals_.empty()) {
        return;
    }

    const GLboolean blendWasEnabled = glIsEnabled(GL_BLEND);
    const GLboolean cullWasEnabled = glIsEnabled(GL_CULL_FACE);
    GLint depthMaskEnabled = GL_TRUE;
    glGetIntegerv(GL_DEPTH_WRITEMASK, &depthMaskEnabled);
    GLint depthFunc = GL_LESS;
    glGetIntegerv(GL_DEPTH_FUNC, &depthFunc);
    GLint cullFace = GL_BACK;
    GLint frontFace = GL_CCW;
    glGetIntegerv(GL_CULL_FACE_MODE, &cullFace);
    glGetIntegerv(GL_FRONT_FACE, &frontFace);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CCW);
    glDepthMask(GL_FALSE);
    glDepthFunc(GL_LEQUAL);

    shader.SetBool("DecalPass", true);
    const bool useLightmap = lightmapTexture_ != 0;
    shader.SetBool("UseLightmap", useLightmap);
    if (useLightmap) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D_ARRAY, lightmapTexture_);
    }
    shader.SetFloat("Opacity", 1.0F);

    GLuint currentBoundTexture = 0xFFFFFFFF;
    int currentUseTexture = -1;
    int currentMode = -1;

    for (const auto& decal : decals_) {
        if (visibility_ != nullptr && visibility_->cullWorld && !decal->faces.empty()) {
            bool faceVisible = false;
            for (const int face : decal->faces) {
                if (face >= 0 && static_cast<std::size_t>(face) < visibility_->visibleFaces.size()
                    && visibility_->visibleFaces[static_cast<std::size_t>(face)] != 0) {
                    faceVisible = true;
                    break;
                }
            }
            if (!faceVisible) {
                continue;
            }
        }
        if (decal->alphaMode != currentMode) {
            currentMode = decal->alphaMode;
            shader.SetInt("AlphaMode", currentMode);
            shader.SetBool("UseLightmap", useLightmap && currentMode != 4);
            if (currentMode == 4) {
                glBlendFunc(GL_DST_COLOR, GL_SRC_COLOR);
            } else if (currentMode == 3) {
                glBlendFunc(GL_SRC_ALPHA, GL_ONE);
            } else {
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            }
        }
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
    glDepthFunc(static_cast<GLenum>(depthFunc));
    glDepthMask(depthMaskEnabled ? GL_TRUE : GL_FALSE);
    glCullFace(static_cast<GLenum>(cullFace));
    glFrontFace(static_cast<GLenum>(frontFace));
    if (cullWasEnabled == GL_FALSE) {
        glDisable(GL_CULL_FACE);
    }
    if (!blendWasEnabled) {
        glDisable(GL_BLEND);
    }
    shader.SetBool("DecalPass", false);
    shader.SetInt("AlphaMode", 0);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    if (useLightmap) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    }
}

void BspLoader::DrawDecals(const Shader& shader) const {
    DrawDecals(shader, glm::mat4(0.0F));
}

void BspLoader::DrawSprites(const Shader& shader, const glm::mat4& view) const {
    if (sprites_.empty() || spriteQuad_ == nullptr) {
        return;
    }

    const glm::vec3 right(view[0][0], view[1][0], view[2][0]);
    const glm::vec3 up(view[0][1], view[1][1], view[2][1]);
    const float rightLength = glm::length(right);
    const float upLength = glm::length(up);
    if (rightLength <= 1.0e-6F || upLength <= 1.0e-6F) {
        return;
    }
    const glm::vec3 cameraRight = right / rightLength;
    const glm::vec3 cameraUp = up / upLength;
    const glm::vec3 cameraForward = glm::normalize(glm::cross(cameraRight, cameraUp));

    const GLboolean blendWasEnabled = glIsEnabled(GL_BLEND);
    GLint depthMaskEnabled = GL_TRUE;
    glGetIntegerv(GL_DEPTH_WRITEMASK, &depthMaskEnabled);
    GLint depthFunc = GL_LESS;
    glGetIntegerv(GL_DEPTH_FUNC, &depthFunc);

    glEnable(GL_BLEND);
    glDepthMask(GL_FALSE);
    glDepthFunc(GL_LEQUAL);
    shader.SetBool("DecalPass", true);
    shader.SetBool("UseLightmap", false);
    shader.SetBool("UseTexture", true);
    shader.SetBool("UseSpriteTint", true);
    shader.SetInt("Texture0", 0);
    glActiveTexture(GL_TEXTURE0);

    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    for (const SpriteInstance& sprite : sprites_) {
        if (sprite.leaf >= 0 && !LeafVisible(sprite.leaf)) {
            continue;
        }
        shader.SetInt("AlphaMode", sprite.alphaMode);
        if (sprite.alphaMode == 3) {
            glBlendFunc(GL_SRC_ALPHA, GL_ONE);
        } else {
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        }
        glm::mat4 model(1.0F);
        model[0] = glm::vec4(cameraRight * sprite.halfSize, 0.0F);
        model[1] = glm::vec4(cameraUp * sprite.halfSize, 0.0F);
        model[2] = glm::vec4(cameraForward * sprite.halfSize, 0.0F);
        model[3] = glm::vec4(sprite.position, 1.0F);
        shader.SetMat4("Model", model);
        shader.SetVec3("SpriteTint", sprite.color);
        shader.SetFloat("Opacity", sprite.alpha);
        glBindTexture(GL_TEXTURE_2D, sprite.texture);
        spriteQuad_->Draw();
    }

    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glDepthFunc(static_cast<GLenum>(depthFunc));
    glDepthMask(depthMaskEnabled ? GL_TRUE : GL_FALSE);
    if (!blendWasEnabled) {
        glDisable(GL_BLEND);
    } else {
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }
    shader.SetBool("DecalPass", false);
    shader.SetBool("UseSpriteTint", false);
    shader.SetInt("AlphaMode", 0);
    shader.SetFloat("Opacity", 1.0F);
    shader.SetMat4("Model", glm::mat4(1.0F));
}

void BspLoader::DrawDepth(const Shader& shader, const glm::vec3& lightPos, float lightRadius) const {
    shader.SetMat4("Model", glm::mat4(1.0F));
    const bool cullDistance = (lightRadius > 0.0F);
    const float maxDistSq = lightRadius * lightRadius;

    for (const Batch* batch : solidBatches_) {
        if (batch->hidden) {
            continue;
        }
        if (cullDistance && DistSqPointAABB(lightPos, batch->aabbMin, batch->aabbMax) > maxDistSq) {
            continue;
        }
        DrawCulledMesh(batch);
    }
    glBindVertexArray(0);
}

void BspLoader::DrawDepth(const Shader& shader) const {
    DrawDepth(shader, glm::vec3(0.0F), -1.0F);
}

namespace {

bool ClusterInPvs(const BspVisibility& visibility, int viewCluster, int cluster) {
    if (cluster < 0 || viewCluster < 0 || visibility.pvsRowBytes <= 0) {
        return false;
    }
    if (cluster == viewCluster) {
        return true;
    }
    if (cluster >= visibility.clusterCount || viewCluster >= visibility.clusterCount) {
        return false;
    }
    const std::size_t offset = static_cast<std::size_t>(viewCluster) * static_cast<std::size_t>(visibility.pvsRowBytes)
        + static_cast<std::size_t>(cluster >> 3);
    if (offset >= visibility.clusterPvs.size()) {
        return false;
    }
    const unsigned int bit = 1U << (cluster & 7);
    return (visibility.clusterPvs[offset] & static_cast<std::uint8_t>(bit)) != 0;
}

bool PortalIsOpen(
    const BspVisibility::Portal& portal,
    const std::vector<std::pair<std::string, bool>>& doorStates
) {
    if (!portal.target.empty()) {
        bool found = false;
        bool anyOpen = false;
        for (const auto& door : doorStates) {
            if (!SameEntityName(door.first, portal.target)) {
                continue;
            }
            found = true;
            anyOpen = anyOpen || door.second;
        }
        if (found) {
            return anyOpen;
        }
    }
    return true;
}

} // namespace

void BspLoader::UpdateVisibility(const glm::vec3& cameraPosition, const glm::mat4& viewProjection) {
    static const std::vector<std::pair<std::string, bool>> kNoDoors;
    UpdateVisibility(cameraPosition, viewProjection, kNoDoors);
}

void BspLoader::UpdateVisibility(
    const glm::vec3& cameraPosition,
    const glm::mat4& viewProjection,
    const std::vector<std::pair<std::string, bool>>& doorStates
) {
    if (visibility_ == nullptr) {
        return;
    }
    BspVisibility& visibility = *visibility_;
    visibility.cullWorld = false;
    visibility.visibleLeaves.clear();
    visibility.visibleFaces.clear();
    if (visibility.clusterCount <= 0 || visibility.clusterPvs.empty() || visibility.leaves.empty() || visibility.nodes.empty()) {
        return;
    }
    const int leafIndex = LeafIndex(cameraPosition);
    if (leafIndex < 0) {
        return;
    }
    const int viewCluster = visibility.leaves[static_cast<std::size_t>(leafIndex)].cluster;
    if (viewCluster < 0 || viewCluster >= visibility.clusterCount) {
        return;
    }

    static_cast<void>(viewProjection);
    const int startArea = visibility.leaves[static_cast<std::size_t>(leafIndex)].area;
    const bool filterAreas = !visibility.areas.empty()
        && startArea >= 0
        && static_cast<std::size_t>(startArea) < visibility.areas.size();
    std::vector<char> areaVisible(visibility.areas.size(), 0);
    if (filterAreas) {
        std::vector<int> stack;
        stack.push_back(startArea);
        areaVisible[static_cast<std::size_t>(startArea)] = 1;
        while (!stack.empty()) {
            const int areaIndex = stack.back();
            stack.pop_back();
            const BspVisibility::Area& area = visibility.areas[static_cast<std::size_t>(areaIndex)];
            for (int portalOffset = 0; portalOffset < area.portalCount; ++portalOffset) {
                const int portalIndex = area.firstPortal + portalOffset;
                if (portalIndex < 0 || static_cast<std::size_t>(portalIndex) >= visibility.portals.size()) {
                    continue;
                }
                const BspVisibility::Portal& portal = visibility.portals[static_cast<std::size_t>(portalIndex)];
                if (!PortalIsOpen(portal, doorStates)) {
                    continue;
                }
                const int otherArea = portal.otherArea;
                if (otherArea < 0 || static_cast<std::size_t>(otherArea) >= areaVisible.size()
                    || areaVisible[static_cast<std::size_t>(otherArea)] != 0) {
                    continue;
                }
                areaVisible[static_cast<std::size_t>(otherArea)] = 1;
                stack.push_back(otherArea);
            }
        }
    }

    visibility.visibleLeaves.assign(visibility.leaves.size(), 0);
    for (std::size_t index = 0; index < visibility.leaves.size(); ++index) {
        const BspVisibility::Leaf& leaf = visibility.leaves[index];
        if (!ClusterInPvs(visibility, viewCluster, leaf.cluster)) {
            continue;
        }
        if (filterAreas) {
            if (leaf.area < 0 || static_cast<std::size_t>(leaf.area) >= areaVisible.size()
                || areaVisible[static_cast<std::size_t>(leaf.area)] == 0) {
                continue;
            }
        }
        visibility.visibleLeaves[index] = 1;
    }

    const std::size_t faceCount = visibility.faceLeafOffset.empty() ? 0 : visibility.faceLeafOffset.size() - 1;
    visibility.visibleFaces.assign(faceCount, 0);
    for (std::size_t face = 0; face < faceCount; ++face) {
        const int begin = visibility.faceLeafOffset[face];
        const int end = visibility.faceLeafOffset[face + 1];
        if (begin == end) {
            visibility.visibleFaces[face] = 1;
            continue;
        }
        for (int cursor = begin; cursor < end; ++cursor) {
            if (cursor < 0 || static_cast<std::size_t>(cursor) >= visibility.faceLeafIndices.size()) {
                continue;
            }
            const int leaf = visibility.faceLeafIndices[static_cast<std::size_t>(cursor)];
            if (leaf >= 0 && static_cast<std::size_t>(leaf) < visibility.visibleLeaves.size()
                && visibility.visibleLeaves[static_cast<std::size_t>(leaf)] != 0) {
                visibility.visibleFaces[face] = 1;
                break;
            }
        }
    }
    visibility.cullWorld = true;
}

int BspLoader::LeafIndex(const glm::vec3& worldPosition) const {
    if (visibility_ == nullptr || visibility_->nodes.empty() || visibility_->planes.empty() || visibility_->leaves.empty()) {
        return -1;
    }
    const Point point = WorldToSourcePoint(worldPosition);
    int nodeIndex = 0;
    for (int step = 0; step < 1024 && nodeIndex >= 0; ++step) {
        if (static_cast<std::size_t>(nodeIndex) >= visibility_->nodes.size()) {
            return -1;
        }
        const BspVisibility::Node& node = visibility_->nodes[static_cast<std::size_t>(nodeIndex)];
        if (node.plane < 0 || static_cast<std::size_t>(node.plane) >= visibility_->planes.size()) {
            return -1;
        }
        const BspVisibility::Plane& plane = visibility_->planes[static_cast<std::size_t>(node.plane)];
        const float side = plane.x * point.x + plane.y * point.y + plane.z * point.z - plane.distance;
        nodeIndex = node.child[side >= 0.0F ? 0 : 1];
    }
    if (nodeIndex >= 0) {
        return -1;
    }
    const int leaf = -1 - nodeIndex;
    if (leaf < 0 || static_cast<std::size_t>(leaf) >= visibility_->leaves.size()) {
        return -1;
    }
    return leaf;
}

int BspLoader::PointContents(const glm::vec3& worldPosition) const {
    const int leaf = LeafIndex(worldPosition);
    if (leaf < 0 || visibility_ == nullptr || static_cast<std::size_t>(leaf) >= visibility_->leaves.size()) {
        return 0;
    }
    return visibility_->leaves[static_cast<std::size_t>(leaf)].contents;
}

bool BspLoader::LeafVisible(int leafIndex) const {
    if (visibility_ == nullptr || !visibility_->cullWorld) {
        return true;
    }
    if (leafIndex < 0 || static_cast<std::size_t>(leafIndex) >= visibility_->visibleLeaves.size()) {
        return true;
    }
    return visibility_->visibleLeaves[static_cast<std::size_t>(leafIndex)] != 0;
}

bool BspLoader::FaceIsVisible(int faceIndex) const {
    if (visibility_ == nullptr || !visibility_->cullWorld) {
        return true;
    }
    if (faceIndex < 0 || static_cast<std::size_t>(faceIndex) >= visibility_->visibleFaces.size()) {
        return true;
    }
    if (static_cast<std::size_t>(faceIndex + 1) >= visibility_->faceLeafOffset.size()) {
        return true;
    }
    if (visibility_->faceLeafOffset[static_cast<std::size_t>(faceIndex)]
        == visibility_->faceLeafOffset[static_cast<std::size_t>(faceIndex) + 1]) {
        return true;
    }
    return visibility_->visibleFaces[static_cast<std::size_t>(faceIndex)] != 0;
}

void BspLoader::ApplySurface(const Shader& shader, const Batch* batch) const {
    if (batch == nullptr) {
        shader.SetBool("WarpSurface", false);
        shader.SetBool("UseEnvmap", false);
        return;
    }
    shader.SetBool("WarpSurface", batch->warp);
    shader.SetBool("UseEnvmap", batch->useEnvmap && envmapBound_);
    shader.SetVec3("EnvmapTint", batch->envmapTint);
}

void BspLoader::PrepareFrame(const Shader& shader, const glm::vec3& cameraPosition, float timeSeconds) const {
    shader.SetFloat("Time", timeSeconds);
    shader.SetVec3("PropTint", glm::vec3(0.35F));
    shader.SetBool("WarpSurface", false);
    shader.SetBool("UseEnvmap", false);
    shader.SetVec3("EnvmapTint", glm::vec3(1.0F));
    envmapBound_ = false;
    const CubemapProbe* nearest = nullptr;
    float bestDistance = 0.0F;
    for (const CubemapProbe& probe : cubemaps_) {
        if (probe.texture == 0) {
            continue;
        }
        const glm::vec3 delta = probe.position - cameraPosition;
        const float distance = glm::dot(delta, delta);
        if (nearest == nullptr || distance < bestDistance) {
            nearest = &probe;
            bestDistance = distance;
        }
    }
    if (nearest != nullptr) {
        glActiveTexture(GL_TEXTURE7);
        glBindTexture(GL_TEXTURE_CUBE_MAP, nearest->texture);
        shader.SetInt("Envmap0", 7);
        envmapBound_ = true;
        glActiveTexture(GL_TEXTURE0);
    }
    float surfaceY = 1.0e6F;
    bool underwater = false;
    const int leaf = LeafIndex(cameraPosition);
    if (leaf >= 0 && visibility_ != nullptr && static_cast<std::size_t>(leaf) < visibility_->leaves.size()) {
        const int water = visibility_->leaves[static_cast<std::size_t>(leaf)].water;
        const int contents = visibility_->leaves[static_cast<std::size_t>(leaf)].contents;
        underwater = (contents & (kBspContentsWater | kBspContentsSlime)) != 0;
        if (water >= 0 && static_cast<std::size_t>(water) < visibility_->waterSurfaceY.size()) {
            surfaceY = visibility_->waterSurfaceY[static_cast<std::size_t>(water)];
            underwater = underwater || cameraPosition.y < surfaceY;
        }
    }
    shader.SetFloat("WaterSurfaceY", surfaceY);
    shader.SetBool("CameraUnderwater", underwater);
}

void BspLoader::DrawCulledMesh(const Batch* batch) const {
    if (batch == nullptr || batch->mesh == nullptr) {
        return;
    }
    const bool cull = visibility_ != nullptr && visibility_->cullWorld && !batch->faceSpans.empty();
    if (!cull) {
        batch->mesh->Draw();
        return;
    }
    drawCounts_.clear();
    drawOffsets_.clear();
    drawCounts_.reserve(batch->faceSpans.size());
    drawOffsets_.reserve(batch->faceSpans.size());
    for (const FaceSpan& span : batch->faceSpans) {
        if (span.indexCount == 0 || !FaceIsVisible(span.faceIndex)) {
            continue;
        }
        drawCounts_.push_back(static_cast<GLsizei>(span.indexCount));
        drawOffsets_.push_back(reinterpret_cast<const void*>(
            static_cast<std::uintptr_t>(span.firstIndex) * sizeof(unsigned int)
        ));
    }
    if (!drawCounts_.empty()) {
        batch->mesh->MultiDraw(drawCounts_.data(), drawOffsets_.data(), static_cast<GLsizei>(drawCounts_.size()));
    }
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

const std::vector<BspMapLight>& BspLoader::MapLights() const {
    return mapLights_;
}

bool BspLoader::HasEnvironmentLight() const {
    return hasEnvironmentLight_;
}

const BspMapLight& BspLoader::EnvironmentLight() const {
    return environmentLight_;
}

const std::string& BspLoader::SkyName() const {
    return skyName_;
}

bool BspLoader::HasSkybox() const {
    return !skyboxFaces_.empty();
}

bool BspLoader::HasSkyCamera() const {
    return skyCamera_.present;
}

const BspSkyCamera& BspLoader::SkyCamera() const {
    return skyCamera_;
}

const BspFog& BspLoader::WorldFog() const {
    return worldFog_;
}

glm::vec3 BspLoader::SkyViewOrigin(const glm::vec3& cameraPosition) const {
    const float scale = skyCamera_.scale >= 1.0F ? skyCamera_.scale : 16.0F;
    return skyCamera_.origin + cameraPosition / scale;
}

void BspLoader::FillLightStyles(float timeSeconds, float values[64]) const {
    for (int index = 0; index < 64; ++index) {
        values[index] = LightStyleAt(lightStyles_[static_cast<std::size_t>(index)], timeSeconds);
    }
    values[0] = 1.0F;
}

const glm::vec3& BspLoader::PlayerStartPosition() const {
    return playerStartPosition_;
}

bool BspLoader::HasPlayerStartPosition() const {
    return hasPlayerStartPosition_;
}

void BspLoader::DrawBrushModel(int modelIndex, const Shader& shader, bool translucentPass) const {
    if (modelIndex < 0 || static_cast<std::size_t>(modelIndex) >= brushModels_.size() || brushModels_[static_cast<std::size_t>(modelIndex)] == nullptr) {
        return;
    }
    const BrushModel& brush = *brushModels_[static_cast<std::size_t>(modelIndex)];
    glActiveTexture(GL_TEXTURE0);
    for (const BrushModel::Part& part : brush.parts) {
        const bool translucent = part.alphaMode == 1 || part.alphaMode == 3;
        if (translucent != translucentPass) {
            continue;
        }
        shader.SetInt("AlphaMode", part.alphaMode);
        if (translucentPass) {
            glEnable(GL_BLEND);
            glDepthMask(GL_FALSE);
            if (part.alphaMode == 3) {
                glBlendFunc(GL_SRC_ALPHA, GL_ONE);
            } else {
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            }
        } else {
            glDisable(GL_BLEND);
            glDepthMask(GL_TRUE);
        }
        glBindTexture(GL_TEXTURE_2D, part.texture);
        shader.SetBool("UseTexture", part.texture != 0);
        shader.SetBool("UseLightmap", false);
        shader.SetBool("WarpSurface", part.warp);
        shader.SetBool("UseEnvmap", part.useEnvmap && envmapBound_);
        shader.SetVec3("EnvmapTint", part.envmapTint);
        shader.SetFloat("Opacity", part.glass ? 0.55F : 1.0F);
        if (part.mesh) {
            part.mesh->Draw();
        }
    }
    shader.SetBool("WarpSurface", false);
    shader.SetBool("UseEnvmap", false);
    shader.SetFloat("Opacity", 1.0F);
}

bool BspLoader::BrushAabb(int modelIndex, glm::vec3& minimum, glm::vec3& maximum) const {
    if (modelIndex < 0 || static_cast<std::size_t>(modelIndex) >= brushModels_.size() || brushModels_[static_cast<std::size_t>(modelIndex)] == nullptr) {
        return false;
    }
    const BrushModel& brush = *brushModels_[static_cast<std::size_t>(modelIndex)];
    if (brush.aabbMin.x > brush.aabbMax.x) {
        return false;
    }
    minimum = brush.aabbMin;
    maximum = brush.aabbMax;
    return true;
}

const std::vector<glm::vec3>& BspLoader::BrushCollisionVertices(int modelIndex) const {
    static const std::vector<glm::vec3> empty;
    if (modelIndex < 0 || static_cast<std::size_t>(modelIndex) >= brushModels_.size() || brushModels_[static_cast<std::size_t>(modelIndex)] == nullptr) {
        return empty;
    }
    return brushModels_[static_cast<std::size_t>(modelIndex)]->collisionVertices;
}

const std::vector<std::uint32_t>& BspLoader::BrushCollisionIndices(int modelIndex) const {
    static const std::vector<std::uint32_t> empty;
    if (modelIndex < 0 || static_cast<std::size_t>(modelIndex) >= brushModels_.size() || brushModels_[static_cast<std::size_t>(modelIndex)] == nullptr) {
        return empty;
    }
    return brushModels_[static_cast<std::size_t>(modelIndex)]->collisionIndices;
}
