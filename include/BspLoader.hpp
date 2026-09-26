#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <glad/gl.h>
#include <glm/vec3.hpp>
#include <glm/mat4x4.hpp>

class Mesh;
class Shader;

struct BspMapEntity {
    glm::vec3 position{0.0F};
    std::string classname;
    std::string model;
    std::string texture;
    std::vector<std::pair<std::string, std::string>> properties;
};

class BspLoader {
public:
    explicit BspLoader(const std::filesystem::path& path);
    ~BspLoader();

    BspLoader(const BspLoader&) = delete;
    BspLoader& operator=(const BspLoader&) = delete;

    void Draw(const Shader& shader) const;
    void Draw(const Shader& shader, const glm::mat4& viewProjection) const;
    void DrawDecals(const Shader& shader) const;
    void DrawDecals(const Shader& shader, const glm::mat4& viewProjection) const;
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
    const glm::vec3& PlayerStartPosition() const;
    bool HasPlayerStartPosition() const;

private:
    struct Batch;
    struct Decal;
    std::vector<std::unique_ptr<Batch>> batches_;
    std::vector<Batch*> solidBatches_;
    std::vector<Batch*> triggerBatches_;
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
    glm::vec3 playerStartPosition_{0.0F};
    bool hasPlayerStartPosition_ = false;
};
