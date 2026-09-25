#include "ShadowManager.hpp"

#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <glm/gtc/matrix_transform.hpp>

#include "Shader.hpp"

ShadowManager::ShadowManager() = default;

ShadowManager::~ShadowManager() {
    if (spotlightFbo_ != 0) {
        glDeleteFramebuffers(1, &spotlightFbo_);
        spotlightFbo_ = 0;
    }
    if (spotlightDepthMap_ != 0) {
        glDeleteTextures(1, &spotlightDepthMap_);
        spotlightDepthMap_ = 0;
    }
    for (auto& res : pointShadowResources_) {
        if (res.fbo != 0) {
            glDeleteFramebuffers(1, &res.fbo);
            res.fbo = 0;
        }
        if (res.depthCubemap != 0) {
            glDeleteTextures(1, &res.depthCubemap);
            res.depthCubemap = 0;
        }
    }
    pointShadowResources_.clear();
}

void ShadowManager::Init(const std::string& shaderDir) {
    // 1. Compile shadow depth shaders
    spotlightDepthShader_ = std::make_unique<Shader>(
        shaderDir + "/shaders/shadow_depth.vert",
        shaderDir + "/shaders/shadow_depth.frag"
    );

    pointDepthShader_ = std::make_unique<Shader>(
        shaderDir + "/shaders/point_shadow_depth.vert",
        shaderDir + "/shaders/point_shadow_depth.frag",
        shaderDir + "/shaders/point_shadow_depth.geom"
    );

    // 2. Initialize Spotlight (Flashlight) shadow resources
    glGenFramebuffers(1, &spotlightFbo_);
    glGenTextures(1, &spotlightDepthMap_);
    glBindTexture(GL_TEXTURE_2D, spotlightDepthMap_);
    glTexImage2D(
        GL_TEXTURE_2D,
        0,
        GL_DEPTH_COMPONENT32F,
        kSpotlightShadowResolution,
        kSpotlightShadowResolution,
        0,
        GL_DEPTH_COMPONENT,
        GL_FLOAT,
        nullptr
    );
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    const float borderColor[] = {1.0F, 1.0F, 1.0F, 1.0F};
    glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, borderColor);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);

    glBindFramebuffer(GL_FRAMEBUFFER, spotlightFbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, spotlightDepthMap_, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        throw std::runtime_error("Flashlight shadow framebuffer is incomplete");
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // 3. Initialize Point Light shadow cubemaps
    pointShadowResources_.resize(kMaxShadowPointLights);
    for (int i = 0; i < kMaxShadowPointLights; ++i) {
        auto& res = pointShadowResources_[static_cast<std::size_t>(i)];
        glGenFramebuffers(1, &res.fbo);
        glGenTextures(1, &res.depthCubemap);
        glBindTexture(GL_TEXTURE_CUBE_MAP, res.depthCubemap);
        for (unsigned int face = 0; face < 6; ++face) {
            glTexImage2D(
                GL_TEXTURE_CUBE_MAP_POSITIVE_X + face,
                0,
                GL_DEPTH_COMPONENT32F,
                kPointShadowResolution,
                kPointShadowResolution,
                0,
                GL_DEPTH_COMPONENT,
                GL_FLOAT,
                nullptr
            );
        }
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);

        glBindFramebuffer(GL_FRAMEBUFFER, res.fbo);
        glFramebufferTexture(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, res.depthCubemap, 0);
        glDrawBuffer(GL_NONE);
        glReadBuffer(GL_NONE);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            throw std::runtime_error("Point light shadow framebuffer is incomplete for light index " + std::to_string(i));
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
}

void ShadowManager::BeginSpotlightPass(const glm::vec3& position, const glm::vec3& direction) {
    const float fovY = glm::radians(42.0F);
    const float aspect = 1.0F;
    const glm::mat4 lightProjection = glm::perspective(fovY, aspect, kSpotlightNearPlane, kSpotlightFarPlane);

    glm::vec3 up(0.0F, 1.0F, 0.0F);
    if (std::abs(direction.y) > 0.99F) {
        up = glm::vec3(0.0F, 0.0F, direction.y > 0.0F ? -1.0F : 1.0F);
    }
    const glm::mat4 lightView = glm::lookAt(position, position + direction, up);
    spotlightSpaceMatrix_ = lightProjection * lightView;

    spotlightDepthShader_->Use();
    spotlightDepthShader_->SetMat4("LightSpaceMatrix", spotlightSpaceMatrix_);

    glBindFramebuffer(GL_FRAMEBUFFER, spotlightFbo_);
    glViewport(0, 0, kSpotlightShadowResolution, kSpotlightShadowResolution);
    glClear(GL_DEPTH_BUFFER_BIT);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(2.0F, 4.0F);
}

void ShadowManager::EndSpotlightPass() {
    glDisable(GL_POLYGON_OFFSET_FILL);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void ShadowManager::BeginPointLightPass(int shadowIndex, const glm::vec3& lightPos, float farPlane) {
    if (shadowIndex < 0 || shadowIndex >= static_cast<int>(pointShadowResources_.size())) {
        return;
    }

    const float aspect = 1.0F;
    const float nearPlane = 0.1F;
    const glm::mat4 shadowProj = glm::perspective(glm::radians(90.0F), aspect, nearPlane, farPlane);

    std::array<glm::mat4, 6> shadowTransforms;
    shadowTransforms[0] = shadowProj * glm::lookAt(lightPos, lightPos + glm::vec3( 1.0F,  0.0F,  0.0F), glm::vec3(0.0F, -1.0F,  0.0F));
    shadowTransforms[1] = shadowProj * glm::lookAt(lightPos, lightPos + glm::vec3(-1.0F,  0.0F,  0.0F), glm::vec3(0.0F, -1.0F,  0.0F));
    shadowTransforms[2] = shadowProj * glm::lookAt(lightPos, lightPos + glm::vec3( 0.0F,  1.0F,  0.0F), glm::vec3(0.0F,  0.0F,  1.0F));
    shadowTransforms[3] = shadowProj * glm::lookAt(lightPos, lightPos + glm::vec3( 0.0F, -1.0F,  0.0F), glm::vec3(0.0F,  0.0F, -1.0F));
    shadowTransforms[4] = shadowProj * glm::lookAt(lightPos, lightPos + glm::vec3( 0.0F,  0.0F,  1.0F), glm::vec3(0.0F, -1.0F,  0.0F));
    shadowTransforms[5] = shadowProj * glm::lookAt(lightPos, lightPos + glm::vec3( 0.0F,  0.0F, -1.0F), glm::vec3(0.0F, -1.0F,  0.0F));

    pointDepthShader_->Use();
    pointDepthShader_->SetMat4Array("ShadowMatrices[0]", shadowTransforms.data(), 6);
    pointDepthShader_->SetVec3("LightPos", lightPos);
    pointDepthShader_->SetFloat("FarPlane", farPlane);

    glBindFramebuffer(GL_FRAMEBUFFER, pointShadowResources_[static_cast<std::size_t>(shadowIndex)].fbo);
    glViewport(0, 0, kPointShadowResolution, kPointShadowResolution);
    glClear(GL_DEPTH_BUFFER_BIT);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(2.0F, 4.0F);
}

void ShadowManager::EndPointLightPass() {
    glDisable(GL_POLYGON_OFFSET_FILL);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void ShadowManager::BindForRendering(
    const Shader& mainShader,
    bool flashlightActive,
    int activeShadowPointLights
) {
    // 1. Bind Spotlight Shadow Map to Texture Unit 2
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, spotlightDepthMap_);
    mainShader.SetInt("FlashlightShadowMap", 2);
    mainShader.SetMat4("FlashlightSpaceMatrix", spotlightSpaceMatrix_);
    mainShader.SetBool("FlashlightCastShadow", shadowsEnabled && flashlightShadows && flashlightActive);

    // 2. Bind Point Light Shadow Cubemaps to Texture Units 3..6
    const int count = std::min(activeShadowPointLights, kMaxShadowPointLights);
    mainShader.SetBool("PointShadowsEnabled", shadowsEnabled && pointLightShadows);
    mainShader.SetFloat("PointShadowFarPlane", kPointShadowFarPlane);
    mainShader.SetFloat("ShadowRenderDistance", kShadowRenderDistance);
    mainShader.SetInt("NumShadowPointLights", count);

    for (int i = 0; i < kMaxShadowPointLights; ++i) {
        glActiveTexture(static_cast<GLenum>(GL_TEXTURE3 + i));
        glBindTexture(GL_TEXTURE_CUBE_MAP, pointShadowResources_[static_cast<std::size_t>(i)].depthCubemap);
    }
    if (!samplersBound_) {
        for (int i = 0; i < kMaxShadowPointLights; ++i) {
            mainShader.SetInt("PointShadowMaps[" + std::to_string(i) + "]", 3 + i);
        }
        samplersBound_ = true;
    }
}
