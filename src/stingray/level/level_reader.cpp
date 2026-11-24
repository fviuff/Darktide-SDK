#include "stingray/level/level_reader.h"

#include <cstring>

#include "stingray/cooked_resource.h"

namespace dtglb::stingray::level {

namespace {

struct Body {
    const std::vector<std::uint8_t>& data;
    bool failed = false;
    template <class T> T at(std::size_t offset) {
        T v{};
        if (offset + sizeof(T) > data.size()) { failed = true; return v; }
        std::memcpy(&v, data.data() + offset, sizeof(T));
        return v;
    }
    std::uint32_t u32(std::size_t offset) { return at<std::uint32_t>(offset); }
    template <std::size_t N> std::array<float, N> floats(std::size_t offset) {
        std::array<float, N> out{};
        for (std::size_t i = 0; i < N; ++i) out[i] = at<float>(offset + i * 4);
        return out;
    }
    Transform transform(std::size_t offset) {
        return {floats<3>(offset), floats<4>(offset + 12), floats<3>(offset + 28)};
    }
    std::string string(std::size_t offset) {
        std::string s;
        while (offset < data.size() && data[offset]) s.push_back(static_cast<char>(data[offset++]));
        if (offset >= data.size()) failed = true;
        return s;
    }
    std::vector<std::uint8_t> bytes(std::size_t offset, std::size_t size) {
        if (offset + size > data.size()) { failed = true; return {}; }
        return {data.begin() + static_cast<std::ptrdiff_t>(offset), data.begin() + static_cast<std::ptrdiff_t>(offset + size)};
    }
};

constexpr std::uint32_t kMaxCount = 1u << 20;

}  // namespace

bool read_level(const std::vector<std::uint8_t>& file, Level& out, std::string& error) {
    std::vector<std::uint8_t> data;
    std::string stream, envelope_error;
    if (!parse_cooked_resource_envelope(file, "level", data, stream, envelope_error)) data = file;
    Body b{data};
    if (data.size() < 0xc0 || b.u32(4) != 125) {
        error = "not a version 125 level";
        return false;
    }
    Level level;
    const auto units = b.u32(0x08);
    level.background_units = b.u32(0x0c);
    if (units > kMaxCount || 0xc0 + static_cast<std::size_t>(units) * 0x60 > data.size()) {
        error = "level unit records run past the end";
        return false;
    }
    level.units.resize(units);
    const auto script_table = b.u32(0x10), sets_table = b.u32(0x7c);
    for (std::uint32_t i = 0; i < units; ++i) {
        auto& u = level.units[i];
        const std::size_t r = 0xc0 + static_cast<std::size_t>(i) * 0x60;
        u.id = b.at<std::uint64_t>(r);
        u.name = b.at<std::uint64_t>(r + 0x08);
        u.resource = b.at<std::uint64_t>(r + 0x10);
        u.transform = b.transform(r + 0x20);
        const auto poses = b.u32(r + 0x48), poses_at = b.u32(r + 0x4c);
        if (poses > kMaxCount) { error = "unit node pose list is implausible"; return false; }
        for (std::uint32_t k = 0; k < poses; ++k) {
            const std::size_t p = poses_at + static_cast<std::size_t>(k) * 48;
            u.node_poses.push_back({b.u32(p + 12), {b.floats<3>(p), b.floats<4>(p + 16), b.floats<3>(p + 32)}});
        }
        u.blob = b.bytes(b.u32(r + 0x54), b.u32(r + 0x50));
        u.script_data = b.u32(script_table + i * 4);
        const auto n = b.u32(sets_table + i * 8), at = b.u32(sets_table + i * 8 + 4);
        if (n > kMaxCount) { error = "object set list is implausible"; return false; }
        for (std::uint32_t k = 0; k < n; ++k) u.object_sets.push_back(b.u32(at + k * 4));
    }
    // material overrides: {u32 count, per unit {u32 unit index, u32 n, n x (u32 slot, u64 material)}}
    {
        std::size_t at = b.u32(0xa4);
        const auto count = b.u32(at);
        at += 4;
        for (std::uint32_t i = 0; i < count && !b.failed; ++i) {
            const auto unit = b.u32(at), n = b.u32(at + 4);
            at += 8;
            if (unit >= units || n > kMaxCount) { error = "material override for a unit the level hasn't"; return false; }
            for (std::uint32_t k = 0; k < n; ++k, at += 12)
                level.units[unit].materials.emplace_back(b.u32(at), b.at<std::uint64_t>(at + 4));
        }
    }
    // nested levels: records chained by their size (+0x38), the path from +0x40
    {
        std::size_t at = b.u32(0x8c);
        const auto count = b.u32(0x88);
        for (std::uint32_t i = 0; i < count && !b.failed; ++i) {
            NestedLevel n;
            n.id = b.at<std::uint64_t>(at);
            n.resource = b.at<std::uint64_t>(at + 0x08);
            n.transform = b.transform(at + 0x10);
            n.path = b.string(at + 0x40);
            const auto size = b.u32(at + 0x38);
            if (size < 0x40) { error = "nested level record is too short"; return false; }
            level.nested.push_back(std::move(n));
            at += size;
        }
    }
    for (std::uint32_t i = 0, count = b.u32(0x50); i < count && !b.failed; ++i) {
        const std::size_t r = b.u32(0x54) + static_cast<std::size_t>(i) * 0x38;
        level.effects.push_back({b.at<std::uint64_t>(r), b.transform(r + 8), b.u32(r + 0x30)});
    }
    // volumes: records chained by their size (+0x3c); the outline sits at min z and is placed by its matrix
    {
        std::size_t at = b.u32(0x64);
        const auto matrices = b.u32(0x68);
        for (std::uint32_t i = 0, count = b.u32(0x60); i < count && !b.failed; ++i) {
            LevelVolume v;
            v.matrix = b.floats<16>(matrices + static_cast<std::size_t>(b.u32(at)) * 0x80);
            v.min = b.floats<3>(at + 0x04);
            v.max = b.floats<3>(at + 0x10);
            v.name = b.u32(at + 0x1c);
            const auto points = b.u32(at + 0x38);
            if (points > kMaxCount) { error = "volume outline is implausible"; return false; }
            for (std::uint32_t k = 0; k < points; ++k) v.outline.push_back(b.floats<2>(at + 0x40 + k * 8));
            const auto size = b.u32(at + 0x3c);
            if (size < 0x40) { error = "volume record is too short"; return false; }
            level.volumes.push_back(std::move(v));
            at += size;
        }
    }
    for (std::uint32_t i = 0, count = b.u32(0x74); i < count && !b.failed; ++i)
        level.object_sets.push_back(b.string(b.u32(b.u32(0x78) + i * 4)));
    for (std::uint32_t i = 0, count = b.u32(0x94); i < count && !b.failed; ++i)
        level.nav_data.push_back({b.at<std::uint64_t>(b.u32(0x98) + i * 16), b.u32(b.u32(0x98) + i * 16 + 8)});
    if (const std::size_t at = b.u32(0x9c)) {
        const auto brushes = b.u32(at);
        if (brushes > kMaxCount) { error = "scatter brush count is implausible"; return false; }
        for (std::uint32_t i = 0; i < brushes && !b.failed; ++i) {
            const std::size_t r = at + 4 + static_cast<std::size_t>(i) * 48;
            ScatterBrush s;
            s.unit = b.at<std::uint64_t>(r);
            s.spawn_distance = b.at<float>(r + 8);
            s.unspawn_distance = b.at<float>(r + 12);
            s.fade_method = b.u32(r + 16);
            s.fade_range = b.at<float>(r + 20);
            s.fade_from = b.at<float>(r + 24);
            s.fade_to = b.at<float>(r + 28);
            s.pool_size = b.u32(r + 32);
            s.preheat_pool = b.at<std::uint8_t>(r + 36) != 0;
            s.user_setting_density = b.at<std::uint8_t>(r + 37) != 0;
            const auto n = b.u32(r + 40);
            if (n > kMaxCount) { error = "scatter unit count is implausible"; return false; }
            for (std::uint32_t k = 0; k < n; ++k) {
                const std::size_t p = at + b.u32(r + 44) + static_cast<std::size_t>(k) * 32;
                s.instances.push_back({b.floats<3>(p), b.u32(p + 12), b.floats<4>(p + 16)});
            }
            level.scatter.push_back(std::move(s));
        }
    }
    if (const std::size_t at = b.u32(0xa0))
        for (std::uint32_t i = 0, count = b.u32(at); i < count && !b.failed; ++i) level.stories.push_back(b.u32(at + 4 + i * 8));
    level.name = b.string(b.u32(0xac));
    if (const auto size = b.u32(0x20)) level.prototype_unit = b.bytes(b.u32(0x1c), size);
    for (std::uint32_t i = 0, count = b.u32(0x6c); i < count && !b.failed; ++i)
        level.volume_units.push_back(b.bytes(b.u32(b.u32(0x70) + i * 8), b.u32(b.u32(0x70) + i * 8 + 4)));
    if (b.failed) {
        error = "level section runs past the end (" + std::to_string(data.size()) + " bytes)";
        return false;
    }
    out = std::move(level);
    return true;
}

}  // namespace dtglb::stingray::level
