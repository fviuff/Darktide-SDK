#pragma once
// A unit's static collision as triangles in the unit's own frame: what the navigation generator walks on. Taken from
// the UNIT actor records whose actor template is "static" and which the unit creates when it spawns: spheres, boxes and
// capsules are tessellated, cooked meshes and convex hulls are read through PhysX. Trigger shapes (shape templates
// "trigger" and #5fa361eb) are left out: nothing stands on them.
#include "stingray/unit/unit_reader.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace dtglb::stingray::unit {

struct CollisionMesh {
    std::vector<std::array<float, 3>> vertices;
    std::vector<std::uint32_t> indices;  // three per triangle, counter-clockwise seen from outside
};

// appends the unit's static collision, moved by `placement` (row vectors, translation in 12..14: the unit's pose)
bool static_collision(const Unit& unit, const std::array<float, 16>& placement, CollisionMesh& out, std::string& error);

// row-vector pose from position, rotation (x, y, z, w) and scale
std::array<float, 16> pose_matrix(const std::array<float, 3>& position, const std::array<float, 4>& rotation,
                                  const std::array<float, 3>& scale);

}  // namespace dtglb::stingray::unit
