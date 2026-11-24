#pragma once
// Reads a LEVEL resource (version 125) the way the engine reaches its sections (Level::spawn, the level resource
// loader and the LevelResource Lua API): a 0xc0-byte header of counts and body offsets, then the sections.
//   +0x08 units (records of 0x60 from 0xc0), +0x0c background units (the last ones), +0x10 per-unit script data
//   +0x1c/+0x20 embedded prototype UNIT, +0x48 level script data, +0x50/+0x54 particle effects, +0x60/+0x64 volumes
//   (matrices at +0x68), +0x6c/+0x70 embedded volume UNITs, +0x74/+0x78 object set names, +0x7c per-unit object sets,
//   +0x88/+0x8c nested levels, +0x94/+0x98 nav data, +0x9c scatter, +0xa0 stories, +0xa4 material overrides,
//   +0xac the level's own name.
// A unit record (0x60): u64 id, u64 instance name, u64 unit, u64 0, position, rotation, scale, u32 n + offset of n
// 48-byte node poses {position, u32 node id32, rotation, scale, u32 0}, u32 size + offset of a byte blob, 8 zero bytes.
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace dtglb::stingray::level {

struct Transform {
    std::array<float, 3> position{};
    std::array<float, 4> rotation{0, 0, 0, 1};  // x y z w
    std::array<float, 3> scale{1, 1, 1};
};

struct NodePose {
    std::uint32_t node = 0;  // IdString32
    Transform transform;
};

struct LevelUnit {
    std::uint64_t id = 0, name = 0, resource = 0;  // name: IdString64 of the instance name
    Transform transform;
    std::vector<NodePose> node_poses;  // nodes of the placed unit moved in the level
    std::vector<std::uint8_t> blob;
    std::vector<std::uint32_t> object_sets;  // IdString32 of the set names
    std::uint32_t script_data = 0;           // body offset of the unit's script data in the level, 0 = none
    std::vector<std::pair<std::uint32_t, std::uint64_t>> materials;  // material overrides: slot id32 -> material
};

struct NestedLevel {
    std::uint64_t id = 0, resource = 0;
    Transform transform;
    std::string path;
};

struct LevelEffect {
    std::uint64_t resource = 0;
    Transform transform;
    std::uint32_t start = 0;
};

struct LevelVolume {
    std::uint32_t name = 0;  // IdString32
    std::array<float, 16> matrix{};  // places the outline (row vectors)
    std::array<float, 3> min{}, max{};
    std::vector<std::array<float, 2>> outline;  // at min z, height max z - min z
};

// scatter (stingray ScatterResource): brushes of one unit each, placed many times
struct ScatterBrush {
    std::uint64_t unit = 0;
    float spawn_distance = 0, unspawn_distance = 0;
    std::uint32_t fade_method = 0;  // 0 pop, 1 slide z, 2 scale
    float fade_range = 0, fade_from = 0, fade_to = 0;
    std::uint32_t pool_size = 0;
    bool preheat_pool = false, user_setting_density = false;
    struct Instance {
        std::array<float, 3> position{};
        std::uint32_t id = 0;
        std::array<float, 4> rotation{0, 0, 0, 1};
    };
    std::vector<Instance> instances;
};

struct LevelNavData {
    std::uint64_t resource = 0;
    std::uint32_t index = 0;
};

struct Level {
    std::vector<LevelUnit> units;
    std::uint32_t background_units = 0;
    std::vector<NestedLevel> nested;
    std::vector<LevelEffect> effects;
    std::vector<LevelVolume> volumes;
    std::vector<std::string> object_sets;
    std::vector<LevelNavData> nav_data;
    std::vector<ScatterBrush> scatter;
    std::vector<std::uint32_t> stories;  // IdString32 names of the level's stories (keyframed unit animation)
    std::string name;                    // the level's own resource path
    std::vector<std::uint8_t> prototype_unit;            // a UNIT body, empty = none
    std::vector<std::vector<std::uint8_t>> volume_units;  // UNIT bodies
};

// file: a cooked resource (with the envelope) or a bare body
bool read_level(const std::vector<std::uint8_t>& file, Level& out, std::string& error);

}  // namespace dtglb::stingray::level
