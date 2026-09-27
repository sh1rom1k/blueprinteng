#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <glad/gl.h>
#include <glm/vec3.hpp>
#include <glm/mat4x4.hpp>

class Mesh;
class Shader;

class GameFileSystem;

struct BspVisibility;

constexpr int kBspContentsSolid = 0x1;
constexpr int kBspContentsWindow = 0x2;
constexpr int kBspContentsGrate = 0x8;
constexpr int kBspContentsSlime = 0x10;
constexpr int kBspContentsWater = 0x20;
constexpr int kBspContentsLadder = 0x20000000;

struct BspMapEntity {
    glm::vec3 position{0.0F};
    std::string classname;
    std::string model;
    std::string texture;
    std::vector<std::pair<std::string, std::string>> properties;
    std::vector<int> visibilityLeaves;
    int brushModel = -1;
    bool inSkybox = false;
    float fadeMin = 0.0F;
    float fadeMax = 0.0F;
    glm::vec3 lightingOrigin{0.0F};
    glm::vec3 lightTint{0.35F};
    bool hasLightingOrigin = false;
};

enum class BspLightKind {
    Point,
    Spot,
    Sun,
};

struct BspFog {
    glm::vec3 color{0.55F, 0.62F, 0.70F};
    float start = 0.0F;
    float end = 0.0F;
    float maxDensity = 1.0F;
    bool enabled = false;
};

struct BspSkyCamera {
    glm::vec3 origin{0.0F};
    float pitch = 0.0F;
    float yaw = 0.0F;
    float roll = 0.0F;
    float scale = 16.0F;
    BspFog fog{};
    bool present = false;
};

struct BspMapLight {
    BspLightKind kind = BspLightKind::Point;
    glm::vec3 position{0.0F};
    glm::vec3 direction{0.0F, 1.0F, 0.0F};
    glm::vec3 color{1.0F};
    glm::vec3 ambient{0.0F};
    float intensity = 0.0F;
    float constant = 1.0F;
    float linear = 0.0F;
    float quadratic = 1.0F;
    float innerCutOff = 1.0F;
    float outerCutOff = 0.707F;
    bool dynamic = false;
};

class BspLoader {
public:
    explicit BspLoader(const std::string& path, const GameFileSystem* files = nullptr);
    ~BspLoader();

    BspLoader(const BspLoader&) = delete;
    BspLoader& operator=(const BspLoader&) = delete;

    void Draw(const Shader& shader) const;
    void Draw(const Shader& shader, const glm::mat4& viewProjection) const;
    void DrawSkybox(const Shader& shader) const;
    void DrawSky(const Shader& shader, const glm::mat4& viewProjection) const;
    void DrawDecals(const Shader& shader) const;
    void DrawDecals(const Shader& shader, const glm::mat4& viewProjection) const;
    void DrawTransparent(const Shader& shader, const glm::mat4& viewProjection) const;
    void DrawSprites(const Shader& shader, const glm::mat4& view) const;
    void DrawDepth(const Shader& shader) const;
    void DrawDepth(const Shader& shader, const glm::vec3& lightPos, float lightRadius) const;
    std::size_t FaceCount() const;
    std::size_t TextureCount() const;
    const glm::vec3& WorldMinimum() const;
    const glm::vec3& WorldMaximum() const;
    const std::vector<glm::vec3>& CollisionVertices() const;
    const std::vector<std::uint32_t>& CollisionIndices() const;
    const std::vector<glm::vec3>& PropPositions() const;
    const std::vector<BspMapEntity>& MapEntities() const;
    const std::vector<BspMapLight>& MapLights() const;
    bool HasEnvironmentLight() const;
    const BspMapLight& EnvironmentLight() const;
    const std::string& SkyName() const;
    bool HasSkybox() const;
    bool HasSkyCamera() const;
    const BspSkyCamera& SkyCamera() const;
    const BspFog& WorldFog() const;
    glm::vec3 SkyViewOrigin(const glm::vec3& cameraPosition) const;
    void FillLightStyles(float timeSeconds, float values[64]) const;
    const glm::vec3& PlayerStartPosition() const;
    bool HasPlayerStartPosition() const;
    void DrawBrushModel(int modelIndex, const Shader& shader, bool translucentPass = false) const;
    bool BrushAabb(int modelIndex, glm::vec3& minimum, glm::vec3& maximum) const;
    const std::vector<glm::vec3>& BrushCollisionVertices(int modelIndex) const;
    const std::vector<std::uint32_t>& BrushCollisionIndices(int modelIndex) const;
    void UpdateVisibility(const glm::vec3& cameraPosition, const glm::mat4& viewProjection);
    void UpdateVisibility(
        const glm::vec3& cameraPosition,
        const glm::mat4& viewProjection,
        const std::vector<std::pair<std::string, bool>>& doorStates
    );
    [[nodiscard]] int LeafIndex(const glm::vec3& worldPosition) const;
    [[nodiscard]] int PointContents(const glm::vec3& worldPosition) const;
    [[nodiscard]] bool LeafVisible(int leafIndex) const;
    void PrepareFrame(const Shader& shader, const glm::vec3& cameraPosition, float timeSeconds) const;

private:
    struct Batch;
    void DrawCulledMesh(const Batch* batch) const;
    void ApplySurface(const Shader& shader, const Batch* batch) const;
    [[nodiscard]] bool FaceIsVisible(int faceIndex) const;
    struct Decal;
    struct BrushModel;
    struct SkyboxFace;
    struct SpriteInstance {
        glm::vec3 position{0.0F};
        glm::vec3 color{1.0F};
        float alpha = 1.0F;
        float halfSize = 0.25F;
        bool additive = false;
        int alphaMode = 1;
        int leaf = -1;
        GLuint texture = 0;
        bool ownsTexture = false;
    };
    struct CubemapProbe {
        glm::vec3 position{0.0F};
        GLuint texture = 0;
    };
    std::vector<std::unique_ptr<Batch>> batches_;
    std::vector<std::unique_ptr<Batch>> skyBatches_;
    std::vector<std::unique_ptr<BrushModel>> brushModels_;
    std::vector<Batch*> solidBatches_;
    std::vector<Batch*> triggerBatches_;
    std::vector<Batch*> transparentBatches_;
    std::vector<Batch*> skySolidBatches_;
    std::vector<Batch*> skyTransparentBatches_;
    std::vector<std::unique_ptr<SkyboxFace>> skyboxFaces_;
    std::vector<std::unique_ptr<Decal>> decals_;
    std::size_t faceCount_ = 0;
    std::size_t textureCount_ = 0;
    GLuint lightmapTexture_ = 0;
    GLuint placeholderTexture_ = 0;
    glm::vec3 worldMinimum_{-16.0F};
    glm::vec3 worldMaximum_{16.0F};
    std::vector<glm::vec3> collisionVertices_;
    std::vector<std::uint32_t> collisionIndices_;
    std::vector<glm::vec3> propPositions_;
    std::vector<BspMapEntity> mapEntities_;
    std::vector<BspMapLight> mapLights_;
    BspMapLight environmentLight_{};
    bool hasEnvironmentLight_ = false;
    std::string skyName_;
    BspSkyCamera skyCamera_{};
    BspFog worldFog_{};
    std::array<std::string, 64> lightStyles_{};
    std::vector<SpriteInstance> sprites_;
    std::unique_ptr<Mesh> spriteQuad_;
    glm::vec3 playerStartPosition_{0.0F};
    bool hasPlayerStartPosition_ = false;
    std::unique_ptr<BspVisibility> visibility_;
    mutable std::vector<GLsizei> drawCounts_;
    mutable std::vector<const void*> drawOffsets_;
    std::vector<CubemapProbe> cubemaps_;
    mutable bool envmapBound_ = false;
};
