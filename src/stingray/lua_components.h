#pragma once
// The game's gameplay components (scripts/components/*.lua): each script declares its component with the global
// component(name), then fills its component_data, the fields a placed unit can set:
// field = {ui_type (check_box, number, slider, text_box, combo_box, resource, vector, color, struct_array, *_array),
// value (the default; vectors Vector3Box(x, y, z), colours QuaternionBox(...)), ui_name, category, options_keys /
// options_values, min / max / step / decimals, filter, size / values (arrays), definition (struct array members)}.
// The scripts are compiled (LuaJIT); their main chunk is run here symbolically: constants, register moves, table
// building (TNEW, TDUP, TSETS, TSETB, TSETV, TGETS), globals (GGET, GSET) and calls of globals with their arguments.
// A placed unit keeps its components in its script data: component_guids (GUID strings) and components/<IdString32
// of the GUID>/{name, component_data/<field>}; Lua reads a field with Unit.get_data(unit, "components", guid,
// "component_data", field[, index][, member][, "resource"]), array indices from 1, stored from 0.
#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace dtglb::stingray {

struct LuaObject;
// a value the main chunk of a script builds: a constant, or a table / call / unknown (LuaObject)
using LuaData = std::variant<std::monostate, bool, std::int64_t, double, std::string, std::shared_ptr<LuaObject>>;

struct LuaObject {
    // table: fields in the order the script sets them (a constructor's constant part in the dump's order first);
    // call: a global function called with arguments (Vector3Box(x, y, z), QuaternionBox(Color(...)));
    // unknown: anything else (local functions, arithmetic, values from other scripts), `function` says what
    enum Kind { table, call, unknown } kind = table;
    std::string function;
    std::vector<std::pair<LuaData, LuaData>> fields;
    std::vector<LuaData> arguments;
    const LuaData* get(const std::string& key) const;
    const LuaData* get(std::int64_t index) const;
};

struct Component {
    std::string name;
    std::shared_ptr<LuaObject> data;  // component_data: field name -> definition table; null when it has none
};

// body: a compiled component script (the resource after its bundle envelope)
bool read_lua_components(const std::vector<std::uint8_t>& body, std::vector<Component>& components, std::string& error);
// the value a compiled data script's main chunk returns (a level's _main_path / _volume_data: return {...})
bool read_lua_returned(const std::vector<std::uint8_t>& body, LuaData& returned, std::string& error);

}  // namespace dtglb::stingray
