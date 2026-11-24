#pragma once
// A LEVEL resource (version 125) as an editable document: every section of the file, read so that writing it back
// gives the same bytes. Sections are laid out in the engine's file order:
//   header (0xc0), units (0x60 each), unit node poses (48 bytes each), unit blobs, unit data table, unit data (dynamic
//   data per unit), level data, lights, cameras (then aligned to 8), prototype UNIT, level flow (then aligned to 8), its dynamic data,
//   triggers (0x58), unit nodes (12), reference nodes (12), navigation mesh (Stingray's own, 12 bytes; then aligned to
//   8), effects (0x38), splines ({u32 name id32, u32 n, n x vec3}; then aligned to 8), volumes, their matrices (0x80),
//   volume UNITs and their table (then aligned to 8), nav data (16), nested levels and their data, scatter, object set
//   names (strings, then their offsets), object sets per unit / effect / trigger (the lists, then the {n, offset}
//   table), stories, material overrides, the level's own path (aligned to 16).
// Records whose fields are not all named yet (lights, cameras, triggers, story curves) are kept as
// their bytes; none of them holds file offsets, so they move freely when the document changes.
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace dtglb::stingray::level {

using Bytes = std::vector<std::uint8_t>;

struct DocumentUnit {
    std::uint64_t id = 0, name = 0, resource = 0, reserved = 0;
    std::array<float, 3> position{};
    std::array<float, 4> rotation{0, 0, 0, 1};
    std::array<float, 3> scale{1, 1, 1};
    std::vector<std::array<std::uint8_t, 48>> node_poses;  // {position, u32 node id32, rotation, scale, u32 0}
    Bytes blob;                                            // the record's +0x50/+0x54 byte list
    Bytes data;                                            // dynamic (script) data, empty = none
    std::vector<std::uint32_t> object_sets;                // id32 of set names
    std::vector<std::pair<std::uint32_t, std::uint64_t>> materials;  // overrides: slot id32 -> material
    std::array<std::uint8_t, 8> tail{};
};

struct DocumentNested {
    Bytes record;  // the whole record (transform at +0x10, path from +0x40, its size at +0x38)
    Bytes data;    // dynamic data handed to the nested level, empty = none
};

struct DocumentVolume {
    Bytes record;  // the whole record (matrix index +0, box, name +0x1c, outline; its size at +0x3c)
};

struct DocumentSpline {
    std::uint32_t name = 0;  // IdString32
    std::vector<std::array<float, 3>> points;
};

struct LevelDocument {
    std::uint32_t background_units = 0;
    std::vector<DocumentUnit> units;
    Bytes level_data;
    // the level's settings for its units' lights (level_light.h), one record each, sorted by unit number
    std::vector<Bytes> lights;
    std::vector<Bytes> cameras;  // 0x30 each (level_camera.h)
    Bytes prototype_unit;
    Bytes flow, flow_data;
    std::vector<std::array<std::uint8_t, 0x58>> triggers;
    std::vector<std::vector<std::uint32_t>> trigger_object_sets;
    std::vector<std::array<std::uint32_t, 3>> unit_nodes, reference_nodes;
    std::array<std::uint8_t, 12> navigation_mesh{};
    std::vector<std::array<std::uint8_t, 0x38>> effects;
    std::vector<std::vector<std::uint32_t>> effect_object_sets;
    std::vector<DocumentSpline> splines;
    std::vector<DocumentVolume> volumes;
    std::vector<std::array<std::uint8_t, 0x80>> volume_matrices;
    std::vector<Bytes> volume_units;
    std::vector<DocumentNested> nested;
    std::vector<std::array<std::uint8_t, 16>> nav_data;
    Bytes scatter;  // ScatterResource (its unit offsets are from its own start)
    std::vector<std::string> object_set_names;
    Bytes stories;  // StoriesBlob (story offsets are from its own index)
    std::string name;
};

// Editing. Units are numbered by their place in the list, and the level flow (unit nodes: {unit, flow node, output})
// and the stories refer to units by that number. A story (version 9) is a 112-byte header ([1] curves, [2] units,
// [3] events, [4] table entries, [6] unit list offset, [7] events offset, [8] table offset, [10] start, [11] end time,
// then a pose matrix), curves of 68 bytes {target type, target (type 0 unit node / 1 unit property: unit number
// first), curve info, first key}, keys, the story's unit numbers, events of 144 bytes {id32, u32, u32, time, name},
// a table of 12-byte entries {unit number, f32, f32} and ([5] entries, [9] offset) 24-byte entries {u64 id, unit number,
// start, end, u32 0}.
// Replaces the unit list with an edited one (units moved, taken out, copied, added; the last `background` of them
// are the background units). The references follow the units by id: a unit the level flow or a story uses must still
// be there, else nothing changes and the error names it and its users. Units with id 0 or an id an earlier unit of the
// list has (copies) get a fresh id and instance name.
bool set_units(LevelDocument& document, std::vector<DocumentUnit> units, std::uint32_t background, std::string& error);
// what uses a unit by its number ("level flow", "story #<id32>"), empty when nothing does
std::vector<std::string> unit_users(const LevelDocument& document, std::size_t unit);

// file: a cooked resource (with the envelope) or a bare body; out gets the body's document
bool read_level_document(const std::vector<std::uint8_t>& file, LevelDocument& out, std::string& error);
// the body (no envelope)
bool write_level_document(const LevelDocument& document, std::vector<std::uint8_t>& body, std::string& error);

}  // namespace dtglb::stingray::level
