#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include "PhysicsWorld.hpp"

class AudioSystem;
class BspLoader;
class GameFileSystem;
class GLFWwindow;
class PlayerController;
class Shader;

struct PlayerTransitionState {
    float health = 100.0F;
    bool owned[5] = {true, true, false, false, true};
    int ammo[5] = {1, 150, 0, 0, 1};
    int weapon = 0;
};

struct CarriedEntity {
    std::string classname;
    std::string model;
    glm::vec3 originOffset{0.0F};
    glm::mat4 rotation{1.0F};
    float health = 0.0F;
    int ammoAmount = 0;
};

struct LevelChangeRequest {
    std::string mapName;
    std::string landmarkName;
    glm::vec3 playerFeet{0.0F};
    glm::vec3 landmarkOrigin{0.0F};
    PlayerTransitionState player;
    std::vector<CarriedEntity> entities;
};

class GameSimulation {
public:
    GameSimulation(const BspLoader& map, PhysicsWorld& physics, const GameFileSystem* files, AudioSystem* audio = nullptr);
    ~GameSimulation();

    GameSimulation(const GameSimulation&) = delete;
    GameSimulation& operator=(const GameSimulation&) = delete;

    void Update(float deltaTime, PlayerController& player, const glm::vec3& eye, const glm::vec3& look, GLFWwindow* window);
    void Draw(const Shader& shader) const;
    void DrawTransparent(const Shader& shader) const;
    void DrawSky(const Shader& shader) const;
    void DrawSkyTransparent(const Shader& shader) const;
    [[nodiscard]] int Health() const;
    [[nodiscard]] bool ConsumeLevelChange(LevelChangeRequest& request);
    [[nodiscard]] bool FindLandmark(const std::string& name, glm::vec3& origin) const;
    void RestorePlayerState(const PlayerTransitionState& state);
    void SpawnCarriedEntities(const std::vector<CarriedEntity>& entities, const glm::vec3& landmarkOrigin);
    void SetCurrentMap(const std::string& mapName);
    void AppendDebugShapes(std::vector<PhysicsDebugShape>& out) const;
    void CollectDoorStates(std::vector<std::pair<std::string, bool>>& out) const;
    void SetViewPosition(const glm::vec3& position);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
