#include "GameSimulation.hpp"

#include "AiNavigation.hpp"
#include "AudioSystem.hpp"
#include "BspLoader.hpp"
#include "GameFileSystem.hpp"
#include "Mesh.hpp"
#include "NpcSpeech.hpp"
#include "PhysicsWorld.hpp"
#include "PlayerController.hpp"
#include "Shader.hpp"
#include "SourceCoords.hpp"
#include "StudioModel.hpp"
#include "VtfTexture.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <glad/gl.h>
#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace {

std::string Lower(std::string value) {
    for (char& character : value) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    return value;
}

std::string Trim(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.erase(value.begin());
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        value.pop_back();
    }
    return value;
}

const std::string& Property(const BspMapEntity& entity, const char* key) {
    static const std::string empty;
    for (const auto& property : entity.properties) {
        if (property.first == key) {
            return property.second;
        }
    }
    return empty;
}

const std::string& PropertyCi(const BspMapEntity& entity, const char* key) {
    static const std::string empty;
    const std::string wanted = Lower(key);
    for (const auto& property : entity.properties) {
        if (Lower(property.first) == wanted) {
            return property.second;
        }
    }
    return empty;
}

std::string MapStem(std::string mapName) {
    mapName = Trim(mapName);
    const std::size_t slash = mapName.find_last_of("/\\");
    if (slash != std::string::npos) {
        mapName.erase(0, slash + 1);
    }
    if (mapName.size() >= 4 && Lower(mapName.substr(mapName.size() - 4)) == ".bsp") {
        mapName.resize(mapName.size() - 4);
    }
    return mapName;
}

bool CanCarryAcrossTransition(const std::string& classname) {
    const std::string name = Lower(classname);
    return name.rfind("prop_physics", 0) == 0
        || name.rfind("npc_", 0) == 0
        || name.rfind("weapon_", 0) == 0
        || name.rfind("item_", 0) == 0;
}

bool AabbOverlap(const glm::vec3& minimumA, const glm::vec3& maximumA, const glm::vec3& minimumB, const glm::vec3& maximumB) {
    return minimumA.x <= maximumB.x && maximumA.x >= minimumB.x
        && minimumA.y <= maximumB.y && maximumA.y >= minimumB.y
        && minimumA.z <= maximumB.z && maximumA.z >= minimumB.z;
}

float PropertyFloatFrom(const std::string& value, float fallback) {
    if (value.empty()) {
        return fallback;
    }
    try {
        return std::stof(value);
    } catch (const std::exception&) {
        return fallback;
    }
}

float PropertyFloat(const BspMapEntity& entity, const char* key, float fallback) {
    return PropertyFloatFrom(Property(entity, key), fallback);
}

float PropertyFloatCi(const BspMapEntity& entity, const char* key, float fallback) {
    return PropertyFloatFrom(PropertyCi(entity, key), fallback);
}

std::string DefaultNpcModel(const std::string& classname) {
    if (classname == "npc_combine_s") {
        return "models/combine_soldier.mdl";
    }
    if (classname == "npc_metropolice") {
        return "models/police.mdl";
    }
    if (classname == "npc_citizen") {
        return "models/humans/group01/male_07.mdl";
    }
    return {};
}

float YawFromMatrix(const glm::mat4& rotation) {
    const glm::vec3 forward(rotation[0]);
    return std::atan2(-forward.z, forward.x) * (180.0F / 3.14159265F);
}

float WrapYaw(float yaw) {
    while (yaw > 180.0F) {
        yaw -= 360.0F;
    }
    while (yaw < -180.0F) {
        yaw += 360.0F;
    }
    return yaw;
}

float YawToward(const glm::vec3& direction) {
    return std::atan2(-direction.z, direction.x) * (180.0F / 3.14159265F);
}

void ApproachYaw(float& yaw, float desired, float deltaTime) {
    const float delta = WrapYaw(desired - yaw);
    const float step = 180.0F * deltaTime;
    if (std::abs(delta) <= step) {
        yaw = desired;
    } else {
        yaw += std::copysign(step, delta);
    }
    yaw = WrapYaw(yaw);
}

constexpr int kNpcIdle = 0;
constexpr int kNpcAlert = 1;
constexpr int kNpcFace = 2;
constexpr int kNpcScript = 3;
constexpr int kNpcShoot = 4;
constexpr int kNpcCover = 5;
constexpr int kNpcLineOfFire = 6;
constexpr int kNpcPress = 7;
constexpr int kNpcWaitForScript = 1 << 5;

std::string NpcSoundPrefix(const std::string& classname) {
    if (classname == "npc_combine_s") {
        return "NPC_CombineS";
    }
    if (classname == "npc_metropolice") {
        return "NPC_MetroPolice";
    }
    if (classname == "npc_citizen") {
        return "NPC_Citizen";
    }
    return {};
}

constexpr float kMaxCarryMass = 35.0F;
constexpr float kGravity = 800.0F * kSourceToWorld;

void SurfaceResponse(const std::string& surface, float& friction, float& restitution) {
    const std::string name = Lower(surface);
    const auto has = [&](const char* token) {
        return name.find(token) != std::string::npos;
    };
    if (has("metal") || has("chain") || has("grate")) {
        friction = 0.35F;
        restitution = 0.2F;
        return;
    }
    if (has("glass") || has("tile") || has("ice")) {
        friction = 0.25F;
        restitution = 0.15F;
        return;
    }
    if (has("rubber")) {
        friction = 0.9F;
        restitution = 0.35F;
        return;
    }
    if (has("wood") || has("cardboard") || has("plastic")) {
        friction = 0.7F;
        restitution = 0.05F;
        return;
    }
    friction = 0.8F;
    restitution = 0.08F;
}

std::vector<glm::vec3> HullBox(const glm::vec3& minimum, const glm::vec3& maximum) {
    const glm::vec3 low = glm::min(minimum, maximum);
    const glm::vec3 high = glm::max(low + glm::vec3(0.08F), glm::max(minimum, maximum));
    return {
        {low.x, low.y, low.z},
        {high.x, low.y, low.z},
        {high.x, high.y, low.z},
        {low.x, high.y, low.z},
        {low.x, low.y, high.z},
        {high.x, low.y, high.z},
        {high.x, high.y, high.z},
        {low.x, high.y, high.z},
    };
}

float EstimateMass(const glm::vec3& minimum, const glm::vec3& maximum) {
    const glm::vec3 size = glm::max(glm::abs(maximum - minimum), glm::vec3(0.05F));
    return std::clamp(size.x * size.y * size.z * 350.0F, 1.0F, 250.0F);
}

glm::quat ViewOrientation(const glm::vec3& look) {
    const glm::vec3 forward = glm::length(look) > 0.001F ? glm::normalize(look) : glm::vec3(0.0F, 0.0F, -1.0F);
    glm::vec3 up(0.0F, 1.0F, 0.0F);
    if (std::abs(glm::dot(forward, up)) > 0.98F) {
        up = glm::vec3(0.0F, 0.0F, 1.0F);
    }
    const glm::vec3 right = glm::normalize(glm::cross(up, forward));
    up = glm::normalize(glm::cross(forward, right));
    const glm::mat3 basis(right, up, forward);
    return glm::quat_cast(basis);
}

bool StartsWith(const std::string& value, const char* prefix) {
    const std::string lowered = Lower(value);
    return lowered.rfind(prefix, 0) == 0;
}

std::string QuotedAfter(const std::string& text, const std::string& lowered, std::string_view key, std::size_t from) {
    std::size_t search = from;
    while (search < lowered.size()) {
        const std::size_t found = lowered.find(key, search);
        if (found == std::string::npos) {
            return {};
        }
        const std::size_t end = found + key.size();
        if (end < lowered.size()) {
            const unsigned char next = static_cast<unsigned char>(lowered[end]);
            if (std::isalnum(next) != 0 || lowered[end] == '_') {
                search = end;
                continue;
            }
        }
        std::size_t cursor = end;
        if (cursor < text.size() && text[cursor] == '"') {
            ++cursor;
        }
        while (cursor < text.size() && std::isspace(static_cast<unsigned char>(text[cursor])) != 0) {
            ++cursor;
        }
        if (cursor >= text.size() || text[cursor] != '"') {
            search = end;
            continue;
        }
        ++cursor;
        const std::size_t close = text.find('"', cursor);
        if (close == std::string::npos) {
            return {};
        }
        return text.substr(cursor, close - cursor);
    }
    return {};
}

std::string StripMaterialComments(std::string text) {
    std::string clean;
    clean.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] == '/' && index + 1 < text.size() && text[index + 1] == '/') {
            while (index < text.size() && text[index] != '\n') {
                ++index;
            }
            continue;
        }
        clean.push_back(text[index]);
    }
    return clean;
}

std::string NormalizeMaterialPath(std::string path) {
    for (char& character : path) {
        if (character == '\\') {
            character = '/';
        }
    }
    while (!path.empty() && path.front() == '/') {
        path.erase(path.begin());
    }
    if (path.rfind("materials/", 0) == 0) {
        path = path.substr(10);
    }
    if (path.size() >= 4) {
        std::string extension = path.substr(path.size() - 4);
        for (char& character : extension) {
            character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
        }
        if (extension == ".vmt" || extension == ".vtf") {
            path.resize(path.size() - 4);
        }
    }
    return path;
}

std::string BaseTextureFromVmt(const GameFileSystem* files, const std::string& material, int depth) {
    if (files == nullptr || material.empty() || depth > 4) {
        return material;
    }
    const std::string path = NormalizeMaterialPath(material);
    std::vector<std::uint8_t> vmt;
    if (!files->Read("materials/" + path + ".vmt", vmt) && !files->Read(path + ".vmt", vmt)) {
        return path;
    }
    const std::string text = StripMaterialComments(std::string(vmt.begin(), vmt.end()));
    const std::string lowered = Lower(text);
    const auto usable = [](const std::string& value) {
        if (value.empty() || value == "0" || Lower(value) == "env_cubemap") {
            return false;
        }
        return Lower(value).rfind("_rt_", 0) != 0;
    };
    for (const char* key : {"$basetexture", "$basetexture2", "$normalmap", "$bumpmap", "$tooltexture"}) {
        const std::string value = QuotedAfter(text, lowered, key, 0);
        if (usable(value)) {
            return NormalizeMaterialPath(value);
        }
    }
    const std::string include = QuotedAfter(text, lowered, "include", 0);
    if (!include.empty()) {
        return BaseTextureFromVmt(files, include, depth + 1);
    }
    return path;
}

std::string MaterialToken(const std::string& text, const std::string& lowered, std::string_view key) {
    std::size_t search = 0;
    while (search < lowered.size()) {
        const std::size_t found = lowered.find(key, search);
        if (found == std::string::npos) {
            return {};
        }
        std::size_t cursor = found + key.size();
        if (cursor < lowered.size()) {
            const unsigned char next = static_cast<unsigned char>(lowered[cursor]);
            if (std::isalnum(next) != 0 || lowered[cursor] == '_') {
                search = cursor;
                continue;
            }
        }
        if (cursor < text.size() && text[cursor] == '"') {
            ++cursor;
        }
        while (cursor < text.size() && std::isspace(static_cast<unsigned char>(text[cursor])) != 0) {
            ++cursor;
        }
        if (cursor >= text.size()) {
            return {};
        }
        if (text[cursor] == '"') {
            ++cursor;
            const std::size_t close = text.find('"', cursor);
            if (close == std::string::npos) {
                return {};
            }
            return text.substr(cursor, close - cursor);
        }
        const std::size_t start = cursor;
        while (cursor < text.size()) {
            const unsigned char character = static_cast<unsigned char>(text[cursor]);
            if (std::isspace(character) != 0 || text[cursor] == '"' || text[cursor] == '{' || text[cursor] == '}') {
                break;
            }
            ++cursor;
        }
        if (cursor == start) {
            search = cursor + 1;
            continue;
        }
        return text.substr(start, cursor - start);
    }
    return {};
}

int MaterialAlphaModeFromText(const std::string& text) {
    const std::string lowered = Lower(text);
    const auto enabled = [&](const char* key) {
        const std::string value = MaterialToken(text, lowered, key);
        return value == "1" || value == "1.0" || value == "true";
    };
    if (enabled("$additive")) {
        return 3;
    }
    if (enabled("$translucent")) {
        return 1;
    }
    if (enabled("$alphatest")) {
        return 2;
    }
    const std::string alpha = MaterialToken(text, lowered, "$alpha");
    if (!alpha.empty()) {
        try {
            if (std::stof(alpha) < 0.99F) {
                return 1;
            }
        } catch (const std::exception&) {
        }
    }
    return 0;
}

int MaterialAlphaMode(const GameFileSystem* files, const std::string& material, int depth = 0) {
    if (files == nullptr || material.empty() || depth > 4) {
        return 0;
    }
    const std::string path = NormalizeMaterialPath(material);
    const std::string loweredPath = Lower(path);
    if (loweredPath.find("glass") != std::string::npos
        || (loweredPath.find("window") != std::string::npos && loweredPath.find("frame") == std::string::npos)) {
        return 1;
    }
    std::vector<std::uint8_t> vmt;
    if (!files->Read("materials/" + path + ".vmt", vmt) && !files->Read(path + ".vmt", vmt)) {
        return 0;
    }
    const std::string text = StripMaterialComments(std::string(vmt.begin(), vmt.end()));
    const int mode = MaterialAlphaModeFromText(text);
    if (mode != 0) {
        return mode;
    }
    const std::string lowered = Lower(text);
    const std::size_t shaderStart = text.find('"');
    if (shaderStart != std::string::npos) {
        const std::size_t shaderEnd = text.find('"', shaderStart + 1);
        if (shaderEnd != std::string::npos) {
            const std::string shader = lowered.substr(shaderStart + 1, shaderEnd - shaderStart - 1);
            if (shader == "refract" || shader == "water") {
                return 1;
            }
        }
    }
    if (Lower(MaterialToken(text, lowered, "$surfaceprop")) == "glass") {
        return 1;
    }
    const std::string include = MaterialToken(text, lowered, "include");
    if (!include.empty()) {
        return MaterialAlphaMode(files, include, depth + 1);
    }
    return 0;
}

GLuint LoadMaterialTexture(const GameFileSystem* files, const std::string& material) {
    if (files == nullptr || material.empty()) {
        return 0;
    }
    std::string vtf = BaseTextureFromVmt(files, material, 0);
    if (vtf.rfind("materials/", 0) != 0) {
        vtf = "materials/" + vtf;
    }
    if (vtf.size() < 4 || Lower(vtf.substr(vtf.size() - 4)) != ".vtf") {
        vtf += ".vtf";
    }
    std::vector<std::uint8_t> bytes;
    if (!files->Read(vtf, bytes)) {
        return 0;
    }
    try {
        return vtf::LoadVtfTextureData(bytes, vtf);
    } catch (const std::exception&) {
        return 0;
    }
}

struct Output {
    std::string name;
    std::string target;
    std::string input;
    std::string parameter;
    float delay = 0.0F;
    int times = -1;
};

std::vector<Output> ParseOutputs(const BspMapEntity& entity) {
    std::vector<Output> outputs;
    for (const auto& property : entity.properties) {
        if (property.first.size() < 3 || Lower(property.first).rfind("on", 0) != 0) {
            continue;
        }
        std::vector<std::string> parts;
        std::string current;
        for (char character : property.second) {
            if (character == ',') {
                parts.push_back(Trim(current));
                current.clear();
            } else {
                current.push_back(character);
            }
        }
        parts.push_back(Trim(current));
        if (parts.size() < 2 || parts[0].empty()) {
            continue;
        }
        Output output;
        output.name = property.first;
        output.target = parts[0];
        output.input = parts[1];
        if (parts.size() >= 5) {
            try {
                output.times = std::stoi(parts.back());
            } catch (const std::exception&) {
                output.times = -1;
            }
            try {
                output.delay = std::stof(parts[parts.size() - 2]);
            } catch (const std::exception&) {
                output.delay = 0.0F;
            }
            for (std::size_t index = 2; index + 2 < parts.size(); ++index) {
                if (!output.parameter.empty()) {
                    output.parameter += ",";
                }
                output.parameter += parts[index];
            }
        } else if (parts.size() == 4) {
            output.parameter = parts[2];
            try {
                output.delay = std::stof(parts[3]);
            } catch (const std::exception&) {
                output.delay = 0.0F;
            }
        } else if (parts.size() == 3) {
            output.parameter = parts[2];
        }
        outputs.push_back(std::move(output));
    }
    return outputs;
}

enum class WeaponKind { Crowbar, Pistol, Smg, Shotgun, Physgun, Count };

WeaponKind WeaponFromClassname(const std::string& classname) {
    if (classname == "weapon_crowbar") {
        return WeaponKind::Crowbar;
    }
    if (classname == "weapon_pistol") {
        return WeaponKind::Pistol;
    }
    if (classname == "weapon_smg1") {
        return WeaponKind::Smg;
    }
    if (classname == "weapon_shotgun") {
        return WeaponKind::Shotgun;
    }
    if (classname == "weapon_physcannon") {
        return WeaponKind::Physgun;
    }
    if (classname.find("pistol") != std::string::npos) {
        return WeaponKind::Pistol;
    }
    if (classname.find("smg") != std::string::npos) {
        return WeaponKind::Smg;
    }
    if (classname.find("buckshot") != std::string::npos || classname.find("shotgun") != std::string::npos) {
        return WeaponKind::Shotgun;
    }
    return WeaponKind::Pistol;
}

struct Visual {
    std::vector<std::unique_ptr<Mesh>> meshes;
    std::vector<GLuint> textures;
    std::vector<int> alphaModes;
    std::vector<glm::vec3> points;
    std::vector<std::uint32_t> indices;
    std::vector<std::vector<glm::vec3>> physicsConvexes;
    glm::vec3 hullMin{-0.3F};
    glm::vec3 hullMax{0.3F};
    float physicsMass = 0.0F;
    float physicsDamping = 0.0F;
    float physicsRotDamping = 0.05F;
    float physicsInertia = 1.0F;
    std::string physicsSurface;
    bool hasPhysicsHull = false;
    std::vector<std::vector<float>> bindVertices;
    std::vector<std::vector<unsigned int>> bindIndices;
    std::vector<std::vector<StudioSkinVertex>> skinParts;
    StudioAnimation animation;
};

struct SimEntity {
    std::string classname;
    std::string targetName;
    std::string model;
    std::vector<Output> outputs;
    glm::vec3 origin{0.0F};
    glm::mat4 rotation{1.0F};
    int brush = -1;
    int body = -1;
    std::string visualKey;
    float health = 0.0F;
    float mass = 10.0F;
    float damageToEnableMotion = 0.0F;
    float motionDamage = 0.0F;
    float physicsDamageScale = 1.0F;
    bool motionDisabled = false;
    bool preventPickup = false;
    bool takePhysicsDamage = true;
    float openAmount = 0.0F;
    float doorSpeed = 100.0F * kSourceToWorld;
    float doorDistance = 1.0F;
    glm::vec3 doorDirection{0.0F, 1.0F, 0.0F};
    glm::vec3 pivot{0.0F};
    bool npc = false;
    bool physicsProp = false;
    bool staticProp = false;
    bool inSkybox = false;
    std::vector<int> visibilityLeaves;
    float fadeMin = 0.0F;
    float fadeMax = 0.0F;
    glm::vec3 lightTint{0.35F};
    bool hasLightingOrigin = false;
    bool door = false;
    bool rotatingDoor = false;
    bool breakable = false;
    std::string parentName;
    bool trigger = false;
    bool button = false;
    bool weaponItem = false;
    bool ammoItem = false;
    bool enabled = true;
    bool alive = true;
    bool fired = false;
    std::string changeMap;
    std::string landmarkName;
    WeaponKind weapon = WeaponKind::Pistol;
    int ammoAmount = 12;
    float nextAttack = 0.0F;
    bool npcHostile = false;
    float npcYaw = 0.0F;
    int npcSchedule = 0;
    int npcSpawnFlags = 0;
    bool npcScriptArmed = false;
    float npcStateTime = 0.0F;
    float npcGoalTime = 0.0F;
    glm::vec3 npcGoal{0.0F};
    bool npcHasGoal = false;
    std::vector<glm::vec3> npcPath;
    int npcPathCursor = 0;
    float npcRepath = 0.0F;
    float npcAnimTime = 0.0F;
    int npcPose = -1;
    bool npcGreeted = false;
    float npcNextSpeech = 0.0F;
    bool npcAlertSpoken = false;
    float npcFootstep = 0.0F;
    bool npcFootLeft = true;
    float npcStuckTime = 0.0F;
    glm::vec3 npcStuckAt{0.0F};
    bool npcRunning = false;
    float npcShootUntil = 0.0F;
    int scriptSlot = -1;
    bool scripted = false;
    std::string scriptNpcName;
    int scriptMove = 1;
    std::string scriptSequence;
    bool scriptActive = false;
    bool scriptFinished = false;
    float scriptHold = 0.0F;
    std::vector<std::unique_ptr<Mesh>> posedMeshes;
};

struct PendingInput {
    float when = 0.0F;
    std::size_t entity = 0;
    std::string input;
    std::string parameter;
};

} // namespace

struct GameSimulation::Impl {
    const BspLoader* map = nullptr;
    PhysicsWorld* physics = nullptr;
    const GameFileSystem* files = nullptr;
    std::vector<SimEntity> entities;
    std::unordered_map<std::string, Visual> visuals;
    std::unordered_map<std::string, GLuint> textures;
    GLuint missingTexture = 0;
    std::vector<PendingInput> pending;
    float time = 0.0F;
    glm::vec3 viewPosition{0.0F};
    float health = 100.0F;
    float nextPlayerHurt = 0.0F;
    WeaponKind weapon = WeaponKind::Crowbar;
    bool owned[5] = {true, true, false, false, true};
    int ammo[5] = {1, 150, 0, 0, 1};
    float nextFire = 0.0F;
    int carriedEntity = -1;
    bool carriedWithPhysgun = false;
    float carryDistance = 72.0F * kSourceToWorld;
    glm::quat carryLocalRotation{1.0F, 0.0F, 0.0F, 0.0F};
    bool fireWasDown = false;
    bool useWasDown = false;
    bool altWasDown = false;
    bool levelChangePending = false;
    LevelChangeRequest levelChange;
    std::string currentMap;
    AudioSystem* audio = nullptr;
    AiNavigation navigation;
    NpcSpeech speech;
    struct DebugHull {
        std::vector<glm::vec3> vertices;
        std::vector<std::uint32_t> indices;
        bool ready = false;
    };
    mutable std::vector<DebugHull> debugHulls;

    [[nodiscard]] glm::vec3 HalfExtents(const SimEntity& entity) const;
    void AppendDebugShapes(std::vector<PhysicsDebugShape>& out) const;
    [[nodiscard]] bool FindLandmark(const std::string& name, glm::vec3& origin) const;
    void TryQueueLevelChange(const SimEntity& trigger, const glm::vec3& playerFeet);
    void SpawnCarried(const CarriedEntity& carried, const glm::vec3& origin);
    void DestroyProp(SimEntity& entity);
    void EnableMotion(SimEntity& entity);
    int SpawnPropBody(SimEntity& entity, const glm::mat4& spawn, std::uint64_t userData, const PhysicsBodyDesc& desc);
    void BindNpcMeshes(SimEntity& entity);
    void Speak(SimEntity& entity, const char* kind);
    void PoseNpc(SimEntity& entity, bool moving, bool shooting);
    void UpdateNpcs(float deltaTime, const glm::vec3& feet);

    GLuint TextureFor(const std::string& material) {
        const std::string key = Lower(material);
        const auto found = textures.find(key);
        if (found != textures.end()) {
            return found->second;
        }
        GLuint texture = LoadMaterialTexture(files, material);
        if (texture == 0) {
            if (missingTexture == 0) {
                missingTexture = vtf::CreateMissingTexture();
            }
            texture = missingTexture;
        }
        textures.emplace(key, texture);
        return texture;
    }

    const Visual* VisualFor(const std::string& modelPath) {
        if (modelPath.empty()) {
            return nullptr;
        }
        const std::string key = Lower(modelPath);
        const auto found = visuals.find(key);
        if (found != visuals.end()) {
            return &found->second;
        }
        StudioModel studio = LoadStudioModel(files, modelPath);
        Visual visual;
        visual.hullMin = SourcePointToWorld(studio.hullMin.x, studio.hullMin.y, studio.hullMin.z);
        visual.hullMax = SourcePointToWorld(studio.hullMax.x, studio.hullMax.y, studio.hullMax.z);
        visual.physicsConvexes = studio.physicsConvexes;
        visual.physicsMass = studio.physicsMass;
        visual.physicsDamping = studio.physicsDamping;
        visual.physicsRotDamping = studio.physicsRotDamping;
        visual.physicsInertia = studio.physicsInertia;
        visual.physicsSurface = studio.physicsSurface;
        visual.hasPhysicsHull = studio.hasPhysicsHull;
        for (const StudioPrimitive& part : studio.parts) {
            if (part.indices.empty()) {
                continue;
            }
            const std::uint32_t base = static_cast<std::uint32_t>(visual.points.size());
            for (std::size_t offset = 0; offset + 9 < part.vertices.size(); offset += 10) {
                visual.points.emplace_back(part.vertices[offset], part.vertices[offset + 1], part.vertices[offset + 2]);
            }
            for (unsigned int index : part.indices) {
                visual.indices.push_back(base + index);
            }
            visual.bindVertices.push_back(part.vertices);
            visual.bindIndices.push_back(part.indices);
            visual.skinParts.push_back(part.skin);
            visual.meshes.push_back(std::make_unique<Mesh>(part.vertices, part.indices));
            visual.textures.push_back(TextureFor(part.material));
            visual.alphaModes.push_back(MaterialAlphaMode(files, part.material));
        }
        visual.animation = std::move(studio.animation);
        const auto inserted = visuals.emplace(key, std::move(visual));
        return &inserted.first->second;
    }

    void FireOutput(SimEntity& entity, const std::string& outputName) {
        const std::string wanted = Lower(outputName);
        for (Output& output : entity.outputs) {
            if (Lower(output.name) != wanted || output.times == 0) {
                continue;
            }
            if (output.times > 0) {
                --output.times;
            }
            PendingInput event;
            event.when = time + output.delay;
            event.input = output.input;
            event.parameter = output.parameter;
            event.entity = entities.size();
            for (std::size_t index = 0; index < entities.size(); ++index) {
                if (Lower(entities[index].targetName) == Lower(output.target)) {
                    event.entity = index;
                    pending.push_back(event);
                }
            }
        }
    }

    void DrawEntities(const Shader& shader, bool translucentPass, bool skyPass);
};

glm::vec3 GameSimulation::Impl::HalfExtents(const SimEntity& entity) const {
    const auto found = visuals.find(entity.visualKey);
    const glm::vec3 minimum = found != visuals.end() ? found->second.hullMin : glm::vec3(-0.3F);
    const glm::vec3 maximum = found != visuals.end() ? found->second.hullMax : glm::vec3(0.3F);
    return glm::max(glm::abs(maximum - minimum) * 0.5F, glm::vec3(0.08F));
}

bool GameSimulation::Impl::FindLandmark(const std::string& name, glm::vec3& origin) const {
    if (name.empty()) {
        return false;
    }
    const std::string wanted = Lower(name);
    for (const SimEntity& entity : entities) {
        if (Lower(entity.classname) == "info_landmark" && Lower(entity.targetName) == wanted) {
            origin = entity.origin;
            return true;
        }
    }
    return false;
}

void GameSimulation::Impl::TryQueueLevelChange(const SimEntity& trigger, const glm::vec3& playerFeet) {
    if (trigger.changeMap.empty()) {
        std::cerr << "trigger_changelevel missing map name\n";
        return;
    }
    // Half-Life 2 places a second trigger_changelevel in the same landmark-relative
    // spot whose map name is the map you are already in. Touching it must not reload.
    if (!currentMap.empty() && Lower(trigger.changeMap) == Lower(currentMap)) {
        return;
    }
    glm::vec3 landmarkOrigin{0.0F};
    if (!FindLandmark(trigger.landmarkName, landmarkOrigin)) {
        std::cerr << "Can't find landmark \"" << trigger.landmarkName << "\"\n";
        return;
    }

    LevelChangeRequest request;
    request.mapName = trigger.changeMap;
    request.landmarkName = trigger.landmarkName;
    request.playerFeet = playerFeet;
    request.landmarkOrigin = landmarkOrigin;
    request.player.health = health;
    request.player.weapon = static_cast<int>(weapon);
    for (int slot = 0; slot < 5; ++slot) {
        request.player.owned[slot] = owned[slot];
        request.player.ammo[slot] = ammo[slot];
    }

    std::vector<std::pair<glm::vec3, glm::vec3>> volumes;
    if (map != nullptr) {
        for (const SimEntity& volume : entities) {
            if (Lower(volume.classname) != "trigger_transition" || volume.brush < 0) {
                continue;
            }
            if (Lower(volume.landmarkName) != Lower(trigger.landmarkName)) {
                continue;
            }
            glm::vec3 minimum;
            glm::vec3 maximum;
            if (map->BrushAabb(volume.brush, minimum, maximum)) {
                volumes.emplace_back(minimum, maximum);
            }
        }
    }

    if (!volumes.empty()) {
        for (const SimEntity& entity : entities) {
            if (!entity.alive || !CanCarryAcrossTransition(entity.classname)) {
                continue;
            }
            const glm::vec3 center = entity.body >= 0 && physics != nullptr
                ? physics->BodyPosition(entity.body)
                : entity.origin;
            const glm::vec3 half = HalfExtents(entity);
            bool inside = false;
            for (const auto& volume : volumes) {
                if (AabbOverlap(center - half, center + half, volume.first, volume.second)) {
                    inside = true;
                    break;
                }
            }
            if (!inside) {
                continue;
            }
            glm::vec3 savedOrigin = entity.origin;
            if (entity.body >= 0 && physics != nullptr) {
                savedOrigin = center;
                if (entity.npc) {
                    savedOrigin.y -= half.y;
                }
            }
            CarriedEntity carried;
            carried.classname = entity.classname;
            carried.model = entity.model;
            carried.originOffset = savedOrigin - landmarkOrigin;
            carried.rotation = entity.rotation;
            carried.health = entity.health;
            carried.ammoAmount = entity.ammoAmount;
            request.entities.push_back(std::move(carried));
        }
    }

    levelChange = std::move(request);
    levelChangePending = true;
    std::cout << "Level transition to map \"" << levelChange.mapName << "\" with landmark \""
              << levelChange.landmarkName << "\"\n";
}

void GameSimulation::Impl::DestroyProp(SimEntity& entity) {
    if (carriedEntity >= 0 && static_cast<std::size_t>(carriedEntity) < entities.size() && &entities[static_cast<std::size_t>(carriedEntity)] == &entity) {
        carriedEntity = -1;
        carriedWithPhysgun = false;
    }
    if (entity.body >= 0 && physics != nullptr) {
        physics->DestroySimBody(entity.body);
        entity.body = -1;
    }
}

void GameSimulation::Impl::EnableMotion(SimEntity& entity) {
    if (!entity.motionDisabled) {
        return;
    }
    entity.motionDisabled = false;
    if (entity.body >= 0 && physics != nullptr) {
        physics->SetBodyMotionEnabled(entity.body, true);
    }
}

int GameSimulation::Impl::SpawnPropBody(
    SimEntity& entity,
    const glm::mat4& spawn,
    std::uint64_t userData,
    const PhysicsBodyDesc& desc
) {
    if (physics == nullptr) {
        return -1;
    }
    const auto found = visuals.find(entity.visualKey);
    const Visual* visual = found == visuals.end() ? nullptr : &found->second;
    std::vector<std::vector<glm::vec3>> convexes;
    if (visual != nullptr && visual->hasPhysicsHull) {
        convexes = visual->physicsConvexes;
    }
    if (convexes.empty()) {
        if (visual != nullptr) {
            convexes.push_back(HullBox(visual->hullMin, visual->hullMax));
        } else {
            convexes.push_back(HullBox(glm::vec3(-0.25F), glm::vec3(0.25F)));
        }
    }
    entity.mass = desc.mass;
    entity.body = physics->AddDynamicCompound(convexes, spawn, desc, userData);
    if (entity.body < 0) {
        convexes.clear();
        convexes.push_back(visual != nullptr
            ? HullBox(visual->hullMin, visual->hullMax)
            : HullBox(glm::vec3(-0.25F), glm::vec3(0.25F)));
        entity.body = physics->AddDynamicCompound(convexes, spawn, desc, userData);
    }
    return entity.body;
}

PhysicsBodyDesc DescribeProp(const Visual* visual, float mass, float inertiaScale, bool asleep, bool frozen) {
    PhysicsBodyDesc desc;
    desc.mass = std::max(0.1F, mass);
    desc.inertiaScale = (visual != nullptr ? visual->physicsInertia : 1.0F) * std::max(0.05F, inertiaScale);
    desc.linearDamping = visual != nullptr ? visual->physicsDamping : 0.0F;
    desc.angularDamping = visual != nullptr ? visual->physicsRotDamping : 0.05F;
    if (desc.angularDamping <= 0.0F) {
        desc.angularDamping = 0.05F;
    }
    SurfaceResponse(visual != nullptr ? visual->physicsSurface : std::string{}, desc.friction, desc.restitution);
    desc.startAsleep = asleep;
    desc.motionDisabled = frozen;
    return desc;
}

void GameSimulation::Impl::SpawnCarried(const CarriedEntity& carried, const glm::vec3& origin) {
    if (physics == nullptr) {
        return;
    }
    SimEntity entity;
    entity.classname = carried.classname;
    entity.model = carried.model;
    entity.origin = origin;
    entity.rotation = carried.rotation;
    entity.health = carried.health;
    entity.ammoAmount = carried.ammoAmount;
    const std::string classname = Lower(entity.classname);
    entity.npc = classname == "npc_citizen" || classname == "npc_combine_s" || classname == "npc_metropolice";
    entity.npcHostile = entity.npc && classname != "npc_citizen";
    entity.npcYaw = YawFromMatrix(entity.rotation);
    if (entity.npc && entity.model.empty()) {
        entity.model = DefaultNpcModel(classname);
    }
    entity.physicsProp = classname.rfind("prop_physics", 0) == 0;
    entity.weaponItem = classname.rfind("weapon_", 0) == 0;
    entity.ammoItem = classname.rfind("item_ammo_", 0) == 0;
    if (entity.weaponItem || entity.ammoItem) {
        entity.weapon = WeaponFromClassname(classname);
    }
    if (entity.npc || entity.physicsProp || entity.weaponItem || entity.ammoItem || classname.rfind("item_", 0) == 0) {
        if (VisualFor(entity.model) != nullptr) {
            entity.visualKey = Lower(entity.model);
        }
    }
    if (entity.npc) {
        BindNpcMeshes(entity);
    }

    const std::size_t index = entities.size();
    const std::uint64_t userData = static_cast<std::uint64_t>(index) + 1;
    const glm::mat4 spawn = glm::translate(glm::mat4(1.0F), entity.origin) * entity.rotation;
    const auto visual = visuals.find(entity.visualKey);
    const glm::vec3 half = HalfExtents(entity);
    if (entity.physicsProp) {
        const Visual* propVisual = visual == visuals.end() ? nullptr : &visual->second;
        const float mass = propVisual != nullptr && propVisual->physicsMass > 0.0F
            ? propVisual->physicsMass
            : EstimateMass(propVisual != nullptr ? propVisual->hullMin : glm::vec3(-0.25F), propVisual != nullptr ? propVisual->hullMax : glm::vec3(0.25F));
        SpawnPropBody(entity, spawn, userData, DescribeProp(propVisual, mass, 1.0F, false, false));
    } else if (entity.npc) {
        entity.body = physics->AddDynamicBox(
            entity.origin + glm::vec3(0.0F, half.y, 0.0F),
            half,
            80.0F,
            userData
        );
    }
    entities.push_back(std::move(entity));
}

void GameSimulation::Impl::BindNpcMeshes(SimEntity& entity) {
    entity.posedMeshes.clear();
    if (!entity.npc || entity.visualKey.empty()) {
        return;
    }
    const auto found = visuals.find(entity.visualKey);
    if (found == visuals.end() || found->second.animation.bones.empty()) {
        return;
    }
    const Visual& visual = found->second;
    const std::size_t count = std::min(visual.bindVertices.size(), visual.bindIndices.size());
    for (std::size_t index = 0; index < count; ++index) {
        entity.posedMeshes.push_back(std::make_unique<Mesh>(visual.bindVertices[index], visual.bindIndices[index]));
    }
}

void GameSimulation::Impl::Speak(SimEntity& entity, const char* kind) {
    if (audio == nullptr || files == nullptr || kind == nullptr) {
        return;
    }
    const bool death = std::string(kind) == "death";
    if (!death && time < entity.npcNextSpeech) {
        return;
    }
    entity.npcNextSpeech = time + (death ? 0.0F : 3.0F);
    const std::string classname = Lower(entity.classname);
    const std::string gender = Lower(entity.model).find("female") != std::string::npos ? "female01" : "male01";
    const std::string prefix = NpcSoundPrefix(classname);
    const char* speechConcept = nullptr;
    std::string fallback;
    if (std::string(kind) == "hello") {
        speechConcept = "TLK_HELLO";
    } else if (std::string(kind) == "stare") {
        speechConcept = "TLK_STARE";
    } else if (std::string(kind) == "alert") {
        speechConcept = "TLK_ALERT";
        fallback = prefix + ".Alert";
    } else if (std::string(kind) == "pain") {
        speechConcept = "TLK_WOUND";
        fallback = prefix + ".Pain";
    } else if (death) {
        speechConcept = "TLK_DEATH";
        fallback = prefix + ".Die";
    }
    bool played = false;
    if (speechConcept != nullptr) {
        played = speech.SpeakConcept(files, audio, classname, speechConcept, gender);
    }
    if (!played && !fallback.empty() && fallback.front() != '.') {
        speech.PlayEntry(files, audio, fallback);
    }
}

void GameSimulation::Impl::PoseNpc(SimEntity& entity, bool moving, bool shooting) {
    if (entity.posedMeshes.empty()) {
        return;
    }
    const auto found = visuals.find(entity.visualKey);
    if (found == visuals.end()) {
        return;
    }
    const Visual& visual = found->second;
    int sequence = visual.animation.idle;
    if (shooting && visual.animation.shoot >= 0) {
        sequence = visual.animation.shoot;
    } else if (entity.npcRunning && visual.animation.run >= 0) {
        sequence = visual.animation.run;
    } else if (moving && visual.animation.walk >= 0) {
        sequence = visual.animation.walk;
    }
    if (entity.scriptSlot >= 0 && entity.scriptHold > 0.0F) {
        const SimEntity& script = entities[static_cast<std::size_t>(entity.scriptSlot)];
        const int named = FindStudioSequence(visual.animation, script.scriptSequence);
        if (named >= 0) {
            sequence = named;
        }
    }
    if (sequence != entity.npcPose) {
        entity.npcPose = sequence;
        entity.npcAnimTime = 0.0F;
    }
    std::vector<glm::mat4> bones;
    if (!SampleStudioPose(visual.animation, sequence, entity.npcAnimTime, bones)) {
        return;
    }
    const std::size_t count = std::min(entity.posedMeshes.size(), visual.skinParts.size());
    for (std::size_t part = 0; part < count; ++part) {
        if (visual.bindVertices[part].size() != visual.skinParts[part].size() * Mesh::kFloatsPerVertex) {
            continue;
        }
        std::vector<float> vertices = visual.bindVertices[part];
        for (std::size_t vertex = 0; vertex < visual.skinParts[part].size(); ++vertex) {
            const StudioSkinVertex& skin = visual.skinParts[part][vertex];
            glm::vec3 source = skin.position;
            if (skin.boneCount > 0) {
                glm::vec3 sum(0.0F);
                float weight = 0.0F;
                const int influences = std::min(static_cast<int>(skin.boneCount), 3);
                for (int influence = 0; influence < influences; ++influence) {
                    const unsigned bone = skin.bones[influence];
                    if (bone >= bones.size()) {
                        continue;
                    }
                    sum += skin.weights[influence] * glm::vec3(bones[bone] * glm::vec4(skin.position, 1.0F));
                    weight += skin.weights[influence];
                }
                if (weight > 0.0F) {
                    source = sum;
                }
            }
            const glm::vec3 world = SourcePointToWorld(source.x, source.y, source.z);
            const std::size_t offset = vertex * Mesh::kFloatsPerVertex;
            vertices[offset] = world.x;
            vertices[offset + 1] = world.y;
            vertices[offset + 2] = world.z;
        }
        entity.posedMeshes[part]->UpdateVertices(vertices);
    }
}

void GameSimulation::Impl::UpdateNpcs(float deltaTime, const glm::vec3& feet) {
    if (physics == nullptr) {
        return;
    }
    for (SimEntity& script : entities) {
        if (!script.scripted || !script.scriptActive || script.scriptFinished) {
            continue;
        }
        SimEntity* npc = nullptr;
        for (SimEntity& candidate : entities) {
            if (candidate.npc && candidate.alive && Lower(candidate.targetName) == Lower(script.scriptNpcName)) {
                npc = &candidate;
                break;
            }
        }
        if (npc == nullptr || npc->body < 0) {
            continue;
        }
        npc->scriptSlot = static_cast<int>(&script - entities.data());
        npc->npcSchedule = kNpcScript;
        npc->npcScriptArmed = true;
        if (script.scriptMove == 4) {
            const glm::vec3 half = HalfExtents(*npc);
            physics->SetBodyTransform(npc->body, glm::translate(glm::mat4(1.0F), script.origin + glm::vec3(0.0F, half.y, 0.0F)));
            physics->SetBodyVelocity(npc->body, glm::vec3(0.0F));
            npc->origin = script.origin;
            npc->npcPath.clear();
        }
    }

    for (std::size_t index = 0; index < entities.size(); ++index) {
        SimEntity& entity = entities[index];
        if (!entity.npc || !entity.alive || entity.body < 0) {
            continue;
        }
        const glm::vec3 half = HalfExtents(entity);
        glm::vec3 center = physics->BodyPosition(entity.body);
        const glm::quat rotation = physics->BodyRotation(entity.body);
        if (std::abs(rotation.x) > 0.08F || std::abs(rotation.z) > 0.08F) {
            physics->SetBodyTransform(entity.body, glm::translate(glm::mat4(1.0F), center));
            center = physics->BodyPosition(entity.body);
        }
        entity.origin = center - glm::vec3(0.0F, half.y, 0.0F);
        const glm::vec3 toPlayer = feet - entity.origin;
        const float distance = glm::length(glm::vec3(toPlayer.x, 0.0F, toPlayer.z));
        const glm::vec3 eyePosition = center + glm::vec3(0.0F, half.y * 0.2F, 0.0F);
        const glm::vec3 chest = feet + glm::vec3(0.0F, 0.8F, 0.0F);
        const float sightRange = glm::length(chest - eyePosition);
        const std::uint64_t selfId = static_cast<std::uint64_t>(index) + 1;
        const PhysicsRayHit sight = physics->RayCast(eyePosition, chest - eyePosition, sightRange, selfId);
        const bool blocked = sight.hit && sight.distance < sightRange - 0.3F;
        const bool visible = !blocked && distance < 720.0F * kSourceToWorld;
        const bool scriptedNow = entity.scriptSlot >= 0
            && static_cast<std::size_t>(entity.scriptSlot) < entities.size()
            && entities[static_cast<std::size_t>(entity.scriptSlot)].scriptActive
            && !entities[static_cast<std::size_t>(entity.scriptSlot)].scriptFinished;
        if (!scriptedNow && entity.npcSchedule == kNpcScript) {
            entity.npcSchedule = kNpcIdle;
            entity.scriptSlot = -1;
            entity.npcPath.clear();
            entity.npcHasGoal = false;
        }

        const bool waitForScript = entity.npcHostile
            && (entity.npcSpawnFlags & kNpcWaitForScript) != 0
            && !entity.npcScriptArmed;
        bool facePlayer = false;
        bool movingOrder = false;
        glm::vec3 goal = entity.origin;
        float speed = 100.0F * kSourceToWorld;
        const float attackRange = 1024.0F * kSourceToWorld;
        const float goalSlop = 48.0F * kSourceToWorld;

        if (scriptedNow) {
            SimEntity& script = entities[static_cast<std::size_t>(entity.scriptSlot)];
            goal = script.origin;
            movingOrder = script.scriptMove != 0 && script.scriptMove != 4;
            speed = script.scriptMove == 2 ? 220.0F * kSourceToWorld : 100.0F * kSourceToWorld;
            if (glm::length(glm::vec3(goal.x - entity.origin.x, 0.0F, goal.z - entity.origin.z)) < 24.0F * kSourceToWorld) {
                movingOrder = false;
                script.scriptHold += deltaTime;
                if (script.scriptHold > 1.6F) {
                    FireOutput(script, "OnEndSequence");
                    script.scriptActive = false;
                    script.scriptFinished = true;
                    entity.npcSchedule = kNpcIdle;
                    entity.scriptSlot = -1;
                    entity.npcPath.clear();
                    entity.npcHasGoal = false;
                }
            }
        } else if (!entity.npcHostile) {
            entity.npcSchedule = kNpcIdle;
            entity.npcHasGoal = false;
            if (visible) {
                facePlayer = true;
                if (distance < 260.0F * kSourceToWorld) {
                    if (!entity.npcGreeted) {
                        entity.npcGreeted = true;
                        Speak(entity, "hello");
                    } else if (time >= entity.npcNextSpeech) {
                        Speak(entity, "stare");
                    }
                }
            }
        } else if (waitForScript) {
            entity.npcSchedule = kNpcIdle;
            entity.npcHasGoal = false;
        } else {
            if (entity.npcSchedule == kNpcIdle && visible) {
                entity.npcSchedule = kNpcAlert;
                entity.npcStateTime = time;
                if (!entity.npcAlertSpoken) {
                    entity.npcAlertSpoken = true;
                    Speak(entity, "alert");
                }
            } else if (entity.npcSchedule == kNpcAlert && time >= entity.npcStateTime + 0.35F) {
                entity.npcSchedule = kNpcFace;
            }

            if (entity.npcSchedule == kNpcAlert) {
                facePlayer = true;
            } else if (entity.npcSchedule == kNpcCover) {
                if (!entity.npcHasGoal && time >= entity.npcGoalTime) {
                    glm::vec3 cover{0.0F};
                    if (navigation.CoverNode(entity.origin, chest, *physics, cover)) {
                        entity.npcGoal = cover;
                        entity.npcHasGoal = true;
                        entity.npcStateTime = 0.0F;
                        entity.npcPath = navigation.FindPath(entity.origin, cover, *physics, selfId);
                        entity.npcPathCursor = 0;
                        if (entity.npcPath.empty()) {
                            entity.npcSchedule = kNpcFace;
                            entity.npcHasGoal = false;
                            facePlayer = true;
                        }
                    } else {
                        entity.npcSchedule = kNpcFace;
                        facePlayer = true;
                    }
                } else if (!entity.npcHasGoal) {
                    facePlayer = true;
                }
                if (entity.npcSchedule == kNpcCover && entity.npcHasGoal) {
                    goal = entity.npcGoal;
                    const float remain = glm::length(glm::vec3(goal.x - entity.origin.x, 0.0F, goal.z - entity.origin.z));
                    if (remain < 24.0F * kSourceToWorld) {
                        facePlayer = true;
                        if (entity.npcStateTime <= 0.0F) {
                            entity.npcStateTime = time + 2.0F;
                        }
                        if (time >= entity.npcStateTime) {
                            entity.npcSchedule = kNpcFace;
                            entity.npcHasGoal = false;
                            entity.npcPath.clear();
                        }
                    } else if (entity.npcPath.empty()) {
                        entity.npcSchedule = kNpcFace;
                        entity.npcHasGoal = false;
                        facePlayer = true;
                    } else {
                        movingOrder = true;
                        speed = 220.0F * kSourceToWorld;
                    }
                }
            }

            const bool choosingCombat = entity.npcSchedule == kNpcFace
                || entity.npcSchedule == kNpcShoot
                || entity.npcSchedule == kNpcLineOfFire
                || entity.npcSchedule == kNpcPress;
            if (choosingCombat) {
                if (visible && distance <= attackRange) {
                    entity.npcSchedule = kNpcShoot;
                    facePlayer = true;
                    entity.npcHasGoal = false;
                } else if (time >= entity.npcGoalTime) {
                    glm::vec3 firing{0.0F};
                    const bool foundFiring = navigation.FiringNode(entity.origin, chest, *physics, firing);
                    const float firingDistance = foundFiring
                        ? glm::length(glm::vec3(firing.x - entity.origin.x, 0.0F, firing.z - entity.origin.z))
                        : 0.0F;
                    if (foundFiring && firingDistance > goalSlop) {
                        entity.npcSchedule = kNpcLineOfFire;
                        goal = firing;
                        movingOrder = true;
                        speed = 220.0F * kSourceToWorld;
                    } else if (distance > attackRange) {
                        entity.npcSchedule = kNpcPress;
                        goal = feet;
                        movingOrder = true;
                        speed = 220.0F * kSourceToWorld;
                    } else {
                        entity.npcSchedule = kNpcFace;
                        facePlayer = true;
                    }
                    entity.npcGoalTime = time + 0.75F;
                    if (movingOrder) {
                        const bool goalMoved = !entity.npcHasGoal
                            || glm::length(glm::vec3(goal.x - entity.npcGoal.x, 0.0F, goal.z - entity.npcGoal.z)) > goalSlop;
                        if (goalMoved || entity.npcPath.empty()) {
                            entity.npcGoal = goal;
                            entity.npcHasGoal = true;
                            entity.npcPath = navigation.FindPath(entity.origin, goal, *physics, selfId);
                            entity.npcPathCursor = 0;
                        }
                        if (entity.npcPath.empty()) {
                            movingOrder = false;
                            facePlayer = true;
                            entity.npcHasGoal = false;
                            entity.npcSchedule = kNpcFace;
                        }
                    } else {
                        entity.npcHasGoal = false;
                    }
                } else if (entity.npcHasGoal
                    && (entity.npcSchedule == kNpcLineOfFire || entity.npcSchedule == kNpcPress)) {
                    goal = entity.npcGoal;
                    movingOrder = true;
                    speed = 220.0F * kSourceToWorld;
                } else {
                    facePlayer = true;
                }
            }
        }

        if (scriptedNow && movingOrder && entity.npcPath.empty() && time >= entity.npcGoalTime) {
            entity.npcPath = navigation.FindPath(entity.origin, goal, *physics, selfId);
            if (entity.npcPath.empty()) {
                const glm::vec3 flat = glm::vec3(goal.x - entity.origin.x, 0.0F, goal.z - entity.origin.z);
                const float span = glm::length(flat);
                const PhysicsRayHit corridor = physics->RayCast(
                    entity.origin + glm::vec3(0.0F, 36.0F * kSourceToWorld, 0.0F),
                    glm::vec3(flat.x, 0.0F, flat.z),
                    span,
                    selfId
                );
                if (span > 1.0F && (!corridor.hit || corridor.distance > span - 8.0F * kSourceToWorld)) {
                    entity.npcPath.push_back(goal);
                }
            } else {
                entity.npcPath.push_back(goal);
            }
            entity.npcPathCursor = 0;
        }
        if (!movingOrder) {
            entity.npcPath.clear();
            entity.npcPathCursor = 0;
        }

        entity.npcRunning = movingOrder && speed > 150.0F * kSourceToWorld;
        glm::vec3 velocity(0.0F);
        bool moving = false;
        glm::vec3 moveDirection(0.0F);
        while (entity.npcPathCursor < static_cast<int>(entity.npcPath.size())) {
            const glm::vec3& waypoint = entity.npcPath[static_cast<std::size_t>(entity.npcPathCursor)];
            const glm::vec3 flat = glm::vec3(waypoint.x - entity.origin.x, 0.0F, waypoint.z - entity.origin.z);
            if (glm::length(flat) < 18.0F * kSourceToWorld) {
                ++entity.npcPathCursor;
                continue;
            }
            const glm::vec3 direction = glm::normalize(flat);
            const glm::vec3 chestStart = entity.origin + glm::vec3(0.0F, 36.0F * kSourceToWorld, 0.0F);
            const glm::vec3 segment = glm::vec3(flat.x, waypoint.y - entity.origin.y, flat.z);
            const float segmentLength = glm::length(segment);
            const PhysicsRayHit segmentHit = physics->RayCast(chestStart, segment, segmentLength, selfId);
            if (segmentLength > 1.0F && segmentHit.hit && segmentHit.distance < segmentLength - 8.0F * kSourceToWorld) {
                entity.npcPath.clear();
                entity.npcPathCursor = 0;
                entity.npcHasGoal = false;
                entity.npcGoalTime = time + 0.5F;
                entity.npcStuckTime = 0.0F;
                break;
            }
            velocity = direction * speed;
            moveDirection = direction;
            moving = true;
            break;
        }
        if (moving) {
            if (glm::length(entity.origin - entity.npcStuckAt) < 6.0F * kSourceToWorld) {
                entity.npcStuckTime += deltaTime;
                if (entity.npcStuckTime > 0.8F) {
                    entity.npcPath.clear();
                    entity.npcPathCursor = 0;
                    entity.npcHasGoal = false;
                    entity.npcGoalTime = time + 0.5F;
                    entity.npcStuckTime = 0.0F;
                    velocity = glm::vec3(0.0F);
                    moving = false;
                }
            } else {
                entity.npcStuckAt = entity.origin;
                entity.npcStuckTime = 0.0F;
            }
            entity.npcFootstep -= deltaTime;
            if (entity.npcFootstep <= 0.0F) {
                entity.npcFootstep = entity.npcRunning ? 0.32F : 0.45F;
                entity.npcFootLeft = !entity.npcFootLeft;
                const std::string prefix = NpcSoundPrefix(Lower(entity.classname));
                if (!prefix.empty()) {
                    const std::string step = prefix + (entity.npcRunning ? ".RunFootstep" : ".Footstep")
                        + (entity.npcFootLeft ? "Left" : "Right");
                    speech.PlayEntry(files, audio, step);
                }
            }
        } else {
            entity.npcStuckTime = 0.0F;
            entity.npcFootstep = 0.15F;
            if (movingOrder) {
                facePlayer = true;
            }
        }

        float desiredYaw = entity.npcYaw;
        if (moving) {
            desiredYaw = YawToward(moveDirection);
        } else if (facePlayer && distance > 1.0F) {
            desiredYaw = YawToward(toPlayer);
        }
        const bool turning = moving || (facePlayer && distance > 1.0F);
        if (turning) {
            ApproachYaw(entity.npcYaw, desiredYaw, deltaTime);
        }
        const float yawError = std::abs(WrapYaw(desiredYaw - entity.npcYaw));
        if (!scriptedNow && entity.npcSchedule == kNpcShoot && !moving && visible
            && yawError < 20.0F && time >= entity.nextAttack && time >= nextPlayerHurt) {
            entity.nextAttack = time + 0.45F;
            nextPlayerHurt = time + 0.35F;
            health -= 4.0F;
            entity.npcShootUntil = time + 0.45F;
        }
        const PhysicsRayHit ground = physics->RayCast(center, glm::vec3(0.0F, -1.0F, 0.0F), half.y + 0.45F, selfId);
        if (!ground.hit) {
            velocity.y = -6.0F;
        } else if (ground.distance > half.y + 0.08F) {
            velocity.y = -2.0F;
        }
        physics->SetBodyVelocity(entity.body, velocity);
        physics->SetBodyAngularVelocity(entity.body, glm::vec3(0.0F));
        entity.rotation = SourceAnglesToWorldMatrix(0.0F, entity.npcYaw, 0.0F);
        entity.npcAnimTime += deltaTime;
        PoseNpc(entity, moving, time < entity.npcShootUntil);
    }
}

GameSimulation::GameSimulation(const BspLoader& map, PhysicsWorld& physics, const GameFileSystem* files, AudioSystem* audio)
    : impl_(std::make_unique<Impl>()) {
    impl_->map = &map;
    impl_->physics = &physics;
    impl_->files = files;
    impl_->audio = audio;
    impl_->navigation.Build(map.MapEntities(), physics);
    impl_->speech.Load(files);

    for (const BspMapEntity& source : map.MapEntities()) {
        SimEntity entity;
        entity.classname = source.classname;
        entity.targetName = Property(source, "targetname");
        entity.model = source.model.empty() ? Property(source, "model") : source.model;
        entity.origin = source.position;
        entity.brush = source.brushModel;
        entity.outputs = ParseOutputs(source);
        const std::string& angles = Property(source, "angles");
        float pitch = 0.0F;
        float yaw = 0.0F;
        float roll = 0.0F;
        if (std::sscanf(angles.c_str(), "%f %f %f", &pitch, &yaw, &roll) == 3) {
            entity.rotation = SourceAnglesToWorldMatrix(pitch, yaw, roll);
        }
        const std::string classname = Lower(entity.classname);
        entity.npc = classname == "npc_citizen" || classname == "npc_combine_s" || classname == "npc_metropolice";
        entity.npcHostile = entity.npc && classname != "npc_citizen";
        entity.npcYaw = YawFromMatrix(entity.rotation);
        if (entity.npc && entity.model.empty()) {
            entity.model = DefaultNpcModel(classname);
        }
        entity.scripted = classname == "scripted_sequence";
        if (entity.scripted) {
            entity.scriptNpcName = PropertyCi(source, "m_iszEntity");
            if (entity.scriptNpcName.empty()) {
                entity.scriptNpcName = PropertyCi(source, "target");
            }
            entity.scriptMove = static_cast<int>(PropertyFloatCi(source, "m_fMoveTo", 1.0F));
            entity.scriptSequence = PropertyCi(source, "m_iszPlay");
            entity.scriptActive = PropertyCi(source, "StartDisabled") != "1" && !entity.scriptNpcName.empty();
        }
        entity.physicsProp = classname == "prop_physics" || classname == "prop_physics_override"
            || classname == "prop_physics_multiplayer";
        entity.staticProp = classname == "prop_static" || classname == "prop_dynamic" || classname == "prop_dynamic_override";
        entity.inSkybox = source.inSkybox;
        entity.visibilityLeaves = source.visibilityLeaves;
        entity.fadeMin = source.fadeMin;
        entity.fadeMax = source.fadeMax;
        entity.lightTint = source.lightTint;
        entity.hasLightingOrigin = source.hasLightingOrigin;
        entity.door = classname == "func_door" || classname == "func_door_rotating" || classname == "func_movelinear";
        entity.rotatingDoor = classname == "func_door_rotating";
        entity.breakable = classname == "func_breakable" || classname == "func_breakable_surf";
        entity.parentName = PropertyCi(source, "parentname");
        if (entity.door) {
            const int spawnFlags = static_cast<int>(PropertyFloat(source, "spawnflags", 0.0F));
            if ((spawnFlags & 1) != 0) {
                entity.openAmount = 1.0F;
            }
        }
        entity.trigger = StartsWith(classname, "trigger_");
        entity.button = classname == "func_button";
        if (classname == "trigger_changelevel") {
            entity.changeMap = MapStem(PropertyCi(source, "map"));
            entity.landmarkName = PropertyCi(source, "landmark");
        } else if (classname == "trigger_transition") {
            entity.landmarkName = PropertyCi(source, "landmark");
            if (entity.landmarkName.empty()) {
                entity.landmarkName = entity.targetName;
            }
        }
        entity.weaponItem = StartsWith(classname, "weapon_");
        entity.ammoItem = StartsWith(classname, "item_ammo_");
        entity.enabled = Property(source, "StartDisabled") != "1";
        if (entity.npc) {
            entity.npcSpawnFlags = static_cast<int>(PropertyFloatCi(source, "spawnflags", 0.0F));
            entity.health = PropertyFloat(source, "health", classname == "npc_combine_s" ? 50.0F : 40.0F);
        } else if (entity.breakable) {
            entity.health = PropertyFloat(source, "health", 0.0F);
            if (entity.health <= 0.0F) {
                entity.health = 25.0F;
            }
        } else if (entity.physicsProp) {
            entity.health = PropertyFloatCi(source, "health", 0.0F);
            entity.damageToEnableMotion = std::max(0.0F, PropertyFloatCi(source, "damagetoenablemotion", 0.0F));
            entity.physicsDamageScale = std::max(0.0F, PropertyFloatCi(source, "physdamagescale", 1.0F));
            const int spawnflags = static_cast<int>(PropertyFloatCi(source, "spawnflags", 0.0F));
            entity.motionDisabled = (spawnflags & 8) != 0 || entity.damageToEnableMotion > 0.0F;
            entity.preventPickup = (spawnflags & 512) != 0;
            entity.takePhysicsDamage = (spawnflags & 2) == 0;
        }
        if (classname == "weapon_crowbar") entity.weapon = WeaponKind::Crowbar;
        else if (classname == "weapon_pistol") entity.weapon = WeaponKind::Pistol;
        else if (classname == "weapon_smg1") entity.weapon = WeaponKind::Smg;
        else if (classname == "weapon_shotgun") entity.weapon = WeaponKind::Shotgun;
        else if (classname == "weapon_physcannon") entity.weapon = WeaponKind::Physgun;
        if (entity.ammoItem) {
            if (classname.find("pistol") != std::string::npos) entity.weapon = WeaponKind::Pistol;
            else if (classname.find("smg") != std::string::npos) entity.weapon = WeaponKind::Smg;
            else if (classname.find("buckshot") != std::string::npos || classname.find("shotgun") != std::string::npos) {
                entity.weapon = WeaponKind::Shotgun;
            }
            entity.ammoAmount = static_cast<int>(PropertyFloat(source, "ammo", 12.0F));
        }

        const bool staticProp = entity.staticProp;
        if (entity.npc || entity.physicsProp || staticProp || entity.weaponItem) {
            if (impl_->VisualFor(entity.model) != nullptr) {
                entity.visualKey = Lower(entity.model);
            }
        }
        if (entity.npc) {
            impl_->BindNpcMeshes(entity);
        }

        auto halfExtents = [&]() {
            const auto found = impl_->visuals.find(entity.visualKey);
            glm::vec3 minimum = found != impl_->visuals.end() ? found->second.hullMin : glm::vec3(-0.3F);
            glm::vec3 maximum = found != impl_->visuals.end() ? found->second.hullMax : glm::vec3(0.3F);
            return glm::max(glm::abs(maximum - minimum) * 0.5F, glm::vec3(0.08F));
        };
        const std::size_t index = impl_->entities.size();
        const std::uint64_t userData = static_cast<std::uint64_t>(index) + 1;
        const glm::mat4 spawn = glm::translate(glm::mat4(1.0F), entity.origin) * entity.rotation;
        const auto visual = impl_->visuals.find(entity.visualKey);
        const std::string solidText = PropertyCi(source, "solid");
        const int solidType = solidText.empty()
            ? 6
            : static_cast<int>(PropertyFloatCi(source, "solid", 6.0F));
        const bool solid = solidType != 0;
        if (entity.physicsProp && solid) {
            const Visual* propVisual = visual == impl_->visuals.end() ? nullptr : &visual->second;
            float mass = propVisual != nullptr && propVisual->physicsMass > 0.0F
                ? propVisual->physicsMass
                : EstimateMass(
                    propVisual != nullptr ? propVisual->hullMin : glm::vec3(-0.25F),
                    propVisual != nullptr ? propVisual->hullMax : glm::vec3(0.25F)
                );
            if (classname == "prop_physics_override" && !PropertyCi(source, "mass").empty()) {
                mass = PropertyFloatCi(source, "mass", mass);
            }
            const float massScale = PropertyFloatCi(source, "massscale", 1.0F);
            if (massScale > 0.0F) {
                mass *= massScale;
            }
            const int spawnflags = static_cast<int>(PropertyFloatCi(source, "spawnflags", 0.0F));
            impl_->SpawnPropBody(
                entity,
                spawn,
                userData,
                DescribeProp(
                    propVisual,
                    mass,
                    PropertyFloatCi(source, "inertiascale", 1.0F),
                    (spawnflags & 1) != 0,
                    entity.motionDisabled
                )
            );
        } else if (staticProp && solid) {
            const Visual* propVisual = visual == impl_->visuals.end() ? nullptr : &visual->second;
            const auto hullCorners = [&]() {
                if (propVisual != nullptr) {
                    return HullBox(propVisual->hullMin, propVisual->hullMax);
                }
                return HullBox(glm::vec3(-0.25F), glm::vec3(0.25F));
            };
            if (solidType == 2 || solidType == 3 || solidType == 4) {
                entity.body = physics.AddConvexBody(hullCorners(), spawn, 0.0F, false, userData);
            } else {
                std::vector<std::vector<glm::vec3>> convexes;
                if (propVisual != nullptr && propVisual->hasPhysicsHull) {
                    convexes = propVisual->physicsConvexes;
                }
                if (convexes.empty()) {
                    convexes.push_back(hullCorners());
                }
                entity.body = physics.AddStaticCompound(convexes, spawn, userData);
                if (entity.body < 0) {
                    entity.body = physics.AddConvexBody(hullCorners(), spawn, 0.0F, false, userData);
                }
            }
        } else if (entity.npc) {
            const glm::vec3 half = halfExtents();
            entity.body = physics.AddDynamicBox(
                entity.origin + glm::vec3(0.0F, half.y, 0.0F),
                half,
                80.0F,
                userData
            );
        } else if (entity.brush >= 0 && (entity.door || entity.button || entity.breakable
            || classname == "func_brush" || classname == "func_wall" || classname == "func_tracktrain")) {
            glm::vec3 minimum;
            glm::vec3 maximum;
            if (map.BrushAabb(entity.brush, minimum, maximum)) {
                entity.pivot = (minimum + maximum) * 0.5F;
                const glm::vec3 size = maximum - minimum;
                entity.doorDistance = std::max(size.x, std::max(size.y, size.z));
            }
            const float speed = PropertyFloat(source, "speed", entity.rotatingDoor ? 90.0F : 100.0F);
            entity.doorSpeed = entity.rotatingDoor ? speed : speed * kSourceToWorld;
            const float distance = PropertyFloat(source, "distance", 0.0F);
            if (distance > 0.0F) {
                entity.doorDistance = entity.rotatingDoor ? distance : distance * kSourceToWorld;
            }
            const std::string& movedir = Property(source, "movedir");
            float dx = 0.0F;
            float dy = 0.0F;
            float dz = 1.0F;
            if (std::sscanf(movedir.c_str(), "%f %f %f", &dx, &dy, &dz) == 3) {
                entity.doorDirection = SourceDirectionToWorld(dx, dy, dz);
                if (glm::length(entity.doorDirection) > 0.0F) {
                    entity.doorDirection = glm::normalize(entity.doorDirection);
                }
            }
            std::vector<glm::vec3> vertices = map.BrushCollisionVertices(entity.brush);
            if (entity.rotatingDoor) {
                for (glm::vec3& vertex : vertices) {
                    vertex -= entity.pivot;
                }
            }
            const glm::mat4 transform = entity.rotatingDoor
                ? glm::translate(glm::mat4(1.0F), entity.pivot)
                : glm::mat4(1.0F);
            entity.body = physics.AddMeshBody(
                vertices,
                map.BrushCollisionIndices(entity.brush),
                transform,
                entity.door || entity.button,
                userData
            );
        }
        impl_->entities.push_back(std::move(entity));
    }

    for (SimEntity& entity : impl_->entities) {
        if (Lower(entity.classname) == "logic_auto" && entity.enabled) {
            impl_->FireOutput(entity, "OnMapSpawn");
            impl_->FireOutput(entity, "OnNewGame");
        }
    }
    std::cout << "Simulation entities: " << impl_->entities.size() << '\n';
}

GameSimulation::~GameSimulation() {
    if (impl_ == nullptr) {
        return;
    }
    for (const auto& texture : impl_->textures) {
        if (texture.second != 0 && texture.second != impl_->missingTexture) {
            glDeleteTextures(1, &texture.second);
        }
    }
    if (impl_->missingTexture != 0) {
        glDeleteTextures(1, &impl_->missingTexture);
    }
}

int GameSimulation::Health() const {
    return static_cast<int>(impl_->health);
}

bool GameSimulation::ConsumeLevelChange(LevelChangeRequest& request) {
    if (!impl_->levelChangePending) {
        return false;
    }
    request = std::move(impl_->levelChange);
    impl_->levelChange = {};
    impl_->levelChangePending = false;
    return true;
}

bool GameSimulation::FindLandmark(const std::string& name, glm::vec3& origin) const {
    return impl_->FindLandmark(name, origin);
}

void GameSimulation::RestorePlayerState(const PlayerTransitionState& state) {
    impl_->health = state.health;
    impl_->weapon = static_cast<WeaponKind>(std::clamp(state.weapon, 0, 4));
    for (int slot = 0; slot < 5; ++slot) {
        impl_->owned[slot] = state.owned[slot];
        impl_->ammo[slot] = state.ammo[slot];
    }
}

void GameSimulation::SpawnCarriedEntities(const std::vector<CarriedEntity>& entities, const glm::vec3& landmarkOrigin) {
    for (const CarriedEntity& carried : entities) {
        impl_->SpawnCarried(carried, landmarkOrigin + carried.originOffset);
    }
}

void GameSimulation::SetCurrentMap(const std::string& mapName) {
    impl_->currentMap = MapStem(mapName);
}

void GameSimulation::Update(
    float deltaTime,
    PlayerController& player,
    const glm::vec3& eye,
    const glm::vec3& look,
    GLFWwindow* window
) {
    deltaTime = std::min(deltaTime, 0.05F);
    impl_->time += deltaTime;
    const glm::vec3 feet = player.Position();
    const float playerRadius = 16.0F * kSourceToWorld;
    const float playerHeight = player.IsGrounded() ? 72.0F * kSourceToWorld : 72.0F * kSourceToWorld;

    std::vector<PendingInput> due;
    std::vector<PendingInput> later;
    for (const PendingInput& event : impl_->pending) {
        if (event.when <= impl_->time) {
            due.push_back(event);
        } else {
            later.push_back(event);
        }
    }
    impl_->pending.swap(later);

    auto findNamed = [&](const std::string& name) -> SimEntity* {
        if (name.empty()) {
            return nullptr;
        }
        for (SimEntity& entity : impl_->entities) {
            if (Lower(entity.targetName) == Lower(name)) {
                return &entity;
            }
        }
        return nullptr;
    };

    auto damageEntity = [&](SimEntity& entity, float amount) {
        if (!entity.alive || amount <= 0.0F) {
            return;
        }
        if (entity.physicsProp && entity.motionDisabled) {
            entity.motionDamage += amount;
            if (entity.motionDamage >= entity.damageToEnableMotion) {
                impl_->EnableMotion(entity);
            }
        }
        if (entity.health > 0.0F) {
            entity.health -= amount;
            if (entity.health <= 0.0F) {
                if (entity.npc) {
                    impl_->Speak(entity, "death");
                }
                entity.alive = false;
                entity.health = 0.0F;
                impl_->FireOutput(entity, "OnBreak");
                impl_->FireOutput(entity, "OnDeath");
                impl_->DestroyProp(entity);
            } else if (entity.npc) {
                impl_->Speak(entity, "pain");
                const bool waitingScript = (entity.npcSpawnFlags & kNpcWaitForScript) != 0 && !entity.npcScriptArmed;
                if (entity.npcHostile && !waitingScript && entity.npcSchedule != kNpcScript && entity.npcSchedule != kNpcCover) {
                    entity.npcSchedule = kNpcCover;
                    entity.npcStateTime = 0.0F;
                    entity.npcGoalTime = 0.0F;
                    entity.npcHasGoal = false;
                    entity.npcPath.clear();
                    entity.npcAlertSpoken = true;
                }
            }
        } else if (entity.physicsProp) {
            impl_->FireOutput(entity, "Break");
        }
    };

    if (impl_->physics != nullptr) {
        for (const PhysicsImpact& impact : impl_->physics->ConsumeImpacts()) {
            const auto hurt = [&](std::uint64_t id) {
                if (id == 0 || id >= 0x8000000000000000ULL || id > impl_->entities.size()) {
                    return;
                }
                SimEntity& hit = impl_->entities[static_cast<std::size_t>(id - 1)];
                if (hit.breakable && hit.alive && hit.health > 0.0F) {
                    const float damage = 0.05F * impact.speed * impact.speed;
                    if (damage >= 1.0F) {
                        damageEntity(hit, damage);
                    }
                    return;
                }
                if (!hit.physicsProp || !hit.alive || !hit.takePhysicsDamage || hit.health <= 0.0F) {
                    return;
                }
                const float damage = 0.02F * hit.mass * impact.speed * impact.speed * hit.physicsDamageScale;
                if (damage >= 1.0F) {
                    damageEntity(hit, damage);
                }
            };
            hurt(impact.first);
            hurt(impact.second);
        }
    }

    auto acceptInput = [&](SimEntity& entity, const std::string& input, const std::string& parameter) {
        const std::string name = Lower(input);
        static_cast<void>(parameter);
        if (!entity.enabled && name != "enable") {
            return;
        }
        if (name == "kill") {
            entity.alive = false;
            entity.enabled = false;
            return;
        }
        if (name == "enable") {
            entity.enabled = true;
            return;
        }
        if (name == "disable") {
            entity.enabled = false;
            return;
        }
        if (name == "trigger") {
            impl_->FireOutput(entity, "OnTrigger");
            return;
        }
        if (name == "open" && entity.door) {
            entity.fired = false;
            entity.openAmount = std::min(1.0F, entity.openAmount);
            entity.nextAttack = 1.0F;
            return;
        }
        if (name == "close" && entity.door) {
            entity.nextAttack = -1.0F;
            return;
        }
        if (name == "toggle" && entity.door) {
            entity.nextAttack = entity.nextAttack < 0.0F ? 1.0F : -1.0F;
            return;
        }
        if (name == "break") {
            damageEntity(entity, 1000.0F);
            return;
        }
        if (name == "enablemotion" && entity.physicsProp) {
            impl_->EnableMotion(entity);
            return;
        }
        if (name == "disablemotion" && entity.physicsProp) {
            entity.motionDisabled = true;
            if (entity.body >= 0) {
                impl_->physics->SetBodyMotionEnabled(entity.body, false);
            }
        }
        if (entity.scripted && (name == "beginsequence" || name == "movesequence")) {
            entity.scriptActive = true;
            entity.scriptFinished = false;
            entity.scriptHold = 0.0F;
            return;
        }
        if (entity.scripted && name == "cancelsequence") {
            entity.scriptActive = false;
            entity.scriptFinished = true;
        }
    };

    for (const PendingInput& event : due) {
        if (event.entity >= impl_->entities.size()) {
            continue;
        }
        acceptInput(impl_->entities[event.entity], event.input, event.parameter);
    }

    for (SimEntity& entity : impl_->entities) {
        if (!entity.enabled) {
            continue;
        }
        if (entity.door || entity.button) {
            const float direction = entity.button ? 0.0F : entity.nextAttack;
            if (direction != 0.0F) {
                const float previous = entity.openAmount;
                entity.openAmount = std::clamp(
                    entity.openAmount + direction * (entity.doorSpeed / std::max(entity.doorDistance, 0.01F)) * deltaTime,
                    0.0F,
                    1.0F
                );
                if (previous < 1.0F && entity.openAmount >= 1.0F) {
                    impl_->FireOutput(entity, "OnFullyOpen");
                }
                if (previous > 0.0F && entity.openAmount <= 0.0F) {
                    impl_->FireOutput(entity, "OnFullyClosed");
                }
            }
            glm::mat4 transform(1.0F);
            if (entity.rotatingDoor) {
                const float angle = glm::radians(entity.doorDistance * entity.openAmount);
                transform = glm::translate(glm::mat4(1.0F), entity.pivot)
                    * glm::rotate(glm::mat4(1.0F), angle, glm::vec3(0.0F, 1.0F, 0.0F))
                    * glm::translate(glm::mat4(1.0F), -entity.pivot);
                transform[3] = glm::vec4(entity.pivot, 1.0F);
                transform = glm::translate(glm::mat4(1.0F), entity.pivot)
                    * glm::rotate(glm::mat4(1.0F), angle, glm::vec3(0.0F, 1.0F, 0.0F));
            } else {
                transform = glm::translate(glm::mat4(1.0F), entity.doorDirection * entity.doorDistance * entity.openAmount);
            }
            if (entity.body >= 0) {
                impl_->physics->SetBodyTransform(entity.body, transform);
            }
        }
        if (entity.trigger && entity.brush >= 0 && entity.alive) {
            glm::vec3 minimum;
            glm::vec3 maximum;
            if (impl_->map->BrushAabb(entity.brush, minimum, maximum)) {
                const glm::vec3 playerMin = feet - glm::vec3(playerRadius, 0.0F, playerRadius);
                const glm::vec3 playerMax = feet + glm::vec3(playerRadius, playerHeight, playerRadius);
                const bool overlap = playerMin.x <= maximum.x && playerMax.x >= minimum.x
                    && playerMin.y <= maximum.y && playerMax.y >= minimum.y
                    && playerMin.z <= maximum.z && playerMax.z >= minimum.z;
                const bool changeLevel = Lower(entity.classname) == "trigger_changelevel";
                if (changeLevel) {
                    if (overlap && !entity.fired && !impl_->levelChangePending) {
                        entity.fired = true;
                        impl_->TryQueueLevelChange(entity, feet);
                    }
                    if (!overlap) {
                        entity.fired = false;
                    }
                } else if (overlap && !entity.fired) {
                    impl_->FireOutput(entity, "OnTrigger");
                    impl_->FireOutput(entity, "OnStartTouch");
                    if (Lower(entity.classname) == "trigger_once") {
                        entity.fired = true;
                        entity.alive = false;
                    }
                    if (Lower(entity.classname) == "trigger_hurt") {
                        impl_->health -= 20.0F * deltaTime;
                    }
                }
                if (!changeLevel && !overlap) {
                    entity.fired = Lower(entity.classname) == "trigger_once" ? entity.fired : false;
                }
            }
        }
        if ((entity.weaponItem || entity.ammoItem) && entity.alive) {
            if (glm::length(entity.origin - feet) < 48.0F * kSourceToWorld) {
                const int slot = static_cast<int>(entity.weapon);
                impl_->owned[slot] = true;
                impl_->ammo[slot] += entity.ammoItem ? entity.ammoAmount : (slot == static_cast<int>(WeaponKind::Shotgun) ? 6 : 30);
                entity.alive = false;
                impl_->FireOutput(entity, "OnPlayerPickup");
            }
        }
    }
    impl_->UpdateNpcs(deltaTime, feet);

    if (impl_->health <= 0.0F) {
        impl_->health = 100.0F;
        impl_->nextPlayerHurt = impl_->time + 1.5F;
    }

    const bool fireDown = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
    const bool altDown = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
    const bool useDown = glfwGetKey(window, GLFW_KEY_E) == GLFW_PRESS;
    for (int slot = 0; slot < 5; ++slot) {
        if (glfwGetKey(window, GLFW_KEY_1 + slot) == GLFW_PRESS && impl_->owned[slot]) {
            impl_->weapon = static_cast<WeaponKind>(slot);
        }
    }

    auto traceDamage = [&](float damage, float range, float spread) {
        const glm::vec3 side = glm::normalize(glm::cross(look, glm::vec3(0.0F, 1.0F, 0.0F)));
        const glm::vec3 aim = glm::normalize(look + side * spread);
        const PhysicsRayHit hit = impl_->physics->RayCast(eye + aim * 0.15F, aim, range);
        if (!hit.hit || hit.userData == 0 || hit.userData >= 0x8000000000000000ULL) {
            return;
        }
        const std::size_t index = static_cast<std::size_t>(hit.userData - 1);
        if (index >= impl_->entities.size()) {
            return;
        }
        SimEntity& target = impl_->entities[index];
        damageEntity(target, damage);
        if (target.body >= 0 && !target.motionDisabled) {
            impl_->physics->ApplyBodyImpulse(target.body, aim * damage * 0.8F);
        }
    };

    auto dropCarry = [&](bool thrown) {
        if (impl_->carriedEntity < 0 || static_cast<std::size_t>(impl_->carriedEntity) >= impl_->entities.size()) {
            impl_->carriedEntity = -1;
            impl_->carriedWithPhysgun = false;
            return;
        }
        SimEntity& carried = impl_->entities[static_cast<std::size_t>(impl_->carriedEntity)];
        impl_->FireOutput(carried, "OnPhysGunDrop");
        if (carried.body >= 0 && impl_->physics != nullptr) {
            impl_->physics->SetBodyGravityScale(carried.body, 1.0F);
            if (thrown) {
                const float lightness = std::clamp(kMaxCarryMass / std::max(carried.mass, 1.0F), 1.0F, 3.0F);
                impl_->physics->SetBodyVelocity(carried.body, look * (4.5F * lightness));
            }
        }
        impl_->carriedEntity = -1;
        impl_->carriedWithPhysgun = false;
    };

    auto tryCarry = [&](std::size_t index, bool physgun, float distance) {
        if (index >= impl_->entities.size()) {
            return false;
        }
        SimEntity& candidate = impl_->entities[index];
        if (!candidate.physicsProp || !candidate.alive || candidate.body < 0 || candidate.motionDisabled || candidate.preventPickup
            || candidate.mass > kMaxCarryMass) {
            return false;
        }
        impl_->carriedEntity = static_cast<int>(index);
        impl_->carriedWithPhysgun = physgun;
        impl_->carryDistance = std::clamp(distance, 24.0F * kSourceToWorld, 72.0F * kSourceToWorld);
        impl_->carryLocalRotation = glm::inverse(ViewOrientation(look)) * impl_->physics->BodyRotation(candidate.body);
        impl_->physics->SetBodyGravityScale(candidate.body, 0.0F);
        impl_->physics->SetBodyVelocity(candidate.body, glm::vec3(0.0F));
        if (physgun) {
            impl_->FireOutput(candidate, "OnPhysGunPickup");
        }
        return true;
    };

    auto propUnderCrosshair = [&](float range, float& distance) -> int {
        distance = range;
        if (impl_->physics == nullptr) {
            return -1;
        }
        const std::vector<PhysicsRayHit> hits = impl_->physics->RayCastAll(eye, look, range);
        int bestIndex = -1;
        float bestDistance = range;
        float worldDistance = range + 1.0F;
        for (const PhysicsRayHit& hit : hits) {
            if (!hit.hit) {
                continue;
            }
            if (hit.userData == 0 || hit.userData >= 0x8000000000000000ULL) {
                worldDistance = std::min(worldDistance, hit.distance);
                continue;
            }
            if (hit.userData > impl_->entities.size()) {
                continue;
            }
            const std::size_t index = static_cast<std::size_t>(hit.userData - 1);
            if (!impl_->entities[index].physicsProp || !impl_->entities[index].alive || impl_->entities[index].body < 0) {
                worldDistance = std::min(worldDistance, hit.distance);
                continue;
            }
            if (hit.distance < bestDistance) {
                bestDistance = hit.distance;
                bestIndex = static_cast<int>(index);
            }
        }
        if (bestIndex < 0 || worldDistance + 0.2F < bestDistance) {
            return -1;
        }
        distance = bestDistance;
        return bestIndex;
    };

    auto puntProp = [&](int index) {
        if (index < 0 || static_cast<std::size_t>(index) >= impl_->entities.size() || impl_->physics == nullptr) {
            return;
        }
        SimEntity& target = impl_->entities[static_cast<std::size_t>(index)];
        if (!target.physicsProp || target.body < 0) {
            return;
        }
        if (impl_->carriedEntity == index) {
            dropCarry(false);
        }
        impl_->EnableMotion(target);
        const float impulse = std::max(40.0F, target.mass * 4.0F);
        impl_->physics->ApplyBodyImpulse(target.body, look * impulse);
    };

    if (useDown && !impl_->useWasDown) {
        bool handledProp = false;
        if (impl_->carriedEntity >= 0 && !impl_->carriedWithPhysgun) {
            dropCarry(false);
            handledProp = true;
        } else if (impl_->carriedEntity < 0) {
            float distance = 0.0F;
            const int index = propUnderCrosshair(80.0F * kSourceToWorld, distance);
            if (index >= 0) {
                handledProp = true;
                SimEntity& target = impl_->entities[static_cast<std::size_t>(index)];
                impl_->FireOutput(target, "OnPlayerUse");
                if (target.motionDisabled) {
                    impl_->EnableMotion(target);
                }
                tryCarry(static_cast<std::size_t>(index), false, distance);
            }
        }
        if (!handledProp) {
            SimEntity* closest = nullptr;
            float closestDistance = 72.0F * kSourceToWorld;
            for (SimEntity& entity : impl_->entities) {
                if (!entity.button && Lower(entity.classname) != "func_door") {
                    continue;
                }
                const float doorDistance = glm::length(entity.pivot - eye);
                if (doorDistance < closestDistance) {
                    closestDistance = doorDistance;
                    closest = &entity;
                }
            }
            if (closest != nullptr) {
                if (closest->button) {
                    impl_->FireOutput(*closest, "OnPressed");
                    closest->openAmount = 1.0F;
                } else {
                    closest->nextAttack = closest->nextAttack < 0.0F ? 1.0F : -1.0F;
                    impl_->FireOutput(*closest, "OnOpen");
                }
            }
        }
    }

    const bool physgun = impl_->weapon == WeaponKind::Physgun;
    if (physgun && fireDown && !impl_->fireWasDown && impl_->carriedEntity < 0) {
        float distance = 0.0F;
        const int index = propUnderCrosshair(250.0F * kSourceToWorld, distance);
        tryCarry(static_cast<std::size_t>(index), true, distance);
    }
    if (physgun && !fireDown && impl_->carriedWithPhysgun) {
        dropCarry(false);
    }
    if (physgun && altDown && !impl_->altWasDown) {
        if (impl_->carriedEntity >= 0) {
            puntProp(impl_->carriedEntity);
        } else {
            float distance = 0.0F;
            puntProp(propUnderCrosshair(250.0F * kSourceToWorld, distance));
        }
    }

    if (impl_->carriedEntity >= 0) {
        if (static_cast<std::size_t>(impl_->carriedEntity) >= impl_->entities.size()) {
            impl_->carriedEntity = -1;
        } else {
            SimEntity& carried = impl_->entities[static_cast<std::size_t>(impl_->carriedEntity)];
            if (!carried.alive || carried.body < 0 || impl_->physics == nullptr) {
                impl_->carriedEntity = -1;
                impl_->carriedWithPhysgun = false;
            } else if (fireDown && !impl_->fireWasDown && !impl_->carriedWithPhysgun) {
                dropCarry(true);
            } else {
                const glm::vec3 current = impl_->physics->BodyPosition(carried.body);
                if (glm::length(current - eye) > 160.0F * kSourceToWorld) {
                    dropCarry(false);
                } else {
                    const glm::vec3 target = eye + look * impl_->carryDistance;
                    glm::vec3 velocity = (target - current) / 0.05F;
                    const float speed = glm::length(velocity);
                    if (speed > 25.0F) {
                        velocity *= 25.0F / speed;
                    }
                    impl_->physics->SetBodyVelocity(carried.body, velocity);
                    glm::quat delta = ViewOrientation(look) * impl_->carryLocalRotation * glm::inverse(impl_->physics->BodyRotation(carried.body));
                    if (delta.w < 0.0F) {
                        delta = -delta;
                    }
                    const glm::vec3 axis(delta.x, delta.y, delta.z);
                    const float sine = glm::length(axis);
                    glm::vec3 angular(0.0F);
                    if (sine > 1.0e-5F) {
                        const float angle = 2.0F * std::atan2(sine, delta.w);
                        angular = (axis / sine) * std::clamp(angle / 0.05F, -20.0F, 20.0F);
                    }
                    impl_->physics->SetBodyAngularVelocity(carried.body, angular);
                }
            }
        }
    } else if (fireDown && impl_->time >= impl_->nextFire) {
        const int slot = static_cast<int>(impl_->weapon);
        if (impl_->weapon == WeaponKind::Crowbar) {
            impl_->nextFire = impl_->time + 0.4F;
            traceDamage(25.0F, 64.0F * kSourceToWorld, 0.0F);
        } else if (impl_->ammo[slot] > 0) {
            if (impl_->weapon == WeaponKind::Pistol) {
                impl_->nextFire = impl_->time + 0.15F;
                --impl_->ammo[slot];
                traceDamage(8.0F, 4096.0F * kSourceToWorld, 0.0F);
            } else if (impl_->weapon == WeaponKind::Smg) {
                impl_->nextFire = impl_->time + 0.075F;
                --impl_->ammo[slot];
                traceDamage(4.0F, 4096.0F * kSourceToWorld, ((impl_->ammo[slot] & 1) != 0 ? 0.01F : -0.01F));
            } else if (impl_->weapon == WeaponKind::Shotgun) {
                impl_->nextFire = impl_->time + 0.7F;
                --impl_->ammo[slot];
                for (int pellet = 0; pellet < 7; ++pellet) {
                    const float spread = (static_cast<float>(pellet) - 3.0F) * 0.03F;
                    traceDamage(8.0F, 512.0F * kSourceToWorld, spread);
                }
            }
        }
    }

    impl_->fireWasDown = fireDown;
    impl_->useWasDown = useDown;
    impl_->altWasDown = altDown;
    static_cast<void>(findNamed);
}

void GameSimulation::Impl::DrawEntities(const Shader& shader, bool translucentPass, bool skyPass) {
    shader.SetBool("UseLightmap", false);
    shader.SetBool("UseSpriteTint", false);
    shader.SetFloat("Opacity", 1.0F);
    shader.SetInt("Texture0", 0);
    shader.SetInt("AlphaMode", 0);
    if (!translucentPass) {
        glDisable(GL_BLEND);
        glDepthMask(GL_TRUE);
    }
    glActiveTexture(GL_TEXTURE0);
    for (const SimEntity& entity : entities) {
        if (entity.inSkybox != skyPass) {
            continue;
        }
        if (!entity.alive && (entity.weaponItem || entity.ammoItem || entity.physicsProp || entity.breakable)) {
            continue;
        }
        if (entity.trigger) {
            continue;
        }
        if (entity.staticProp && !entity.inSkybox && !entity.visibilityLeaves.empty() && map != nullptr) {
            bool propVisible = false;
            for (const int leaf : entity.visibilityLeaves) {
                if (map->LeafVisible(leaf)) {
                    propVisible = true;
                    break;
                }
            }
            if (!propVisible) {
                continue;
            }
        }
        float fadeOpacity = 1.0F;
        bool fading = false;
        if (entity.staticProp && entity.fadeMax > entity.fadeMin) {
            const float distance = glm::length(entity.origin - viewPosition);
            if (distance >= entity.fadeMax) {
                continue;
            }
            if (distance > entity.fadeMin) {
                fadeOpacity = 1.0F - (distance - entity.fadeMin) / (entity.fadeMax - entity.fadeMin);
                fading = true;
            }
        }
        if (fading && !translucentPass) {
            continue;
        }
        glm::vec3 propTint(0.35F);
        if (entity.hasLightingOrigin && glm::length(entity.lightTint) > 0.05F) {
            propTint = entity.lightTint;
        }
        shader.SetVec3("PropTint", propTint);
        if (entity.brush >= 0 && map != nullptr) {
            glm::mat4 transform(1.0F);
            if (entity.rotatingDoor) {
                const float angle = glm::radians(entity.doorDistance * entity.openAmount);
                transform = glm::translate(glm::mat4(1.0F), entity.pivot)
                    * glm::rotate(glm::mat4(1.0F), angle, glm::vec3(0.0F, 1.0F, 0.0F))
                    * glm::translate(glm::mat4(1.0F), -entity.pivot);
            } else if (entity.door || entity.button) {
                transform = glm::translate(glm::mat4(1.0F), entity.doorDirection * entity.doorDistance * entity.openAmount);
            }
            shader.SetMat4("Model", transform);
            map->DrawBrushModel(entity.brush, shader, translucentPass);
        }
        const auto visual = visuals.find(entity.visualKey);
        if (entity.visualKey.empty() || visual == visuals.end()) {
            continue;
        }
        glm::mat4 model = glm::translate(glm::mat4(1.0F), entity.origin) * entity.rotation;
        if (entity.npc) {
            model = glm::translate(glm::mat4(1.0F), entity.origin) * SourceAnglesToWorldMatrix(0.0F, entity.npcYaw, 0.0F);
        } else if (entity.body >= 0 && (entity.physicsProp || entity.staticProp)) {
            model = physics->BodyDrawMatrix(entity.body);
        }
        shader.SetMat4("Model", model);
        const bool posed = entity.npc && !entity.posedMeshes.empty();
        const std::size_t partCount = posed ? entity.posedMeshes.size() : visual->second.meshes.size();
        for (std::size_t part = 0; part < partCount; ++part) {
            const int mode = part < visual->second.alphaModes.size() ? visual->second.alphaModes[part] : 0;
            const bool translucent = mode == 1 || mode == 3;
            if (!fading && translucent != translucentPass) {
                continue;
            }
            if (fading || translucentPass) {
                glEnable(GL_BLEND);
                glDepthMask(GL_FALSE);
                glDepthFunc(GL_LEQUAL);
                if (mode == 3) {
                    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
                } else {
                    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                }
            } else {
                glDisable(GL_BLEND);
                glDepthMask(GL_TRUE);
            }
            shader.SetInt("AlphaMode", fading && mode == 0 ? 1 : mode);
            shader.SetFloat("Opacity", fading ? fadeOpacity : 1.0F);
            const GLuint texture = visual->second.textures[part];
            glBindTexture(GL_TEXTURE_2D, texture);
            shader.SetBool("UseTexture", texture != 0);
            if (posed) {
                entity.posedMeshes[part]->Draw();
            } else {
                visual->second.meshes[part]->Draw();
            }
        }
    }
    shader.SetMat4("Model", glm::mat4(1.0F));
    shader.SetBool("UseTexture", false);
    shader.SetInt("AlphaMode", 0);
    shader.SetFloat("Opacity", 1.0F);
    shader.SetVec3("PropTint", glm::vec3(0.35F));
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    if (!translucentPass) {
        glDisable(GL_BLEND);
        glDepthMask(GL_TRUE);
    }
}

void FillDebugBox(
    const glm::vec3& minimum,
    const glm::vec3& maximum,
    std::vector<glm::vec3>& vertices,
    std::vector<std::uint32_t>& indices
) {
    vertices = {
        {minimum.x, minimum.y, minimum.z},
        {maximum.x, minimum.y, minimum.z},
        {maximum.x, maximum.y, minimum.z},
        {minimum.x, maximum.y, minimum.z},
        {minimum.x, minimum.y, maximum.z},
        {maximum.x, minimum.y, maximum.z},
        {maximum.x, maximum.y, maximum.z},
        {minimum.x, maximum.y, maximum.z},
    };
    indices = {
        0, 1, 2, 0, 2, 3,
        4, 6, 5, 4, 7, 6,
        0, 4, 5, 0, 5, 1,
        1, 5, 6, 1, 6, 2,
        2, 6, 7, 2, 7, 3,
        3, 7, 4, 3, 4, 0,
    };
}

void GameSimulation::Impl::AppendDebugShapes(std::vector<PhysicsDebugShape>& out) const {
    if (debugHulls.size() < entities.size()) {
        debugHulls.resize(entities.size());
    }
    for (std::size_t index = 0; index < entities.size(); ++index) {
        const SimEntity& entity = entities[index];
        if (entity.body >= 0) {
            continue;
        }
        if (!entity.alive && (entity.weaponItem || entity.ammoItem || entity.physicsProp)) {
            continue;
        }
        if (entity.brush >= 0 && map != nullptr) {
            const std::vector<glm::vec3>& vertices = map->BrushCollisionVertices(entity.brush);
            const std::vector<std::uint32_t>& indices = map->BrushCollisionIndices(entity.brush);
            if (indices.size() < 3) {
                continue;
            }
            glm::mat4 transform(1.0F);
            if (entity.rotatingDoor) {
                const float angle = glm::radians(entity.doorDistance * entity.openAmount);
                transform = glm::translate(glm::mat4(1.0F), entity.pivot)
                    * glm::rotate(glm::mat4(1.0F), angle, glm::vec3(0.0F, 1.0F, 0.0F))
                    * glm::translate(glm::mat4(1.0F), -entity.pivot);
            } else if (entity.door || entity.button) {
                transform = glm::translate(glm::mat4(1.0F), entity.doorDirection * entity.doorDistance * entity.openAmount);
            }
            PhysicsDebugShape shape;
            shape.key = (1ULL << 32) | static_cast<std::uint64_t>(index);
            shape.transform = transform;
            shape.vertices = &vertices;
            shape.indices = &indices;
            out.push_back(shape);
            continue;
        }
        if (entity.visualKey.empty()) {
            continue;
        }
        const auto visual = visuals.find(entity.visualKey);
        if (visual == visuals.end()) {
            continue;
        }
        DebugHull& hull = debugHulls[index];
        if (!hull.ready) {
            const glm::vec3 minimum = glm::min(visual->second.hullMin, visual->second.hullMax);
            const glm::vec3 maximum = glm::max(visual->second.hullMin, visual->second.hullMax);
            FillDebugBox(minimum, maximum, hull.vertices, hull.indices);
            hull.ready = true;
        }
        if (hull.indices.size() < 3) {
            continue;
        }
        PhysicsDebugShape shape;
        shape.key = (1ULL << 33) | static_cast<std::uint64_t>(index);
        shape.transform = glm::translate(glm::mat4(1.0F), entity.origin) * entity.rotation;
        shape.vertices = &hull.vertices;
        shape.indices = &hull.indices;
        out.push_back(shape);
    }
}

void GameSimulation::AppendDebugShapes(std::vector<PhysicsDebugShape>& out) const {
    impl_->AppendDebugShapes(out);
}

void GameSimulation::CollectDoorStates(std::vector<std::pair<std::string, bool>>& out) const {
    out.clear();
    if (impl_ == nullptr) {
        return;
    }
    for (const SimEntity& entity : impl_->entities) {
        if (!entity.door || entity.targetName.empty()) {
            continue;
        }
        out.emplace_back(entity.targetName, entity.openAmount > 0.05F);
    }
}

void GameSimulation::SetViewPosition(const glm::vec3& position) {
    if (impl_ != nullptr) {
        impl_->viewPosition = position;
    }
}

void GameSimulation::Draw(const Shader& shader) const {
    impl_->DrawEntities(shader, false, false);
}

void GameSimulation::DrawTransparent(const Shader& shader) const {
    impl_->DrawEntities(shader, true, false);
}

void GameSimulation::DrawSky(const Shader& shader) const {
    impl_->DrawEntities(shader, false, true);
}

void GameSimulation::DrawSkyTransparent(const Shader& shader) const {
    impl_->DrawEntities(shader, true, true);
}
