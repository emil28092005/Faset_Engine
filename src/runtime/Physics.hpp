#pragma once
#include <faset/runtime/Runtime.hpp>
#include <memory>

namespace faset::runtime::detail {
struct BodySettings {
    std::string type{"dynamic"};
    Vec3 halfExtents{0.5f, 0.5f, 0.5f};
    Vec3 linearVelocity{};
    float density{1};
    float friction{0.3f};
    float restitution{};
    float gravityScale{1};
    std::uint64_t categoryBits{1};
    std::uint64_t maskBits{~std::uint64_t{0}};
};
struct Contact { std::uint32_t first; std::uint32_t second; bool began; };
class Physics {
public:
    Physics(int dimension, Vec3 gravity, int substeps);
    ~Physics();
    void add(std::uint32_t id, const Transform&, const BodySettings&);
    void remove(std::uint32_t id);
    bool contains(std::uint32_t id) const;
    bool dynamic(std::uint32_t id) const;
    Transform transform(std::uint32_t id, Transform previous) const;
    Vec3 velocity(std::uint32_t id) const;
    void teleport(std::uint32_t id, const Transform&);
    void setVelocity(std::uint32_t id, Vec3);
    void impulse(std::uint32_t id, Vec3);
    std::vector<Contact> step(float delta);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
