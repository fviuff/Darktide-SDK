#pragma once
// Navigation data (NavData) for a level, made by the game's own navigation generator: the gwnav plugin the game ships
// (<game>/binaries/plugins/gwnav_pluginw64_release.dll) holds Fatshark's generator. It is loaded into this process and
// given the few engine services it asks for (allocators, logging, locks, jobs); its generation C API then takes
// triangles, seed points and settings and writes the mesh and its sparse companion as standalone NavData files
// (BlobAggFile), the bytes a NavData resource holds.
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace dtglb::stingray::navigation {

// GwNavGeneration settings; the defaults are the game's (scripts/loading/expedition_spawner.lua navgen_settings),
// which the generator normalises to the grid every retail mesh stores (32 raster pixels of ~0.095 m per cell).
struct NavSettings {
    float entity_radius = 0.38f, entity_height = 1.6f;
    float slope_max = 60.0f, step_max = 0.5f;
    float min_navigable_surface = 0.5f, altitude_tolerance = 0.5f;
    float raster_precision = 0.1f, cell_size = 3.0f;
    float height_field_sampling = 1.0f;
    bool consume_terrain = true;
    // what a level unit gives the generator unless its script data says (gwnavgen_fromphysicsmesh /
    // gwnavgen_fromrendermesh): its static collision (as the game's own runtime bake, bake_navmesh.lua), or its render
    // meshes (the Stingray editor's default; a whole mission's render meshes crash the generator, transit/hab_b)
    bool consume_physics_meshes = true;
};

// a tag volume (the Stingray editor's gwnav volumes: level volumes whose type names gwnavexclusivetagvolume,
// gwnavtagvolume_layer<n> or gwnavtagvolume_smartobject<n>): an outline extruded between two heights. Exclusive ones
// cut their area out of the navigation (the only kind retail levels use); the others tag it with a layer or smart object.
struct NavTagVolume {
    std::vector<std::array<float, 3>> outline;  // world space, z up
    float bottom = 0, top = 0;                  // heights
    bool exclusive = true;
    int layer = -1, smart_object = -1;
    std::array<float, 4> color{255, 255, 0, 0};  // as Stingray's Color(a, r, g, b)
};

struct NavInput {
    std::vector<std::array<float, 3>> vertices;  // world space, z up
    std::vector<std::uint32_t> indices;          // three per triangle
    std::vector<std::array<float, 3>> seeds;     // points in the walkable areas to keep; none keeps every area
    std::vector<NavTagVolume> tag_volumes;
};

struct NavOutput {
    std::vector<std::uint8_t> mesh, sparse;  // <sector>.NavData and <sector>_sparse.NavData
    std::string log;                         // what the generator reported
};

// sector: the sector's name (it is written into both files); guid: its GUID text (xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx).
bool generate_navdata(const std::filesystem::path& game_folder, const NavInput& input, const NavSettings& settings,
                      const std::string& sector, const std::string& guid, NavOutput& out, std::string& error);

// a stable sector GUID for a name
std::string sector_guid(const std::string& name);

}  // namespace dtglb::stingray::navigation
