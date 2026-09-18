#include <algorithm>
#include <array>
#include <cmath>
#include <faset/authoring/transforms.hpp>
#include <faset/core/error.hpp>
#include <set>

namespace faset::authoring {
namespace {
using Matrix = std::array<double, 16>;
constexpr Matrix identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
Json& find_entity(Json& scene, const std::string& id) {
    for (auto& item : scene["entities"])
        if (item.at("id") == id)
            return item;
    throw Error("entity.missing", "Entity does not exist: " + id);
}
Json* transform(Json& entity) {
    for (auto& component : entity["components"])
        if (component.at("type") == "faset.transform")
            return &component;
    return nullptr;
}
Matrix multiply(const Matrix& a, const Matrix& b) {
    Matrix result{};
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column)
            for (int k = 0; k < 4; ++k)
                result[column * 4 + row] += a[k * 4 + row] * b[column * 4 + k];
    return result;
}
Matrix local(Json& entity) {
    const auto* component = transform(entity);
    if (!component)
        return identity;
    const auto& fields = component->at("fields");
    const auto p = fields.value("position", std::array<double, 3>{0, 0, 0}),
               r = fields.value("rotation", std::array<double, 3>{0, 0, 0}),
               s = fields.value("scale", std::array<double, 3>{1, 1, 1});
    const auto cx = std::cos(r[0]), sx = std::sin(r[0]), cy = std::cos(r[1]), sy = std::sin(r[1]),
               cz = std::cos(r[2]), sz = std::sin(r[2]);
    return {cz * cy * s[0],
            sz * cy * s[0],
            -sy * s[0],
            0,
            (cz * sy * sx - sz * cx) * s[1],
            (sz * sy * sx + cz * cx) * s[1],
            cy * sx * s[1],
            0,
            (cz * sy * cx + sz * sx) * s[2],
            (sz * sy * cx - cz * sx) * s[2],
            cy * cx * s[2],
            0,
            p[0],
            p[1],
            p[2],
            1};
}
Matrix world(Json& scene, const std::string& id, std::set<std::string>& visited) {
    require(visited.insert(id).second, "entity.cycle", "Hierarchy contains a cycle");
    auto& item = find_entity(scene, id);
    auto result = local(item);
    if (item.contains("parent") && !item["parent"].is_null())
        result = multiply(world(scene, item["parent"].get<std::string>(), visited), result);
    return result;
}
Matrix world(Json& scene, const std::string& id) {
    std::set<std::string> visited;
    return world(scene, id, visited);
}
Matrix inverse(const Matrix& matrix) {
    std::array<std::array<double, 8>, 4> rows{};
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c)
            rows[r][c] = matrix[c * 4 + r];
        rows[r][r + 4] = 1;
    }
    for (int column = 0; column < 4; ++column) {
        int pivot = column;
        for (int row = column + 1; row < 4; ++row)
            if (std::abs(rows[row][column]) > std::abs(rows[pivot][column]))
                pivot = row;
        require(std::abs(rows[pivot][column]) > 1e-12, "transform.singular",
                "Cannot preserve world transform under a non-invertible parent");
        std::swap(rows[pivot], rows[column]);
        const auto scale = rows[column][column];
        for (auto& entry : rows[column])
            entry /= scale;
        for (int row = 0; row < 4; ++row)
            if (row != column) {
                const auto factor = rows[row][column];
                for (int c = 0; c < 8; ++c)
                    rows[row][c] -= factor * rows[column][c];
            }
    }
    Matrix result{};
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column)
            result[column * 4 + row] = rows[row][column + 4];
    return result;
}
Json decompose(const Matrix& matrix) {
    std::array<double, 3> scale{};
    Matrix rotation = matrix;
    for (int c = 0; c < 3; ++c) {
        scale[c] = std::sqrt(matrix[c * 4] * matrix[c * 4] + matrix[c * 4 + 1] * matrix[c * 4 + 1] +
                             matrix[c * 4 + 2] * matrix[c * 4 + 2]);
        require(scale[c] > 1e-12, "transform.singular", "Cannot decompose zero scale");
    }
    const double determinant = matrix[0] * (matrix[5] * matrix[10] - matrix[9] * matrix[6]) -
                               matrix[4] * (matrix[1] * matrix[10] - matrix[9] * matrix[2]) +
                               matrix[8] * (matrix[1] * matrix[6] - matrix[5] * matrix[2]);
    if (determinant < 0)
        scale[0] = -scale[0];
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r)
            rotation[c * 4 + r] /= scale[c];
    for (int a = 0; a < 3; ++a)
        for (int b = a + 1; b < 3; ++b) {
            double dot = 0;
            for (int r = 0; r < 3; ++r)
                dot += rotation[a * 4 + r] * rotation[b * 4 + r];
            require(std::abs(dot) < 1e-5, "transform.shear",
                    "Preserving this world transform would require shear; choose keep local or "
                    "change parent scale");
        }
    std::array<double, 3> angles{};
    angles[1] = std::asin(std::clamp(-rotation[2], -1.0, 1.0));
    if (std::abs(std::cos(angles[1])) > 1e-7) {
        angles[0] = std::atan2(rotation[6], rotation[10]);
        angles[2] = std::atan2(rotation[1], rotation[0]);
    } else {
        angles[0] = std::atan2(-rotation[9], rotation[5]);
        angles[2] = 0;
    }
    return {
        {"position", {matrix[12], matrix[13], matrix[14]}}, {"rotation", angles}, {"scale", scale}};
}
} // namespace
void reparent_entity(Json& scene, const std::string& id, const Json& parent, bool keep_world) {
    auto& item = find_entity(scene, id);
    if (!parent.is_null())
        find_entity(scene, parent.get<std::string>());
    if (keep_world) {
        auto* component = transform(item);
        require(component != nullptr, "transform.required",
                "World-preserving reparent requires a Transform component");
        const auto previous = world(scene, id);
        const auto basis = parent.is_null() ? identity : world(scene, parent.get<std::string>());
        const auto fields = decompose(multiply(inverse(basis), previous));
        for (const auto& [field, value] : fields.items())
            (*component)["fields"][field] = value;
    }
    item["parent"] = parent;
}
} // namespace faset::authoring
