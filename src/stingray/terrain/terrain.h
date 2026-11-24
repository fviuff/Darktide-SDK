#pragma once
// Darktide terrains: a unit's terrain record (unit_reader.h UnitTerrain) with what goes with it in the unit and in two
// textures. A terrain unit holds one root node, one static actor with a height field shape on it, the terrain record and
// the material slot base_material. Its heights are 16-bit samples over the terrain's box (min..max x/y in the node's
// frame): height = min z + sample / 65535 x (max z - min z). The same samples are in three places:
//   - the height map (layer type 0): an R16 texture, its top row at +y, the terrain shader's vertices read it with
//     texel centres at (i + 0.5) / resolution of the box (bilinear, clamped at the edges);
//   - the height field shape: PhysX samples {i16 sample - 32768, u8 material | tessellation bit 7, u8 material}, row r
//     along +y (local x), column c along +x (local z), the sample at the box's min corner + (c, r) x size / resolution;
//     its material is the blend mask's last channel of at least 128 (r, g, b, a = 0..3), 4 where none is;
//   - the quadtree (UnitTerrain::nodes), each node's heights from the samples it covers.
// The blend mask (layer type 1): an RGBA texture laid out like the height map whose channels lay the terrain material's
// four blend layers over its base, in that order.
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "stingray/unit/unit_reader.h"

namespace dtglb::stingray::terrain {

struct HeightGrid {
    std::uint32_t columns = 0, rows = 0;   // along +x, along +y
    std::vector<std::uint16_t> heights;    // row-major from the box's min corner
    std::vector<std::uint8_t> materials;   // per sample: the height field's material (0-3 a mask channel, 4 the base, 127
                                           // a hole), bit 7 = tessellation flag (cell diagonal from (c, r) to (c+1, r+1))
};

constexpr std::uint32_t kLeafTexels = 32;  // a quadtree leaf covers 32 x 32 height texels
constexpr std::uint8_t kBaseMaterial = 4, kHole = 127;

// the terrain's box as the Stingray terrain compiler sets it: x and y centred on the node, z from min to max; the
// radius from the half sizes and the max z
std::array<float, 10> terrain_bounds(float size_x, float size_y, float z_min, float z_max);

float sample_height(const unit::UnitTerrain& terrain, std::uint16_t sample);
std::uint16_t height_sample(const unit::UnitTerrain& terrain, float height);  // rounded, clamped to the box

const unit::UnitTerrainLayer* terrain_layer(const unit::UnitTerrain& terrain, std::uint32_t type);
// the unit's height field shape for the terrain (an actor shape of type 5 on the terrain's node), null if none
const unit::UnitActorShape* height_field(const unit::Unit& unit, const unit::UnitTerrain& terrain);

bool grid_from_height_field(const unit::UnitActorShape& shape, HeightGrid& out, std::string& error);
// an R16 height map's samples (texture rows top = +y); materials left empty
bool grid_from_height_map(std::uint32_t width, std::uint32_t height, const std::vector<std::uint8_t>& texels, HeightGrid& out,
                          std::string& error);
// the materials of a grid from a blend mask (RGBA8, rows top = +y, any size: sampled at the grid's texel)
void materials_from_mask(const std::vector<std::uint8_t>& rgba, std::uint32_t width, std::uint32_t height, HeightGrid& grid);

// The terrain's surface in the unit's frame (the terrain node's world applied): a grid every `spacing` metres (0 = the
// height grid's own), triangles counter-clockwise seen from above. drawn: placed where the terrain shader draws it (the
// whole box, heights filtered between texel centres), else where the height field collides (cell diagonals per the
// tessellation flag, holes left out). uvs as the shader's: u along +x, v from the +y edge down.
struct Surface {
    std::vector<std::array<float, 3>> positions, normals;
    std::vector<std::array<float, 2>> uvs;
    std::vector<std::uint32_t> indices;
};
bool terrain_surface(const unit::Unit& unit, const unit::UnitTerrain& terrain, const HeightGrid& grid, float spacing, bool drawn,
                     Surface& out, std::string& error);
// the same in another frame: `frame` (row vectors) takes the terrain node's frame there (identity: the node's own)
bool terrain_surface_in(const std::array<float, 16>& frame, const unit::UnitTerrain& terrain, const HeightGrid& grid, float spacing,
                        bool drawn, Surface& out, std::string& error);
constexpr std::array<float, 16> kIdentity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

// the quadtree as the Stingray terrain compiler builds it (build_quad_tree), depth log2(resolution / 32)
std::vector<unit::UnitTerrainNode> build_quadtree(const unit::UnitTerrain& terrain, const HeightGrid& grid);

// the height field shape of a terrain from its grid: samples, sizes and the local matrix
void set_height_field(const unit::UnitTerrain& terrain, const HeightGrid& grid, unit::UnitActorShape& shape);

// texture bodies as the game ships them (kind 0, a plain DDS): the R16 height map
std::vector<std::uint8_t> height_map_body(const HeightGrid& grid);

// a terrain unit's body (UNIT v115): its nodes, actors, terrains, material slots, script data and the byte sections the
// reader keeps; false for a unit with anything else (meshes, skins, lights, ...)
bool write_terrain_unit(const unit::Unit& unit, std::vector<std::uint8_t>& body, std::string& error);

}  // namespace dtglb::stingray::terrain
