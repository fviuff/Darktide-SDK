#include "stingray/level/level_prototype_writer.h"

#include "scene/scene.h"
#include "stingray/binary_writer.h"
#include "stingray/murmur_hash.h"
#include "stingray/physics/physx_cooking.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <tuple>

namespace dtglb::stingray::level {
namespace {

using Vec = std::array<float, 3>;

constexpr std::uint32_t kRootNode = 0xbccf91e5u;      // IdString32 "root_point"
constexpr std::uint32_t kPrototypeSlot = 0x46cab355u;  // IdString32 "prototype"
constexpr std::uint64_t kPrototypeMaterial = 0x7ca93e49ab27c394ull;
constexpr std::uint32_t kActorTemplate = 0x072ce5bau;

std::array<float, 2> oct_encode(const Vec& v) {
    const float sum = std::fabs(v[0]) + std::fabs(v[1]) + std::fabs(v[2]);
    float x = v[0] / sum, y = v[1] / sum;
    if (v[2] < 0) {
        const float ox = (1 - std::fabs(y)) * (x >= 0 ? 1.0f : -1.0f), oy = (1 - std::fabs(x)) * (y >= 0 ? 1.0f : -1.0f);
        x = ox;
        y = oy;
    }
    return {x * 0.5f + 0.5f, y * 0.5f + 0.5f};
}

std::uint8_t colour_byte(float c) {
    return static_cast<std::uint8_t>(std::clamp(c, 0.0f, 1.0f) * 255.0f + 0.5f);
}

struct Bounds {
    Vec min{}, max{}, centre{};
    float radius = 0;
};

Bounds bounds_of(const std::vector<const LevelPrototype*>& boxes) {
    Bounds b;
    bool first = true;
    for (const auto* box : boxes)
        for (const auto& p : prototype_vertices(*box))
            for (int k = 0; k < 3; ++k) {
                b.min[k] = first ? p[k] : std::min(b.min[k], p[k]);
                b.max[k] = first ? p[k] : std::max(b.max[k], p[k]);
                if (k == 2) first = false;
            }
    float squared = 0;
    for (int k = 0; k < 3; ++k) {
        b.centre[k] = (b.min[k] + b.max[k]) / 2;
        squared += (b.max[k] - b.centre[k]) * (b.max[k] - b.centre[k]);
    }
    b.radius = std::sqrt(squared);
    return b;
}

void write_bounds(BinaryWriter& w, const Bounds& b) {
    for (const auto& v : {b.min, b.max, b.centre})
        for (const float f : v) w.f32(f);
    w.f32(b.radius);
}

// MeshGeometry: one stream of position, octahedral normal, colour, texture coordinates; 32-bit indices in order
void write_geometry(BinaryWriter& w, const std::vector<const LevelPrototype*>& boxes) {
    const auto vertices = static_cast<std::uint32_t>(boxes.size() * 36);
    w.u32(1);  // version
    w.u32(1);  // streams
    w.u32(vertices * 32);
    for (const auto* box : boxes) {
        const auto corners = prototype_vertices(*box);
        for (int v = 0; v < 36; ++v) {
            const int axis = v / 12;
            const float sign = (v / 6) % 2 ? 1.0f : -1.0f;
            const Vec normal{box->axes[axis][0] * sign, box->axes[axis][1] * sign, box->axes[axis][2] * sign};
            for (const float f : corners[v]) w.f32(f);
            for (const float f : oct_encode(normal)) w.f32(f);
            for (const float c : box->colour) w.u8(colour_byte(c));
            w.f32(0);
            w.f32(0);
        }
    }
    w.u32(0);  // validity
    w.u32(0);  // stream type
    w.u32(vertices);
    w.u32(32);
    w.u32(4);
    for (const auto& channel : std::array<std::array<std::uint32_t, 2>, 4>{{{0, 2}, {1, 1}, {6, 4}, {5, 1}}}) {
        w.u32(channel[0]);
        w.u32(channel[1]);
        w.u32(0);
        w.u32(0);
        w.u8(0);
    }
    w.u32(0);  // index validity
    w.u32(0);  // index stream type
    w.u32(1);  // 32-bit
    w.u32(vertices);
    w.u32(vertices * 4);
    for (std::uint32_t i = 0; i < vertices; ++i) w.u32(i);
    w.u32(1);  // one batch
    w.u32(0);
    w.u32(0);
    w.u32(vertices / 3);
    w.u32(0);
    write_bounds(w, bounds_of(boxes));
    w.u32(1);
    w.u32(kPrototypeSlot);
    w.u32(0);
}

// the boxes' faces with the same corners shared, as the cooker wants them
bool cook(const std::vector<const LevelPrototype*>& boxes, std::vector<std::uint8_t>& out, std::string& error) {
    Primitive primitive;
    VertexChannel position;
    position.semantic = VertexChannel::Semantic::Position;
    position.components = 3;
    std::map<Vec, std::uint32_t> known;
    for (const auto* box : boxes)
        for (const auto& corner : prototype_vertices(*box)) {
            const auto [found, added] = known.emplace(corner, static_cast<std::uint32_t>(known.size()));
            if (added) position.values.insert(position.values.end(), corner.begin(), corner.end());
            primitive.indices.push_back(found->second);
        }
    primitive.channels.push_back(std::move(position));
    physics::CookedTriangleMesh cooked;
    if (!physics::cook_triangle_mesh(primitive, cooked, error)) return false;
    out = std::move(cooked.bytes);
    return true;
}

struct Mesh {
    std::uint32_t name = 0, flags = 0;
    std::vector<const LevelPrototype*> boxes;
};
struct Shape {
    std::uint32_t material = 0, shape_template = 0, mesh = 0;
    std::vector<std::uint8_t> cooked;
};
struct Actor {
    std::uint32_t name = 0;
    std::vector<Shape> shapes;
};

}  // namespace

bool write_level_prototype_unit(const std::vector<LevelPrototype>& boxes, std::vector<std::uint8_t>& out, std::string& error) {
    std::vector<Mesh> meshes;
    std::map<std::uint32_t, std::size_t> visible_meshes;
    for (const auto& box : boxes) {
        if (!box.visible) continue;
        const auto set = box.object_set ? box.object_set : kNoObjectSet;
        const auto [found, added] = visible_meshes.emplace(set, meshes.size());
        if (added) meshes.push_back({set, 3, {}});
        meshes[found->second].boxes.push_back(&box);
    }
    std::vector<Actor> actors;
    std::map<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>, std::size_t> solid_meshes;
    std::vector<std::array<std::uint32_t, 3>> solid_of_mesh;
    const auto first_solid = meshes.size();
    for (const auto& box : boxes) {
        if (!box.physical) continue;
        const auto set = box.object_set ? box.object_set : kNoObjectSet;
        const auto [found, added] = solid_meshes.emplace(std::make_tuple(set, box.material, box.shape_template), meshes.size());
        if (added) {
            const auto name = id32_from_id64("__physics_mesh" + std::to_string(meshes.size() - first_solid + 1));
            meshes.push_back({name, 0, {}});
            solid_of_mesh.push_back({set, box.material, box.shape_template});
        }
        meshes[found->second].boxes.push_back(&box);
    }
    for (std::size_t i = first_solid; i < meshes.size(); ++i) {
        const auto& solid = solid_of_mesh[i - first_solid];
        auto actor = std::find_if(actors.begin(), actors.end(), [&](const Actor& a) { return a.name == solid[0]; });
        if (actor == actors.end()) actor = actors.insert(actors.end(), Actor{solid[0], {}});
        Shape shape{solid[1], solid[2], meshes[i].name, {}};
        if (!cook(meshes[i].boxes, shape.cooked, error)) return false;
        actor->shapes.push_back(std::move(shape));
    }

    BinaryWriter w;
    w.u32(0x73);
    w.u32(static_cast<std::uint32_t>(meshes.size()));
    for (const auto& mesh : meshes) write_geometry(w, mesh.boxes);
    w.u32(0);  // skins
    w.u32(0);  // simple animation
    w.u32(0);  // groups
    w.u32(1);  // nodes: the root
    for (const float f : {1.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f, 1.f, 1.f}) w.f32(f);
    for (const float f : {1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f}) w.f32(f);
    w.u32(0);  // parent
    w.u32(kRootNode);
    w.u32(0);
    w.u32(static_cast<std::uint32_t>(meshes.size()));
    for (std::size_t i = 0; i < meshes.size(); ++i) {
        w.u32(meshes[i].name);
        w.u32(0);  // node
        w.u32(static_cast<std::uint32_t>(i + 1));
        w.u32(0);  // skin
        w.u32(meshes[i].flags);
        w.u32(0);
        w.u32(1);
        write_bounds(w, bounds_of(meshes[i].boxes));
        w.u32(0);
    }
    w.u32(static_cast<std::uint32_t>(actors.size()));
    for (const auto& actor : actors) {
        w.u32(actor.name);
        w.u32(kActorTemplate);
        w.u32(kRootNode);
        w.f32(0);  // mass
        w.u32(static_cast<std::uint32_t>(actor.shapes.size()));
        for (const auto& shape : actor.shapes) {
            w.u32(3);  // triangle mesh
            w.u32(shape.material);
            w.u32(shape.shape_template);
            for (const float f : {1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f}) w.f32(f);
            w.u32(static_cast<std::uint32_t>(shape.cooked.size()));
            w.bytes(shape.cooked.data(), shape.cooked.size());
            w.u32(0);
            w.u32(shape.mesh);
        }
        for (int i = 0; i < 6; ++i) w.u32(0xffffffffu);  // touch and trigger callbacks
        w.u32(1);                                        // flag bytes
    }
    for (int i = 0; i < 11; ++i) w.u32(0);  // second actors .. unknown after movers
    w.u8(0);                                // animation blender
    w.u32(0);                               // state machine
    w.u32(8);                               // script data: empty
    w.u32(0xffffffffu);
    w.u32(0);
    for (int i = 0; i < 5; ++i) w.u32(0);  // visibility groups, flow, its data, triangle finder, physics scene
    w.u64(0);                              // default material
    w.u32(1);
    w.u32(kPrototypeSlot);
    w.u64(kPrototypeMaterial);
    for (int i = 0; i < 4; ++i) w.u32(0);  // second bindings, layers, two unknown
    w.u64(0);                              // skeleton
    w.u32(0);
    out = w.data();
    out.resize((out.size() + 3) / 4 * 4);
    return true;
}

}  // namespace dtglb::stingray::level
