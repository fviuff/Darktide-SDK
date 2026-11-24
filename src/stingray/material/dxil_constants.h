#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace dtglb::stingray::material {

// DXBC container hash (bytes 4..20), checked by D3D12
std::array<std::uint8_t, 16> dxil_container_hash(const std::uint8_t* data, std::size_t size);

// Replaces float32 constants by value ({old bits: new bits}) and re-signs the container
bool patch_dxil_float_constants(std::vector<std::uint8_t>& container, const std::map<std::uint32_t, std::uint32_t>& edits,
                                std::set<std::uint32_t>& matched, std::string& error);

// Same for every shader stage in a material stream's shader section
bool patch_stream_float_constants(std::vector<std::uint8_t>& stream, const std::map<std::uint32_t, std::uint32_t>& edits,
                                  std::set<std::uint32_t>& matched, std::string& error);

}
