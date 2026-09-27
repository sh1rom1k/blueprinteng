#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

struct StudioSkinVertex {
    glm::vec3 position{0.0F};
    float weights[3]{1.0F, 0.0F, 0.0F};
    unsigned char bones[3]{0, 0, 0};
    unsigned char boneCount = 0;
};

struct StudioPrimitive {
    std::vector<float> vertices;
    std::vector<unsigned int> indices;
    std::vector<StudioSkinVertex> skin;
    std::string material;
};

struct StudioBone {
    int parent = -1;
    glm::vec3 position{0.0F};
    float rotation[4]{0.0F, 0.0F, 0.0F, 1.0F};
    glm::vec3 euler{0.0F};
    glm::vec3 positionScale{1.0F};
    glm::vec3 rotationScale{1.0F};
    float poseToBone[12]{};
};

struct StudioAnimBuffer {
    std::vector<std::uint8_t> bytes;
    std::vector<std::uint8_t> ani;
    int animBlockCount = 0;
    int animBlockIndex = 0;
};

struct StudioSequence {
    std::string name;
    float fps = 30.0F;
    int frameCount = 1;
    bool loop = true;
    int source = 0;
    int descOffset = 0;
    int animBlock = 0;
    int animIndex = 0;
    int sectionOffset = -1;
    int sectionFrames = 0;
};

struct StudioAnimation {
    std::vector<StudioBone> bones;
    std::vector<StudioSequence> sequences;
    std::vector<StudioAnimBuffer> sources;
    int idle = -1;
    int walk = -1;
    int run = -1;
    int shoot = -1;
};

struct StudioModel {
    glm::vec3 hullMin{-16.0F};
    glm::vec3 hullMax{16.0F};
    std::vector<std::string> materialFolders;
    std::vector<StudioPrimitive> parts;
    std::vector<std::vector<glm::vec3>> physicsConvexes;
    float physicsMass = 0.0F;
    float physicsDamping = 0.0F;
    float physicsRotDamping = 0.05F;
    float physicsInertia = 1.0F;
    std::string physicsSurface;
    bool hasMesh = false;
    bool hasPhysicsHull = false;
    StudioAnimation animation;
};

class GameFileSystem;

StudioModel LoadStudioModel(const GameFileSystem* files, const std::string& modelPath);
int FindStudioSequence(const StudioAnimation& animation, std::string_view hint);
bool SampleStudioPose(const StudioAnimation& animation, int sequence, float timeSeconds, std::vector<glm::mat4>& skin);
