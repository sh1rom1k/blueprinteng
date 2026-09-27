#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <glm/vec3.hpp>

class PhysicsWorld;
struct BspMapEntity;

class AiNavigation {
public:
    void Build(const std::vector<BspMapEntity>& entities, const PhysicsWorld& physics);

    [[nodiscard]] bool Empty() const;
    [[nodiscard]] std::vector<glm::vec3> FindPath(
        const glm::vec3& from,
        const glm::vec3& to,
        const PhysicsWorld& physics,
        std::uint64_t ignoreUserData = 0
    ) const;
    [[nodiscard]] bool FiringNode(
        const glm::vec3& from,
        const glm::vec3& target,
        const PhysicsWorld& physics,
        glm::vec3& out
    ) const;
    [[nodiscard]] bool CoverNode(
        const glm::vec3& from,
        const glm::vec3& target,
        const PhysicsWorld& physics,
        glm::vec3& out
    ) const;

private:
    struct Node {
        glm::vec3 position{0.0F};
        std::string name;
        std::vector<int> links;
    };

    [[nodiscard]] int Nearest(const glm::vec3& position) const;
    [[nodiscard]] int NearestClear(const glm::vec3& position, const PhysicsWorld& physics, std::uint64_t ignoreUserData) const;

    std::vector<Node> nodes_;
};
