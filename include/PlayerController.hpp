#pragma once

#include <glm/vec3.hpp>

class PhysicsWorld;
struct GLFWwindow;

class PlayerController {
public:
    PlayerController();

    void SetWindow(GLFWwindow* window);
    void SetPosition(const glm::vec3& position);
    void SetWorldBounds(const glm::vec3& minimum, const glm::vec3& maximum);
    void SetPhysicsWorld(PhysicsWorld* physicsWorld);
    void Update(float deltaTime, const glm::vec3& lookDirection, const glm::vec3& rightDirection, bool noclip, bool fast);

    const glm::vec3& Position() const;
    glm::vec3 EyePosition() const;
    bool IsGrounded() const;
    bool IsNoclip() const;

private:
    GLFWwindow* window_ = nullptr;
    glm::vec3 position_;
    glm::vec3 velocity_{0.0F};
    glm::vec3 worldMinimum_{-16.0F, -16.0F, -16.0F};
    glm::vec3 worldMaximum_{16.0F, 16.0F, 16.0F};
    float radius_ = 0.22F;
    float height_ = 1.26F;
    float eyeHeight_ = 0.95F;
    bool grounded_ = false;
    bool noclip_ = false;
    bool jumpWasPressed_ = false;
    PhysicsWorld* physicsWorld_ = nullptr;
};
