#include "AiNavigation.hpp"

#include "BspLoader.hpp"
#include "PhysicsWorld.hpp"
#include "SourceCoords.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <queue>
#include <string>
#include <unordered_map>
#include <utility>

namespace {

std::string Lower(std::string value) {
    for (char& character : value) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    return value;
}

std::string PropertyCi(const BspMapEntity& entity, const char* key) {
    const std::string wanted = Lower(key);
    for (const auto& property : entity.properties) {
        if (Lower(property.first) == wanted) {
            return property.second;
        }
    }
    return {};
}

constexpr float kAutoLinkDistance = 384.0F * kSourceToWorld;
constexpr float kStepHeight = 48.0F * kSourceToWorld;
constexpr float kCoverRadius = 512.0F * kSourceToWorld;
constexpr float kCoverSeparation = 64.0F * kSourceToWorld;

struct CellKey {
    int x = 0;
    int z = 0;

    bool operator==(const CellKey& other) const {
        return x == other.x && z == other.z;
    }
};

struct CellKeyHash {
    std::size_t operator()(const CellKey& key) const {
        return static_cast<std::size_t>(key.x) * 73856093U ^ static_cast<std::size_t>(key.z) * 19349663U;
    }
};

bool RayClear(const PhysicsWorld& physics, const glm::vec3& from, const glm::vec3& to, float height, std::uint64_t ignore) {
    const glm::vec3 start = from + glm::vec3(0.0F, height, 0.0F);
    const glm::vec3 end = to + glm::vec3(0.0F, height, 0.0F);
    const glm::vec3 delta = end - start;
    const float distance = glm::length(delta);
    if (distance < 1.0F * kSourceToWorld) {
        return true;
    }
    const PhysicsRayHit hit = physics.RayCast(start, delta, distance, ignore);
    return !hit.hit || hit.distance > distance - 8.0F * kSourceToWorld;
}

bool WalkLinkClear(const PhysicsWorld& physics, const glm::vec3& from, const glm::vec3& to) {
    if (std::abs(from.y - to.y) > kStepHeight) {
        return false;
    }
    return RayClear(physics, from, to, 18.0F * kSourceToWorld, 0)
        && RayClear(physics, from, to, 36.0F * kSourceToWorld, 0);
}

bool SeesTarget(const PhysicsWorld& physics, const glm::vec3& from, const glm::vec3& target) {
    const glm::vec3 start = from + glm::vec3(0.0F, 36.0F * kSourceToWorld, 0.0F);
    const glm::vec3 delta = target - start;
    const float distance = glm::length(delta);
    if (distance < 1.0F * kSourceToWorld) {
        return true;
    }
    const PhysicsRayHit hit = physics.RayCast(start, delta, distance);
    return !hit.hit || hit.distance > distance - 8.0F * kSourceToWorld;
}

} // namespace

void AiNavigation::Build(const std::vector<BspMapEntity>& entities, const PhysicsWorld& physics) {
    nodes_.clear();
    std::vector<BspMapEntity> linkEntities;
    for (const BspMapEntity& entity : entities) {
        const std::string classname = Lower(entity.classname);
        if (classname == "info_node" || classname == "info_node_hint") {
            Node node;
            node.position = entity.position;
            node.name = Lower(PropertyCi(entity, "targetname"));
            nodes_.push_back(std::move(node));
        } else if (classname == "info_node_link") {
            linkEntities.push_back(entity);
        }
    }
    if (nodes_.size() < 2) {
        return;
    }

    std::unordered_map<std::string, int> named;
    for (int index = 0; index < static_cast<int>(nodes_.size()); ++index) {
        const std::string& name = nodes_[static_cast<std::size_t>(index)].name;
        if (!name.empty()) {
            named.emplace(name, index);
        }
    }

    auto connect = [this](int left, int right) {
        auto& leftLinks = nodes_[static_cast<std::size_t>(left)].links;
        auto& rightLinks = nodes_[static_cast<std::size_t>(right)].links;
        if (std::find(leftLinks.begin(), leftLinks.end(), right) == leftLinks.end()) {
            leftLinks.push_back(right);
        }
        if (std::find(rightLinks.begin(), rightLinks.end(), left) == rightLinks.end()) {
            rightLinks.push_back(left);
        }
    };

    int explicitLinks = 0;
    for (const BspMapEntity& link : linkEntities) {
        const auto start = named.find(Lower(PropertyCi(link, "StartNode")));
        const auto end = named.find(Lower(PropertyCi(link, "EndNode")));
        if (start == named.end() || end == named.end() || start->second == end->second) {
            continue;
        }
        connect(start->second, end->second);
        ++explicitLinks;
    }
    if (explicitLinks > 0) {
        return;
    }

    const float cellSize = kAutoLinkDistance;
    std::unordered_map<CellKey, std::vector<int>, CellKeyHash> grid;
    grid.reserve(nodes_.size());
    for (int index = 0; index < static_cast<int>(nodes_.size()); ++index) {
        const glm::vec3& position = nodes_[static_cast<std::size_t>(index)].position;
        grid[CellKey{
            static_cast<int>(std::floor(position.x / cellSize)),
            static_cast<int>(std::floor(position.z / cellSize)),
        }].push_back(index);
    }

    for (int index = 0; index < static_cast<int>(nodes_.size()); ++index) {
        const glm::vec3& position = nodes_[static_cast<std::size_t>(index)].position;
        const int cellX = static_cast<int>(std::floor(position.x / cellSize));
        const int cellZ = static_cast<int>(std::floor(position.z / cellSize));
        for (int dz = -1; dz <= 1; ++dz) {
            for (int dx = -1; dx <= 1; ++dx) {
                const auto found = grid.find(CellKey{cellX + dx, cellZ + dz});
                if (found == grid.end()) {
                    continue;
                }
                for (const int other : found->second) {
                    if (other <= index) {
                        continue;
                    }
                    const glm::vec3& otherPosition = nodes_[static_cast<std::size_t>(other)].position;
                    if (glm::length(otherPosition - position) > kAutoLinkDistance) {
                        continue;
                    }
                    if (!WalkLinkClear(physics, position, otherPosition)) {
                        continue;
                    }
                    connect(index, other);
                }
            }
        }
    }
}

bool AiNavigation::Empty() const {
    return nodes_.empty();
}

int AiNavigation::Nearest(const glm::vec3& position) const {
    int best = -1;
    float bestDistance = 0.0F;
    for (int index = 0; index < static_cast<int>(nodes_.size()); ++index) {
        const float distance = glm::length(nodes_[static_cast<std::size_t>(index)].position - position);
        if (best < 0 || distance < bestDistance) {
            best = index;
            bestDistance = distance;
        }
    }
    return best;
}

int AiNavigation::NearestClear(const glm::vec3& position, const PhysicsWorld& physics, std::uint64_t ignoreUserData) const {
    int best = -1;
    float bestDistance = 0.0F;
    for (int index = 0; index < static_cast<int>(nodes_.size()); ++index) {
        const glm::vec3& node = nodes_[static_cast<std::size_t>(index)].position;
        const float distance = glm::length(node - position);
        if (best >= 0 && distance >= bestDistance) {
            continue;
        }
        if (!RayClear(physics, position, node, 36.0F * kSourceToWorld, ignoreUserData)) {
            continue;
        }
        best = index;
        bestDistance = distance;
    }
    return best;
}

std::vector<glm::vec3> AiNavigation::FindPath(
    const glm::vec3& from,
    const glm::vec3& to,
    const PhysicsWorld& physics,
    std::uint64_t ignoreUserData
) const {
    if (nodes_.empty()) {
        return {};
    }
    const int start = NearestClear(from, physics, ignoreUserData);
    const int goal = Nearest(to);
    if (start < 0 || goal < 0) {
        return {};
    }

    if (start == goal) {
        return {nodes_[static_cast<std::size_t>(goal)].position};
    }

    const int count = static_cast<int>(nodes_.size());
    std::vector<float> cost(static_cast<std::size_t>(count), 1.0e30F);
    std::vector<int> previous(static_cast<std::size_t>(count), -1);
    std::vector<char> closed(static_cast<std::size_t>(count), 0);
    using Rank = std::pair<float, int>;
    std::priority_queue<Rank, std::vector<Rank>, std::greater<Rank>> open;
    cost[static_cast<std::size_t>(start)] = 0.0F;
    open.emplace(glm::length(nodes_[static_cast<std::size_t>(goal)].position - nodes_[static_cast<std::size_t>(start)].position), start);

    while (!open.empty()) {
        const int current = open.top().second;
        open.pop();
        if (closed[static_cast<std::size_t>(current)] != 0) {
            continue;
        }
        if (current == goal) {
            break;
        }
        closed[static_cast<std::size_t>(current)] = 1;
        for (const int next : nodes_[static_cast<std::size_t>(current)].links) {
            const float step = glm::length(
                nodes_[static_cast<std::size_t>(next)].position - nodes_[static_cast<std::size_t>(current)].position
            );
            const float nextCost = cost[static_cast<std::size_t>(current)] + step;
            if (nextCost >= cost[static_cast<std::size_t>(next)]) {
                continue;
            }
            cost[static_cast<std::size_t>(next)] = nextCost;
            previous[static_cast<std::size_t>(next)] = current;
            const float rank = nextCost + glm::length(nodes_[static_cast<std::size_t>(goal)].position - nodes_[static_cast<std::size_t>(next)].position);
            open.emplace(rank, next);
        }
    }

    if (previous[static_cast<std::size_t>(goal)] < 0) {
        return {};
    }

    std::vector<int> reversed;
    for (int cursor = goal; cursor >= 0; cursor = previous[static_cast<std::size_t>(cursor)]) {
        reversed.push_back(cursor);
        if (cursor == start) {
            break;
        }
    }
    if (reversed.empty() || reversed.back() != start) {
        return {};
    }
    std::vector<glm::vec3> path;
    path.reserve(reversed.size());
    for (auto it = reversed.rbegin(); it != reversed.rend(); ++it) {
        path.push_back(nodes_[static_cast<std::size_t>(*it)].position);
    }
    return path;
}

bool AiNavigation::FiringNode(
    const glm::vec3& from,
    const glm::vec3& target,
    const PhysicsWorld& physics,
    glm::vec3& out
) const {
    int best = -1;
    float bestDistance = 0.0F;
    for (int index = 0; index < static_cast<int>(nodes_.size()); ++index) {
        const glm::vec3& node = nodes_[static_cast<std::size_t>(index)].position;
        if (!SeesTarget(physics, node, target)) {
            continue;
        }
        const float distance = glm::length(glm::vec3(node.x - from.x, 0.0F, node.z - from.z));
        if (best < 0 || distance < bestDistance) {
            best = index;
            bestDistance = distance;
        }
    }
    if (best < 0) {
        return false;
    }
    out = nodes_[static_cast<std::size_t>(best)].position;
    return true;
}

bool AiNavigation::CoverNode(
    const glm::vec3& from,
    const glm::vec3& target,
    const PhysicsWorld& physics,
    glm::vec3& out
) const {
    int best = -1;
    float bestDistance = 0.0F;
    for (int index = 0; index < static_cast<int>(nodes_.size()); ++index) {
        const glm::vec3& node = nodes_[static_cast<std::size_t>(index)].position;
        const float distance = glm::length(glm::vec3(node.x - from.x, 0.0F, node.z - from.z));
        if (distance < kCoverSeparation || distance > kCoverRadius) {
            continue;
        }
        if (SeesTarget(physics, node, target)) {
            continue;
        }
        if (best < 0 || distance < bestDistance) {
            best = index;
            bestDistance = distance;
        }
    }
    if (best < 0) {
        return false;
    }
    out = nodes_[static_cast<std::size_t>(best)].position;
    return true;
}
