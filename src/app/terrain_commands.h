#pragma once
// The terrain commands of the compiler (stingray/terrain/terrain.h):
//   --terrain-read <unit>...                     each unit's terrains as one JSON line, checked against the game's data
//   --terrain-unit <spec.json> <out folder>      a terrain unit (compiler/terrain_builder.h) as a Custom Assets folder.
//     The spec: {"name": unit resource path, "size": [x, y] metres, "height": [min z, max z], "resolution": samples per
//     side, "heights": a file of resolution^2 little-endian u16 (rows from -y up), "mask": a file of resolution^2 RGBA8
//     (rows from +y down), "material": the game's terrain material, "surface_materials": [base, r, g, b, a],
//     "source": a game terrain unit to start from}; files relative to the spec, resources as paths or "#ID[<hex>]".
namespace dtglb::app {

// true when argv[1] is a terrain command (exit_code is then its result)
bool run_terrain_command(int argc, char** argv, int& exit_code);

}  // namespace dtglb::app
