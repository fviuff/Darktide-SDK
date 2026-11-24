#pragma once
// Writes a level's blockout boxes as its embedded UNIT, like the level compiler (level_prototypes.h): visible boxes
// join in a mesh per object set, solid ones in a mesh per physics material, shape template and object set, cooked into
// the triangle mesh shapes of the object set's actor. Meshes and actors follow the order of the first box in each.
#include "stingray/level/level_prototypes.h"

#include <cstdint>
#include <string>
#include <vector>

namespace dtglb::stingray::level {

bool write_level_prototype_unit(const std::vector<LevelPrototype>& boxes, std::vector<std::uint8_t>& out, std::string& error);

}  // namespace dtglb::stingray::level
