#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

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
    void UpdateCharacter(float deltaTime, const glm::vec3& desiredVelocity, bool jump);
    void UpdateNoclip(const glm::vec3& position);

    glm::vec3 CharacterPosition() const;
    bool IsCharacterGrounded() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
