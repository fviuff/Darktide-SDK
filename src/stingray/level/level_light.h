#pragma once
// A level's light record (level header +0x14: u32 count, then the records sorted by unit number): the level's
// settings for one light of one of its units (Level spawn "Set light data": finds the light by name on the spawned
// unit and copies the block from word 2 into the light object +0x28.., then sets the material and the IES profile).
// 0x60 bytes: u32 unit number, u32 light name (IdString32), colour rgb (filter), temperature (kelvin), intensity,
// volumetric intensity, particle intensity, 6 parameters (falloff end; falloff start / radius / half width; spot angle
// start / half height / half length; spot angle end; box y, z: as the UNIT light record), 2 more parameter words (light
// +0x5c always 0, +0x60 0.8 in retail; meaning open), flags (1 casts shadows, 2 disabled, 4 baked, 8 dynamic, 0x10 spot
// reflector, 0x20 lens flare, 0x40 force static), type (0 omni, 1 spot, 2 box, 3 directional, 4 directional box,
// 5 sphere, 6 disk, 7 rectangle, 8 tube, 9 ddgi probe), max shadow resolution, material u64, IES profile u64 (resource
// name hash). A ddgi probe (type 9) carries its 0x44-byte probe block after (0xa4 bytes).
#include <array>
#include <cstdint>
#include <vector>

namespace dtglb::stingray::level {

struct LevelLight {
    std::uint32_t unit = 0, name = 0;
    std::array<float, 3> colour{1, 1, 1};
    float temperature = 6570, intensity = 600, volumetric = 0, particle = 0;
    std::array<float, 6> parameters{};
    std::array<float, 2> extra{0, 0.8f};
    std::uint32_t flags = 0, type = 0, shadow_resolution = 128;
    std::uint64_t material = 0, ies = 0;
    std::vector<std::uint8_t> probe;  // type 9: the 0x44-byte probe block
};

bool read_level_light(const std::vector<std::uint8_t>& record, LevelLight& light);
std::vector<std::uint8_t> write_level_light(const LevelLight& light);

}  // namespace dtglb::stingray::level
