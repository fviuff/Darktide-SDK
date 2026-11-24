#pragma once
// A terrain unit from authored heights and paint (stingray/terrain/terrain.h): the unit (a root node, a static actor
// with the height field, the terrain record), its height map and blend mask textures, and copies of the game's terrain
// material (with its shader material and textures) under content-named game-copy names, so the unit carries
// everything it draws with.
#include "compiler/compilation_context.h"
#include "compiler/resource_graph.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace dtglb::compiler {

struct TerrainSpec {
    std::string name;                        // the unit's resource path; its textures are <name>_height / <name>_mask
    std::array<float, 2> size{64, 64};       // metres along x and y, centred on the unit's origin
    std::array<float, 2> height{0, 64};      // the box's min and max z: samples 0 and 65535
    std::uint32_t resolution = 256;          // height samples (and blend mask texels) per side
    std::vector<std::uint16_t> heights;      // resolution x resolution, row-major from -y up (empty: all at min z)
    std::vector<std::uint8_t> mask;          // RGBA8, resolution x resolution, top row at +y (empty: the base material)
    std::uint64_t material = 0;              // the game's terrain material (0: the one most retail terrains use)
    std::array<std::uint32_t, 5> surface_materials{};  // footstep/impact surfaces: base, then per mask channel (0: brick)
    std::uint64_t source = 0;                // a game terrain unit whose node, actor and terrain settings are kept
};

// the resources go into graph (roots: the unit)
bool build_terrain_resources(const TerrainSpec& spec, ResourceGraph& graph, std::string& error);

}  // namespace dtglb::compiler
