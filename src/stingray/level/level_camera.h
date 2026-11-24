#pragma once
// A level's camera record (level header +0x18: u32 count, then 0x30-byte records sorted by unit number): the level's
// settings for one camera of one of its units, copied over the named camera at spawn (Level spawn: Unit::find_camera,
// Camera::set_camera_data, projection data). u32 unit number, u32 camera name (IdString32), near range, far range,
// vertical fov (radians), the projection's other words (0 in retail but +0x28, 1 = perspective).
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

namespace dtglb::stingray::level {

struct LevelCamera {
    std::uint32_t unit = 0, name = 0;
    float near_range = 0.1f, far_range = 1000, vertical_fov = 0.785398f;
    std::array<std::uint32_t, 7> projection{0, 0, 0, 0, 0, 1, 0};
};

inline bool read_level_camera(const std::vector<std::uint8_t>& record, LevelCamera& camera) {
    if (record.size() != 0x30) return false;
    std::memcpy(&camera.unit, record.data(), 4);
    std::memcpy(&camera.name, record.data() + 4, 4);
    std::memcpy(&camera.near_range, record.data() + 8, 4);
    std::memcpy(&camera.far_range, record.data() + 12, 4);
    std::memcpy(&camera.vertical_fov, record.data() + 16, 4);
    std::memcpy(camera.projection.data(), record.data() + 20, 28);
    return true;
}

inline std::vector<std::uint8_t> write_level_camera(const LevelCamera& camera) {
    std::vector<std::uint8_t> record(0x30);
    std::memcpy(record.data(), &camera.unit, 4);
    std::memcpy(record.data() + 4, &camera.name, 4);
    std::memcpy(record.data() + 8, &camera.near_range, 4);
    std::memcpy(record.data() + 12, &camera.far_range, 4);
    std::memcpy(record.data() + 16, &camera.vertical_fov, 4);
    std::memcpy(record.data() + 20, camera.projection.data(), 28);
    return record;
}

}  // namespace dtglb::stingray::level
