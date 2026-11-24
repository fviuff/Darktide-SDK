#pragma once
// Names for the IdString32 hashes a flow stores instead of names (events, variables): the string constants of the
// game's compiled Lua (the names its scripts send to and read from flows), from the game's bundles.
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace dtglb::stingray::flow {

// a name the flows of the game use, by IdString32 (from the game's scripts, flow strings and node definitions); empty
// when unknown
const char* known_flow_name(std::uint32_t id);

// only: the Lua resources to read (their names), empty = every one the game has
bool lua_string_names(const std::filesystem::path& bundle_folder, std::unordered_map<std::uint32_t, std::string>& names,
                      std::string& error, const std::vector<std::uint64_t>& only = {});

}  // namespace dtglb::stingray::flow
