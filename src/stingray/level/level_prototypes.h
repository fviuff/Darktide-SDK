#pragma once
// A level's blockout boxes. The level compiler (LevelResourceBuilder::make_prototype_unit_resource) joins them into one
// embedded UNIT (level header +0x1c/+0x20) on a single root node: a mesh per object set for the visible boxes (flags 3,
// named after the set) and a mesh "__physics_mesh<n>" (flags 0, not drawn) per physics material, shape template and
// object set for the solid ones, each cooked into a triangle mesh shape of the object set's actor. Every box is 36
// vertices: per axis, the negative then the positive face, 6 vertices each (make_box_vertices), the box's colour on
// every vertex.
#include "stingray/unit/unit_reader.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace dtglb::stingray::level {

struct LevelPrototype {
    std::array<float, 3> center{};
    std::array<std::array<float, 3>, 3> axes{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};  // unit length
    std::array<float, 3> half_size{0.5f, 0.5f, 0.5f};
    std::array<float, 4> colour{1, 1, 1, 1};
    bool visible = false, physical = false;
    std::uint32_t object_set = 0;                    // IdString32 of the set, 0 = none
    std::uint32_t material = 0, shape_template = 0;  // physics; IdString32
};

// The 36 vertices of a box in the order the level compiler writes them.
std::array<std::array<float, 3>, 36> prototype_vertices(const LevelPrototype& box);

constexpr std::uint32_t kNoObjectSet = 0xe43f8295u;  // IdString32 NO_OBJECT_SET_MEMBERSHIP: names boxes in no object set

// the boxes as the meshes hold them: a box both visible and solid comes twice
bool read_level_prototypes(const unit::Unit& unit, std::vector<LevelPrototype>& out, std::string& error);

// the same place and size, to within `tolerance` metres (of size, and along an axis)
bool same_prototype(const LevelPrototype& a, const LevelPrototype& b, float tolerance = 1e-3f);

// a visible and a solid box of one place and object set as one box
std::vector<LevelPrototype> merged_prototypes(const std::vector<LevelPrototype>& boxes);

}  // namespace dtglb::stingray::level
