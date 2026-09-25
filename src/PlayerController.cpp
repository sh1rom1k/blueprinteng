#include "PlayerController.hpp"

#include <algorithm>

#include <GLFW/glfw3.h>
#include <glm/geometric.hpp>

#include "PhysicsWorld.hpp"

PlayerController::PlayerController()
    : position_(0.0F, 1.8F, 3.0F) {}

void PlayerController::SetWindow(GLFWwindow* window) {
    window_ = window;
}

void PlayerController::SetPosition(const glm::vec3& position) {
    position_ = position;
    velocity_ = glm::vec3(0.0F);
    grounded_ = false;
    if (physicsWorld_ != nullptr) {
        physicsWorld_->SetCharacterPosition(position_);
    }
}

void PlayerController::SetWorldBounds(const glm::vec3& minimum, const glm::vec3& maximum) {
    worldMinimum_ = minimum;
    worldMaximum_ = maximum;
}

void PlayerController::SetPhysicsWorld(PhysicsWorld* physicsWorld) {
    physicsWorld_ = physicsWorld;
    if (physicsWorld_ != nullptr) {
        physicsWorld_->SetCharacterPosition(position_);
    }
}

void PlayerController::Update(
    float deltaTime,
    const glm::vec3& lookDirection,
    const glm::vec3& rightDirection,
    bool noclip,
    bool fast
) {
    GLFWwindow* window = window_ ? window_ : glfwGetCurrentContext();
    noclip_ = noclip;
    const float speed = (fast ? 10.0F : 4.0F);
    glm::vec3 movement(0.0F);
    if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) movement += lookDirection;
    if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) movement -= lookDirection;
    if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) movement -= rightDirection;
    if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) movement += rightDirection;

    if (noclip_) {
        if (glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS) movement.y += 1.0F;
        if (glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS) movement.y -= 1.0F;
        if (glm::length(movement) > 0.0F) movement = glm::normalize(movement) * speed * deltaTime;
        position_ += movement;
        if (physicsWorld_ != nullptr) {
            physicsWorld_->UpdateNoclip(position_);
        }
        return;
    }

    movement.y = 0.0F;
    if (glm::length(movement) > 0.0F) movement = glm::normalize(movement) * speed;
    const bool jumpPressed = glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS;
    const bool jump = jumpPressed && !jumpWasPressed_;
    jumpWasPressed_ = jumpPressed;

    if (physicsWorld_ != nullptr) {
        physicsWorld_->UpdateCharacter(deltaTime, movement, jump);
        position_ = physicsWorld_->CharacterPosition();
        grounded_ = physicsWorld_->IsCharacterGrounded();
        return;
    }

    position_.x += movement.x * deltaTime;
    position_.z += movement.z * deltaTime;
    position_.x = std::clamp(position_.x, worldMinimum_.x, worldMaximum_.x);
    position_.z = std::clamp(position_.z, worldMinimum_.z, worldMaximum_.z);
}

const glm::vec3& PlayerController::Position() const {
    return position_;
}

glm::vec3 PlayerController::EyePosition() const {
    return position_ + glm::vec3(0.0F, eyeHeight_, 0.0F);
}

bool PlayerController::IsGrounded() const {
    return grounded_;
}

bool PlayerController::IsNoclip() const {
    return noclip_;
}
