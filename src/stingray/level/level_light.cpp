#include "stingray/level/level_light.h"

#include <cstring>

namespace dtglb::stingray::level {

namespace {

template <typename T>
T get(const std::vector<std::uint8_t>& b, std::size_t at) {
    T v{};
    std::memcpy(&v, b.data() + at, sizeof v);
    return v;
}

template <typename T>
void put(std::vector<std::uint8_t>& b, T v) {
    const std::size_t at = b.size();
    b.resize(at + sizeof v);
    std::memcpy(b.data() + at, &v, sizeof v);
}

}  // namespace

bool read_level_light(const std::vector<std::uint8_t>& record, LevelLight& light) {
    if (record.size() < 0x60) return false;
    light.unit = get<std::uint32_t>(record, 0);
    light.name = get<std::uint32_t>(record, 4);
    for (std::size_t i = 0; i < 3; ++i) light.colour[i] = get<float>(record, 8 + 4 * i);
    light.temperature = get<float>(record, 0x14);
    light.intensity = get<float>(record, 0x18);
    light.volumetric = get<float>(record, 0x1c);
    light.particle = get<float>(record, 0x20);
    for (std::size_t i = 0; i < 6; ++i) light.parameters[i] = get<float>(record, 0x24 + 4 * i);
    for (std::size_t i = 0; i < 2; ++i) light.extra[i] = get<float>(record, 0x3c + 4 * i);
    light.flags = get<std::uint32_t>(record, 0x44);
    light.type = get<std::uint32_t>(record, 0x48);
    light.shadow_resolution = get<std::uint32_t>(record, 0x4c);
    light.material = get<std::uint64_t>(record, 0x50);
    light.ies = get<std::uint64_t>(record, 0x58);
    if (light.type == 9) {
        if (record.size() < 0xa4) return false;
        light.probe.assign(record.begin() + 0x60, record.begin() + 0xa4);
    } else {
        light.probe.clear();
    }
    return true;
}

std::vector<std::uint8_t> write_level_light(const LevelLight& light) {
    std::vector<std::uint8_t> b;
    put(b, light.unit);
    put(b, light.name);
    for (const float c : light.colour) put(b, c);
    put(b, light.temperature);
    put(b, light.intensity);
    put(b, light.volumetric);
    put(b, light.particle);
    for (const float p : light.parameters) put(b, p);
    for (const float e : light.extra) put(b, e);
    put(b, light.flags);
    put(b, light.type);
    put(b, light.shadow_resolution);
    put(b, light.material);
    put(b, light.ies);
    if (light.type == 9) {
        std::vector<std::uint8_t> probe = light.probe;
        probe.resize(0x44, 0);
        b.insert(b.end(), probe.begin(), probe.end());
    }
    return b;
}

}  // namespace dtglb::stingray::level
