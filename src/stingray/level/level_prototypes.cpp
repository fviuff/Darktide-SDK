#include "stingray/level/level_prototypes.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace dtglb::stingray::level {
namespace {

using Vec = std::array<float, 3>;

Vec mean(const float* positions, int first, int count) {
    Vec sum{};
    for (int i = first; i < first + count; ++i)
        for (int k = 0; k < 3; ++k) sum[k] += positions[i * 3 + k];
    for (auto& v : sum) v /= static_cast<float>(count);
    return sum;
}

// the corner of a face's vertex along the two other axes: make_box_vertices' triangle patterns
void face_signs(bool positive, int vertex, float& first, float& second) {
    if (!positive) {
        first = (vertex == 2 || vertex == 4 || vertex == 5) ? 1.0f : -1.0f;
        second = (vertex == 1 || vertex == 2 || vertex == 4) ? 1.0f : -1.0f;
    } else {
        first = (vertex == 1 || vertex == 4 || vertex == 5) ? 1.0f : -1.0f;
        second = (vertex == 1 || vertex == 2 || vertex == 5) ? 1.0f : -1.0f;
    }
}

}  // namespace

std::array<std::array<float, 3>, 36> prototype_vertices(const LevelPrototype& box) {
    std::array<Vec, 36> out{};
    int at = 0;
    for (int axis = 0; axis < 3; ++axis) {
        const int a = (axis + 1) % 3, b = (axis + 2) % 3;
        for (int side = 0; side < 2; ++side) {
            const float sign = side ? 1.0f : -1.0f;
            for (int vertex = 0; vertex < 6; ++vertex) {
                float first, second;
                face_signs(side != 0, vertex, first, second);
                float signs[3];
                signs[axis] = sign;
                signs[a] = first;
                signs[b] = second;
                // always added in the same order, so that faces meeting at a corner share it exactly
                for (int k = 0; k < 3; ++k)
                    out[at][k] = box.center[k] + signs[0] * box.axes[0][k] * box.half_size[0] +
                                 signs[1] * box.axes[1][k] * box.half_size[1] + signs[2] * box.axes[2][k] * box.half_size[2];
                ++at;
            }
        }
    }
    return out;
}

bool read_level_prototypes(const unit::Unit& unit, std::vector<LevelPrototype>& out, std::string& error) {
    out.clear();
    struct Shape {
        const unit::UnitActorShape* shape;
        std::uint32_t set;
    };
    std::map<std::uint32_t, Shape> shapes;  // by the mesh they came from
    for (const auto& actor : unit.actors)
        for (const auto& shape : actor.shapes) shapes[shape.node] = {&shape, actor.name};
    const auto set_of = [](std::uint32_t name) { return name == kNoObjectSet ? 0 : name; };
    for (const auto& mesh : unit.meshes) {
        if (mesh.geometry == 0 || mesh.geometry > unit.geometries.size()) continue;
        unit::DecodedGeometry geometry;
        if (!unit::decode_geometry(unit, unit.geometries[mesh.geometry - 1], geometry, error)) return false;
        const auto count = geometry.positions.size() / 3;
        if (count % 36) { error = "a prototype mesh has " + std::to_string(count) + " vertices"; return false; }
        const bool visible = (mesh.flags & 1) != 0;
        const auto shape = shapes.find(mesh.name);
        if (!visible && shape == shapes.end()) { error = "a prototype mesh has no physics shape"; return false; }
        for (std::size_t first = 0; first < count; first += 36) {
            LevelPrototype box;
            const float* p = geometry.positions.data() + first * 3;
            box.center = mean(p, 0, 36);
            for (int axis = 0; axis < 3; ++axis) {
                const auto low = mean(p, axis * 12, 6), high = mean(p, axis * 12 + 6, 6);
                const Vec along{high[0] - low[0], high[1] - low[1], high[2] - low[2]};
                const float length = std::sqrt(along[0] * along[0] + along[1] * along[1] + along[2] * along[2]);
                box.half_size[axis] = length / 2;
                if (length > 0) for (int k = 0; k < 3; ++k) box.axes[axis][k] = along[k] / length;
            }
            if (geometry.colors.size() >= (first + 1) * 4)
                for (int k = 0; k < 4; ++k) box.colour[k] = geometry.colors[first * 4 + k];
            box.visible = visible;
            box.physical = !visible;
            if (visible) {
                box.object_set = set_of(mesh.name);
            } else {
                box.material = shape->second.shape->material;
                box.shape_template = shape->second.shape->shape_template;
                box.object_set = set_of(shape->second.set);
            }
            out.push_back(box);
        }
    }
    return true;
}

bool same_prototype(const LevelPrototype& a, const LevelPrototype& b, float tolerance) {
    const auto close = [&](float x, float y) { return std::fabs(x - y) <= tolerance + 1e-6f * std::fabs(x); };
    for (int i = 0; i < 3; ++i) {
        if (!close(a.center[i], b.center[i]) || !close(a.half_size[i], b.half_size[i])) return false;
        for (int k = 0; k < 3; ++k) if (!close(a.axes[i][k], b.axes[i][k])) return false;
    }
    return true;
}

std::vector<LevelPrototype> merged_prototypes(const std::vector<LevelPrototype>& boxes) {
    std::vector<LevelPrototype> out;
    for (const auto& box : boxes) {
        auto twin = std::find_if(out.begin(), out.end(), [&](const LevelPrototype& other) {
            return other.visible != box.visible && other.physical != box.physical && other.object_set == box.object_set &&
                   same_prototype(other, box);
        });
        if (twin == out.end()) {
            out.push_back(box);
            continue;
        }
        twin->visible = twin->physical = true;
        if (box.physical) {
            twin->material = box.material;
            twin->shape_template = box.shape_template;
        } else {
            twin->colour = box.colour;  // the one that shows
        }
    }
    return out;
}

}  // namespace dtglb::stingray::level
