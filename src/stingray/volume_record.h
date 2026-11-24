#pragma once
// A volume record, as UNIT volumes and level volumes both store them (engine application/world/volume.cpp): a prism in
// a frame (a UNIT's node, or a level volume's matrix), its floor outline a counter-clockwise polygon in x/y, its
// height lo z..hi z. Layout: {u32 frame (node / matrix index), lo xyz, hi xyz, u32 name id32, f32 floor area,
// u32 triangle count, u32 triangle offset, u32 shape offset (0 = none), u64 runtime pointer 0, u32 point count,
// u32 record size}, the points {f32 x, y}, a triangulation of the floor {f32 area, u32 i, j, k} the engine picks
// random points from (area-weighted), then the optional shape (kind 1: a box from lo/hi, then u32 0; kinds 2/3:
// cooked PhysX meshes). Rectangles get the box shape (as retail's UNIT volumes do), other outlines none.
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace dtglb::stingray {

struct VolumeShape {
    std::uint32_t frame = 0;  // UNIT: SceneGraph node; level: matrix index
    std::uint32_t name = 0;   // IdString32
    std::vector<std::array<double, 2>> outline;
    double bottom = 0, top = 0;
};

// the record of a shape (points on a straight edge dropped, made counter-clockwise, ear-clipped); false when the
// outline has fewer than 3 points, no height, or crosses itself
bool make_volume_record(VolumeShape shape, bool box_shape, std::vector<std::uint8_t>& record, std::string& error);
// the shape a record holds (frame, name, outline, height)
bool read_volume_record(const std::vector<std::uint8_t>& record, VolumeShape& shape, std::string& error);

}  // namespace dtglb::stingray
