#pragma once
// A SHADING_ENVIRONMENT resource (version 22): the lighting, sky, fog, exposure and post settings a level names
// (level data "shading_environment") and the engine hands to every shader as global_environment_settings plus its
// textures. Body:
//   u32 version 22
//   u32 n, n x {u32 IdString32 name, u64 texture}: the engine's own render textures (shadow masks, cutters)
//   u32 n, n x {u32 type, u32 1, u32 IdString32 name, u32 offset}: the variables; types 0 scalar, 1 vector2,
//     2 vector3, 3 vector4, 4 matrix, 6 float array, 10 texture {u64, 8 zero bytes}, 11 volume texture, 12 vector4
//   u32 n, n x {u32 IdString32 setting name ("default"), u32 size, size bytes}: each setting's variable values
//   u32 n, n floats: the variable weights
//   the engine's variable mapping (kept as it is)
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace dtglb::stingray::shading_environment {

struct Variable {
    std::uint32_t type = 0;
    std::uint32_t flags = 1;
    std::uint32_t name = 0;  // IdString32
    std::uint32_t offset = 0, size = 0;  // in a setting's values; size reaches the next variable
};

struct Setting {
    std::uint32_t name = 0;  // IdString32
    std::vector<std::uint8_t> values;
};

struct ShadingEnvironment {
    std::uint32_t version = 22;
    std::vector<std::pair<std::uint32_t, std::uint64_t>> engine_textures;
    std::vector<Variable> variables;
    std::vector<Setting> settings;
    std::vector<float> weights;
    std::vector<std::uint8_t> mapping;

    // by IdString32 or by name: the variable, its value in the first setting as floats (scalar..vector4), the texture
    // it names
    const Variable* find(std::uint32_t name) const;
    std::optional<std::vector<float>> floats(std::uint32_t name) const;
    std::uint64_t texture(std::uint32_t name) const;
    const Variable* find(const char* name) const;
    std::optional<std::vector<float>> floats(const char* name) const;
    std::uint64_t texture(const char* name) const;
};

// body: the resource after its bundle envelope
bool read_shading_environment(const std::vector<std::uint8_t>& body, ShadingEnvironment& out, std::string& error);
void write_shading_environment(const ShadingEnvironment& environment, std::vector<std::uint8_t>& body);

}  // namespace dtglb::stingray::shading_environment
