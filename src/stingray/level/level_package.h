#pragma once
// A level as a Custom Assets folder: the cooked .level, a build.json naming it as the folder's root, and copies of the
// mod's own resources it places (units from other Custom folders, with everything their build.json lists), so that
// loading the level's package loads them too. That package is named after the level, as the game's level packages are
// (a mission loads its level by loading the package of the level's name). The game's resources it uses stay outside,
// listed in external_resources for whatever spawns the level to load first (Custom Levels does it through Resource Loader): the units it places,
// its effects, its modules (nested levels), its shading environment, and the units and effects its flow names. They
// are listed by path where the names dictionary knows it, else as #ID[<16 hex>].
// The level's data scripts the game loads by name (<level>_volume_data, <level>_main_path) come from the Lua next to the
// level file, compiled as Lua resources (lua_data_compiler.h) with their source beside them; the level's own script
// (<stem>_script.lua, <stem>_scripts/) is copied as plain files for Custom Levels to run.
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "stingray/level/level_document.h"

namespace dtglb::stingray::level {

// an empty level named `path` (a resource path without extension): no units, the empty level flow every small game
// level has
LevelDocument new_level_document(const std::string& path);

// the mod's own resources: everything the build.json files in the Custom folders list
struct ModFiles {
    std::map<std::pair<std::string, std::uint64_t>, std::filesystem::path> resources;  // {type, name hash} -> file
    std::map<std::string, std::filesystem::path> streams;                              // stream name (data/xx/...) -> file
    std::map<std::uint64_t, std::string> names;                                        // name hash -> resource path
};
ModFiles mod_files(const std::vector<std::filesystem::path>& custom_folders);

// the mod's own unit files: the "unit" resources of every build.json in the Custom folders, by name hash
std::map<std::uint64_t, std::filesystem::path> mod_unit_files(const std::vector<std::filesystem::path>& custom_folders);

// `level_file`: a cooked .level; `custom_folders`: Custom folders whose assets the level may place
bool write_level_folder(const std::filesystem::path& level_file, const std::filesystem::path& folder,
                        const std::vector<std::filesystem::path>& custom_folders, std::string& summary, std::string& error,
                        const std::unordered_map<std::uint64_t, std::string>& names = {});

// a resource name dictionary (one resource path per line) by name hash
std::unordered_map<std::uint64_t, std::string> read_name_dictionary(const std::filesystem::path& file);

}  // namespace dtglb::stingray::level
