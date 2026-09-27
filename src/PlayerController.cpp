#include "PlayerController.hpp"

#include <algorithm>

#include <GLFW/glfw3.h>
#include <glm/geometric.hpp>

#include "PhysicsWorld.hpp"
#include "SourceCoords.hpp"

PlayerController::PlayerController()
    : position_(0.0F, 1.8F, 3.0F) {}

void PlayerController::SetWindow(GLFWwindow* window) {
    window_ = window;
}

void PlayerController::SetPosition(const glm::vec3& position) {
    position_ = position;
    velocity_ = glm::vec3(0.0F);
    horizontalVelocity_ = glm::vec3(0.0F);
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

void PlayerController::SetInWater(bool inWater) {
    inWater_ = inWater;
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
    const bool duck = glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS;
    if (physicsWorld_ != nullptr) {
        physicsWorld_->SetDucked(duck && !noclip);
    }
    eyeHeight_ = (duck && !noclip ? 28.0F : 64.0F) * kSourceToWorld;
    const float speed = (duck ? 63.0F : fast ? 320.0F : 190.0F) * kSourceToWorld;
    glm::vec3 movement(0.0F);
    if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) movement += lookDirection;
    if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) movement -= lookDirection;
    if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) movement -= rightDirection;
    if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) movement += rightDirection;

    if (noclip_) {
        const float noclipSpeed = (fast ? 12.0F : 6.0F);
        if (glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS) movement.y += 1.0F;
        if (glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS) movement.y -= 1.0F;
        if (glm::length(movement) > 0.0F) movement = glm::normalize(movement) * noclipSpeed * deltaTime;
        position_ += movement;
        horizontalVelocity_ = glm::vec3(0.0F);
        if (physicsWorld_ != nullptr) {
            physicsWorld_->UpdateNoclip(position_);
        }
        return;
    }

    movement.y = 0.0F;
    glm::vec3 wishDirection(0.0F);
    if (glm::length(movement) > 0.0F) {
        wishDirection = glm::normalize(movement);
    }
    auto accelerate = [&](const glm::vec3& direction, float wishSpeed, float acceleration) {
        const float current = glm::dot(horizontalVelocity_, direction);
        const float add = wishSpeed - current;
        if (add <= 0.0F) {
            return;
        }
        float accelSpeed = acceleration * wishSpeed * deltaTime;
        if (accelSpeed > add) {
            accelSpeed = add;
        }
        horizontalVelocity_ += direction * accelSpeed;
    };
    if (inWater_ && !noclip_) {
        const float horizontalSpeed = glm::length(horizontalVelocity_);
        if (horizontalSpeed > 0.0F) {
            const float newSpeed = std::max(0.0F, horizontalSpeed - horizontalSpeed * deltaTime);
            horizontalVelocity_ *= newSpeed / horizontalSpeed;
        }
        const float swimSpeed = speed * 0.8F;
        if (glm::length(wishDirection) > 0.0F) {
            accelerate(wishDirection, swimSpeed, 10.0F);
        }
        float vertical = 0.0F;
        const bool ascend = glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS;
        const bool descend = glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS;
        if (ascend) {
            vertical += 150.0F * kSourceToWorld;
        }
        if (descend) {
            vertical -= 150.0F * kSourceToWorld;
        }
        jumpWasPressed_ = ascend;
        if (physicsWorld_ != nullptr) {
            physicsWorld_->UpdateCharacter(
                deltaTime,
                glm::vec3(horizontalVelocity_.x, vertical, horizontalVelocity_.z),
                false,
                true
            );
            position_ = physicsWorld_->CharacterPosition();
            grounded_ = physicsWorld_->IsCharacterGrounded();
        }
        return;
    }
    if (grounded_) {
        const float horizontalSpeed = glm::length(horizontalVelocity_);
        if (horizontalSpeed > 0.0F) {
            const float stopSpeed = 100.0F * kSourceToWorld;
            const float drop = std::max(horizontalSpeed, stopSpeed) * 4.0F * deltaTime;
            const float newSpeed = std::max(0.0F, horizontalSpeed - drop);
            horizontalVelocity_ *= newSpeed / horizontalSpeed;
        }
        if (glm::length(wishDirection) > 0.0F) {
            accelerate(wishDirection, speed, 10.0F);
        }
    } else if (glm::length(wishDirection) > 0.0F) {
        accelerate(wishDirection, std::min(speed, 30.0F * kSourceToWorld), 10.0F);
    }
    const bool jumpPressed = glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS;
    const bool jump = jumpPressed && !jumpWasPressed_;
    jumpWasPressed_ = jumpPressed;

    if (physicsWorld_ != nullptr) {
        physicsWorld_->UpdateCharacter(deltaTime, horizontalVelocity_, jump);
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
