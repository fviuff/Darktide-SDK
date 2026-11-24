#pragma once
// Compiled Lua resources (the game's scripts are LuaJIT 2.1 bytecode dumps, magic ESC "FS" in place of ESC "LJ"): their
// prototypes (bytecode and constants) and the constant tables (KTAB) of every prototype, the literal fields of a table
// constructor.
// Resource body: u32 0, u32 dump size, ..., the dump as its last bytes. Dump: ESC F S, version, uleb flags (2 =
// stripped), the chunk name unless stripped, prototypes {uleb size, flags, params, frame size, upvalue count, uleb
// constant counts (gc, numbers), uleb bytecode count, debug sizes unless stripped, bytecode, upvalues, gc constants
// (0 child, 1 table {uleb array size, uleb hash size, array values, key/value pairs}, 2/3 64-bit, 4 complex,
// 5+ string of n - 5 bytes), numbers (uleb128_33: low bit of the first byte set = a double, its high word in a second
// uleb; else a 32-bit integer), debug info}, a 0 size at the end; the main chunk is the last prototype. Values: 0 nil,
// 1 false, 2 true, 3 uleb integer, 4 two uleb halves of a double, 5+ a string.
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace dtglb::stingray {

using LuaValue = std::variant<std::monostate, bool, std::int64_t, double, std::string>;

struct LuaTable {
    std::vector<LuaValue> array;
    std::vector<std::pair<LuaValue, LuaValue>> hash;
    const LuaValue* field(const std::string& key) const;  // a string key's value, or null
};

struct LuaPrototype {
    std::vector<std::uint32_t> bytecode;  // op = low byte, A = next byte, C = third, B = high byte, D = high 16 bits
    // gc constants in dump order (bytecode indexes them from the end): a string, a table, or none (child prototype,
    // 64-bit or complex numbers)
    std::vector<std::variant<std::monostate, std::string, LuaTable>> gc;
    std::vector<LuaValue> numbers;  // integers and doubles
};

// body: the resource after its bundle envelope
bool read_lua_prototypes(const std::vector<std::uint8_t>& body, std::vector<LuaPrototype>& prototypes, std::string& error);
bool read_lua_tables(const std::vector<std::uint8_t>& body, std::vector<LuaTable>& tables, std::string& error);

}  // namespace dtglb::stingray
