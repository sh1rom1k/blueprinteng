#include "PhysicsWorld.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <mutex>
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
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Collision/TransformedShape.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Body/MassProperties.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Collision/ShapeFilter.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Body/BodyLock.h>

#include "SourceCoords.hpp"

#include <glm/gtc/quaternion.hpp>

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>

namespace {
using namespace JPH;

namespace Layers {
constexpr ObjectLayer NonMoving = 0;
constexpr ObjectLayer Moving = 1;
constexpr ObjectLayer Character = 2;
constexpr uint Count = 3;
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
        mapping_[Layers::Character] = BroadPhaseLayers::Moving;
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
        if (first > second) {
            std::swap(first, second);
        }
        if (first == Layers::NonMoving) {
            return second == Layers::Moving || second == Layers::Character;
        }
        if (first == Layers::Moving) {
            return second == Layers::Moving;
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
        if (layer == Layers::Character) {
            return broadPhaseLayer == BroadPhaseLayers::NonMoving;
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
    explicit CharacterContactListenerImpl(PhysicsSystem& system) : system_(system) {}

    void OnContactAdded(
        const CharacterVirtual* inCharacter,
        const BodyID& inBodyID2,
        const SubShapeID& inSubShapeID2,
        RVec3Arg inContactPosition,
        Vec3Arg inContactNormal,
        CharacterContactSettings& ioSettings
    ) override {
        static_cast<void>(inCharacter);
        static_cast<void>(inSubShapeID2);
        static_cast<void>(inContactPosition);
        static_cast<void>(inContactNormal);
        // The kinematic inner body does not collide with dynamic props. This controller is the
        // only thing that shoves them, using the character mass and max strength.
        BodyLockRead lock(system_.GetBodyLockInterface(), inBodyID2);
        if (!lock.Succeeded()) {
            return;
        }
        ioSettings.mCanPushCharacter = true;
        ioSettings.mCanReceiveImpulses = lock.GetBody().IsDynamic();
    }

private:
    PhysicsSystem& system_;
};

constexpr std::uint64_t kPlayerUserData = 0x8000000000000000ULL;

class SimContactListener final : public ContactListener {
public:
    ValidateResult OnContactValidate(const Body& inBody1, const Body& inBody2, RVec3Arg inBaseOffset, const CollideShapeResult& inCollisionResult) override {
        static_cast<void>(inBody1);
        static_cast<void>(inBody2);
        static_cast<void>(inBaseOffset);
        static_cast<void>(inCollisionResult);
        return ValidateResult::AcceptAllContactsForThisBodyPair;
    }

    void OnContactAdded(const Body& inBody1, const Body& inBody2, const ContactManifold& inManifold, ContactSettings& ioSettings) override {
        static_cast<void>(ioSettings);
        const std::uint64_t first = inBody1.GetUserData();
        const std::uint64_t second = inBody2.GetUserData();
        if (first == kPlayerUserData || second == kPlayerUserData) {
            return;
        }
        if (first == 0 && second == 0) {
            return;
        }
        const Vec3 relative = inBody1.GetLinearVelocity() - inBody2.GetLinearVelocity();
        const float speed = std::abs(relative.Dot(inManifold.mWorldSpaceNormal));
        if (speed < 4.0F) {
            return;
        }
        std::lock_guard<std::mutex> guard(mutex);
        impacts.push_back(PhysicsImpact{first, second, speed});
    }

    std::vector<PhysicsImpact> Consume() {
        std::lock_guard<std::mutex> guard(mutex);
        std::vector<PhysicsImpact> taken;
        taken.swap(impacts);
        return taken;
    }

private:
    std::mutex mutex;
    std::vector<PhysicsImpact> impacts;
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
    std::vector<BodyID> simBodies;
    RefConst<Shape> characterShape;
    RefConst<Shape> standingShape;
    RefConst<Shape> duckedShape;
    bool ducked = false;
    std::unique_ptr<CharacterContactListenerImpl> contactListener;
    std::unique_ptr<SimContactListener> simContacts;
    std::unique_ptr<CharacterVirtual> character;
    bool initialized = false;
    struct DebugBake {
        const Shape* shape = nullptr;
        std::uint32_t revision = 0;
        std::vector<glm::vec3> vertices;
        std::vector<std::uint32_t> indices;
    };
    mutable std::unordered_map<std::uint64_t, std::unique_ptr<DebugBake>> debugBakes;

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
    impl_->simContacts = std::make_unique<SimContactListener>();
    impl_->physicsSystem.SetContactListener(impl_->simContacts.get());
}

PhysicsWorld::~PhysicsWorld() {
    impl_->physicsSystem.SetContactListener(nullptr);
    impl_->character.reset();
    for (BodyID body : impl_->dynamicBoxes) {
        if (body.IsInvalid()) {
            continue;
        }
        impl_->physicsSystem.GetBodyInterface().RemoveBody(body);
        impl_->physicsSystem.GetBodyInterface().DestroyBody(body);
    }
    for (BodyID body : impl_->simBodies) {
        if (body.IsInvalid()) {
            continue;
        }
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
    for (BodyID body : impl_->simBodies) {
        impl_->physicsSystem.GetBodyInterface().RemoveBody(body);
        impl_->physicsSystem.GetBodyInterface().DestroyBody(body);
    }
    impl_->simBodies.clear();
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
    meshTriangles.reserve(indices.size() / 3);

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
        // Source faces are clockwise when seen from outside. Jolt treats counter-clockwise
        // as the front face and ignores the back, so keep a single reversed winding.
        meshTriangles.emplace_back(i0, i2, i1);
    }

    MeshShapeSettings shapeSettings;
    shapeSettings.mTriangleVertices = std::move(meshVertices);
    shapeSettings.mIndexedTriangles = std::move(meshTriangles);
    shapeSettings.mActiveEdgeCosThresholdAngle = std::cos(glm::radians(5.0F));
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

    constexpr float kCharacterRadius = 16.0F * kSourceToWorld;
    auto makeShape = [](float height) {
        const float cylinder = std::max(0.05F, height - 2.0F * kCharacterRadius);
        Ref<Shape> capsule = new CapsuleShape(0.5F * cylinder, kCharacterRadius);
        return RotatedTranslatedShapeSettings(
            Vec3(0.0F, 0.5F * cylinder + kCharacterRadius, 0.0F),
            Quat::sIdentity(),
            capsule
        ).Create().Get();
    };
    impl_->standingShape = makeShape(72.0F * kSourceToWorld);
    impl_->duckedShape = makeShape(36.0F * kSourceToWorld);

    CharacterVirtualSettings settings;
    settings.mShape = impl_->ducked ? impl_->duckedShape : impl_->standingShape;
    settings.mSupportingVolume = Plane(Vec3::sAxisY(), -kCharacterRadius);
    settings.mMaxSlopeAngle = DegreesToRadians(50.0F);
    settings.mBackFaceMode = EBackFaceMode::CollideWithBackFaces;
    settings.mCharacterPadding = 0.02F;
    settings.mPenetrationRecoverySpeed = 1.0F;
    settings.mPredictiveContactDistance = 0.1F;
    settings.mMaxStrength = 4000.0F;
    settings.mMass = 80.0F;
    settings.mInnerBodyShape = settings.mShape;
    settings.mInnerBodyLayer = Layers::Character;

    impl_->characterShape = settings.mShape;
    impl_->contactListener = std::make_unique<CharacterContactListenerImpl>(impl_->physicsSystem);
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
    bool jump,
    bool inWater
) {
    if (!impl_->character || !impl_->initialized) return;

    impl_->character->UpdateGroundVelocity();

    Vec3 currentVerticalVelocity = impl_->character->GetLinearVelocity().GetY() * Vec3::sAxisY();
    Vec3 groundVelocity = impl_->character->GetGroundVelocity();
    Vec3 newVelocity;

    const bool onGround = impl_->character->GetGroundState() == CharacterVirtual::EGroundState::OnGround;
    const bool movingTowardsGround = (currentVerticalVelocity.GetY() - groundVelocity.GetY()) < 0.1F;
    const float gravityUnits = inWater ? -100.0F : -800.0F;
    const Vec3 gravity(0.0F, gravityUnits * kSourceToWorld, 0.0F);

    if (inWater) {
        const float vertical = std::abs(desiredVelocity.y) > 1.0e-4F
            ? desiredVelocity.y
            : currentVerticalVelocity.GetY() + gravity.GetY() * deltaTime;
        newVelocity = Vec3(desiredVelocity.x, vertical, desiredVelocity.z);
    } else if (onGround && movingTowardsGround) {
        newVelocity = groundVelocity;
        if (jump) {
            newVelocity += (268.0F * kSourceToWorld) * Vec3::sAxisY();
        }
        newVelocity += gravity * deltaTime;
        newVelocity += Vec3(desiredVelocity.x, 0.0F, desiredVelocity.z);
    } else {
        newVelocity = currentVerticalVelocity;
        newVelocity += gravity * deltaTime;
        newVelocity += Vec3(desiredVelocity.x, 0.0F, desiredVelocity.z);
    }

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
        Vec3(0.0F, (inWater ? -100.0F : -800.0F) * kSourceToWorld, 0.0F),
        updateSettings,
        broadPhaseFilter,
        objectLayerFilter,
        bodyFilter,
        shapeFilter,
        impl_->tempAllocator
    );
    const BodyID inner = impl_->character->GetInnerBodyID();
    if (!inner.IsInvalid()) {
        impl_->physicsSystem.GetBodyInterface().SetUserData(inner, 0x8000000000000000ULL);
    }
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

namespace {
class AllRayHits final : public CastRayCollector {
public:
    void AddHit(const RayCastResult& inResult) override {
        hits.push_back(inResult);
    }

    std::vector<RayCastResult> hits;
};

class SkipUserDataFilter final : public JPH::BodyFilter {
public:
    SkipUserDataFilter(std::uint64_t first, std::uint64_t second) : first_(first), second_(second) {}

    bool ShouldCollideLocked(const JPH::Body& body) const override {
        const std::uint64_t userData = body.GetUserData();
        return userData != first_ && userData != second_;
    }

private:
    std::uint64_t first_ = 0;
    std::uint64_t second_ = 0;
};

}

int PhysicsWorld::AddStaticBox(const glm::vec3& position, const glm::vec3& halfExtents, std::uint64_t userData) {
    if (!impl_->initialized) {
        return -1;
    }
    const Vec3 half(
        std::max(0.05F, halfExtents.x),
        std::max(0.05F, halfExtents.y),
        std::max(0.05F, halfExtents.z)
    );
    BodyCreationSettings settings(
        new BoxShape(half),
        RVec3(position.x, position.y, position.z),
        Quat::sIdentity(),
        EMotionType::Static,
        Layers::NonMoving
    );
    settings.mUserData = userData;
    const BodyID body = impl_->physicsSystem.GetBodyInterface().CreateAndAddBody(settings, EActivation::DontActivate);
    if (body.IsInvalid()) {
        return -1;
    }
    impl_->simBodies.push_back(body);
    return static_cast<int>(impl_->simBodies.size() - 1);
}

int PhysicsWorld::AddDynamicBox(
    const glm::vec3& position,
    const glm::vec3& halfExtents,
    float mass,
    std::uint64_t userData
) {
    if (!impl_->initialized) {
        return -1;
    }
    const Vec3 half(
        std::max(0.05F, halfExtents.x),
        std::max(0.05F, halfExtents.y),
        std::max(0.05F, halfExtents.z)
    );
    BodyCreationSettings settings(
        new BoxShape(half),
        RVec3(position.x, position.y, position.z),
        Quat::sIdentity(),
        EMotionType::Dynamic,
        Layers::Moving
    );
    settings.mMotionQuality = EMotionQuality::LinearCast;
    settings.mFriction = 0.6F;
    settings.mRestitution = 0.1F;
    settings.mOverrideMassProperties = EOverrideMassProperties::CalculateInertia;
    settings.mMassPropertiesOverride.mMass = std::max(1.0F, mass);
    settings.mUserData = userData;
    const BodyID body = impl_->physicsSystem.GetBodyInterface().CreateAndAddBody(settings, EActivation::Activate);
    if (body.IsInvalid()) {
        return -1;
    }
    impl_->simBodies.push_back(body);
    return static_cast<int>(impl_->simBodies.size() - 1);
}

int PhysicsWorld::AddConvexBody(
    const std::vector<glm::vec3>& points,
    const glm::mat4& transform,
    float mass,
    bool dynamic,
    std::uint64_t userData
) {
    if (!impl_->initialized || points.size() < 4) {
        return -1;
    }
    std::vector<Vec3> cloud;
    cloud.reserve(std::min<std::size_t>(points.size(), 96));
    const std::size_t stride = std::max<std::size_t>(1, points.size() / 96);
    for (std::size_t index = 0; index < points.size(); index += stride) {
        const glm::vec3& point = points[index];
        cloud.emplace_back(point.x, point.y, point.z);
    }
    ConvexHullShapeSettings settings(cloud.data(), static_cast<int>(cloud.size()), 0.001F);
    ShapeSettings::ShapeResult shapeResult = settings.Create();
    if (shapeResult.HasError()) {
        glm::vec3 minimum = points.front();
        glm::vec3 maximum = points.front();
        for (const glm::vec3& point : points) {
            minimum = glm::min(minimum, point);
            maximum = glm::max(maximum, point);
        }
        const glm::vec3 half = glm::max((maximum - minimum) * 0.5F, glm::vec3(0.02F));
        const glm::vec3 center = (minimum + maximum) * 0.5F;
        cloud.clear();
        for (int corner = 0; corner < 8; ++corner) {
            cloud.emplace_back(
                center.x + half.x * ((corner & 1) != 0 ? 1.0F : -1.0F),
                center.y + half.y * ((corner & 2) != 0 ? 1.0F : -1.0F),
                center.z + half.z * ((corner & 4) != 0 ? 1.0F : -1.0F)
            );
        }
        ConvexHullShapeSettings boxSettings(cloud.data(), static_cast<int>(cloud.size()), 0.001F);
        shapeResult = boxSettings.Create();
        if (shapeResult.HasError()) {
            return -1;
        }
    }
    const glm::vec3 modelOrigin(transform[3]);
    const glm::quat modelRotation = glm::quat_cast(transform);
    BodyCreationSettings bodySettings(
        shapeResult.Get(),
        RVec3(modelOrigin.x, modelOrigin.y, modelOrigin.z),
        Quat(modelRotation.x, modelRotation.y, modelRotation.z, modelRotation.w),
        dynamic ? EMotionType::Dynamic : EMotionType::Static,
        dynamic ? Layers::Moving : Layers::NonMoving
    );
    bodySettings.mUserData = userData;
    if (dynamic) {
        bodySettings.mMotionQuality = EMotionQuality::LinearCast;
        bodySettings.mFriction = 0.7F;
        bodySettings.mRestitution = 0.05F;
        bodySettings.mLinearDamping = 0.1F;
        bodySettings.mAngularDamping = 0.2F;
        bodySettings.mOverrideMassProperties = EOverrideMassProperties::CalculateInertia;
        bodySettings.mMassPropertiesOverride.mMass = std::max(1.0F, mass);
    }
    const BodyID body = impl_->physicsSystem.GetBodyInterface().CreateAndAddBody(
        bodySettings,
        dynamic ? EActivation::Activate : EActivation::DontActivate
    );
    if (body.IsInvalid()) {
        return -1;
    }
    impl_->simBodies.push_back(body);
    return static_cast<int>(impl_->simBodies.size() - 1);
}

int PhysicsWorld::AddDynamicCompound(
    const std::vector<std::vector<glm::vec3>>& convexes,
    const glm::mat4& transform,
    const PhysicsBodyDesc& desc,
    std::uint64_t userData
) {
    if (!impl_->initialized || convexes.empty()) {
        return -1;
    }
    StaticCompoundShapeSettings compound;
    for (const std::vector<glm::vec3>& convex : convexes) {
        if (convex.size() < 4) {
            continue;
        }
        std::vector<Vec3> cloud;
        cloud.reserve(std::min<std::size_t>(convex.size(), 128));
        for (std::size_t index = 0; index < convex.size() && cloud.size() < 128; ++index) {
            const glm::vec3& point = convex[index];
            if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z)) {
                continue;
            }
            cloud.emplace_back(point.x, point.y, point.z);
        }
        if (cloud.size() < 4) {
            continue;
        }
        Ref<ConvexHullShapeSettings> hull = new ConvexHullShapeSettings(cloud.data(), static_cast<int>(cloud.size()), 0.01F);
        if (hull->Create().HasError()) {
            continue;
        }
        compound.AddShape(Vec3::sZero(), Quat::sIdentity(), hull);
    }
    if (compound.mSubShapes.empty()) {
        return -1;
    }
    ShapeSettings::ShapeResult shapeResult = compound.mSubShapes.size() == 1 && compound.mSubShapes.front().mShape != nullptr
        ? compound.mSubShapes.front().mShape->Create()
        : compound.Create();
    if (shapeResult.HasError() || shapeResult.Get() == nullptr) {
        return -1;
    }
    const Shape* shape = shapeResult.Get();
    MassProperties massProperties = shape->GetMassProperties();
    massProperties.ScaleToMass(std::max(0.1F, desc.mass));
    const float inertiaScale = std::max(0.05F, desc.inertiaScale);
    massProperties.mInertia = massProperties.mInertia * inertiaScale;
    const glm::vec3 modelOrigin(transform[3]);
    const glm::quat modelRotation = glm::quat_cast(transform);
    const bool frozen = desc.motionDisabled;
    BodyCreationSettings bodySettings(
        shape,
        RVec3(modelOrigin.x, modelOrigin.y, modelOrigin.z),
        Quat(modelRotation.x, modelRotation.y, modelRotation.z, modelRotation.w),
        frozen ? EMotionType::Kinematic : EMotionType::Dynamic,
        Layers::Moving
    );
    bodySettings.mUserData = userData;
    bodySettings.mMotionQuality = EMotionQuality::LinearCast;
    bodySettings.mFriction = std::clamp(desc.friction, 0.0F, 1.0F);
    bodySettings.mRestitution = std::clamp(desc.restitution, 0.0F, 1.0F);
    bodySettings.mLinearDamping = std::max(0.0F, desc.linearDamping);
    bodySettings.mAngularDamping = std::max(0.0F, desc.angularDamping);
    bodySettings.mAllowSleeping = !frozen;
    bodySettings.mOverrideMassProperties = EOverrideMassProperties::MassAndInertiaProvided;
    bodySettings.mMassPropertiesOverride = massProperties;
    const BodyID body = impl_->physicsSystem.GetBodyInterface().CreateAndAddBody(
        bodySettings,
        frozen || desc.startAsleep ? EActivation::DontActivate : EActivation::Activate
    );
    if (body.IsInvalid()) {
        return -1;
    }
    impl_->simBodies.push_back(body);
    return static_cast<int>(impl_->simBodies.size() - 1);
}

int PhysicsWorld::AddStaticCompound(
    const std::vector<std::vector<glm::vec3>>& convexes,
    const glm::mat4& transform,
    std::uint64_t userData
) {
    if (!impl_->initialized || convexes.empty()) {
        return -1;
    }
    StaticCompoundShapeSettings compound;
    for (const std::vector<glm::vec3>& convex : convexes) {
        if (convex.size() < 4) {
            continue;
        }
        std::vector<Vec3> cloud;
        cloud.reserve(std::min<std::size_t>(convex.size(), 128));
        for (std::size_t index = 0; index < convex.size() && cloud.size() < 128; ++index) {
            const glm::vec3& point = convex[index];
            if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z)) {
                continue;
            }
            cloud.emplace_back(point.x, point.y, point.z);
        }
        if (cloud.size() < 4) {
            continue;
        }
        Ref<ConvexHullShapeSettings> hull = new ConvexHullShapeSettings(cloud.data(), static_cast<int>(cloud.size()), 0.01F);
        if (hull->Create().HasError()) {
            continue;
        }
        compound.AddShape(Vec3::sZero(), Quat::sIdentity(), hull);
    }
    if (compound.mSubShapes.empty()) {
        return -1;
    }
    ShapeSettings::ShapeResult shapeResult = compound.mSubShapes.size() == 1 && compound.mSubShapes.front().mShape != nullptr
        ? compound.mSubShapes.front().mShape->Create()
        : compound.Create();
    if (shapeResult.HasError() || shapeResult.Get() == nullptr) {
        return -1;
    }
    const glm::vec3 modelOrigin(transform[3]);
    const glm::quat modelRotation = glm::quat_cast(transform);
    BodyCreationSettings bodySettings(
        shapeResult.Get(),
        RVec3(modelOrigin.x, modelOrigin.y, modelOrigin.z),
        Quat(modelRotation.x, modelRotation.y, modelRotation.z, modelRotation.w),
        EMotionType::Static,
        Layers::NonMoving
    );
    bodySettings.mUserData = userData;
    const BodyID body = impl_->physicsSystem.GetBodyInterface().CreateAndAddBody(
        bodySettings,
        EActivation::DontActivate
    );
    if (body.IsInvalid()) {
        return -1;
    }
    impl_->simBodies.push_back(body);
    return static_cast<int>(impl_->simBodies.size() - 1);
}

int PhysicsWorld::AddMeshBody(
    const std::vector<glm::vec3>& vertices,
    const std::vector<std::uint32_t>& indices,
    const glm::mat4& transform,
    bool kinematic,
    std::uint64_t userData
) {
    if (!impl_->initialized || indices.size() < 3) {
        return -1;
    }
    VertexList meshVertices;
    IndexedTriangleList meshTriangles;
    for (std::size_t index = 0; index + 2 < indices.size(); index += 3) {
        if (indices[index] >= vertices.size() || indices[index + 1] >= vertices.size()
            || indices[index + 2] >= vertices.size()) {
            continue;
        }
        const glm::vec3 triangle[3] = {
            vertices[indices[index]],
            vertices[indices[index + 1]],
            vertices[indices[index + 2]],
        };
        const std::uint32_t base = static_cast<std::uint32_t>(meshVertices.size());
        for (const glm::vec3& vertex : triangle) {
            meshVertices.emplace_back(vertex.x, vertex.y, vertex.z);
        }
        meshTriangles.emplace_back(base, base + 2, base + 1);
    }
    if (meshTriangles.empty()) {
        return -1;
    }
    MeshShapeSettings shapeSettings;
    shapeSettings.mTriangleVertices = std::move(meshVertices);
    shapeSettings.mIndexedTriangles = std::move(meshTriangles);
    const ShapeSettings::ShapeResult shapeResult = shapeSettings.Create();
    if (shapeResult.HasError()) {
        return -1;
    }
    const glm::vec3 translation(transform[3]);
    const glm::quat rotation = glm::quat_cast(transform);
    BodyCreationSettings settings(
        shapeResult.Get(),
        RVec3(translation.x, translation.y, translation.z),
        Quat(rotation.x, rotation.y, rotation.z, rotation.w),
        kinematic ? EMotionType::Kinematic : EMotionType::Static,
        kinematic ? Layers::Moving : Layers::NonMoving
    );
    settings.mUserData = userData;
    const BodyID body = impl_->physicsSystem.GetBodyInterface().CreateAndAddBody(
        settings,
        kinematic ? EActivation::Activate : EActivation::DontActivate
    );
    if (body.IsInvalid()) {
        return -1;
    }
    impl_->simBodies.push_back(body);
    return static_cast<int>(impl_->simBodies.size() - 1);
}

void PhysicsWorld::SetBodyTransform(int body, const glm::mat4& transform) {
    if (body < 0 || static_cast<std::size_t>(body) >= impl_->simBodies.size()) {
        return;
    }
    const glm::vec3 translation(transform[3]);
    const glm::quat rotation = glm::quat_cast(transform);
    impl_->physicsSystem.GetBodyInterface().SetPositionAndRotation(
        impl_->simBodies[static_cast<std::size_t>(body)],
        RVec3(translation.x, translation.y, translation.z),
        Quat(rotation.x, rotation.y, rotation.z, rotation.w),
        EActivation::DontActivate
    );
}

void PhysicsWorld::SetBodyVelocity(int body, const glm::vec3& linear) {
    if (body < 0 || static_cast<std::size_t>(body) >= impl_->simBodies.size()) {
        return;
    }
    const BodyID id = impl_->simBodies[static_cast<std::size_t>(body)];
    if (id.IsInvalid()) {
        return;
    }
    BodyInterface& bodyInterface = impl_->physicsSystem.GetBodyInterface();
    bodyInterface.SetLinearVelocity(id, Vec3(linear.x, linear.y, linear.z));
    bodyInterface.ActivateBody(id);
}

void PhysicsWorld::SetBodyAngularVelocity(int body, const glm::vec3& angular) {
    if (body < 0 || static_cast<std::size_t>(body) >= impl_->simBodies.size()) {
        return;
    }
    const BodyID id = impl_->simBodies[static_cast<std::size_t>(body)];
    if (id.IsInvalid()) {
        return;
    }
    impl_->physicsSystem.GetBodyInterface().SetAngularVelocity(id, Vec3(angular.x, angular.y, angular.z));
}

void PhysicsWorld::ApplyBodyImpulse(int body, const glm::vec3& impulse) {
    if (body < 0 || static_cast<std::size_t>(body) >= impl_->simBodies.size()) {
        return;
    }
    const BodyID id = impl_->simBodies[static_cast<std::size_t>(body)];
    if (id.IsInvalid()) {
        return;
    }
    impl_->physicsSystem.GetBodyInterface().AddImpulse(id, Vec3(impulse.x, impulse.y, impulse.z));
}

void PhysicsWorld::AddBodyForce(int body, const glm::vec3& force) {
    if (body < 0 || static_cast<std::size_t>(body) >= impl_->simBodies.size()) {
        return;
    }
    const BodyID id = impl_->simBodies[static_cast<std::size_t>(body)];
    if (id.IsInvalid()) {
        return;
    }
    BodyInterface& bodyInterface = impl_->physicsSystem.GetBodyInterface();
    bodyInterface.ActivateBody(id);
    bodyInterface.AddForce(id, Vec3(force.x, force.y, force.z));
}

void PhysicsWorld::SetBodyGravityScale(int body, float scale) {
    if (body < 0 || static_cast<std::size_t>(body) >= impl_->simBodies.size()) {
        return;
    }
    const BodyID id = impl_->simBodies[static_cast<std::size_t>(body)];
    if (id.IsInvalid()) {
        return;
    }
    impl_->physicsSystem.GetBodyInterface().SetGravityFactor(id, scale);
}

void PhysicsWorld::SetBodyMotionEnabled(int body, bool enabled) {
    if (body < 0 || static_cast<std::size_t>(body) >= impl_->simBodies.size()) {
        return;
    }
    const BodyID id = impl_->simBodies[static_cast<std::size_t>(body)];
    if (id.IsInvalid()) {
        return;
    }
    BodyInterface& bodyInterface = impl_->physicsSystem.GetBodyInterface();
    if (enabled) {
        bodyInterface.SetMotionType(id, EMotionType::Dynamic, EActivation::Activate);
        bodyInterface.SetObjectLayer(id, Layers::Moving);
        return;
    }
    bodyInterface.DeactivateBody(id);
    bodyInterface.SetMotionType(id, EMotionType::Kinematic, EActivation::DontActivate);
}

void PhysicsWorld::DestroySimBody(int body) {
    if (body < 0 || static_cast<std::size_t>(body) >= impl_->simBodies.size()) {
        return;
    }
    BodyID& id = impl_->simBodies[static_cast<std::size_t>(body)];
    if (id.IsInvalid()) {
        return;
    }
    BodyInterface& bodyInterface = impl_->physicsSystem.GetBodyInterface();
    bodyInterface.RemoveBody(id);
    bodyInterface.DestroyBody(id);
    id = BodyID();
}

glm::quat PhysicsWorld::BodyRotation(int body) const {
    if (body < 0 || static_cast<std::size_t>(body) >= impl_->simBodies.size()) {
        return glm::quat(1.0F, 0.0F, 0.0F, 0.0F);
    }
    const BodyID id = impl_->simBodies[static_cast<std::size_t>(body)];
    if (id.IsInvalid()) {
        return glm::quat(1.0F, 0.0F, 0.0F, 0.0F);
    }
    const Quat rotation = impl_->physicsSystem.GetBodyInterface().GetRotation(id);
    return glm::quat(rotation.GetW(), rotation.GetX(), rotation.GetY(), rotation.GetZ());
}

glm::vec3 PhysicsWorld::BodyLinearVelocity(int body) const {
    if (body < 0 || static_cast<std::size_t>(body) >= impl_->simBodies.size()) {
        return glm::vec3(0.0F);
    }
    const BodyID id = impl_->simBodies[static_cast<std::size_t>(body)];
    if (id.IsInvalid()) {
        return glm::vec3(0.0F);
    }
    const Vec3 velocity = impl_->physicsSystem.GetBodyInterface().GetLinearVelocity(id);
    return glm::vec3(velocity.GetX(), velocity.GetY(), velocity.GetZ());
}

glm::vec3 PhysicsWorld::BodyAngularVelocity(int body) const {
    if (body < 0 || static_cast<std::size_t>(body) >= impl_->simBodies.size()) {
        return glm::vec3(0.0F);
    }
    const BodyID id = impl_->simBodies[static_cast<std::size_t>(body)];
    if (id.IsInvalid()) {
        return glm::vec3(0.0F);
    }
    const Vec3 velocity = impl_->physicsSystem.GetBodyInterface().GetAngularVelocity(id);
    return glm::vec3(velocity.GetX(), velocity.GetY(), velocity.GetZ());
}

std::vector<PhysicsImpact> PhysicsWorld::ConsumeImpacts() {
    if (!impl_->simContacts) {
        return {};
    }
    return impl_->simContacts->Consume();
}

glm::vec3 PhysicsWorld::BodyPosition(int body) const {
    if (body < 0 || static_cast<std::size_t>(body) >= impl_->simBodies.size()) {
        return glm::vec3(0.0F);
    }
    const BodyID id = impl_->simBodies[static_cast<std::size_t>(body)];
    if (id.IsInvalid()) {
        return glm::vec3(0.0F);
    }
    const RVec3 position = impl_->physicsSystem.GetBodyInterface().GetPosition(id);
    return glm::vec3(
        static_cast<float>(position.GetX()),
        static_cast<float>(position.GetY()),
        static_cast<float>(position.GetZ())
    );
}

glm::mat4 PhysicsWorld::BodyDrawMatrix(int body) const {
    if (body < 0 || static_cast<std::size_t>(body) >= impl_->simBodies.size()) {
        return glm::mat4(1.0F);
    }
    const BodyInterface& bodyInterface = impl_->physicsSystem.GetBodyInterface();
    const BodyID id = impl_->simBodies[static_cast<std::size_t>(body)];
    if (id.IsInvalid()) {
        return glm::mat4(1.0F);
    }
    const RVec3 position = bodyInterface.GetPosition(id);
    const Quat rotation = bodyInterface.GetRotation(id);
    const glm::quat orientation(rotation.GetW(), rotation.GetX(), rotation.GetY(), rotation.GetZ());
    const glm::vec3 origin(
        static_cast<float>(position.GetX()),
        static_cast<float>(position.GetY()),
        static_cast<float>(position.GetZ())
    );
    return glm::translate(glm::mat4(1.0F), origin) * glm::mat4_cast(orientation);
}

PhysicsRayHit PhysicsWorld::RayCast(
    const glm::vec3& origin,
    const glm::vec3& direction,
    float distance,
    std::uint64_t ignoreUserData
) const {
    PhysicsRayHit hit;
    if (!impl_->initialized || distance <= 0.0F) {
        return hit;
    }
    const glm::vec3 normalized = glm::length(direction) > 0.0F ? glm::normalize(direction) : glm::vec3(0.0F, 0.0F, -1.0F);
    const RRayCast ray(
        RVec3(origin.x, origin.y, origin.z),
        Vec3(normalized.x, normalized.y, normalized.z) * distance
    );
    RayCastResult result;
    SkipUserDataFilter filter(kPlayerUserData, ignoreUserData);
    if (!impl_->physicsSystem.GetNarrowPhaseQuery().CastRay(ray, result, {}, {}, filter)) {
        return hit;
    }
    hit.hit = true;
    hit.distance = result.mFraction * distance;
    BodyLockRead lock(impl_->physicsSystem.GetBodyLockInterface(), result.mBodyID);
    if (lock.Succeeded()) {
        hit.userData = lock.GetBody().GetUserData();
        const Vec3 normal = lock.GetBody().GetWorldSpaceSurfaceNormal(result.mSubShapeID2, ray.GetPointOnRay(result.mFraction));
        hit.normal = glm::vec3(normal.GetX(), normal.GetY(), normal.GetZ());
    }
    return hit;
}

std::vector<PhysicsRayHit> PhysicsWorld::RayCastAll(
    const glm::vec3& origin,
    const glm::vec3& direction,
    float distance,
    std::uint64_t ignoreUserData
) const {
    std::vector<PhysicsRayHit> hits;
    if (!impl_->initialized || distance <= 0.0F) {
        return hits;
    }
    const glm::vec3 normalized = glm::length(direction) > 0.0F ? glm::normalize(direction) : glm::vec3(0.0F, 0.0F, -1.0F);
    const RRayCast ray(
        RVec3(origin.x, origin.y, origin.z),
        Vec3(normalized.x, normalized.y, normalized.z) * distance
    );
    AllRayHits collector;
    SkipUserDataFilter filter(kPlayerUserData, ignoreUserData);
    RayCastSettings settings;
    impl_->physicsSystem.GetNarrowPhaseQuery().CastRay(ray, settings, collector, {}, {}, filter, {});
    hits.reserve(collector.hits.size());
    for (const RayCastResult& result : collector.hits) {
        PhysicsRayHit hit;
        hit.hit = true;
        hit.distance = result.mFraction * distance;
        BodyLockRead lock(impl_->physicsSystem.GetBodyLockInterface(), result.mBodyID);
        if (!lock.Succeeded()) {
            continue;
        }
        hit.userData = lock.GetBody().GetUserData();
        const Vec3 normal = lock.GetBody().GetWorldSpaceSurfaceNormal(result.mSubShapeID2, ray.GetPointOnRay(result.mFraction));
        hit.normal = glm::vec3(normal.GetX(), normal.GetY(), normal.GetZ());
        hits.push_back(hit);
    }
    return hits;
}

namespace {

using namespace JPH;

void AppendBoxTriangles(
    const Vec3& minimum,
    const Vec3& maximum,
    std::vector<glm::vec3>& vertices,
    std::vector<std::uint32_t>& indices
) {
    const std::uint32_t base = static_cast<std::uint32_t>(vertices.size());
    const Vec3 corners[8] = {
        Vec3(minimum.GetX(), minimum.GetY(), minimum.GetZ()),
        Vec3(maximum.GetX(), minimum.GetY(), minimum.GetZ()),
        Vec3(maximum.GetX(), maximum.GetY(), minimum.GetZ()),
        Vec3(minimum.GetX(), maximum.GetY(), minimum.GetZ()),
        Vec3(minimum.GetX(), minimum.GetY(), maximum.GetZ()),
        Vec3(maximum.GetX(), minimum.GetY(), maximum.GetZ()),
        Vec3(maximum.GetX(), maximum.GetY(), maximum.GetZ()),
        Vec3(minimum.GetX(), maximum.GetY(), maximum.GetZ()),
    };
    for (const Vec3& corner : corners) {
        vertices.emplace_back(corner.GetX(), corner.GetY(), corner.GetZ());
    }
    const std::uint32_t boxIndices[] = {
        0, 1, 2, 0, 2, 3,
        4, 6, 5, 4, 7, 6,
        0, 4, 5, 0, 5, 1,
        1, 5, 6, 1, 6, 2,
        2, 6, 7, 2, 7, 3,
        3, 7, 4, 3, 4, 0,
    };
    for (const std::uint32_t index : boxIndices) {
        indices.push_back(base + index);
    }
}

void AppendLeafTriangles(
    const Shape* shape,
    const Mat44& toParent,
    std::vector<glm::vec3>& vertices,
    std::vector<std::uint32_t>& indices
) {
    if (shape == nullptr) {
        return;
    }
    if (shape->GetType() == EShapeType::Compound) {
        class LeafCollector final : public TransformedShapeCollector {
        public:
            void AddHit(const TransformedShape& hit) override {
                hits.push_back(hit);
            }

            std::vector<TransformedShape> hits;
        };

        LeafCollector collector;
        const AABox bounds(Vec3(-1.0e6F, -1.0e6F, -1.0e6F), Vec3(1.0e6F, 1.0e6F, 1.0e6F));
        shape->CollectTransformedShapes(
            bounds,
            Vec3::sZero(),
            Quat::sIdentity(),
            Vec3::sReplicate(1.0F),
            SubShapeIDCreator(),
            collector,
            {}
        );
        for (const TransformedShape& hit : collector.hits) {
            const Shape* leaf = hit.mShape.GetPtr();
            if (leaf == nullptr || leaf == shape) {
                continue;
            }
            const Vec3 scale = hit.GetShapeScale();
            const Mat44 leafToParent = Mat44::sRotationTranslation(hit.mShapeRotation, Vec3(hit.mShapePositionCOM))
                * Mat44::sScale(scale);
            AppendLeafTriangles(leaf, toParent * leafToParent, vertices, indices);
        }
        return;
    }

    Shape::GetTrianglesContext context{};
    const AABox bounds(Vec3(-1.0e6F, -1.0e6F, -1.0e6F), Vec3(1.0e6F, 1.0e6F, 1.0e6F));
    shape->GetTrianglesStart(context, bounds, Vec3::sZero(), Quat::sIdentity(), Vec3::sReplicate(1.0F));
    constexpr int batch = 1024;
    std::vector<Float3> triangleVertices(static_cast<std::size_t>(batch) * 3);
    for (;;) {
        const int count = shape->GetTrianglesNext(context, batch, triangleVertices.data());
        if (count <= 0) {
            break;
        }
        for (int triangle = 0; triangle < count; ++triangle) {
            const std::uint32_t base = static_cast<std::uint32_t>(vertices.size());
            for (int vertex = 0; vertex < 3; ++vertex) {
                const Float3& point = triangleVertices[static_cast<std::size_t>(triangle * 3 + vertex)];
                const Vec3 transformed = toParent * Vec3(point.x, point.y, point.z);
                vertices.emplace_back(transformed.GetX(), transformed.GetY(), transformed.GetZ());
            }
            indices.push_back(base);
            indices.push_back(base + 1);
            indices.push_back(base + 2);
        }
    }
}

void BakeShapeTriangles(const Shape* shape, std::vector<glm::vec3>& vertices, std::vector<std::uint32_t>& indices) {
    vertices.clear();
    indices.clear();
    if (shape == nullptr) {
        return;
    }
    AppendLeafTriangles(shape, Mat44::sIdentity(), vertices, indices);
    if (indices.empty()) {
        const AABox bounds = shape->GetLocalBounds();
        AppendBoxTriangles(bounds.mMin, bounds.mMax, vertices, indices);
    }
}

glm::mat4 CenterOfMassMatrix(const Body& body) {
    // Baked hull triangles are relative to the center of mass, not the model origin.
    const Quat rotation = body.GetRotation();
    const RVec3 position = body.GetCenterOfMassPosition();
    const glm::quat orientation(rotation.GetW(), rotation.GetX(), rotation.GetY(), rotation.GetZ());
    const glm::vec3 origin(
        static_cast<float>(position.GetX()),
        static_cast<float>(position.GetY()),
        static_cast<float>(position.GetZ())
    );
    return glm::translate(glm::mat4(1.0F), origin) * glm::mat4_cast(orientation);
}

}

void PhysicsWorld::CollectDebugShapes(std::vector<PhysicsDebugShape>& out) const {
    out.clear();
    if (!impl_->initialized) {
        return;
    }
    BodyIDVector bodyIds;
    impl_->physicsSystem.GetBodies(bodyIds);
    std::unordered_set<std::uint64_t> live;
    live.reserve(bodyIds.size());
    for (const BodyID id : bodyIds) {
        live.insert(id.GetIndexAndSequenceNumber());
    }
    for (auto it = impl_->debugBakes.begin(); it != impl_->debugBakes.end();) {
        if (!live.contains(it->first)) {
            it = impl_->debugBakes.erase(it);
        } else {
            ++it;
        }
    }

    out.reserve(bodyIds.size());
    for (const BodyID id : bodyIds) {
        BodyLockRead lock(impl_->physicsSystem.GetBodyLockInterface(), id);
        if (!lock.Succeeded()) {
            continue;
        }
        const Body& body = lock.GetBody();
        const Shape* shape = body.GetShape();
        if (shape == nullptr) {
            continue;
        }
        const std::uint64_t key = id.GetIndexAndSequenceNumber();
        std::unique_ptr<Impl::DebugBake>& slot = impl_->debugBakes[key];
        if (slot == nullptr) {
            slot = std::make_unique<Impl::DebugBake>();
        }
        if (slot->shape != shape) {
            slot->shape = shape;
            ++slot->revision;
            BakeShapeTriangles(shape, slot->vertices, slot->indices);
        }
        if (slot->indices.size() < 3) {
            continue;
        }
        PhysicsDebugShape debugShape;
        debugShape.key = key;
        debugShape.revision = slot->revision;
        debugShape.transform = CenterOfMassMatrix(body);
        debugShape.vertices = &slot->vertices;
        debugShape.indices = &slot->indices;
        out.push_back(debugShape);
    }
}

void PhysicsWorld::SetDucked(bool ducked) {
    if (!impl_->character || impl_->ducked == ducked || impl_->standingShape == nullptr || impl_->duckedShape == nullptr) {
        impl_->ducked = ducked;
        return;
    }
    impl_->ducked = ducked;
    MovingBroadPhaseFilter broadPhaseFilter;
    MovingObjectLayerFilter objectLayerFilter;
    BodyFilter bodyFilter;
    ShapeFilter shapeFilter;
    impl_->character->SetShape(
        ducked ? impl_->duckedShape.GetPtr() : impl_->standingShape.GetPtr(),
        0.2F,
        broadPhaseFilter,
        objectLayerFilter,
        bodyFilter,
        shapeFilter,
        impl_->tempAllocator
    );
}
