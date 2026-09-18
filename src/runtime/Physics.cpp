#include "Physics.hpp"
#include <box2d/box2d.h>
#include <box3d/box3d.h>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_map>

namespace faset::runtime::detail {
namespace {
b3Quat quaternion(Vec3 e) {
    auto x = b3MakeQuatFromAxisAngle({1, 0, 0}, e[0]);
    auto y = b3MakeQuatFromAxisAngle({0, 1, 0}, e[1]);
    auto z = b3MakeQuatFromAxisAngle({0, 0, 1}, e[2]);
    return b3MulQuat(z, b3MulQuat(y, x));
}
Vec3 euler(b3Quat q) {
    const float x=q.v.x, y=q.v.y, z=q.v.z, w=q.s;
    return {std::atan2(2*(w*x+y*z), 1-2*(x*x+y*y)),
            std::asin(std::clamp(2*(w*y-z*x), -1.0f, 1.0f)),
            std::atan2(2*(w*z+x*y), 1-2*(y*y+z*z))};
}
}
struct Physics::Impl {
    struct Body { b2BodyId two{}; b3BodyId three{}; std::uint64_t shape{}; bool dynamic{}; };
    int dimension;
    int substeps;
    b2WorldId world2{};
    b3WorldId world3{};
    std::unordered_map<std::uint32_t, Body> bodies;
    std::unordered_map<std::uint64_t, std::uint32_t> shapes;
    Impl(int dim, Vec3 gravity, int count):dimension(dim),substeps(count) {
        if(dim==2) { auto def=b2DefaultWorldDef(); def.gravity={gravity[0],gravity[1]}; world2=b2CreateWorld(&def); }
        else { auto def=b3DefaultWorldDef(); def.gravity={gravity[0],gravity[1],gravity[2]}; world3=b3CreateWorld(&def); }
    }
    ~Impl() { if(dimension==2) b2DestroyWorld(world2); else b3DestroyWorld(world3); }
};
Physics::Physics(int dimension, Vec3 gravity, int substeps):impl_(std::make_unique<Impl>(dimension,gravity,substeps)){}
Physics::~Physics()=default;
void Physics::add(std::uint32_t id, const Transform& t, const BodySettings& settings) {
    if(contains(id)) throw std::logic_error("physics body already exists");
    Impl::Body body{}; body.dynamic=settings.type=="dynamic";
    if(impl_->dimension==2) {
        auto def=b2DefaultBodyDef();
        def.type=settings.type=="static"?b2_staticBody:settings.type=="kinematic"?b2_kinematicBody:b2_dynamicBody;
        def.position={t.position[0],t.position[1]}; def.rotation=b2MakeRot(t.rotation[2]);
        def.linearVelocity={settings.linearVelocity[0],settings.linearVelocity[1]}; def.gravityScale=settings.gravityScale;
        body.two=b2CreateBody(impl_->world2,&def);
        auto shape=b2DefaultShapeDef(); shape.density=settings.density; shape.material.friction=settings.friction;
        shape.material.restitution=settings.restitution; shape.enableContactEvents=true;
        shape.filter.categoryBits=settings.categoryBits; shape.filter.maskBits=settings.maskBits;
        const auto box=b2MakeBox(settings.halfExtents[0]*std::abs(t.scale[0]),settings.halfExtents[1]*std::abs(t.scale[1]));
        body.shape=b2StoreShapeId(b2CreatePolygonShape(body.two,&shape,&box));
    } else {
        auto def=b3DefaultBodyDef();
        def.type=settings.type=="static"?b3_staticBody:settings.type=="kinematic"?b3_kinematicBody:b3_dynamicBody;
        def.position={t.position[0],t.position[1],t.position[2]}; def.rotation=quaternion(t.rotation);
        def.linearVelocity={settings.linearVelocity[0],settings.linearVelocity[1],settings.linearVelocity[2]}; def.gravityScale=settings.gravityScale;
        body.three=b3CreateBody(impl_->world3,&def);
        auto shape=b3DefaultShapeDef(); shape.density=settings.density; shape.baseMaterial.friction=settings.friction;
        shape.baseMaterial.restitution=settings.restitution; shape.enableContactEvents=true;
        shape.filter.categoryBits=settings.categoryBits; shape.filter.maskBits=settings.maskBits;
        auto box=b3MakeBoxHull(settings.halfExtents[0]*std::abs(t.scale[0]),settings.halfExtents[1]*std::abs(t.scale[1]),settings.halfExtents[2]*std::abs(t.scale[2]));
        body.shape=b3StoreShapeId(b3CreateHullShape(body.three,&shape,&box.base));
    }
    impl_->shapes.emplace(body.shape,id); impl_->bodies.emplace(id,body);
}
void Physics::remove(std::uint32_t id) {
    const auto it=impl_->bodies.find(id); if(it==impl_->bodies.end()) return;
    impl_->shapes.erase(it->second.shape);
    if(impl_->dimension==2) b2DestroyBody(it->second.two); else b3DestroyBody(it->second.three);
    impl_->bodies.erase(it);
}
bool Physics::contains(std::uint32_t id) const { return impl_->bodies.contains(id); }
bool Physics::dynamic(std::uint32_t id) const { return impl_->bodies.at(id).dynamic; }
Transform Physics::transform(std::uint32_t id, Transform t) const {
    const auto& body=impl_->bodies.at(id);
    if(impl_->dimension==2) { auto p=b2Body_GetPosition(body.two); t.position[0]=p.x;t.position[1]=p.y;t.rotation[2]=b2Rot_GetAngle(b2Body_GetRotation(body.two)); }
    else { auto p=b3Body_GetPosition(body.three);t.position={float(p.x),float(p.y),float(p.z)};t.rotation=euler(b3Body_GetRotation(body.three)); }
    return t;
}
Vec3 Physics::velocity(std::uint32_t id) const {
    const auto& body=impl_->bodies.at(id);
    if(impl_->dimension==2) { auto v=b2Body_GetLinearVelocity(body.two);return {v.x,v.y,0}; }
    auto v=b3Body_GetLinearVelocity(body.three);return {v.x,v.y,v.z};
}
void Physics::teleport(std::uint32_t id, const Transform& t) {
    const auto& body=impl_->bodies.at(id);
    if(impl_->dimension==2) b2Body_SetTransform(body.two,{t.position[0],t.position[1]},b2MakeRot(t.rotation[2]));
    else b3Body_SetTransform(body.three,{t.position[0],t.position[1],t.position[2]},quaternion(t.rotation));
}
void Physics::setVelocity(std::uint32_t id, Vec3 v) {
    const auto& body=impl_->bodies.at(id);
    if(impl_->dimension==2) b2Body_SetLinearVelocity(body.two,{v[0],v[1]}); else b3Body_SetLinearVelocity(body.three,{v[0],v[1],v[2]});
}
void Physics::impulse(std::uint32_t id, Vec3 v) {
    const auto& body=impl_->bodies.at(id);
    if(impl_->dimension==2) b2Body_ApplyLinearImpulseToCenter(body.two,{v[0],v[1]},true); else b3Body_ApplyLinearImpulseToCenter(body.three,{v[0],v[1],v[2]},true);
}
std::vector<Contact> Physics::step(float delta) {
    std::vector<Contact> contacts;
    auto append=[&](std::uint64_t a,std::uint64_t b,bool began) {
        auto first=impl_->shapes.find(a),second=impl_->shapes.find(b);
        if(first!=impl_->shapes.end() && second!=impl_->shapes.end()) contacts.push_back({first->second,second->second,began});
    };
    if(impl_->dimension==2) {
        b2World_Step(impl_->world2,delta,impl_->substeps);auto events=b2World_GetContactEvents(impl_->world2);
        for(int i=0;i<events.beginCount;++i) append(b2StoreShapeId(events.beginEvents[i].shapeIdA),b2StoreShapeId(events.beginEvents[i].shapeIdB),true);
        for(int i=0;i<events.endCount;++i) append(b2StoreShapeId(events.endEvents[i].shapeIdA),b2StoreShapeId(events.endEvents[i].shapeIdB),false);
    } else {
        b3World_Step(impl_->world3,delta,impl_->substeps);auto events=b3World_GetContactEvents(impl_->world3);
        for(int i=0;i<events.beginCount;++i) append(b3StoreShapeId(events.beginEvents[i].shapeIdA),b3StoreShapeId(events.beginEvents[i].shapeIdB),true);
        for(int i=0;i<events.endCount;++i) append(b3StoreShapeId(events.endEvents[i].shapeIdA),b3StoreShapeId(events.endEvents[i].shapeIdB),false);
    }
    return contacts;
}
}
