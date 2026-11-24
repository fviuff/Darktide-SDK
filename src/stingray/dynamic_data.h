#pragma once
// The engine's flat dynamic data (stingray::level_resource::unpack_dynamic_data): a level's own data, the data it gives
// each placed unit and each nested level (a UNIT's own script data is a tree, stingray/unit/script_data.h). u32 count; per entry u32 n, n x u32 path (IdString32 keys), u32 type, then the value: 1 u32, 2 f32, 3 u32 size
// + bytes padded to 4.
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

namespace dtglb::stingray {

struct DynamicEntry {
    std::vector<std::uint32_t> path;
    std::uint32_t type = 0;
    std::uint32_t number = 0;    // type 1 (also booleans)
    float scalar = 0;            // type 2
    std::vector<std::uint8_t> bytes;  // type 3 (strings without their terminator when they are text)
};

// at: where the data starts; size gets the bytes it takes
bool read_dynamic_data(const std::vector<std::uint8_t>& data, std::size_t at, std::vector<DynamicEntry>& out,
                       std::size_t& size, std::string& error);

// the entries as read_dynamic_data reads them (string padding zero)
void write_dynamic_data(const std::vector<DynamicEntry>& entries, std::vector<std::uint8_t>& out);

// the entry at a path of names (each hashed to IdString32), or null
const DynamicEntry* find_dynamic(const std::vector<DynamicEntry>& entries, std::initializer_list<const char*> path);

}  // namespace dtglb::stingray
