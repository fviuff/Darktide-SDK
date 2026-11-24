#include "stingray/unit/unit_collision.h"

#include "stingray/murmur_hash.h"
#ifdef DTGLB_HAS_PHYSICS_COLLECTIONS
#include "stingray/physics/physics_collection.h"
#endif

#include <cmath>
#include <utility>

namespace dtglb::stingray::unit {

namespace {

using Matrix = std::array<float, 16>;
using Vec3 = std::array<float, 3>;

Matrix multiply(const Matrix& a, const Matrix& b) {  // row vectors: a first, then b
    Matrix m{};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            for (int k = 0; k < 4; ++k) m[r * 4 + c] += a[r * 4 + k] * b[k * 4 + c];
    return m;
}

Vec3 transform(const Vec3& v, const Matrix& m) {
    return {v[0] * m[0] + v[1] * m[4] + v[2] * m[8] + m[12], v[0] * m[1] + v[1] * m[5] + v[2] * m[9] + m[13],
            v[0] * m[2] + v[1] * m[6] + v[2] * m[10] + m[14]};
}

// a closed surface around the origin: rings of points from -x pole to +x pole (capsules lie along +X)
void add_round(CollisionMesh& mesh, const Matrix& m, float radius, float half_length) {
    constexpr int segments = 12, rings = 8;  // rings: latitude steps from pole to pole
    const auto first = static_cast<std::uint32_t>(mesh.vertices.size());
    for (int ring = 0; ring <= rings; ++ring) {
        const float a = 3.14159265f * ring / rings;                 // 0 at -x, pi at +x
        const float x = -std::cos(a) * radius + (ring * 2 <= rings ? -half_length : half_length);
        const float r = std::sin(a) * radius;
        for (int s = 0; s < segments; ++s) {
            const float b = 6.28318531f * s / segments;
            mesh.vertices.push_back(transform({x, r * std::cos(b), r * std::sin(b)}, m));
        }
        if (ring * 2 == rings && half_length > 0) {  // the cylinder: the equator once more, on the other side
            for (int s = 0; s < segments; ++s) {
                const float b = 6.28318531f * s / segments;
                mesh.vertices.push_back(transform({half_length, radius * std::cos(b), radius * std::sin(b)}, m));
            }
        }
    }
    const int rows = rings + 1 + (half_length > 0 ? 1 : 0);
    for (int row = 0; row + 1 < rows; ++row)
        for (int s = 0; s < segments; ++s) {
            const std::uint32_t a = first + row * segments + s, b = first + row * segments + (s + 1) % segments;
            const std::uint32_t c = a + segments, d = b + segments;
            mesh.indices.insert(mesh.indices.end(), {a, b, c, b, d, c});
        }
}

void add_box(CollisionMesh& mesh, const Matrix& m, const Vec3& h) {
    const auto first = static_cast<std::uint32_t>(mesh.vertices.size());
    for (int i = 0; i < 8; ++i)
        mesh.vertices.push_back(transform({i & 1 ? h[0] : -h[0], i & 2 ? h[1] : -h[1], i & 4 ? h[2] : -h[2]}, m));
    static const std::uint32_t faces[12][3] = {{0, 2, 1}, {1, 2, 3}, {4, 5, 6}, {5, 7, 6}, {0, 1, 4}, {1, 5, 4},
                                               {2, 6, 3}, {3, 6, 7}, {0, 4, 2}, {2, 4, 6}, {1, 3, 5}, {3, 7, 5}};
    for (const auto& f : faces) mesh.indices.insert(mesh.indices.end(), {first + f[0], first + f[1], first + f[2]});
}

}  // namespace

Matrix pose_matrix(const Vec3& p, const std::array<float, 4>& q, const Vec3& s) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    return {(1 - 2 * (y * y + z * z)) * s[0], 2 * (x * y + z * w) * s[0], 2 * (x * z - y * w) * s[0], 0,
            2 * (x * y - z * w) * s[1], (1 - 2 * (x * x + z * z)) * s[1], 2 * (y * z + x * w) * s[1], 0,
            2 * (x * z + y * w) * s[2], 2 * (y * z - x * w) * s[2], (1 - 2 * (x * x + y * y)) * s[2], 0,
            p[0], p[1], p[2], 1};
}

bool static_collision(const Unit& unit, const Matrix& placement, CollisionMesh& out, std::string& error) {
    const std::uint32_t static_template = id32_from_id64("static");
    const std::uint32_t trigger_templates[] = {id32_from_id64("trigger"), 0x5fa361ebu};
    struct Pending { std::uint32_t type; const std::vector<std::uint8_t>* cooked; Matrix frame; };
    std::vector<Pending> cooked_shapes;
    for (const auto& actor : unit.actors) {
        if (actor.actor_template != static_template || !actor.enabled) continue;
        Matrix node_world{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        for (const auto& node : unit.nodes)
            if (node.name == actor.node) { node_world = node.world; break; }
        const Matrix actor_frame = multiply(node_world, placement);
        for (const auto& shape : actor.shapes) {
            if (shape.shape_template == trigger_templates[0] || shape.shape_template == trigger_templates[1]) continue;
            const Matrix frame = multiply(shape.local, actor_frame);
            switch (shape.type) {
                case 0: add_round(out, frame, shape.radius, 0); break;
                case 1: add_box(out, frame, shape.half_extents); break;
                case 2: add_round(out, frame, shape.radius, shape.height * 0.5f); break;
                case 3: case 4: cooked_shapes.push_back({shape.type, &shape.cooked, frame}); break;
                default: break;  // height fields and cloth: not in level units' static collision
            }
        }
    }
    if (cooked_shapes.empty()) return true;
#ifdef DTGLB_HAS_PHYSICS_COLLECTIONS
    std::vector<std::pair<std::uint32_t, std::vector<std::uint8_t>>> meshes;
    for (const auto& c : cooked_shapes) meshes.emplace_back(c.type, *c.cooked);
    std::vector<physics::CookedMesh> read;
    if (!physics::read_cooked_meshes(meshes, read, error)) return false;
    for (std::size_t i = 0; i < read.size(); ++i) {
        const auto first = static_cast<std::uint32_t>(out.vertices.size());
        for (const auto& v : read[i].vertices) out.vertices.push_back(transform(v, cooked_shapes[i].frame));
        for (const auto t : read[i].triangles) out.indices.push_back(first + t);
    }
    return true;
#else
    error = "reading cooked collision meshes needs PhysX (Windows build)";
    return false;
#endif
}

}  // namespace dtglb::stingray::unit
