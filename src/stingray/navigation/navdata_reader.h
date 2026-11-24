#pragma once
// Reads a NavData resource (a standalone BlobAggFile: "BlobAggFile\0", u32 endianness 0, u32 entry count, then entries
// {u32 type, u32 version, u32 deep size, u32 shallow size, payload}). Arrays are {u32 count, i32 offset from the offset
// field}; child references {u32 shallow size, i32 offset}. Types: 0x500 SDK version text, 0x001 nav mesh element,
// 0x002 nav graph (the sparse companion's points), 0x00e sector descriptor.
// A mesh element: cell box (4 i32), its size (2 u32), GUIDs, timestamp, settings (radius, height, step, slope, altitude
// tolerance, integer precision as f32, cell size in raster pixels as i32), flags, cells. A cell: grid x, y, stitch
// records (0x2c), floors. A floor: connex index per triangle, nav tags, packed half-edges (three per triangle; bits
// 14-25 the start vertex), vertices (u8 x, u8 y raster pixels in the cell), their altitudes (f32). A vertex lies at
// ((cell * cell size + local) * 128 * integer precision, altitude) in navigation space.
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace dtglb::stingray::navigation {

struct NavDataMesh {
    std::array<float, 6> settings{};  // radius, height, step, slope, altitude tolerance, integer precision
    std::int32_t cell_size = 0;       // raster pixels
    std::array<std::int32_t, 4> cells_box{};
    std::size_t cells = 0, floors = 0;
    std::vector<std::array<float, 3>> vertices;  // world space, three per triangle
};

struct NavData {
    std::string version;
    std::vector<NavDataMesh> meshes;
    std::vector<std::array<float, 3>> graph_points;  // the sparse companion's
    std::string sector;
};

bool read_navdata(const std::vector<std::uint8_t>& bytes, NavData& out, std::string& error);

}  // namespace dtglb::stingray::navigation
