#include "stingray/terrain/terrain.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace dtglb::stingray::terrain {
namespace {

using unit::Unit;
using unit::UnitActorShape;
using unit::UnitTerrain;
using unit::UnitTerrainNode;

struct Writer {
    std::vector<std::uint8_t>& out;
    template <typename T> void put(T v) {
        const auto* p = reinterpret_cast<const std::uint8_t*>(&v);
        out.insert(out.end(), p, p + sizeof(T));
    }
    void u8(std::uint8_t v) { put(v); }
    void u16(std::uint16_t v) { put(v); }
    void u32(std::uint32_t v) { put(v); }
    void u64(std::uint64_t v) { put(v); }
    void f32(float v) { put(v); }
    template <std::size_t N> void floats(const std::array<float, N>& v) { for (const float f : v) f32(f); }
    void bytes(const std::vector<std::uint8_t>& v) {
        u32(static_cast<std::uint32_t>(v.size()));
        out.insert(out.end(), v.begin(), v.end());
    }
    void u32s(const std::vector<std::uint32_t>& v) {
        u32(static_cast<std::uint32_t>(v.size()));
        for (const auto x : v) u32(x);
    }
};

float size_x(const UnitTerrain& t) { return t.bounds[3] - t.bounds[0]; }
float size_y(const UnitTerrain& t) { return t.bounds[4] - t.bounds[1]; }

// a point of the terrain node's frame in the unit's (the node's world, row vectors)
std::array<float, 3> to_unit(const std::array<float, 16>& m, float x, float y, float z) {
    return {x * m[0] + y * m[4] + z * m[8] + m[12], x * m[1] + y * m[5] + z * m[9] + m[13], x * m[2] + y * m[6] + z * m[10] + m[14]};
}
std::array<float, 3> direction_to_unit(const std::array<float, 16>& m, float x, float y, float z) {
    std::array<float, 3> d{x * m[0] + y * m[4] + z * m[8], x * m[1] + y * m[5] + z * m[9], x * m[2] + y * m[6] + z * m[10]};
    const float length = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (length > 0) for (auto& v : d) v /= length;
    return d;
}

// build_quad_tree (Stingray terrain.cpp): a node over the rectangle at `centre` of `size`; leaves read the grid's
// samples between their edges
// (cx, cy) relative to the box's centre
void build_node(std::vector<UnitTerrainNode>& nodes, const HeightGrid& grid, float cx, float cy, float sx, float sy,
                float terrain_x, float terrain_y, float z_min, float z_max, std::uint32_t level, std::uint32_t depth,
                std::uint16_t& out_min, std::uint16_t& out_max) {
    const auto index = static_cast<std::uint32_t>(nodes.size());
    nodes.push_back({0, 0, {~0u, ~0u, ~0u, ~0u}});
    const float hx = sx * 0.5f, hy = sy * 0.5f;
    std::uint16_t low = 0xffff, high = 0;
    if (level < depth) {
        const float qx = hx * 0.5f, qy = hy * 0.5f;
        const float centres[4][2] = {{cx - qx, cy - qy}, {cx + qx, cy - qy}, {cx + qx, cy + qy}, {cx - qx, cy + qy}};
        for (int k = 0; k < 4; ++k) {
            nodes[index].children[k] = static_cast<std::uint32_t>(nodes.size());
            std::uint16_t child_min, child_max;
            build_node(nodes, grid, centres[k][0], centres[k][1], hx, hy, terrain_x, terrain_y, z_min, z_max, level + 1, depth,
                       child_min, child_max);
            low = std::min(low, child_min);
            high = std::max(high, child_max);
        }
    } else if (!grid.heights.empty()) {
        const float w = static_cast<float>(grid.columns), h = static_cast<float>(grid.rows);
        const int row_from = static_cast<int>(((1.0f / terrain_y) * (cy - hy) + 0.5f) * h);
        const int row_to = static_cast<int>(((1.0f / terrain_y) * (cy + hy) + 0.5f) * h);
        const int column_from = static_cast<int>(((1.0f / terrain_x) * (cx - hx) + 0.5f) * w);
        const int column_to = static_cast<int>(((1.0f / terrain_x) * (cx + hx) + 0.5f) * w);
        for (int r = std::max(row_from, 0); r < row_to && r < static_cast<int>(grid.rows); ++r)
            for (int c = std::max(column_from, 0); c < column_to && c < static_cast<int>(grid.columns); ++c) {
                const std::uint16_t v = grid.heights[static_cast<std::size_t>(r) * grid.columns + c];
                low = std::min(low, v);
                high = std::max(high, v);
            }
    } else {
        low = 0;
    }
    nodes[index].height_min = static_cast<float>(low) / 65535.0f * z_min;
    nodes[index].height_max = static_cast<float>(high) / 65535.0f * z_max;
    out_min = low;
    out_max = high;
}

// grid heights between samples (bilinear over sample positions, clamped)
float grid_value(const HeightGrid& g, float column, float row) {
    column = std::clamp(column, 0.0f, static_cast<float>(g.columns - 1));
    row = std::clamp(row, 0.0f, static_cast<float>(g.rows - 1));
    const auto c0 = static_cast<std::uint32_t>(column), r0 = static_cast<std::uint32_t>(row);
    const std::uint32_t c1 = std::min(c0 + 1, g.columns - 1), r1 = std::min(r0 + 1, g.rows - 1);
    const float fc = column - static_cast<float>(c0), fr = row - static_cast<float>(r0);
    const auto at = [&](std::uint32_t c, std::uint32_t r) { return static_cast<float>(g.heights[static_cast<std::size_t>(r) * g.columns + c]); };
    return (at(c0, r0) * (1 - fc) + at(c1, r0) * fc) * (1 - fr) + (at(c0, r1) * (1 - fc) + at(c1, r1) * fc) * fr;
}

}  // namespace

std::array<float, 10> terrain_bounds(float size_x, float size_y, float z_min, float z_max) {
    const float hx = size_x * 0.5f, hy = size_y * 0.5f;
    return {-hx, -hy, z_min, hx, hy, z_max, 0, 0, (z_min + z_max) * 0.5f, std::sqrt(hx * hx + hy * hy + z_max * z_max)};
}

float sample_height(const UnitTerrain& t, std::uint16_t sample) {
    return t.bounds[2] + static_cast<float>(sample) / 65535.0f * (t.bounds[5] - t.bounds[2]);
}

std::uint16_t height_sample(const UnitTerrain& t, float height) {
    const float range = t.bounds[5] - t.bounds[2];
    if (!(range > 0)) return 0;
    const float v = (height - t.bounds[2]) / range * 65535.0f;
    return static_cast<std::uint16_t>(std::clamp(std::lround(v), 0l, 65535l));
}

const unit::UnitTerrainLayer* terrain_layer(const UnitTerrain& t, std::uint32_t type) {
    for (const auto& l : t.layers)
        if (l.type == type) return &l;
    return nullptr;
}

const UnitActorShape* height_field(const Unit& u, const UnitTerrain& t) {
    // the actor names its node, the shape the terrain
    const std::uint32_t node = t.node < u.nodes.size() ? u.nodes[t.node].name : 0;
    for (const auto& actor : u.actors)
        for (const auto& shape : actor.shapes)
            if (shape.type == 5 && (shape.node == t.name || (actor.node == node && u.terrains.size() == 1))) return &shape;
    return nullptr;
}

bool grid_from_height_field(const UnitActorShape& shape, HeightGrid& out, std::string& error) {
    out = {};
    const std::size_t n = static_cast<std::size_t>(shape.columns) * shape.rows;
    if (shape.type != 5 || !n || shape.cooked.size() != n * 4) {
        error = "the height field shape's samples don't match its rows and columns";
        return false;
    }
    out.columns = shape.columns;
    out.rows = shape.rows;
    out.heights.resize(n);
    out.materials.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        std::int16_t h;
        std::memcpy(&h, shape.cooked.data() + i * 4, 2);
        out.heights[i] = static_cast<std::uint16_t>(static_cast<int>(h) + 32768);
        out.materials[i] = shape.cooked[i * 4 + 2];
    }
    return true;
}

bool grid_from_height_map(std::uint32_t width, std::uint32_t height, const std::vector<std::uint8_t>& texels, HeightGrid& out,
                          std::string& error) {
    out = {};
    if (!width || !height || texels.size() < static_cast<std::size_t>(width) * height * 2) {
        error = "the height map's texels don't match its size";
        return false;
    }
    out.columns = width;
    out.rows = height;
    out.heights.resize(static_cast<std::size_t>(width) * height);
    for (std::uint32_t r = 0; r < height; ++r)
        std::memcpy(out.heights.data() + static_cast<std::size_t>(r) * width,
                    texels.data() + static_cast<std::size_t>(height - 1 - r) * width * 2, static_cast<std::size_t>(width) * 2);
    return true;
}

void materials_from_mask(const std::vector<std::uint8_t>& rgba, std::uint32_t width, std::uint32_t height, HeightGrid& grid) {
    grid.materials.assign(grid.heights.size(), kBaseMaterial);
    if (!width || !height || rgba.size() < static_cast<std::size_t>(width) * height * 4) return;
    for (std::uint32_t r = 0; r < grid.rows; ++r)
        for (std::uint32_t c = 0; c < grid.columns; ++c) {
            const std::uint32_t x = static_cast<std::uint32_t>(static_cast<std::uint64_t>(c) * width / grid.columns);
            const std::uint32_t y = height - 1 - static_cast<std::uint32_t>(static_cast<std::uint64_t>(r) * height / grid.rows);
            const std::uint8_t* px = rgba.data() + (static_cast<std::size_t>(y) * width + x) * 4;
            std::uint8_t m = kBaseMaterial;
            for (std::uint8_t k = 0; k < 4; ++k)
                if (px[k] >= 128) m = k;
            grid.materials[static_cast<std::size_t>(r) * grid.columns + c] = m;
        }
}

bool terrain_surface(const Unit& u, const UnitTerrain& t, const HeightGrid& g, float spacing, bool drawn, Surface& out,
                     std::string& error) {
    if (t.node >= u.nodes.size()) { error = "the terrain's node is not in the unit"; return false; }
    return terrain_surface_in(u.nodes[t.node].world, t, g, spacing, drawn, out, error);
}

bool terrain_surface_in(const std::array<float, 16>& world, const UnitTerrain& t, const HeightGrid& g, float spacing, bool drawn,
                        Surface& out, std::string& error) {
    out = {};
    if (g.columns < 2 || g.rows < 2 || g.heights.size() != static_cast<std::size_t>(g.columns) * g.rows) {
        error = "the terrain has no height samples";
        return false;
    }
    const float sx = size_x(t), sy = size_y(t);
    const float cell_x = sx / static_cast<float>(g.columns), cell_y = sy / static_cast<float>(g.rows);
    const std::uint32_t step = spacing > 0 ? std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::lround(spacing / std::min(cell_x, cell_y)))) : 1;
    const auto scale = (t.bounds[5] - t.bounds[2]) / 65535.0f;
    // vertex columns / rows: samples 0, step, 2 step, ... and the last; drawn: the box's edges in steps over the texels
    std::vector<float> xs, ys;    // node frame
    std::vector<float> cs, rs;    // grid coordinates the heights come from
    if (drawn) {
        const std::uint32_t nx = std::max<std::uint32_t>(1, (g.columns + step - 1) / step), ny = std::max<std::uint32_t>(1, (g.rows + step - 1) / step);
        for (std::uint32_t i = 0; i <= nx; ++i) {
            const float f = static_cast<float>(i) / static_cast<float>(nx);
            xs.push_back(t.bounds[0] + f * sx);
            cs.push_back(f * static_cast<float>(g.columns) - 0.5f);
        }
        for (std::uint32_t i = 0; i <= ny; ++i) {
            const float f = static_cast<float>(i) / static_cast<float>(ny);
            ys.push_back(t.bounds[1] + f * sy);
            rs.push_back(f * static_cast<float>(g.rows) - 0.5f);
        }
    } else {
        for (std::uint32_t c = 0;; c = std::min(c + step, g.columns - 1)) {
            xs.push_back(t.bounds[0] + static_cast<float>(c) * cell_x);
            cs.push_back(static_cast<float>(c));
            if (c == g.columns - 1) break;
        }
        for (std::uint32_t r = 0;; r = std::min(r + step, g.rows - 1)) {
            ys.push_back(t.bounds[1] + static_cast<float>(r) * cell_y);
            rs.push_back(static_cast<float>(r));
            if (r == g.rows - 1) break;
        }
    }
    const auto columns = static_cast<std::uint32_t>(xs.size()), rows = static_cast<std::uint32_t>(ys.size());
    std::vector<float> z(static_cast<std::size_t>(columns) * rows);
    for (std::uint32_t r = 0; r < rows; ++r)
        for (std::uint32_t c = 0; c < columns; ++c) z[static_cast<std::size_t>(r) * columns + c] = t.bounds[2] + grid_value(g, cs[c], rs[r]) * scale;
    out.positions.reserve(z.size());
    for (std::uint32_t r = 0; r < rows; ++r)
        for (std::uint32_t c = 0; c < columns; ++c) {
            const std::size_t i = static_cast<std::size_t>(r) * columns + c;
            out.positions.push_back(to_unit(world, xs[c], ys[r], z[i]));
            // normal from the neighbours' slopes
            const std::uint32_t c0 = c ? c - 1 : c, c1 = std::min(c + 1, columns - 1), r0 = r ? r - 1 : r, r1 = std::min(r + 1, rows - 1);
            const float dzdx = (z[static_cast<std::size_t>(r) * columns + c1] - z[static_cast<std::size_t>(r) * columns + c0]) / (xs[c1] - xs[c0]);
            const float dzdy = (z[static_cast<std::size_t>(r1) * columns + c] - z[static_cast<std::size_t>(r0) * columns + c]) / (ys[r1] - ys[r0]);
            out.normals.push_back(direction_to_unit(world, -dzdx, -dzdy, 1.0f));
            out.uvs.push_back({(xs[c] - t.bounds[0]) / sx, 1.0f - (ys[r] - t.bounds[1]) / sy});
        }
    // a cell takes the material (hole, diagonal) of the sample at its min corner
    const bool has_materials = !drawn && g.materials.size() == g.heights.size();
    for (std::uint32_t r = 0; r + 1 < rows; ++r)
        for (std::uint32_t c = 0; c + 1 < columns; ++c) {
            const std::uint32_t a = r * columns + c, b = a + 1, cc = a + columns, d = cc + 1;
            std::uint8_t m = 0;
            if (has_materials) m = g.materials[static_cast<std::size_t>(rs[r]) * g.columns + static_cast<std::size_t>(cs[c])];
            if (has_materials && (m & 0x7f) == kHole) continue;
            if (m & 0x80) out.indices.insert(out.indices.end(), {a, b, d, a, d, cc});  // diagonal (c, r) - (c+1, r+1)
            else out.indices.insert(out.indices.end(), {a, b, cc, b, d, cc});
        }
    return true;
}

std::vector<UnitTerrainNode> build_quadtree(const UnitTerrain& t, const HeightGrid& g) {
    std::vector<UnitTerrainNode> nodes;
    const auto* layer = terrain_layer(t, 0);
    const float resolution = static_cast<float>(layer ? layer->resolution : g.columns);
    // the Stingray compiler's log2 (logf x 1.442695), truncated
    const float levels = std::log(resolution / static_cast<float>(kLeafTexels)) * 1.442695f;
    const auto depth = levels > 0 ? static_cast<std::uint32_t>(levels) : 0u;
    std::uint16_t low, high;
    build_node(nodes, g, 0.0f, 0.0f, size_x(t), size_y(t), size_x(t), size_y(t), t.bounds[2], t.bounds[5], 0, depth, low, high);
    return nodes;
}

void set_height_field(const UnitTerrain& t, const HeightGrid& g, UnitActorShape& shape) {
    shape.type = 5;
    shape.columns = g.columns;
    shape.rows = g.rows;
    shape.height_scale = (t.bounds[5] - t.bounds[2]) / 65535.0f;
    shape.row_scale = size_y(t) / static_cast<float>(g.rows);
    shape.column_scale = size_x(t) / static_cast<float>(g.columns);
    // more than one material among the samples
    shape.multiple_materials = false;
    for (std::size_t i = 1; i < g.materials.size() && !shape.multiple_materials; ++i)
        shape.multiple_materials = (g.materials[i] & 0x7f) != (g.materials[0] & 0x7f);
    // local x (rows) along +y, local y (height) along +z, local z (columns) along +x; samples are signed around 32768
    shape.local = {0, 1, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, t.bounds[0], t.bounds[1], t.bounds[2] + 32768.0f * shape.height_scale, 1};
    shape.cooked.resize(g.heights.size() * 4);
    for (std::size_t i = 0; i < g.heights.size(); ++i) {
        const auto h = static_cast<std::int16_t>(static_cast<int>(g.heights[i]) - 32768);
        std::memcpy(shape.cooked.data() + i * 4, &h, 2);
        const std::uint8_t m = i < g.materials.size() ? g.materials[i] : kBaseMaterial;
        shape.cooked[i * 4 + 2] = m;
        shape.cooked[i * 4 + 3] = static_cast<std::uint8_t>(m & 0x7f);
    }
    shape.second.clear();
}

std::vector<std::uint8_t> height_map_body(const HeightGrid& g) {
    std::vector<std::uint8_t> body;
    Writer w{body};
    for (int i = 0; i < 3; ++i) w.u32(0);
    // DDS: legacy header, 16-bit luminance (R16), one mip
    const std::uint32_t pitch = g.columns * 2;
    body.insert(body.end(), {'D', 'D', 'S', ' '});
    w.u32(124);
    w.u32(0x1007);  // caps, height, width, pixel format
    w.u32(g.rows);
    w.u32(g.columns);
    w.u32(pitch);
    w.u32(0);
    w.u32(1);
    for (int i = 0; i < 11; ++i) w.u32(0);
    w.u32(32);
    w.u32(0x20000);  // luminance
    w.u32(0);
    w.u32(16);
    w.u32(0xffff);
    w.u32(0);
    w.u32(0);
    w.u32(0);
    w.u32(0x1000);
    for (int i = 0; i < 4; ++i) w.u32(0);
    for (std::uint32_t r = 0; r < g.rows; ++r) {
        const auto* row = reinterpret_cast<const std::uint8_t*>(g.heights.data() + static_cast<std::size_t>(g.rows - 1 - r) * g.columns);
        body.insert(body.end(), row, row + pitch);
    }
    // the mip block left empty (the DDS header describes everything), a zero word, the footer
    w.u32(67);
    for (int i = 0; i < 36; ++i) w.u32(0);
    w.u32(0);
    w.u32(0x02e6355bu);
    return body;
}

bool write_terrain_unit(const Unit& u, std::vector<std::uint8_t>& body, std::string& error) {
    if (!u.geometries.empty() || !u.skins.empty() || !u.meshes.empty() || !u.cameras.empty() || !u.lights.empty() || !u.lods.empty() ||
        !u.visibility_groups.empty() || u.skeleton || !u.state_machine.empty() || u.unkept_sections) {
        error = "only a unit made of nodes, actors and terrains is written as a terrain unit";
        return false;
    }
    body.clear();
    Writer w{body};
    w.u32(0x73);
    w.u32(0);  // geometries
    w.u32(0);  // skins
    w.u32(0);  // simple animation
    w.u32(0);  // its groups
    w.u32(static_cast<std::uint32_t>(u.nodes.size()));
    for (const auto& n : u.nodes) { w.floats(n.rotation); w.floats(n.position); w.floats(n.scale); }
    for (const auto& n : u.nodes) w.floats(n.world);
    for (const auto& n : u.nodes) { w.u16(n.parent_type); w.u16(n.parent); }
    for (const auto& n : u.nodes) w.u32(n.name);
    w.u32(0);  // the node list of 8-byte entries
    w.u32(0);  // mesh objects
    w.u32(static_cast<std::uint32_t>(u.actors.size()));
    for (const auto& a : u.actors) {
        w.u32(a.name);
        w.u32(a.actor_template);
        w.u32(a.node);
        w.f32(a.mass);
        w.u32(static_cast<std::uint32_t>(a.shapes.size()));
        for (const auto& s : a.shapes) {
            w.u32(s.type);
            w.u32(s.material);
            w.u32(s.shape_template);
            w.floats(s.local);
            w.bytes(s.cooked);
            w.bytes(s.second);
            w.u32(s.node);
            switch (s.type) {
                case 0: w.f32(s.radius); break;
                case 1: w.floats(s.half_extents); break;
                case 2: w.f32(s.radius); w.f32(s.height); break;
                case 5:
                    w.u32(s.columns);
                    w.u32(s.rows);
                    w.f32(s.height_scale);
                    w.f32(s.row_scale);
                    w.f32(s.column_scale);
                    w.u8(s.multiple_materials ? 1 : 0);
                    break;
                default: break;
            }
        }
        for (const auto c : a.callbacks) w.u32(c);
        w.u8(a.enabled ? 1 : 0);
        for (const auto b : a.flag_bytes) w.u8(b);
    }
    w.u32(0);  // second actor list
    w.u32(0);  // u32 list
    w.u32(0);  // cameras
    w.u32(0);  // lights
    w.u32(0);  // volumes
    w.u32(0);  // LOD objects
    w.u32(static_cast<std::uint32_t>(u.terrains.size()));
    for (const auto& t : u.terrains) {
        if (t.layers.size() > 16) { error = "a terrain has more than 16 layers"; return false; }
        w.u32(t.name);
        w.u32(t.node);
        w.floats(t.bounds);
        w.u32(t.flags);
        w.u32(static_cast<std::uint32_t>(t.nodes.size()));
        for (const auto& n : t.nodes) {
            w.f32(n.height_min);
            w.f32(n.height_max);
            for (const auto c : n.children) w.u32(c);
        }
        w.u32(t.base_material);
        w.u32(static_cast<std::uint32_t>(t.layers.size()));
        for (const auto& l : t.layers) {
            w.u64(l.map);
            w.u32(l.resolution);
            w.u32(l.type);
            for (std::size_t i = 0; i < 4; ++i) {
                w.u32(l.materials[i]);
                w.u32(l.decoration_counts[i]);
                for (const auto& d : l.decorations[i]) { w.u64(d.unit); w.u32(d.mesh); w.f32(d.density); }
            }
            w.u32(static_cast<std::uint32_t>(l.contexts.size()));
            for (const auto& c : l.contexts)
                for (const auto v : c) w.u32(v);
        }
        w.bytes(t.height_data);
    }
    w.u32(0);  // unknown
    w.u32(0);  // joints
    w.u32(0);  // movers
    w.u32(0);  // unknown
    w.u8(u.animation_blender);
    w.u32(0);  // state machine
    w.bytes(u.script_data);
    w.u32(0);  // visibility groups
    w.bytes(u.flow);
    w.bytes(u.flow_dynamic_data);
    w.bytes(u.triangle_finder);
    w.bytes(u.physics_scene);
    w.u64(u.default_material);
    w.u32(static_cast<std::uint32_t>(u.materials.size()));
    for (const auto& [slot, material] : u.materials) { w.u32(slot); w.u64(material); }
    w.u32(0);  // material bindings 2
    w.u32(0);  // material layers
    w.u32(0);
    w.u32(0);
    w.u64(0);  // skeleton
    w.u32(0);
    return true;
}

}  // namespace dtglb::stingray::terrain
