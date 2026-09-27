#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

struct PhysicsRayHit {
    float distance = 0.0F;
    glm::vec3 normal{0.0F, 1.0F, 0.0F};
    std::uint64_t userData = 0;
    bool hit = false;
};

struct PhysicsImpact {
    std::uint64_t first = 0;
    std::uint64_t second = 0;
    float speed = 0.0F;
};

struct PhysicsDebugShape {
    std::uint64_t key = 0;
    std::uint32_t revision = 0;
    glm::mat4 transform{1.0F};
    const std::vector<glm::vec3>* vertices = nullptr;
    const std::vector<std::uint32_t>* indices = nullptr;
};

struct PhysicsBodyDesc {
    float mass = 10.0F;
    float inertiaScale = 1.0F;
    float friction = 0.8F;
    float restitution = 0.05F;
    float linearDamping = 0.0F;
    float angularDamping = 0.05F;
    bool startAsleep = false;
    bool motionDisabled = false;
};

class PhysicsWorld {
public:
    PhysicsWorld();
    ~PhysicsWorld();

    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;

    void SetCollisionMesh(const std::vector<glm::vec3>& vertices, const std::vector<std::uint32_t>& indices);
    void SetDynamicBoxes(const std::vector<glm::vec3>& positions);
    void SpawnDynamicBox(const glm::vec3& position, const glm::vec3& linearVelocity = glm::vec3(0.0F));
    std::vector<glm::vec3> DynamicBoxPositions() const;
    std::vector<glm::mat4> DynamicBoxMatrices() const;
    std::size_t DynamicBoxCount() const;
    void StepSimulation(float deltaTime);
    void SetCharacterPosition(const glm::vec3& position);
    void UpdateCharacter(float deltaTime, const glm::vec3& desiredVelocity, bool jump, bool inWater = false);
    void UpdateNoclip(const glm::vec3& position);

    glm::vec3 CharacterPosition() const;
    bool IsCharacterGrounded() const;

    int AddStaticBox(const glm::vec3& position, const glm::vec3& halfExtents, std::uint64_t userData);
    int AddDynamicBox(const glm::vec3& position, const glm::vec3& halfExtents, float mass, std::uint64_t userData);
    int AddConvexBody(
        const std::vector<glm::vec3>& points,
        const glm::mat4& transform,
        float mass,
        bool dynamic,
        std::uint64_t userData
    );
    int AddDynamicCompound(
        const std::vector<std::vector<glm::vec3>>& convexes,
        const glm::mat4& transform,
        const PhysicsBodyDesc& desc,
        std::uint64_t userData
    );
    int AddStaticCompound(
        const std::vector<std::vector<glm::vec3>>& convexes,
        const glm::mat4& transform,
        std::uint64_t userData
    );
    int AddMeshBody(
        const std::vector<glm::vec3>& vertices,
        const std::vector<std::uint32_t>& indices,
        const glm::mat4& transform,
        bool kinematic,
        std::uint64_t userData
    );
    void SetBodyTransform(int body, const glm::mat4& transform);
    void SetBodyVelocity(int body, const glm::vec3& linear);
    void SetBodyAngularVelocity(int body, const glm::vec3& angular);
    void ApplyBodyImpulse(int body, const glm::vec3& impulse);
    void AddBodyForce(int body, const glm::vec3& force);
    void SetBodyGravityScale(int body, float scale);
    void SetBodyMotionEnabled(int body, bool enabled);
    void DestroySimBody(int body);
    [[nodiscard]] glm::vec3 BodyPosition(int body) const;
    [[nodiscard]] glm::quat BodyRotation(int body) const;
    [[nodiscard]] glm::vec3 BodyLinearVelocity(int body) const;
    [[nodiscard]] glm::vec3 BodyAngularVelocity(int body) const;
    [[nodiscard]] std::vector<PhysicsImpact> ConsumeImpacts();
    [[nodiscard]] glm::mat4 BodyDrawMatrix(int body) const;
    [[nodiscard]] PhysicsRayHit RayCast(
        const glm::vec3& origin,
        const glm::vec3& direction,
        float distance,
        std::uint64_t ignoreUserData = 0
    ) const;
    [[nodiscard]] std::vector<PhysicsRayHit> RayCastAll(
        const glm::vec3& origin,
        const glm::vec3& direction,
        float distance,
        std::uint64_t ignoreUserData = 0
    ) const;
    void SetDucked(bool ducked);
    void CollectDebugShapes(std::vector<PhysicsDebugShape>& out) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
