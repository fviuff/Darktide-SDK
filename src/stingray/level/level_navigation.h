#pragma once
// A level's navigation, as the game's levels carry it: two NavData resources (the mesh and its sparse companion, made by
// the game's generator: stingray/navigation/gwnav_generator.h) listed in the level's nav data table, mesh first.
// The generator walks on the level's static collision: the solid blockout boxes, the static actors of every unit
// the level places (game units from the game's bundles, the mod's own from its Custom folders) and their terrains
// (NavSettings::consume_terrain). Units of
// core/gwnav/units/seedpoint/seedpoint mark the walkable areas to keep (the game's seed point unit); with none, every
// area is kept.
// The resources are named after the level: <level folder>/navigation/<level name> and <...>_sparse, and their files
// (cooked resources: the 38-byte envelope, then the NavData) sit next to the level file: navigation/<level name>.navdata
// and navigation/<level name>_sparse.navdata.
// Level volumes whose type names gwnavexclusivetagvolume cut their area out (gwnavtagvolume_layer<n> /
// gwnavtagvolume_smartobject<n> tag it), read from the level's volume data script next to the level file
// (<level file stem>_volume_data.lua, as bake_navmesh.lua reads it). A placed unit's own level data can set the same keys
// as a unit's script data (gwnavgenexcluded, gwnavseedpoint, gwnavgen_fromphysicsmesh, gwnavgen_fromrendermesh); they win.
// The level data keeps the level's settings (gwnavgen/entity_radius, entity_height, slope_max, step_max) and, after a
// build, a hash of what it was built from (gwnavgen/inputs).
#include "stingray/level/level_document.h"
#include "stingray/navigation/gwnav_generator.h"

#include <array>
#include <filesystem>
#include <string>
#include <vector>

namespace dtglb::stingray::level {

constexpr std::uint64_t kNavDataType = 0x23ee957dd4b1bb94ull;  // "NavData"

struct NavigationNames {
    std::string mesh, sparse;                    // resource names
    std::filesystem::path mesh_file, sparse_file;  // next to the level file
};
// level_name: the level's resource name (without .level)
inline NavigationNames navigation_names(const std::filesystem::path& level_file, const std::string& level_name) {
    NavigationNames n;
    const auto slash = level_name.find_last_of('/');
    const std::string folder = slash == std::string::npos ? std::string() : level_name.substr(0, slash + 1);
    const std::string base = slash == std::string::npos ? level_name : level_name.substr(slash + 1);
    n.mesh = folder + "navigation/" + base;
    n.sparse = n.mesh + "_sparse";
    n.mesh_file = level_file.parent_path() / "navigation" / std::filesystem::u8path(base + ".navdata");
    n.sparse_file = level_file.parent_path() / "navigation" / std::filesystem::u8path(base + "_sparse.navdata");
    return n;
}

// the level's settings from its level data over `settings`
void level_navigation_settings(const LevelDocument& d, navigation::NavSettings& settings);

// a hash of everything the navigation is made from: the blockout, the placed units (with the file of each the mod has)
// and their level data, the volumes and their script, the level's settings. Equal to stored_navigation_inputs while the
// navigation is current.
std::string navigation_inputs(const LevelDocument& d, const std::string& volume_data_script,
                              const std::vector<std::filesystem::path>& custom_folders);
std::string stored_navigation_inputs(const LevelDocument& d);  // empty: never built

// the tag volumes of a volume data script (Lua source as the editor writes it)
std::vector<navigation::NavTagVolume> navigation_tag_volumes(const std::string& volume_data_script);

// Builds the level's navigation and writes it next to the level file, then lists it in the level (the level file is
// written back). seeds are added to the level's own seed points.
bool build_level_navigation(const std::filesystem::path& level_file, const std::filesystem::path& game_folder,
                            const std::vector<std::filesystem::path>& custom_folders,
                            const std::vector<std::array<float, 3>>& seeds, const navigation::NavSettings& settings,
                            std::string& summary, std::string& error, std::string* report = nullptr);

}  // namespace dtglb::stingray::level
