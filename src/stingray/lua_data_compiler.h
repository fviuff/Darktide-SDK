#pragma once
// Data scripts (a level's <level>_volume_data and <level>_main_path) compiled to the game's Lua resources.
// The script form: `local <name> = <value>` lines, then `return <value>`; a value is a table constructor ({...} with
// positional items, `name = value`, `[number] = value`, `["string"] = value}), a number, a string, true / false / nil or
// a local's name; -- comments. The result is a LuaJIT 2.1 bytecode dump (the game runs LuaJIT 2.1 without 64-bit
// frames, magic ESC "FS" and version 0x82 where stock LuaJIT writes ESC "LJ" and 2); its bytecode builds the same
// tables the script does: constant parts as template tables (TDUP), the rest stored field by field.
#include <cstdint>
#include <string>
#include <vector>

namespace dtglb::stingray {

enum class LuaDumpForm {
    game,     // ESC "FS", version 0x82, flags 0: what the game loads
    luajit,   // ESC "LJ", version 2, flag FR2: what a stock 64-bit LuaJIT 2.1 loads (for checking)
};

// chunk_name: the script's name as the game's own carry it ("@<resource path>.lua")
bool compile_lua_data(const std::string& source, const std::string& chunk_name, LuaDumpForm form, std::vector<std::uint8_t>& dump,
                      std::string& error);

// a Lua resource's body around a dump (as every retail one: u32 0, dump size, 40, 2, 0, dump size + 40, then the dump)
std::vector<std::uint8_t> lua_resource_body(const std::vector<std::uint8_t>& dump);

}  // namespace dtglb::stingray
