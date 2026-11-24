#include "stingray/level/level_document.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <random>
#include <unordered_map>

#include "stingray/cooked_resource.h"
#include "stingray/murmur_hash.h"

namespace dtglb::stingray::level {

namespace {

constexpr std::uint32_t kVersion = 125;
constexpr std::uint32_t kMaxCount = 1u << 22;

struct In {
    const Bytes& b;
    bool failed = false;
    std::uint32_t u32(std::size_t at) {
        std::uint32_t v = 0;
        if (at + 4 > b.size()) { failed = true; return 0; }
        std::memcpy(&v, b.data() + at, 4);
        return v;
    }
    std::uint64_t u64(std::size_t at) { return u32(at) | (static_cast<std::uint64_t>(u32(at + 4)) << 32); }
    Bytes bytes(std::size_t at, std::size_t size) {
        if (at > b.size() || size > b.size() - at) { failed = true; return {}; }
        return {b.begin() + static_cast<std::ptrdiff_t>(at), b.begin() + static_cast<std::ptrdiff_t>(at + size)};
    }
    template <std::size_t N> std::array<std::uint8_t, N> fixed(std::size_t at) {
        std::array<std::uint8_t, N> out{};
        if (at + N > b.size()) { failed = true; return out; }
        std::memcpy(out.data(), b.data() + at, N);
        return out;
    }
    float f32(std::size_t at) {
        const auto v = u32(at);
        float f;
        std::memcpy(&f, &v, 4);
        return f;
    }
    // dynamic data: u32 count; per entry u32 n, n x u32 (path), u32 type, value (1 u32, 2 f32, 3 u32 size + bytes)
    std::size_t dynamic_size(std::size_t at) {
        std::size_t o = at;
        const auto count = u32(o);
        o += 4;
        for (std::uint32_t i = 0; i < count && !failed; ++i) {
            o += 4 + 4 * static_cast<std::size_t>(u32(o));
            const auto type = u32(o);
            o += 4;
            if (type == 1 || type == 2) o += 4;
            else if (type == 3) o += 4 + ((static_cast<std::size_t>(u32(o)) + 3) & ~std::size_t{3});
            else failed = true;
        }
        return o - at;
    }
    std::vector<std::uint32_t> list(std::size_t at) {  // {u32 n, u32 offset} -> n u32
        std::vector<std::uint32_t> out(u32(at));
        if (out.size() > kMaxCount) { failed = true; return {}; }
        const auto from = u32(at + 4);
        for (std::size_t i = 0; i < out.size(); ++i) out[i] = u32(from + 4 * i);
        return out;
    }
};

struct Out {
    Bytes b;
    void align(std::size_t n) { b.resize((b.size() + n - 1) / n * n, 0); }
    std::uint32_t at() const { return static_cast<std::uint32_t>(b.size()); }
    void u32(std::uint32_t v) { const auto o = b.size(); b.resize(o + 4); std::memcpy(b.data() + o, &v, 4); }
    void u64(std::uint64_t v) { u32(static_cast<std::uint32_t>(v)); u32(static_cast<std::uint32_t>(v >> 32)); }
    void f32(float f) { std::uint32_t v; std::memcpy(&v, &f, 4); u32(v); }
    void put(const std::uint8_t* d, std::size_t n) { b.insert(b.end(), d, d + n); }
    void put(const Bytes& d) { b.insert(b.end(), d.begin(), d.end()); }
    void set(std::size_t at, std::uint32_t v) { std::memcpy(b.data() + at, &v, 4); }
};

}  // namespace

bool read_level_document(const std::vector<std::uint8_t>& file, LevelDocument& out, std::string& error) {
    Bytes body;
    std::string stream, envelope_error;
    if (!parse_cooked_resource_envelope(file, "level", body, stream, envelope_error)) body = file;
    In r{body};
    if (body.size() < 0xc0 || r.u32(4) != kVersion) { error = "not a version 125 level"; return false; }
    LevelDocument d;
    const auto count = r.u32(0x08);
    if (count > kMaxCount) { error = "level unit count is implausible"; return false; }
    d.background_units = r.u32(0x0c);
    d.units.resize(count);
    for (std::uint32_t i = 0; i < count && !r.failed; ++i) {
        auto& u = d.units[i];
        const std::size_t at = 0xc0 + static_cast<std::size_t>(i) * 0x60;
        u.id = r.u64(at);
        u.name = r.u64(at + 0x08);
        u.resource = r.u64(at + 0x10);
        u.reserved = r.u64(at + 0x18);
        for (int k = 0; k < 3; ++k) u.position[k] = r.f32(at + 0x20 + 4 * k);
        for (int k = 0; k < 4; ++k) u.rotation[k] = r.f32(at + 0x2c + 4 * k);
        for (int k = 0; k < 3; ++k) u.scale[k] = r.f32(at + 0x3c + 4 * k);
        const auto poses = r.u32(at + 0x48), poses_at = r.u32(at + 0x4c);
        if (poses > kMaxCount) { error = "unit node poses are implausible"; return false; }
        for (std::uint32_t k = 0; k < poses; ++k) u.node_poses.push_back(r.fixed<48>(poses_at + 48 * k));
        u.blob = r.bytes(r.u32(at + 0x54), r.u32(at + 0x50));
        u.tail = r.fixed<8>(at + 0x58);
        if (const auto data = r.u32(r.u32(0x10) + 4 * i)) u.data = r.bytes(data, r.dynamic_size(data));
        u.object_sets = r.list(r.u32(0x7c) + 8 * i);
    }
    {
        std::size_t at = r.u32(0xa4);
        const auto overrides = r.u32(at);
        at += 4;
        for (std::uint32_t i = 0; i < overrides && !r.failed; ++i) {
            const auto unit = r.u32(at), n = r.u32(at + 4);
            at += 8;
            if (unit >= count || n > kMaxCount) { error = "material override for a unit the level hasn't"; return false; }
            for (std::uint32_t k = 0; k < n; ++k, at += 12) d.units[unit].materials.emplace_back(r.u32(at), r.u64(at + 4));
        }
    }
    d.level_data = r.bytes(r.u32(0x48), r.dynamic_size(r.u32(0x48)));
    {
        const std::size_t at = r.u32(0x14);
        std::size_t end = at + 4;
        for (std::uint32_t i = 0, n = r.u32(at); i < n && !r.failed; ++i) {
            const std::size_t size = r.u32(end + 0x12 * 4) == 9 ? 0xa4 : 0x60;
            d.lights.push_back(r.bytes(end, size));
            end += size;
        }
    }
    for (std::uint32_t i = 0, count = r.u32(r.u32(0x18)); i < count; ++i)
        d.cameras.push_back(r.bytes(r.u32(0x18) + 4 + 0x30 * static_cast<std::size_t>(i), 0x30));
    d.prototype_unit = r.bytes(r.u32(0x1c), r.u32(0x20));
    // the flow blob's own size is its header word 21; its dynamic data follows 8-byte aligned
    if (r.u32(0x28) > r.u32(0x24)) d.flow = r.bytes(r.u32(0x24), r.u32(r.u32(0x24) + 0x54));
    d.flow_data = r.bytes(r.u32(0x28), r.u32(r.u32(0x24) + 0x58));
    for (std::uint32_t i = 0, n = r.u32(0x2c); i < n && !r.failed; ++i) {
        d.triggers.push_back(r.fixed<0x58>(r.u32(0x30) + 0x58 * i));
        d.trigger_object_sets.push_back(r.list(r.u32(0x84) + 8 * i));
    }
    for (std::uint32_t i = 0, n = r.u32(0x34); i < n && !r.failed; ++i)
        d.unit_nodes.push_back({r.u32(r.u32(0x38) + 12 * i), r.u32(r.u32(0x38) + 12 * i + 4), r.u32(r.u32(0x38) + 12 * i + 8)});
    for (std::uint32_t i = 0, n = r.u32(0x3c); i < n && !r.failed; ++i)
        d.reference_nodes.push_back({r.u32(r.u32(0x40) + 12 * i), r.u32(r.u32(0x40) + 12 * i + 4), r.u32(r.u32(0x40) + 12 * i + 8)});
    d.navigation_mesh = r.fixed<12>(r.u32(0x44));
    for (std::uint32_t i = 0, n = r.u32(0x50); i < n && !r.failed; ++i) {
        d.effects.push_back(r.fixed<0x38>(r.u32(0x54) + 0x38 * i));
        d.effect_object_sets.push_back(r.list(r.u32(0x80) + 8 * i));
    }
    {
        std::size_t at = r.u32(0x5c);
        for (std::uint32_t i = 0, n = r.u32(0x58); i < n && !r.failed; ++i) {
            DocumentSpline spline;
            spline.name = r.u32(at);
            const auto points = r.u32(at + 4);
            if (points > kMaxCount) { error = "spline point count is implausible"; return false; }
            for (std::uint32_t k = 0; k < points; ++k)
                spline.points.push_back({r.f32(at + 8 + 12 * k), r.f32(at + 12 + 12 * k), r.f32(at + 16 + 12 * k)});
            at += 8 + 12 * static_cast<std::size_t>(points);
            d.splines.push_back(std::move(spline));
        }
    }
    {
        std::size_t at = r.u32(0x64);
        std::uint32_t matrices = 0;
        for (std::uint32_t i = 0, n = r.u32(0x60); i < n && !r.failed; ++i) {
            const auto size = r.u32(at + 0x3c);
            if (size < 0x40) { error = "volume record is too short"; return false; }
            matrices = std::max(matrices, r.u32(at) + 1);
            d.volumes.push_back({r.bytes(at, size)});
            at += size;
        }
        for (std::uint32_t i = 0; i < matrices && !r.failed; ++i) d.volume_matrices.push_back(r.fixed<0x80>(r.u32(0x68) + 0x80 * i));
    }
    for (std::uint32_t i = 0, n = r.u32(0x6c); i < n && !r.failed; ++i)
        d.volume_units.push_back(r.bytes(r.u32(r.u32(0x70) + 8 * i), r.u32(r.u32(0x70) + 8 * i + 4)));
    {
        std::size_t at = r.u32(0x8c);
        for (std::uint32_t i = 0, n = r.u32(0x88); i < n && !r.failed; ++i) {
            const auto size = r.u32(at + 0x38);
            if (size < 0x40) { error = "nested level record is too short"; return false; }
            DocumentNested nested{r.bytes(at, size), {}};
            if (const auto data = r.u32(r.u32(0x90) + 4 * i)) nested.data = r.bytes(data, r.dynamic_size(data));
            d.nested.push_back(std::move(nested));
            at += size;
        }
    }
    for (std::uint32_t i = 0, n = r.u32(0x94); i < n && !r.failed; ++i) d.nav_data.push_back(r.fixed<16>(r.u32(0x98) + 16 * i));
    if (const std::size_t at = r.u32(0x9c)) {
        std::size_t end = at + 4 + 48 * static_cast<std::size_t>(r.u32(at));
        for (std::uint32_t i = 0, n = r.u32(at); i < n && !r.failed; ++i)
            end = std::max(end, at + r.u32(at + 4 + 48 * i + 44) + 32 * static_cast<std::size_t>(r.u32(at + 4 + 48 * i + 40)));
        d.scatter = r.bytes(at, end - at);
    }
    for (std::uint32_t i = 0, n = r.u32(0x74); i < n && !r.failed; ++i) {
        std::size_t at = r.u32(r.u32(0x78) + 4 * i);
        std::string name;
        while (at < body.size() && body[at]) name.push_back(static_cast<char>(body[at++]));
        d.object_set_names.push_back(name);
    }
    if (const std::size_t at = r.u32(0xa0)) d.stories = r.bytes(at, r.u32(0xa4) - at);
    for (std::size_t at = r.u32(0xac); at < body.size() && body[at]; ++at) d.name.push_back(static_cast<char>(body[at]));
    if (r.failed) { error = "level section runs past the end (" + std::to_string(body.size()) + " bytes)"; return false; }
    out = std::move(d);
    return true;
}

bool write_level_document(const LevelDocument& d, std::vector<std::uint8_t>& body, std::string& error) {
    Out w;
    w.b.assign(0xc0, 0);
    w.set(0x04, kVersion);
    const auto units = static_cast<std::uint32_t>(d.units.size());
    w.set(0x08, units);
    w.set(0x0c, d.background_units);
    // units (their list offsets are filled in below)
    for (const auto& u : d.units) {
        w.u64(u.id);
        w.u64(u.name);
        w.u64(u.resource);
        w.u64(u.reserved);
        for (float f : u.position) w.f32(f);
        for (float f : u.rotation) w.f32(f);
        for (float f : u.scale) w.f32(f);
        for (int k = 0; k < 4; ++k) w.u32(0);
        w.put(u.tail.data(), u.tail.size());
    }
    const auto record = [](std::size_t i) { return 0xc0 + i * 0x60; };
    for (std::size_t i = 0; i < d.units.size(); ++i) {
        if (d.units[i].node_poses.empty()) continue;
        w.set(record(i) + 0x48, static_cast<std::uint32_t>(d.units[i].node_poses.size()));
        w.set(record(i) + 0x4c, w.at());
        for (const auto& p : d.units[i].node_poses) w.put(p.data(), p.size());
    }
    for (std::size_t i = 0; i < d.units.size(); ++i) {
        if (d.units[i].blob.empty()) continue;
        w.set(record(i) + 0x50, static_cast<std::uint32_t>(d.units[i].blob.size()));
        w.set(record(i) + 0x54, w.at());
        w.put(d.units[i].blob);
        w.align(4);
    }
    const auto data_table = w.at();
    w.set(0x10, data_table);
    w.b.resize(w.b.size() + 4 * units, 0);
    for (std::size_t i = 0; i < d.units.size(); ++i) {
        if (d.units[i].data.empty()) continue;
        w.set(data_table + 4 * i, w.at());
        w.put(d.units[i].data);
    }
    w.set(0x48, w.at());
    w.put(d.level_data);
    w.set(0x14, w.at());
    w.u32(static_cast<std::uint32_t>(d.lights.size()));
    for (const auto& light : d.lights) w.put(light);
    w.set(0x18, w.at());
    w.u32(static_cast<std::uint32_t>(d.cameras.size()));
    for (const auto& camera : d.cameras) w.put(camera);
    w.align(8);
    w.set(0x1c, w.at());
    w.set(0x20, static_cast<std::uint32_t>(d.prototype_unit.size()));
    w.put(d.prototype_unit);
    w.set(0x24, w.at());
    w.put(d.flow);
    if (!d.flow.empty()) w.align(8);
    w.set(0x28, w.at());
    w.put(d.flow_data);
    w.set(0x2c, static_cast<std::uint32_t>(d.triggers.size()));
    w.set(0x30, w.at());
    for (const auto& t : d.triggers) w.put(t.data(), t.size());
    w.set(0x34, static_cast<std::uint32_t>(d.unit_nodes.size()));
    w.set(0x38, w.at());
    for (const auto& n : d.unit_nodes) for (auto v : n) w.u32(v);
    w.set(0x3c, static_cast<std::uint32_t>(d.reference_nodes.size()));
    w.set(0x40, w.at());
    for (const auto& n : d.reference_nodes) for (auto v : n) w.u32(v);
    w.set(0x44, w.at());
    w.put(d.navigation_mesh.data(), d.navigation_mesh.size());
    w.align(8);
    w.set(0x50, static_cast<std::uint32_t>(d.effects.size()));
    w.set(0x54, w.at());
    for (const auto& e : d.effects) w.put(e.data(), e.size());
    w.set(0x58, static_cast<std::uint32_t>(d.splines.size()));
    w.set(0x5c, w.at());
    for (const auto& spline : d.splines) {
        w.u32(spline.name);
        w.u32(static_cast<std::uint32_t>(spline.points.size()));
        for (const auto& p : spline.points) for (float f : p) w.f32(f);
    }
    w.align(8);
    w.set(0x60, static_cast<std::uint32_t>(d.volumes.size()));
    w.set(0x64, w.at());
    for (const auto& v : d.volumes) w.put(v.record);
    w.set(0x68, d.volumes.empty() ? 0 : w.at());
    for (const auto& m : d.volume_matrices) w.put(m.data(), m.size());
    std::vector<std::pair<std::uint32_t, std::uint32_t>> volume_units;
    for (const auto& v : d.volume_units) {
        volume_units.emplace_back(w.at(), static_cast<std::uint32_t>(v.size()));
        w.put(v);
    }
    w.align(4);
    w.set(0x6c, static_cast<std::uint32_t>(d.volume_units.size()));
    w.set(0x70, d.volume_units.empty() ? 0 : w.at());
    for (const auto& [at, size] : volume_units) { w.u32(at); w.u32(size); }
    w.align(8);
    w.set(0x94, static_cast<std::uint32_t>(d.nav_data.size()));
    w.set(0x98, w.at());
    for (const auto& n : d.nav_data) w.put(n.data(), n.size());
    w.set(0x88, static_cast<std::uint32_t>(d.nested.size()));
    w.set(0x8c, w.at());
    for (const auto& n : d.nested) w.put(n.record);
    const auto nested_table = w.at();
    w.set(0x90, nested_table);
    w.b.resize(w.b.size() + 4 * d.nested.size(), 0);
    for (std::size_t i = 0; i < d.nested.size(); ++i) {
        if (d.nested[i].data.empty()) continue;
        w.set(nested_table + 4 * i, w.at());
        w.put(d.nested[i].data);
    }
    if (!d.scatter.empty()) {
        w.set(0x9c, w.at());
        w.put(d.scatter);
    }
    // object set names: the strings (each padded to 4), then their offsets
    w.set(0x74, static_cast<std::uint32_t>(d.object_set_names.size()));
    std::vector<std::uint32_t> name_offsets;
    for (const auto& name : d.object_set_names) {
        name_offsets.push_back(w.at());
        w.put(reinterpret_cast<const std::uint8_t*>(name.c_str()), name.size() + 1);
        w.align(4);
    }
    w.set(0x78, w.at());
    for (auto at : name_offsets) w.u32(at);
    // object sets per unit / effect / trigger: the lists (each pointing at where it starts, empty ones too), then the
    // {n, offset} table
    const auto set_lists = [&](std::size_t header, const std::vector<std::vector<std::uint32_t>>& lists) {
        std::vector<std::uint32_t> starts;
        for (const auto& list : lists) {
            starts.push_back(w.at());
            for (auto v : list) w.u32(v);
        }
        w.set(header, w.at());
        for (std::size_t i = 0; i < lists.size(); ++i) {
            w.u32(static_cast<std::uint32_t>(lists[i].size()));
            w.u32(starts[i]);
        }
    };
    std::vector<std::vector<std::uint32_t>> unit_sets;
    for (const auto& u : d.units) unit_sets.push_back(u.object_sets);
    set_lists(0x7c, unit_sets);
    set_lists(0x80, d.effect_object_sets);
    set_lists(0x84, d.trigger_object_sets);
    if (!d.stories.empty()) {
        w.set(0xa0, w.at());
        w.put(d.stories);
    }
    w.set(0xa4, w.at());
    {
        std::uint32_t overridden = 0;
        for (const auto& u : d.units) overridden += !u.materials.empty();
        w.u32(overridden);
        for (std::size_t i = 0; i < d.units.size(); ++i) {
            if (d.units[i].materials.empty()) continue;
            w.u32(static_cast<std::uint32_t>(i));
            w.u32(static_cast<std::uint32_t>(d.units[i].materials.size()));
            for (const auto& [slot, material] : d.units[i].materials) { w.u32(slot); w.u64(material); }
        }
    }
    w.set(0xac, w.at());
    w.put(reinterpret_cast<const std::uint8_t*>(d.name.c_str()), d.name.size() + 1);
    w.align(16);
    body = std::move(w.b);
    (void)error;
    return true;
}

namespace {

std::uint32_t word(const Bytes& b, std::size_t at) {
    std::uint32_t v = 0;
    if (at + 4 <= b.size()) std::memcpy(&v, b.data() + at, 4);
    return v;
}
void set_word(Bytes& b, std::size_t at, std::uint32_t v) {
    if (at + 4 <= b.size()) std::memcpy(b.data() + at, &v, 4);
}

// every unit number in the stories section ({count, {id32, offset}}, then the stories; offsets from after the index):
// visit(story id32, byte position of the number)
template <typename Visit>
void story_units(const Bytes& stories, Visit visit) {
    const std::uint32_t count = word(stories, 0);
    const std::size_t start = 4 + 8 * static_cast<std::size_t>(count);
    for (std::uint32_t i = 0; i < count && start <= stories.size(); ++i) {
        const std::uint32_t id = word(stories, 4 + 8 * i);
        const std::size_t base = start + word(stories, 8 + 8 * i);
        if (base + 112 > stories.size() || word(stories, base) != 9) continue;
        const std::uint32_t curves = word(stories, base + 4), units = word(stories, base + 8), table = word(stories, base + 16);
        for (std::uint32_t c = 0; c < curves; ++c) {
            const std::size_t entry = base + 112 + 68 * static_cast<std::size_t>(c);
            const std::uint32_t type = word(stories, entry);
            if (type == 0 || type == 1) visit(id, entry + 4);
        }
        for (std::uint32_t u = 0; u < units; ++u) visit(id, base + word(stories, base + 24) + 4 * u);
        for (std::uint32_t t = 0; t < table; ++t) visit(id, base + word(stories, base + 32) + 12 * t);
        // 24-byte entries {u64 id, unit number, start, end, u32 0}
        for (std::uint32_t t = 0, timed = word(stories, base + 20); t < timed; ++t) visit(id, base + word(stories, base + 36) + 24 * t + 8);
    }
}

// renumbers every reference: number -> renumber(number)
template <typename Renumber>
void renumber_units(LevelDocument& d, Renumber renumber) {
    for (auto& n : d.unit_nodes) n[0] = renumber(n[0]);
    std::vector<std::size_t> sites;
    story_units(d.stories, [&](std::uint32_t, std::size_t at) { sites.push_back(at); });
    for (const auto at : sites) set_word(d.stories, at, renumber(word(d.stories, at)));
}

// a fresh unit id (one no unit of the list has) and instance name: a GUID like the editor gives every placed unit, as
// its IdString64
void fresh_identity(DocumentUnit& unit, const std::vector<DocumentUnit>& units) {
    static std::mt19937_64 random{std::random_device{}()};
    for (bool taken = true; taken;) {
        unit.id = random();
        taken = unit.id == 0 || std::any_of(units.begin(), units.end(), [&](const DocumentUnit& u) { return &u != &unit && u.id == unit.id; });
    }
    char guid[40];
    const std::uint64_t a = random(), b = random();
    std::snprintf(guid, sizeof guid, "%08llx-%04llx-%04llx-%04llx-%012llx", static_cast<unsigned long long>(a >> 32),
                  static_cast<unsigned long long>((a >> 16) & 0xffff), static_cast<unsigned long long>(a & 0xffff),
                  static_cast<unsigned long long>(b >> 48), static_cast<unsigned long long>(b & 0xffffffffffffull));
    unit.name = id64(guid);
}

}  // namespace


std::vector<std::string> unit_users(const LevelDocument& d, std::size_t unit) {
    std::vector<std::string> users;
    for (const auto& n : d.unit_nodes)
        if (n[0] == unit) { users.push_back("level flow"); break; }
    story_units(d.stories, [&](std::uint32_t id, std::size_t at) {
        if (word(d.stories, at) != unit) return;
        char name[24];
        std::snprintf(name, sizeof name, "story #%08x", id);
        if (std::find(users.begin(), users.end(), name) == users.end()) users.push_back(name);
    });
    return users;
}

bool set_units(LevelDocument& d, std::vector<DocumentUnit> units, std::uint32_t background, std::string& error) {
    // copies and new units: a fresh identity
    std::unordered_map<std::uint64_t, std::size_t> number_of;
    for (std::size_t i = 0; i < units.size(); ++i) {
        if (units[i].id == 0 || number_of.count(units[i].id)) fresh_identity(units[i], units);
        number_of.emplace(units[i].id, i);
    }
    // every referenced unit must still be there
    std::string missing;
    for (std::size_t old = 0; old < d.units.size(); ++old) {
        if (number_of.count(d.units[old].id)) continue;
        const auto users = unit_users(d, old);
        if (users.empty()) continue;
        char id[20];
        std::snprintf(id, sizeof id, "%016llx", static_cast<unsigned long long>(d.units[old].id));
        missing += std::string(missing.empty() ? "" : "; ") + "unit " + id + " (used by";
        for (std::size_t i = 0; i < users.size(); ++i) missing += (i ? ", " : " ") + users[i];
        missing += ")";
    }
    if (!missing.empty()) { error = "units the level still uses were taken out: " + missing; return false; }
    const auto old_units = d.units;
    renumber_units(d, [&](std::uint32_t n) {
        return n < old_units.size() ? static_cast<std::uint32_t>(number_of.at(old_units[n].id)) : n;
    });
    // light and camera records follow their units (sorted by unit number, a unit's own in their order); a removed unit's go
    for (auto* records : {&d.lights, &d.cameras}) {
        std::vector<Bytes> kept;
        for (auto& record : *records) {
            const std::uint32_t old = word(record, 0);
            if (old >= old_units.size()) continue;
            const auto found = number_of.find(old_units[old].id);
            if (found == number_of.end()) continue;
            set_word(record, 0, static_cast<std::uint32_t>(found->second));
            kept.push_back(std::move(record));
        }
        std::stable_sort(kept.begin(), kept.end(), [](const Bytes& a, const Bytes& b) { return word(a, 0) < word(b, 0); });
        *records = std::move(kept);
    }
    d.units = std::move(units);
    d.background_units = std::min<std::uint32_t>(background, static_cast<std::uint32_t>(d.units.size()));
    return true;
}

}  // namespace dtglb::stingray::level
