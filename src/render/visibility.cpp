#include <faset/render/visibility.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace faset::render {
namespace {
void validate(const Bounds& bounds) {
    for (int axis = 0; axis < 3; ++axis)
        if (!std::isfinite(bounds.min[axis]) || !std::isfinite(bounds.max[axis]) ||
            bounds.min[axis] > bounds.max[axis])
            throw std::invalid_argument("Invalid mesh bounds");
}
} // namespace

Bounds local_bounds(const Mesh& mesh) {
    if (mesh.vertices.empty())
        throw std::invalid_argument("Empty mesh has no bounds");
    Bounds out{{std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity(),
                std::numeric_limits<float>::infinity()},
               {-std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(),
                -std::numeric_limits<float>::infinity()}};
    for (const auto& vertex : mesh.vertices)
        for (int axis = 0; axis < 3; ++axis) {
            const float value = vertex.position[axis];
            if (!std::isfinite(value))
                throw std::invalid_argument("Nonfinite mesh vertex");
            out.min[axis] = std::min(out.min[axis], value);
            out.max[axis] = std::max(out.max[axis], value);
        }
    return out;
}

Bounds transformed_bounds(const Mesh& mesh, const Mat4& model) {
    return transformed_bounds(local_bounds(mesh), model);
}

Bounds transformed_bounds(const Bounds& local, const Mat4& model) {
    validate(local);
    for (float value : model)
        if (!std::isfinite(value))
            throw std::invalid_argument("Nonfinite mesh transform");
    Bounds out{{std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity(),
                std::numeric_limits<float>::infinity()},
               {-std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(),
                -std::numeric_limits<float>::infinity()}};
    for (unsigned corner = 0; corner < 8; ++corner) {
        const Vec3 point{corner & 1 ? local.max[0] : local.min[0],
                         corner & 2 ? local.max[1] : local.min[1],
                         corner & 4 ? local.max[2] : local.min[2]};
        for (int axis = 0; axis < 3; ++axis) {
            const float transformed = model[12 + axis] + model[axis] * point[0] +
                                      model[4 + axis] * point[1] + model[8 + axis] * point[2];
            if (!std::isfinite(transformed))
                throw std::invalid_argument("Nonfinite transformed mesh bound");
            out.min[axis] = std::min(out.min[axis], transformed);
            out.max[axis] = std::max(out.max[axis], transformed);
        }
    }
    // Float transforms can round an extremum inward by one ULP. Expand outward
    // before using the result for a visibility rejection.
    for (int axis = 0; axis < 3; ++axis) {
        out.min[axis] = std::nextafter(out.min[axis], -std::numeric_limits<float>::infinity());
        out.max[axis] = std::nextafter(out.max[axis], std::numeric_limits<float>::infinity());
    }
    return out;
}

InstanceUpdate InstanceTracker::update(std::string_view key,
                                        std::shared_ptr<const Mesh> mesh_identity,
                                        const Mat4& model, const Bounds& world_bounds,
                                        std::string_view view_id) {
    auto result = update(key, mesh_identity.get(), model, world_bounds, view_id);
    records_.at(std::string(key)).mesh_owner = std::move(mesh_identity);
    return result;
}

InstanceUpdate InstanceTracker::update(std::string_view key, const Mesh* mesh_identity,
                                        const Mat4& model, const Bounds& world_bounds,
                                        std::string_view view_id) {
    if (key.empty() || !mesh_identity)
        throw std::invalid_argument("Tracked instance requires a key and mesh");
    validate(world_bounds);
    for (float value : model)
        if (!std::isfinite(value))
            throw std::invalid_argument("Nonfinite instance transform");

    auto [it, inserted] = records_.try_emplace(std::string(key));
    auto& record = it->second;
    if (inserted) {
        if (free_slots_.empty()) {
            if (slot_generations_.size() >= std::numeric_limits<std::uint32_t>::max())
                throw std::overflow_error("Instance slot capacity exhausted");
            record.slot = static_cast<std::uint32_t>(slot_generations_.size());
            slot_generations_.push_back(1);
        } else {
            record.slot = free_slots_.back();
            free_slots_.pop_back();
            ++slot_generations_[record.slot];
        }
        record.generation = slot_generations_[record.slot];
    } else if (record.mesh != mesh_identity) {
        record.generation = ++slot_generations_[record.slot];
        record.committed = false;
    }

    const bool previous_valid = record.committed && record.previous_view == view_id &&
                                !invalid_views_.contains(std::string(view_id));
    InstanceUpdate result{record.slot, record.generation, record.previous_model,
                          record.previous_bounds, previous_valid};
    record.mesh = mesh_identity;
    record.mesh_owner.reset();
    record.current_model = model;
    record.current_bounds = world_bounds;
    record.current_view = view_id;
    record.seen_frame = frame_;
    return result;
}

void InstanceTracker::finish_frame() {
    for (auto it = records_.begin(); it != records_.end();) {
        auto& record = it->second;
        if (record.seen_frame != frame_) {
            free_slots_.push_back(record.slot);
            it = records_.erase(it);
        } else {
            record.previous_model = record.current_model;
            record.previous_bounds = record.current_bounds;
            record.previous_view = record.current_view;
            record.committed = true;
            ++it;
        }
    }
    invalid_views_.clear();
    ++frame_;
}

void InstanceTracker::invalidate_view(std::string_view view_id) {
    invalid_views_.emplace(view_id);
}

std::size_t select_lod(float projected_pixels, std::size_t previous_level,
                       std::span<const float> thresholds, float hysteresis,
                       std::span<const std::uint8_t> available) {
    if (!std::isfinite(projected_pixels) || projected_pixels < 0 ||
        !std::isfinite(hysteresis) || hysteresis < 0 || hysteresis >= 1)
        throw std::invalid_argument("Invalid LOD projection or hysteresis");
    for (std::size_t i = 0; i < thresholds.size(); ++i)
        if (!std::isfinite(thresholds[i]) || thresholds[i] <= 0 ||
            (i && thresholds[i] >= thresholds[i - 1]))
            throw std::invalid_argument("LOD thresholds must descend strictly");
    const auto count = thresholds.size() + 1;
    if (!available.empty() && available.size() != count)
        throw std::invalid_argument("LOD availability count mismatch");
    std::size_t level = previous_level;
    if (level >= count) {
        level = 0;
        while (level < thresholds.size() && projected_pixels < thresholds[level])
            ++level;
    } else {
        while (level < thresholds.size() &&
               projected_pixels < thresholds[level] * (1 - hysteresis))
            ++level;
        while (level && projected_pixels > thresholds[level - 1] * (1 + hysteresis))
            --level;
    }
    if (available.empty() || available[level])
        return level;
    for (std::size_t distance = 1; distance < count; ++distance) {
        if (level >= distance && available[level - distance])
            return level - distance;
        if (level + distance < count && available[level + distance])
            return level + distance;
    }
    throw std::invalid_argument("No available mesh LOD");
}
} // namespace faset::render
