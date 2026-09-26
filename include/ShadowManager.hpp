#pragma once

#include <filesystem>
#include <vector>
#include <memory>
#include <string>

#include <glad/gl.h>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

class Shader;

class ShadowManager {
public:
    static constexpr int kMaxShadowPointLights = 4;
    static constexpr int kSpotlightShadowResolution = 512;
    static constexpr int kPointShadowResolution = 256;
    // Skip shadow map rendering when the light is farther than this from the camera.
    static constexpr float kShadowCameraCullDistance = 32.0F;
    static constexpr float kShadowRenderDistance = 32.0F;
    static constexpr float kPointShadowFarPlane = kShadowRenderDistance;
    static constexpr float kSpotlightNearPlane = 0.1F;
    static constexpr float kSpotlightFarPlane = kShadowRenderDistance;

    ShadowManager();
    ~ShadowManager();

    ShadowManager(const ShadowManager&) = delete;
    ShadowManager& operator=(const ShadowManager&) = delete;

    void Init(const std::filesystem::path& shaderDir);

    // Spotlight (flashlight) shadow pass
    void BeginSpotlightPass(const glm::vec3& position, const glm::vec3& direction);
    void EndSpotlightPass();

    // Omnidirectional point light shadow pass
    void BeginPointLightPass(int shadowIndex, const glm::vec3& lightPos, float farPlane = kPointShadowFarPlane);
    void EndPointLightPass();

    // Bind shadow maps to texture units (Unit 2 for spotlight, Units 3..6 for point lights)
    void BindForRendering(
        const Shader& mainShader,
        bool flashlightActive,
        int activeShadowPointLights
    );

    const Shader& GetSpotlightDepthShader() const { return *spotlightDepthShader_; }
    const Shader& GetPointDepthShader() const { return *pointDepthShader_; }

    const glm::mat4& SpotlightSpaceMatrix() const { return spotlightSpaceMatrix_; }
    GLuint SpotlightDepthMap() const { return spotlightDepthMap_; }

    bool shadowsEnabled = true;
    bool flashlightShadows = true;
    bool pointLightShadows = true;

private:
    std::unique_ptr<Shader> spotlightDepthShader_;
    std::unique_ptr<Shader> pointDepthShader_;

    GLuint spotlightFbo_ = 0;
    GLuint spotlightDepthMap_ = 0;
    glm::mat4 spotlightSpaceMatrix_{1.0F};

    struct PointShadowResource {
        GLuint fbo = 0;
        GLuint depthCubemap = 0;
    };
    std::vector<PointShadowResource> pointShadowResources_;
    bool samplersBound_ = false;
};
