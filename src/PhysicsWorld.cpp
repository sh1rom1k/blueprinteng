#include "PhysicsWorld.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <thread>
#include <stdexcept>
#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Collision/ShapeFilter.h>

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>

namespace {
using namespace JPH;

namespace Layers {
constexpr ObjectLayer NonMoving = 0;
constexpr ObjectLayer Moving = 1;
constexpr uint Count = 2;
}

namespace BroadPhaseLayers {
constexpr BroadPhaseLayer NonMoving(0);
constexpr BroadPhaseLayer Moving(1);
constexpr uint Count = 2;
}

class BPLayerInterfaceImpl final : public JPH::BroadPhaseLayerInterface {
public:
    BPLayerInterfaceImpl() {
        mapping_[Layers::NonMoving] = BroadPhaseLayers::NonMoving;
        mapping_[Layers::Moving] = BroadPhaseLayers::Moving;
    }

    uint GetNumBroadPhaseLayers() const override {
        return BroadPhaseLayers::Count;
    }

    BroadPhaseLayer GetBroadPhaseLayer(ObjectLayer layer) const override {
        JPH_ASSERT(layer < Layers::Count);
        return mapping_[layer];
    }

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(BroadPhaseLayer layer) const override {
        return layer == BroadPhaseLayers::NonMoving ? "NonMoving" : "Moving";
    }
#endif

private:
    BroadPhaseLayer mapping_[Layers::Count];
};

class ObjectLayerPairFilterImpl final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(ObjectLayer first, ObjectLayer second) const override {
        if (first == Layers::NonMoving) {
            return second == Layers::Moving;
        }
        if (first == Layers::Moving) {
            return true;
        }
        return false;
    }
};

class ObjectVsBroadPhaseLayerFilterImpl final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(ObjectLayer layer, BroadPhaseLayer broadPhaseLayer) const override {
        if (layer == Layers::NonMoving) {
            return broadPhaseLayer == BroadPhaseLayers::Moving;
        }
        if (layer == Layers::Moving) {
            return true;
        }
        return false;
    }
};

class MovingBroadPhaseFilter final : public JPH::BroadPhaseLayerFilter {
public:
    bool ShouldCollide(BroadPhaseLayer layer) const override {
        return true;
    }
};

class MovingObjectLayerFilter final : public JPH::ObjectLayerFilter {
public:
    bool ShouldCollide(ObjectLayer layer) const override {
        return true;
    }
};

class CharacterContactListenerImpl final : public CharacterContactListener {
public:
    void OnContactAdded(
        const CharacterVirtual* inCharacter,
        const BodyID& inBodyID2,
        const SubShapeID& inSubShapeID2,
        RVec3Arg inContactPosition,
        Vec3Arg inContactNormal,
        CharacterContactSettings& ioSettings
    ) override {
        ioSettings.mCanPushCharacter = true;
        ioSettings.mCanReceiveImpulses = true;
    }
};
}

struct PhysicsWorld::Impl {
    BPLayerInterfaceImpl broadPhaseLayers;
    ObjectLayerPairFilterImpl objectLayerPairFilter;
    ObjectVsBroadPhaseLayerFilterImpl objectVsBroadPhaseLayerFilter;
    PhysicsSystem physicsSystem;
    TempAllocatorImpl tempAllocator{16 * 1024 * 1024};
    JobSystemThreadPool jobSystem{
        cMaxPhysicsJobs,
        cMaxPhysicsBarriers,
        static_cast<int>(std::max(1u, std::thread::hardware_concurrency() - 1))
    };
    BodyID worldBody;
    std::vector<BodyID> dynamicBoxes;
    RefConst<Shape> characterShape;
    std::unique_ptr<CharacterContactListenerImpl> contactListener;
    std::unique_ptr<CharacterVirtual> character;
    bool initialized = false;

    static constexpr uint cMaxBodies = 4096;
    static constexpr uint cMaxBodyPairs = 16384;
    static constexpr uint cMaxContactConstraints = 16384;

    Impl() {
        physicsSystem.Init(
            cMaxBodies,
            0,
            cMaxBodyPairs,
            cMaxContactConstraints,
            broadPhaseLayers,
            objectVsBroadPhaseLayerFilter,
            objectLayerPairFilter
        );
    }
};

namespace {
struct VertexKey {
    int x;
    int y;
    int z;

    bool operator==(const VertexKey& other) const {
        return x == other.x && y == other.y && z == other.z;
    }
};

struct VertexKeyHash {
    std::size_t operator()(const VertexKey& key) const {
        std::size_t hash = static_cast<std::size_t>(key.x);
        hash = hash * 31U + static_cast<std::size_t>(key.y);
        hash = hash * 31U + static_cast<std::size_t>(key.z);
        return hash;
    }
};

struct TriangleKey {
    std::uint32_t a;
    std::uint32_t b;
    std::uint32_t c;

    bool operator==(const TriangleKey& other) const {
        return a == other.a && b == other.b && c == other.c;
    }
};

struct TriangleKeyHash {
    std::size_t operator()(const TriangleKey& key) const {
        return static_cast<std::size_t>(key.a) * 73856093U
            ^ static_cast<std::size_t>(key.b) * 19349663U
            ^ static_cast<std::size_t>(key.c) * 83492791U;
    }
};
}

PhysicsWorld::PhysicsWorld() {
    JPH::RegisterDefaultAllocator();
    JPH::Factory::sInstance = new JPH::Factory();
    JPH::RegisterTypes();
    impl_ = std::make_unique<Impl>();
}

PhysicsWorld::~PhysicsWorld() {
    impl_->character.reset();
    for (BodyID body : impl_->dynamicBoxes) {
        impl_->physicsSystem.GetBodyInterface().RemoveBody(body);
        impl_->physicsSystem.GetBodyInterface().DestroyBody(body);
    }
    if (!impl_->worldBody.IsInvalid()) {
        impl_->physicsSystem.GetBodyInterface().RemoveBody(impl_->worldBody);
        impl_->physicsSystem.GetBodyInterface().DestroyBody(impl_->worldBody);
    }
    JPH::UnregisterTypes();
    delete JPH::Factory::sInstance;
    JPH::Factory::sInstance = nullptr;
}

void PhysicsWorld::SetCollisionMesh(
    const std::vector<glm::vec3>& vertices,
    const std::vector<std::uint32_t>& indices
) {
    impl_->character.reset();
    impl_->contactListener.reset();
    for (BodyID body : impl_->dynamicBoxes) {
        impl_->physicsSystem.GetBodyInterface().RemoveBody(body);
        impl_->physicsSystem.GetBodyInterface().DestroyBody(body);
    }
    impl_->dynamicBoxes.clear();
    if (!impl_->worldBody.IsInvalid()) {
        impl_->physicsSystem.GetBodyInterface().RemoveBody(impl_->worldBody);
        impl_->physicsSystem.GetBodyInterface().DestroyBody(impl_->worldBody);
        impl_->worldBody = BodyID();
    }
    if (indices.empty()) {
        RefConst<Shape> floorShape = new BoxShape(Vec3(30.0F, 0.5F, 30.0F));
        BodyCreationSettings floorSettings(
            floorShape,
            RVec3(0.0F, -0.5F, 0.0F),
            Quat::sIdentity(),
            EMotionType::Static,
            Layers::NonMoving
        );
        impl_->worldBody = impl_->physicsSystem.GetBodyInterface().CreateAndAddBody(
            floorSettings,
            EActivation::DontActivate
        );
        impl_->physicsSystem.OptimizeBroadPhase();
        impl_->initialized = true;
        return;
    }

    VertexList meshVertices;
    IndexedTriangleList meshTriangles;
    std::unordered_map<VertexKey, std::uint32_t, VertexKeyHash> vertexLookup;
    std::unordered_set<TriangleKey, TriangleKeyHash> triangleLookup;
    meshVertices.reserve(vertices.size());
    meshTriangles.reserve((indices.size() / 3) * 2);

    for (std::size_t index = 0; index + 2 < indices.size(); index += 3) {
        if (indices[index] >= vertices.size() || indices[index + 1] >= vertices.size()
            || indices[index + 2] >= vertices.size()) {
            continue;
        }
        const glm::vec3& a = vertices.at(indices[index]);
        const glm::vec3& b = vertices.at(indices[index + 1]);
        const glm::vec3& c = vertices.at(indices[index + 2]);
        const glm::vec3 area = glm::cross(b - a, c - a);
        if (!std::isfinite(a.x) || !std::isfinite(a.y) || !std::isfinite(a.z)
            || !std::isfinite(b.x) || !std::isfinite(b.y) || !std::isfinite(b.z)
            || !std::isfinite(c.x) || !std::isfinite(c.y) || !std::isfinite(c.z)
            || glm::dot(area, area) < 1.0e-8F) {
            continue;
        }

        auto getVertexIndex = [&](const glm::vec3& vertex) {
            const VertexKey key{
                static_cast<int>(std::lround(vertex.x * 10000.0F)),
                static_cast<int>(std::lround(vertex.y * 10000.0F)),
                static_cast<int>(std::lround(vertex.z * 10000.0F)),
            };
            const auto found = vertexLookup.find(key);
            if (found != vertexLookup.end()) return found->second;
            const std::uint32_t result = static_cast<std::uint32_t>(meshVertices.size());
            vertexLookup.emplace(key, result);
            meshVertices.emplace_back(vertex.x, vertex.y, vertex.z);
            return result;
        };

        const std::uint32_t i0 = getVertexIndex(a);
        const std::uint32_t i1 = getVertexIndex(b);
        const std::uint32_t i2 = getVertexIndex(c);
        if (i0 == i1 || i1 == i2 || i0 == i2) {
            continue;
        }

        std::array<std::uint32_t, 3> triangleKey = {i0, i1, i2};
        std::sort(triangleKey.begin(), triangleKey.end());
        if (!triangleLookup.emplace(TriangleKey{triangleKey[0], triangleKey[1], triangleKey[2]}).second) {
            continue;
        }
        // Add double-sided collision so objects never fall through inverted faces or thin surfaces
        meshTriangles.emplace_back(i0, i1, i2);
        meshTriangles.emplace_back(i0, i2, i1);
    }

    MeshShapeSettings shapeSettings;
    shapeSettings.mTriangleVertices = std::move(meshVertices);
    shapeSettings.mIndexedTriangles = std::move(meshTriangles);
    shapeSettings.mActiveEdgeCosThresholdAngle = std::cos(glm::radians(15.0F));
    std::cout << "Jolt collision create: " << shapeSettings.mIndexedTriangles.size() << " triangles" << std::endl;
    ShapeSettings::ShapeResult shapeResult = shapeSettings.Create();
    std::cout << "Jolt collision created" << std::endl;
    if (shapeResult.HasError()) {
        throw std::runtime_error(shapeResult.GetError().c_str());
    }
    BodyCreationSettings bodySettings(
        shapeResult.Get(),
        RVec3::sZero(),
        Quat::sIdentity(),
        EMotionType::Static,
        Layers::NonMoving
    );
    impl_->worldBody = impl_->physicsSystem.GetBodyInterface().CreateAndAddBody(
        bodySettings,
        EActivation::DontActivate
    );
    if (impl_->worldBody.IsInvalid()) {
        throw std::runtime_error("Jolt could not create BSP collision body");
    }
    impl_->physicsSystem.OptimizeBroadPhase();
    impl_->initialized = true;
}

void PhysicsWorld::SetDynamicBoxes(const std::vector<glm::vec3>& positions) {
    BodyInterface& bodyInterface = impl_->physicsSystem.GetBodyInterface();
    for (BodyID body : impl_->dynamicBoxes) {
        bodyInterface.RemoveBody(body);
        bodyInterface.DestroyBody(body);
    }
    impl_->dynamicBoxes.clear();
    if (!impl_->initialized) {
        return;
    }

    constexpr float halfExtent = 0.35F;
    RefConst<Shape> shape = new BoxShape(Vec3(halfExtent, halfExtent, halfExtent));
    for (const glm::vec3& position : positions) {
        // Offset Y slightly above floor so the box does not spawn embedded in the floor geometry
        BodyCreationSettings settings(
            shape,
            RVec3(position.x, position.y + halfExtent + 0.02F, position.z),
            Quat::sIdentity(),
            EMotionType::Dynamic,
            Layers::Moving
        );
        settings.mMotionQuality = EMotionQuality::LinearCast;
        settings.mFriction = 0.7F;
        settings.mRestitution = 0.2F;
        settings.mLinearDamping = 0.2F;
        settings.mAngularDamping = 0.2F;
        settings.mOverrideMassProperties = EOverrideMassProperties::CalculateInertia;
        settings.mMassPropertiesOverride.mMass = 25.0F;
        const BodyID body = bodyInterface.CreateAndAddBody(settings, EActivation::Activate);
        if (!body.IsInvalid()) {
            impl_->dynamicBoxes.push_back(body);
        }
    }
    std::cout << "PhysicsWorld: Created " << impl_->dynamicBoxes.size() << " dynamic prop bodies" << std::endl;
}

void PhysicsWorld::SpawnDynamicBox(const glm::vec3& position, const glm::vec3& linearVelocity) {
    if (!impl_->initialized) {
        return;
    }
    constexpr float halfExtent = 0.35F;
    RefConst<Shape> shape = new BoxShape(Vec3(halfExtent, halfExtent, halfExtent));
    BodyCreationSettings settings(
        shape,
        RVec3(position.x, position.y, position.z),
        Quat::sIdentity(),
        EMotionType::Dynamic,
        Layers::Moving
    );
    settings.mMotionQuality = EMotionQuality::LinearCast;
    settings.mFriction = 0.7F;
    settings.mRestitution = 0.2F;
    settings.mLinearDamping = 0.2F;
    settings.mAngularDamping = 0.2F;
    settings.mOverrideMassProperties = EOverrideMassProperties::CalculateInertia;
    settings.mMassPropertiesOverride.mMass = 25.0F;
    settings.mLinearVelocity = Vec3(linearVelocity.x, linearVelocity.y, linearVelocity.z);
    const BodyID body = impl_->physicsSystem.GetBodyInterface().CreateAndAddBody(
        settings,
        EActivation::Activate
    );
    if (!body.IsInvalid()) {
        impl_->dynamicBoxes.push_back(body);
        std::cout << "PhysicsWorld: Spawned dynamic prop, total count = " << impl_->dynamicBoxes.size() << std::endl;
    }
}

std::vector<glm::vec3> PhysicsWorld::DynamicBoxPositions() const {
    std::vector<glm::vec3> positions;
    positions.reserve(impl_->dynamicBoxes.size());
    const BodyInterface& bodyInterface = impl_->physicsSystem.GetBodyInterface();
    for (BodyID body : impl_->dynamicBoxes) {
        const RVec3 position = bodyInterface.GetPosition(body);
        positions.emplace_back(
            static_cast<float>(position.GetX()),
            static_cast<float>(position.GetY()),
            static_cast<float>(position.GetZ())
        );
    }
    return positions;
}

std::vector<glm::mat4> PhysicsWorld::DynamicBoxMatrices() const {
    std::vector<glm::mat4> matrices;
    matrices.reserve(impl_->dynamicBoxes.size());
    const BodyInterface& bodyInterface = impl_->physicsSystem.GetBodyInterface();
    constexpr float boxScale = 0.7F; // halfExtent 0.35F * 2.0F = 0.7F
    for (BodyID body : impl_->dynamicBoxes) {
        const RVec3 pos = bodyInterface.GetPosition(body);
        const Quat rot = bodyInterface.GetRotation(body);

        const float qx = rot.GetX();
        const float qy = rot.GetY();
        const float qz = rot.GetZ();
        const float qw = rot.GetW();

        const float x2 = qx + qx;
        const float y2 = qy + qy;
        const float z2 = qz + qz;
        const float xx = qx * x2;
        const float xy = qx * y2;
        const float xz = qx * z2;
        const float yy = qy * y2;
        const float yz = qy * z2;
        const float zz = qz * z2;
        const float wx = qw * x2;
        const float wy = qw * y2;
        const float wz = qw * z2;

        glm::mat4 m(1.0F);
        m[0][0] = (1.0F - (yy + zz)) * boxScale;
        m[0][1] = (xy + wz) * boxScale;
        m[0][2] = (xz - wy) * boxScale;
        m[0][3] = 0.0F;

        m[1][0] = (xy - wz) * boxScale;
        m[1][1] = (1.0F - (xx + zz)) * boxScale;
        m[1][2] = (yz + wx) * boxScale;
        m[1][3] = 0.0F;

        m[2][0] = (xz + wy) * boxScale;
        m[2][1] = (yz - wx) * boxScale;
        m[2][2] = (1.0F - (xx + yy)) * boxScale;
        m[2][3] = 0.0F;

        m[3][0] = static_cast<float>(pos.GetX());
        m[3][1] = static_cast<float>(pos.GetY());
        m[3][2] = static_cast<float>(pos.GetZ());
        m[3][3] = 1.0F;

        matrices.push_back(m);
    }
    return matrices;
}

std::size_t PhysicsWorld::DynamicBoxCount() const {
    return impl_->dynamicBoxes.size();
}

void PhysicsWorld::StepSimulation(float deltaTime) {
    if (!impl_->initialized) return;
    const int collisionSteps = std::clamp(static_cast<int>(std::ceil(deltaTime / (1.0F / 60.0F))), 1, 4);
    impl_->physicsSystem.Update(
        deltaTime,
        collisionSteps,
        &impl_->tempAllocator,
        &impl_->jobSystem
    );
}

void PhysicsWorld::SetCharacterPosition(const glm::vec3& position) {
    if (impl_->character) {
        impl_->character->SetPosition(RVec3(position.x, position.y, position.z));
        return;
    }

    constexpr float kCharacterHeight = 1.08F; // Standing height of cylinder portion
    constexpr float kCharacterRadius = 0.22F; // Radius of capsule

    CharacterVirtualSettings settings;
    Ref<Shape> capsule = new CapsuleShape(0.5F * kCharacterHeight, kCharacterRadius);
    settings.mShape = RotatedTranslatedShapeSettings(
        Vec3(0.0F, 0.5F * kCharacterHeight + kCharacterRadius, 0.0F),
        Quat::sIdentity(),
        capsule
    ).Create().Get();
    settings.mSupportingVolume = Plane(Vec3::sAxisY(), -kCharacterRadius);
    settings.mMaxSlopeAngle = DegreesToRadians(50.0F);
    settings.mBackFaceMode = EBackFaceMode::CollideWithBackFaces;
    settings.mCharacterPadding = 0.02F;
    settings.mPenetrationRecoverySpeed = 1.0F;
    settings.mPredictiveContactDistance = 0.1F;
    settings.mMaxStrength = 4000.0F;
    settings.mMass = 80.0F;
    settings.mInnerBodyShape = settings.mShape;
    settings.mInnerBodyLayer = Layers::Moving;

    impl_->characterShape = settings.mShape;
    impl_->contactListener = std::make_unique<CharacterContactListenerImpl>();
    impl_->character = std::make_unique<CharacterVirtual>(
        &settings,
        RVec3(position.x, position.y, position.z),
        Quat::sIdentity(),
        &impl_->physicsSystem
    );
    impl_->character->SetListener(impl_->contactListener.get());
}

void PhysicsWorld::UpdateCharacter(
    float deltaTime,
    const glm::vec3& desiredVelocity,
    bool jump
) {
    if (!impl_->character || !impl_->initialized) return;

    impl_->character->UpdateGroundVelocity();

    Vec3 currentVerticalVelocity = impl_->character->GetLinearVelocity().GetY() * Vec3::sAxisY();
    Vec3 groundVelocity = impl_->character->GetGroundVelocity();
    Vec3 newVelocity;

    const bool onGround = impl_->character->GetGroundState() == CharacterVirtual::EGroundState::OnGround;
    const bool movingTowardsGround = (currentVerticalVelocity.GetY() - groundVelocity.GetY()) < 0.1F;

    if (onGround && movingTowardsGround) {
        newVelocity = groundVelocity;
        if (jump) {
            newVelocity += 5.0F * Vec3::sAxisY();
        }
    } else {
        newVelocity = currentVerticalVelocity;
    }

    // Apply gravity
    newVelocity += Vec3(0.0F, -14.0F, 0.0F) * deltaTime;

    // Player horizontal input
    newVelocity += Vec3(desiredVelocity.x, 0.0F, desiredVelocity.z);

    impl_->character->SetLinearVelocity(newVelocity);

    MovingBroadPhaseFilter broadPhaseFilter;
    MovingObjectLayerFilter objectLayerFilter;
    BodyFilter bodyFilter;
    ShapeFilter shapeFilter;
    CharacterVirtual::ExtendedUpdateSettings updateSettings;
    updateSettings.mStickToFloorStepDown = Vec3(0.0F, -0.5F, 0.0F);
    updateSettings.mWalkStairsStepUp = Vec3(0.0F, 0.3F, 0.0F);
    updateSettings.mWalkStairsMinStepForward = 0.02F;
    updateSettings.mWalkStairsStepForwardTest = 0.15F;

    impl_->character->ExtendedUpdate(
        deltaTime,
        Vec3(0.0F, -14.0F, 0.0F),
        updateSettings,
        broadPhaseFilter,
        objectLayerFilter,
        bodyFilter,
        shapeFilter,
        impl_->tempAllocator
    );
}

void PhysicsWorld::UpdateNoclip(const glm::vec3& position) {
    if (impl_->character) {
        impl_->character->SetPosition(RVec3(position.x, position.y, position.z));
    }
}

glm::vec3 PhysicsWorld::CharacterPosition() const {
    if (!impl_->character) return glm::vec3(0.0F);
    const RVec3 position = impl_->character->GetPosition();
    return glm::vec3(static_cast<float>(position.GetX()), static_cast<float>(position.GetY()), static_cast<float>(position.GetZ()));
}

bool PhysicsWorld::IsCharacterGrounded() const {
    return impl_->character != nullptr && impl_->character->IsSupported();
}
