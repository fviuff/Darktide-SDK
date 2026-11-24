#include "stingray/unit/unit_reader.h"

#include "stingray/cooked_resource.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace dtglb::stingray::unit {
namespace {

constexpr std::uint32_t kUnitVersion = 0x73;
constexpr std::uint32_t kMaxCount = 1u << 24;

// A bounds-checked little-endian cursor; any read past the end sets failed and returns zeros.
struct Reader {
    const std::uint8_t* data;
    std::size_t size, at = 0;
    bool failed = false;

    bool take(std::size_t n) {
        if (failed || n > size - at) { failed = true; return false; }
        return true;
    }
    template <typename T> T get() {
        T value{};
        if (take(sizeof(T))) { std::memcpy(&value, data + at, sizeof(T)); at += sizeof(T); }
        return value;
    }
    std::uint32_t u32() { return get<std::uint32_t>(); }
    std::uint64_t u64() { return get<std::uint64_t>(); }
    float f32() { return get<float>(); }
    void skip(std::size_t n) { if (take(n)) at += n; }
    std::uint32_t count() {
        const auto n = u32();
        if (n > kMaxCount) failed = true;
        return failed ? 0 : n;
    }
    std::vector<std::uint8_t> bytes() {
        const auto n = count();
        if (!take(n)) return {};
        std::vector<std::uint8_t> out(data + at, data + at + n);
        at += n;
        return out;
    }
    void skip_bytes() { skip(count()); }
    std::vector<std::uint32_t> u32s() {
        const auto n = count();
        std::vector<std::uint32_t> out;
        if (!take(static_cast<std::size_t>(n) * 4)) return out;
        out.resize(n);
        std::memcpy(out.data(), data + at, static_cast<std::size_t>(n) * 4);
        at += static_cast<std::size_t>(n) * 4;
        return out;
    }
    template <std::size_t N> std::array<float, N> floats() {
        std::array<float, N> out{};
        for (auto& value : out) value = f32();
        return out;
    }
};

bool read_geometry(Reader& r, UnitGeometry& g) {
    r.u32();  // MeshGeometry version
    const auto streams = r.count();
    for (std::uint32_t i = 0; i < streams && !r.failed; ++i) {
        g.streams.push_back(r.bytes());
        r.u32();  // validity
        r.u32();  // stream type
        const auto vertices = r.u32();
        if (i == 0) g.vertices = vertices;
        g.strides.push_back(r.u32());
    }
    const auto channels = r.count();
    for (std::uint32_t i = 0; i < channels && !r.failed; ++i) {
        UnitChannel c;
        c.component = r.u32();
        c.type = r.u32();
        c.set = r.u32();
        c.stream = r.u32();
        r.skip(1);  // instance data flag
        g.channels.push_back(c);
    }
    g.index_words[0] = r.u32();  // index validity
    g.index_words[1] = r.u32();  // index stream type
    g.index32 = r.u32() == 1;
    g.index_count = r.u32();
    g.indices = r.bytes();
    const auto batches = r.count();
    for (std::uint32_t i = 0; i < batches && !r.failed; ++i) {
        UnitBatch b;
        b.material = r.u32();
        b.first_triangle = r.u32();
        b.triangles = r.u32();
        b.bone_set = r.u32();
        g.batches.push_back(b);
    }
    g.bounds = r.floats<10>();
    g.materials = r.u32s();
    r.u32();
    return !r.failed;
}

// The sections between the mesh objects and the state machine; read only to get past them.
bool read_actor(Reader& r, UnitActor& actor, std::string& error) {
    actor.name = r.u32();
    actor.actor_template = r.u32();
    actor.node = r.u32();
    actor.mass = r.f32();
    actor.shapes.resize(r.count());
    for (auto& shape : actor.shapes) {
        const auto type = shape.type = r.u32();
        // as stingray::ActorResource::Shape::serialize (FUN_1402b52f0): material, shape template, local matrix, two
        // byte arrays (the cooked PhysX shape; type 7's own data, e.g. a cloth), shape node, then per type
        shape.material = r.u32();
        shape.shape_template = r.u32();
        shape.local = r.floats<16>();
        shape.cooked = r.bytes();
        shape.second = r.bytes();
        shape.node = r.u32();
        switch (type) {
            case 0: shape.radius = r.f32(); break;                                   // sphere: radius
            case 1: shape.half_extents = r.floats<3>(); break;                       // box: half extents
            case 2: shape.radius = r.f32(); shape.height = r.f32(); break;           // capsule: radius, height
            case 3: case 4: case 7: break;           // mesh, convex, type 7: all in the byte arrays
            case 5:                                  // height field: the samples are the first byte array
                shape.columns = r.u32();
                shape.rows = r.u32();
                shape.height_scale = r.f32();
                shape.row_scale = r.f32();
                shape.column_scale = r.f32();
                shape.multiple_materials = r.get<std::uint8_t>() != 0;
                break;
            default: error = "UNIT actor shape type " + std::to_string(type) + " is not known to the engine"; return false;
        }
        if (r.failed) break;
    }
    for (auto& c : actor.callbacks) c = r.u32();
    actor.enabled = r.get<std::uint8_t>() != 0;
    for (auto& b : actor.flag_bytes) b = r.get<std::uint8_t>();
    if (r.failed) error = "UNIT ends early (in an actor)";
    return !r.failed;
}

bool skip_joint(Reader& r, std::string& error) {
    r.skip(4 + 1 + 8 + 8 * 12 + 12);  // name, enabled, actors, normals/axes/anchors, max force/torque, flags
    const auto type = r.u32();
    switch (type) {
        case 1: r.skip(4 * 2 + 4 * 2 + 4 * 3); break;            // revolute: limits, motor, spring
        case 5: r.skip(4 * 2 + 4 * 3); break;                    // distance: min/max, spring
        case 0: r.skip(12 + 4 * 3 + 3 * 12); break;              // spherical: axis, limits, three springs
        case 8: {                                                 // D6
            constexpr std::size_t sub1 = 16, sub2 = 13, sub3 = 20;
            r.skip(4 + 12 + sub1 + 12 + sub1 + 8 + 6 * sub2 + 9 * 4 + sub1 + sub3 + 4 + sub3 + 8 + sub3 + 16 + 4 + 3);
            break;
        }
        default: error = "UNIT joint type " + std::to_string(type) + " is not known"; return false;
    }
    if (r.failed) error = "UNIT ends early (in a joint)";
    return !r.failed;
}

// as stingray::TerrainData::serialize: name, node, bounding volume, flags, quadtree nodes, base material, layers {map,
// resolution, type, per blend channel {material, decoration count, 4 decoration units}, surface contexts}, height data
bool read_terrain(Reader& r, UnitTerrain& t, std::string& error) {
    t.name = r.u32();
    t.node = r.u32();
    t.bounds = r.floats<10>();
    t.flags = r.u32();
    t.nodes.resize(r.count());
    for (auto& n : t.nodes) {
        n.height_min = r.f32();
        n.height_max = r.f32();
        for (auto& c : n.children) c = r.u32();
    }
    t.base_material = r.u32();
    const auto layers = r.count();
    if (layers > 16) { error = "UNIT terrain has more than 16 layers"; return false; }
    t.layers.resize(layers);
    for (auto& l : t.layers) {
        l.map = r.u64();
        l.resolution = r.u32();
        l.type = r.u32();
        for (std::size_t i = 0; i < 4; ++i) {
            l.materials[i] = r.u32();
            l.decoration_counts[i] = r.u32();
            for (auto& d : l.decorations[i]) {
                d.unit = r.u64();
                d.mesh = r.u32();
                d.density = r.f32();
            }
        }
        l.contexts.resize(r.count());
        for (auto& c : l.contexts)
            for (auto& v : c) v = r.u32();
        if (r.failed) break;
    }
    t.height_data = r.bytes();
    if (r.failed) error = "UNIT ends early (in a terrain)";
    return !r.failed;
}

bool require_empty(Reader& r, const char* what, std::string& error) {
    if (r.u32() != 0) { error = std::string("UNIT ") + what + " section is not empty (not read yet)"; return false; }
    if (r.failed) error = std::string("UNIT ends early (at ") + what + ", " + std::to_string(r.size) + " bytes)";
    return !r.failed;
}

float half(std::uint16_t h) {
    const std::uint32_t sign = (h & 0x8000u) << 16, exponent = (h >> 10) & 0x1f, mantissa = h & 0x3ff;
    std::uint32_t bits;
    if (exponent == 0) {
        if (mantissa == 0) bits = sign;
        else {
            float value = std::ldexp(static_cast<float>(mantissa), -24);
            return sign ? -value : value;
        }
    } else if (exponent == 31) {
        bits = sign | 0x7f800000u | (mantissa << 13);
    } else {
        bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
    }
    float out;
    std::memcpy(&out, &bits, 4);
    return out;
}

std::size_t channel_size(std::uint32_t type) {
    switch (type) {  // stingray ChannelType
        case kFloat2: case kHalf4: return 8;
        case kFloat3: return 12;
        case 3: return 16;  // float4
        case 14: return 2;  // half1
        case 16: return 6;  // half3
        default: return 4;  // float1, half2, the byte quadruples
    }
}

std::array<float, 3> oct_decode(float u, float v) {
    float x = u * 2.0f - 1.0f, y = v * 2.0f - 1.0f;
    const float z = 1.0f - std::fabs(x) - std::fabs(y);
    if (z < 0.0f) {
        const float ox = (1.0f - std::fabs(y)) * (x >= 0.0f ? 1.0f : -1.0f);
        const float oy = (1.0f - std::fabs(x)) * (y >= 0.0f ? 1.0f : -1.0f);
        x = ox;
        y = oy;
    }
    const float length = std::sqrt(x * x + y * y + z * z);
    return length > 0.0f ? std::array<float, 3>{x / length, y / length, z / length} : std::array<float, 3>{0, 0, 1};
}

}  // namespace

bool read_unit(const std::vector<std::uint8_t>& file, Unit& unit, std::string& error) {
    unit = {};
    std::vector<std::uint8_t> body;
    std::uint32_t first = 0;
    if (file.size() >= 4) std::memcpy(&first, file.data(), 4);
    if (first == kUnitVersion) {
        body = file;
    } else if (!parse_cooked_resource_envelope(file, "unit", body, unit.stream_name, error)) {
        return false;
    }
    Reader r{body.data(), body.size()};
    if (r.u32() != kUnitVersion) { error = "not a UNIT v115"; return false; }

    unit.geometries.resize(r.count());
    for (auto& g : unit.geometries) if (!read_geometry(r, g)) break;
    unit.skins.resize(r.count());
    for (auto& skin : unit.skins) {
        const auto matrices = r.count();
        for (std::uint32_t i = 0; i < matrices && !r.failed; ++i) skin.inverse_binds.push_back(r.floats<16>());
        skin.nodes = r.u32s();
        const auto sets = r.count();
        for (std::uint32_t i = 0; i < sets && !r.failed; ++i) skin.bone_sets.push_back(r.u32s());
    }
    r.skip_bytes();  // simple animation
    for (auto groups = r.count(), i = 0u; i < groups && !r.failed; ++i) { r.u32(); r.u32s(); }

    unit.nodes.resize(r.count());
    for (auto& n : unit.nodes) { n.rotation = r.floats<9>(); n.position = r.floats<3>(); n.scale = r.floats<3>(); }
    for (auto& n : unit.nodes) n.world = r.floats<16>();
    for (auto& n : unit.nodes) { n.parent_type = r.get<std::uint16_t>(); n.parent = r.get<std::uint16_t>(); }
    for (auto& n : unit.nodes) n.name = r.u32();
    if (const auto n = r.count()) { unit.unkept_sections = true; r.skip(static_cast<std::size_t>(n) * 8); }

    unit.meshes.resize(r.count());
    for (auto& m : unit.meshes) {
        m.name = r.u32();
        m.node = r.u32();
        m.geometry = r.u32();
        m.skin = r.u32();
        m.flags = r.u32();
        r.skip(8);  // kind, enabled
        m.bounds = r.floats<10>();
        r.u32();
    }

    unit.actors.resize(r.count());
    for (auto& actor : unit.actors) if (!r.failed && !read_actor(r, actor, error)) return false;
    // a second list in the same record format (a cloth's)
    for (auto actors = r.count(), i = 0u; i < actors && !r.failed; ++i) {
        unit.unkept_sections = true;
        UnitActor cloth;
        if (!read_actor(r, cloth, error)) return false;
    }
    if (!r.u32s().empty()) unit.unkept_sections = true;  // a u32 list (one entry, 0xffffffff, in the unit with a second actor list)
    unit.cameras.resize(r.count());
    for (auto& camera : unit.cameras) {
        camera.name = r.u32();
        camera.node = r.u32();
        camera.near_range = r.f32();
        camera.far_range = r.f32();
        camera.vertical_fov = r.f32();
        r.skip(12);  // a float, two u16, u32
    }
    unit.lights.resize(r.count());
    for (auto& light : unit.lights) {
        light.name = r.u32();
        light.node = r.u32();
        for (auto& c : light.colour) c = r.f32();
        light.temperature = r.f32();
        light.intensity = r.f32();
        light.volumetric = r.f32();
        light.particle = r.f32();
        for (auto& p : light.parameters) p = r.f32();
        light.flags = r.u32();
        light.type = r.u32();
        light.material = r.u64();
        light.ies = r.u64();
        light.shadow_resolution = r.u32();
        r.skip(160 - 88);  // the GI probe block (constant on every lamp that isn't a probe)
    }
    if (!r.bytes().empty()) unit.unkept_sections = true;  // volumes

    unit.lods.resize(r.count());
    for (auto& lod : unit.lods) {
        lod.name = r.u32();
        r.u64();  // source unit
        lod.node = r.u32();
        lod.steps.resize(r.count());
        for (auto& step : lod.steps) {
            step.height_from = r.f32();
            step.height_to = r.f32();
            step.meshes = r.u32s();
            step.stream_offset = r.u32();
            r.u32();  // step index
        }
        lod.bounds = r.floats<10>();
        r.skip(8);  // flags, word
        lod.stream_mesh_order = r.u32s();
        r.skip(4 + 1);
    }
    unit.terrains.resize(r.count());
    for (auto& terrain : unit.terrains) if (!r.failed && !read_terrain(r, terrain, error)) return false;
    if (!require_empty(r, "unknown (after terrains)", error)) return false;
    for (auto joints = r.count(), i = 0u; i < joints && !r.failed; ++i) if (!skip_joint(r, error)) return false;
    r.skip(static_cast<std::size_t>(r.count()) * 28);  // movers
    if (!require_empty(r, "unknown (after movers)", error)) return false;

    unit.animation_blender = r.get<std::uint8_t>();
    {
        const auto name = r.bytes();
        unit.state_machine.assign(name.begin(), name.end());
    }
    unit.script_data = r.bytes();  // script data (dynamic data, Unit.get_data)
    for (auto groups = r.count(), i = 0u; i < groups && !r.failed; ++i) {  // visibility groups: name, mesh indices
        UnitVisibilityGroup group;
        group.name = r.u32();
        group.meshes = r.u32s();
        unit.visibility_groups.push_back(std::move(group));
    }
    unit.flow = r.bytes();
    unit.flow_dynamic_data = r.bytes();
    unit.triangle_finder = r.bytes();
    unit.physics_scene = r.bytes();
    unit.default_material = r.u64();
    for (auto bindings = r.count(), i = 0u; i < bindings && !r.failed; ++i) {
        const auto slot = r.u32();
        unit.materials.emplace_back(slot, r.u64());
    }
    if (!require_empty(r, "material bindings 2", error)) return false;
    for (auto layers = r.count(), i = 0u; i < layers && !r.failed; ++i) { r.skip(12); r.u32s(); }  // material layers
    if (!require_empty(r, "unknown (after material layers)", error) || !require_empty(r, "unknown (before skeleton)", error))
        return false;
    unit.skeleton = r.u64();
    for (auto entries = r.count(), i = 0u; i < entries && !r.failed; ++i) {
        unit.unkept_sections = true;
        r.skip(static_cast<std::size_t>(r.count()) * 8);
        r.skip(12 + 40 + 12);
    }
    if (r.failed) { error = "UNIT ends early (" + std::to_string(body.size()) + " bytes)"; return false; }
    // a UNIT embedded in a level is padded to 4 bytes
    const auto padded = body.size() - r.at < 4 && std::all_of(body.begin() + r.at, body.end(), [](std::uint8_t b) { return b == 0; });
    if (r.at != body.size() && !padded) {
        error = "UNIT has " + std::to_string(body.size() - r.at) + " bytes left after the last section";
        return false;
    }
    for (auto& m : unit.meshes) {
        if (m.skin) m.skin = m.skin > unit.geometries.size() ? m.skin - static_cast<std::uint32_t>(unit.geometries.size()) : ~0u;
        if (m.geometry > unit.geometries.size() || m.skin > unit.skins.size() || m.node >= unit.nodes.size()) {
            error = "UNIT mesh object refers past its geometry, skin or node lists";
            return false;
        }
    }
    return true;
}

bool apply_unit_stream(Unit& unit, const std::vector<std::uint8_t>& stream, std::string& error) {
    Reader r{stream.data(), stream.size()};
    for (const auto& lod : unit.lods) {
        for (const auto mesh : lod.stream_mesh_order) {
            if (mesh >= unit.meshes.size() || unit.meshes[mesh].geometry == 0) {
                error = "stream order names a mesh the unit doesn't have";
                return false;
            }
            auto& g = unit.geometries[unit.meshes[mesh].geometry - 1];
            const auto streams = r.count();
            if (streams != g.streams.size()) {
                error = "stream record has " + std::to_string(streams) + " vertex streams, the geometry " +
                        std::to_string(g.streams.size());
                return false;
            }
            // only what the unit leaves out is in the file: meshes with surface queries keep positions and indices
            for (auto& s : g.streams)
                if (s.empty()) s = r.bytes();
            if (g.indices.empty()) g.indices = r.bytes();
        }
    }
    if (r.failed) { error = "stream file ends early"; return false; }
    if (r.at != stream.size()) { error = "stream file has data after the last record"; return false; }
    return true;
}

bool decode_geometry(const Unit& unit, const UnitGeometry& g, DecodedGeometry& out, std::string& error) {
    out = {};
    const std::size_t n = g.vertices;
    // channels sharing a stream lie one after the other in each vertex
    const auto offset_in_vertex = [&](const UnitChannel& c) {
        std::size_t offset = 0;
        for (const auto& other : g.channels) {
            if (&other == &c) break;
            if (other.stream == c.stream) offset += channel_size(other.type);
        }
        return offset;
    };
    const auto element = [&](const UnitChannel& c, std::size_t i, std::size_t byte) -> const std::uint8_t* {
        return g.streams[c.stream].data() + i * g.strides[c.stream] + offset_in_vertex(c) + byte;
    };
    const auto h = [&](const UnitChannel& c, std::size_t i, std::size_t k) {
        std::uint16_t v;
        std::memcpy(&v, element(c, i, k * 2), 2);
        return half(v);
    };
    const auto f = [&](const UnitChannel& c, std::size_t i, std::size_t k) {
        float v;
        std::memcpy(&v, element(c, i, k * 4), 4);
        return v;
    };
    const auto number = [&](const UnitChannel& c, std::size_t i, std::size_t k) { return c.type <= kFloat3 ? f(c, i, k) : h(c, i, k); };
    const UnitChannel* normal_channel = nullptr;
    const UnitChannel* frame_channel = nullptr;
    const UnitChannel* index_channel = nullptr;
    const UnitChannel* weight_channel = nullptr;
    for (const auto& c : g.channels) {
        if (c.stream >= g.streams.size()) { error = "vertex channel refers past the streams"; return false; }
        const auto& s = g.streams[c.stream];
        if (s.size() < n * g.strides[c.stream]) { error = "vertex stream is missing or short (streamed geometry not applied?)"; return false; }
        switch (c.component) {
            case kPosition:
                out.positions.resize(n * 3);
                for (std::size_t i = 0; i < n; ++i)
                    for (std::size_t k = 0; k < 3; ++k)
                        out.positions[i * 3 + k] = number(c, i, k);
                break;
            case kNormal: normal_channel = &c; break;
            case kTangentFrame: frame_channel = &c; break;
            case kTexcoord:
                if (out.uvs.size() <= c.set) out.uvs.resize(c.set + 1);
                out.uvs[c.set].resize(n * 2);
                for (std::size_t i = 0; i < n; ++i) { out.uvs[c.set][i * 2] = number(c, i, 0); out.uvs[c.set][i * 2 + 1] = number(c, i, 1); }
                break;
            case kColor:
                if (c.set != 0) break;
                out.colors.resize(n * 4);
                for (std::size_t i = 0; i < n; ++i)
                    for (std::size_t k = 0; k < 4; ++k)
                        out.colors[i * 4 + k] = c.type == kFloat3 ? (k < 3 ? f(c, i, k) : 1.0f) : element(c, i, k)[0] / 255.0f;
                break;
            case kBlendIndices: index_channel = &c; break;
            case kBlendWeights: weight_channel = &c; break;
            default: break;  // component 9 (8-byte type 8): not used for display
        }
    }
    if (out.positions.empty()) { error = "geometry has no positions"; return false; }
    if (normal_channel) {
        out.normals.resize(n * 3);
        for (std::size_t i = 0; i < n; ++i) {
            const auto v = oct_decode(number(*normal_channel, i, 0), number(*normal_channel, i, 1));
            for (std::size_t k = 0; k < 3; ++k) out.normals[i * 3 + k] = v[k];
        }
    }
    if (frame_channel && normal_channel) {
        // tangent and the negated bitangent, both octahedral; handedness = side of the bitangent against n x t
        out.tangents.resize(n * 4);
        for (std::size_t i = 0; i < n; ++i) {
            const auto t = oct_decode(h(*frame_channel, i, 0), h(*frame_channel, i, 1));
            const auto nb = oct_decode(h(*frame_channel, i, 2), h(*frame_channel, i, 3));
            const float* nn = &out.normals[i * 3];
            const float cx = nn[1] * t[2] - nn[2] * t[1], cy = nn[2] * t[0] - nn[0] * t[2], cz = nn[0] * t[1] - nn[1] * t[0];
            const float side = -(cx * nb[0] + cy * nb[1] + cz * nb[2]);
            out.tangents[i * 4] = t[0];
            out.tangents[i * 4 + 1] = t[1];
            out.tangents[i * 4 + 2] = t[2];
            out.tangents[i * 4 + 3] = side < 0.0f ? -1.0f : 1.0f;
        }
    }

    const std::size_t index_size = g.index32 ? 4 : 2;
    if (g.indices.size() < static_cast<std::size_t>(g.index_count) * index_size) { error = "index stream is missing or short"; return false; }
    out.indices.resize(g.index_count);
    for (std::size_t i = 0; i < g.index_count; ++i) {
        std::uint32_t v = 0;
        std::memcpy(&v, g.indices.data() + i * index_size, index_size);
        if (v >= n) { error = "index past the vertex count"; return false; }
        out.indices[i] = v;
    }

    if (index_channel && weight_channel) {
        // a vertex's blend indices index its batch's bone set, whose entries index the skin
        const UnitSkin* skin = nullptr;
        for (const auto& m : unit.meshes)
            if (m.skin && m.geometry && &unit.geometries[m.geometry - 1] == &g) { skin = &unit.skins[m.skin - 1]; break; }
        out.joints.assign(n * 4, 0);
        out.weights.resize(n * 4);
        std::vector<std::uint32_t> vertex_set(n, 0);
        for (const auto& b : g.batches)
            for (std::size_t t = b.first_triangle * 3; t < (static_cast<std::size_t>(b.first_triangle) + b.triangles) * 3 && t < out.indices.size(); ++t)
                vertex_set[out.indices[t]] = b.bone_set;
        for (std::size_t i = 0; i < n; ++i)
            for (std::size_t k = 0; k < 4; ++k) {
                out.weights[i * 4 + k] = h(*weight_channel, i, k);
                std::uint32_t local = element(*index_channel, i, k)[0];
                if (skin && vertex_set[i] < skin->bone_sets.size() && local < skin->bone_sets[vertex_set[i]].size())
                    local = skin->bone_sets[vertex_set[i]][local];
                out.joints[i * 4 + k] = local;
            }
    }
    return true;
}

}  // namespace dtglb::stingray::unit
