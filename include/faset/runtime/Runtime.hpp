#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace faset::runtime {

using Vec2 = std::array<float, 2>;
using Vec3 = std::array<float, 3>;
using Vec4 = std::array<float, 4>;

struct Transform {
    Vec3 position{0, 0, 0};
    Vec3 rotation{0, 0, 0}; // Euler XYZ, radians.
    Vec3 scale{1, 1, 1};
};

// Process-local identity. Never serialize this into an authoring scene.
struct EntityHandle {
    std::uint64_t session{};
    std::uint32_t slot{};
    std::uint64_t generation{};
    explicit operator bool() const noexcept { return session != 0; }
    bool operator==(const EntityHandle&) const = default;
};

struct InputState {
    float horizontal{};
    float vertical{};
    bool jumpPressed{};
    bool interactPressed{};
};

struct Sprite { Vec4 color{1, 1, 1, 1}; Vec2 size{1, 1}; std::string texture; int layer{}; };
struct Mesh { std::string asset; Vec4 color{1, 1, 1, 1}; std::string primitive{"cube"}; };
struct RenderEntity {
    std::string id;
    std::string name;
    std::optional<std::string> parent;
    Transform transform; // Presentation-space local transform; compose parent for rendering.
    std::optional<Sprite> sprite;
    std::optional<Mesh> mesh;
};
struct RuntimeSnapshot {
    int dimension{3};
    std::uint64_t tick{};
    double alpha{};
    std::vector<RenderEntity> entities;
};
struct FrameStats {
    unsigned fixedTicks{};
    double droppedTime{};
    double interpolationAlpha{};
    std::uint64_t tick{};
};
struct RuntimeConfig {
    double fixedDelta{1.0 / 60.0};
    unsigned maxCatchUpTicks{4};
    int physicsSubsteps{4};
    Vec3 gravity{0, -9.81f, 0};
};

class Runtime;
struct CollisionEvent;
struct Behavior {
    using Callback = std::function<void(Runtime&, EntityHandle, double)>;
    Callback onStart;
    Callback fixedUpdate;
    Callback update;
    Callback lateUpdate;
    Callback onDestroy;
    std::function<void(Runtime&, EntityHandle, const CollisionEvent&)> onCollision;
};
struct CollisionEvent {
    EntityHandle first;
    EntityHandle second;
    bool began{};
};

// Single-owner sequential runtime. Gameplay callbacks run on the caller's thread.
// No Editor, MCP, renderer or platform service is linked by this API.
class Runtime {
public:
    explicit Runtime(RuntimeConfig config = {});
    ~Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    Runtime(Runtime&&) = delete;
    Runtime& operator=(Runtime&&) = delete;

    void registerBehavior(std::string componentType, Behavior behavior);
    // Validates and prepares a replacement world before destroying the old world.
    // Throws a validation/JSON exception on unsupported or invalid scene data.
    void load(const nlohmann::json& scene);
    void clear();
    FrameStats advance(double elapsedSeconds, InputState input = {});
    FrameStats singleStep(InputState input = {});
    void setPaused(bool paused);
    bool paused() const noexcept;

    EntityHandle find(const std::string& persistentId) const;
    bool valid(EntityHandle handle) const noexcept;
    Transform transform(EntityHandle handle) const;
    Transform presentation(EntityHandle handle) const;
    // Configuration copy. Live poses and velocities have their own typed accessors.
    nlohmann::json fields(EntityHandle handle, const std::string& componentType) const;
    Vec3 velocity(EntityHandle handle) const;
    InputState input() const noexcept;
    // Valid until the next fixed tick or scene replacement. No native solver pointers.
    const std::vector<CollisionEvent>& collisions() const noexcept;

    // Non-physical transforms may be changed in Update/FixedUpdate. Physics poses
    // are owned by the solver and require explicit teleport/velocity operations.
    void setTransform(EntityHandle handle, const Transform& transform);
    void setPresentation(EntityHandle handle, const Transform& transform);
    void teleport(EntityHandle handle, const Transform& transform);
    void setVelocity(EntityHandle handle, Vec3 velocity);
    void applyImpulse(EntityHandle handle, Vec3 impulse);

    // All structural changes are applied in FIFO order at the NEXT fixed tick.
    // Returned handles, component copies and presentation snapshots are not pointers.
    void spawn(nlohmann::json entity);
    void destroy(EntityHandle handle);
    void addComponent(EntityHandle handle, nlohmann::json component);
    void removeComponent(EntityHandle handle, const std::string& componentType);

    RuntimeSnapshot snapshot() const;
    nlohmann::json snapshotJson() const;
    std::uint64_t session() const noexcept;
    const std::vector<std::string>& diagnostics() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace faset::runtime
