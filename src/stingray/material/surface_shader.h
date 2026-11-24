#pragma once

#include "stingray/material/material_v61.h"

#include <cstdint>
#include <string>
#include <vector>

namespace dtglb::stingray::material {

// A lit shader of the asset's own, written as one HLSL surface function the way the Stingray material editor's
// output node takes its inputs:
//     void surface(SurfaceInput i, inout Surface s)
// The compiler wraps it in the game's vertex shaders (static and skinned), the G-buffer writer, texture streaming
// feedback and the shadow pass, and builds a shader material stream (default + shadow_caster contexts) from it.
// A decal instead draws in the decals layer like the game's projection decals: the mesh's box is projected along
// its local Y axis onto whatever the G-buffer holds inside it, and the surface's base colour is blended over that
// by its opacity (the surface underneath keeps its normal, roughness and lighting).
struct SurfaceParameter {
    std::string name;          // an HLSL identifier the code reads; a material variable of the same name
    std::vector<float> value;  // 1..4 floats: its default
};

struct SurfaceShaderSpec {
    std::string code;                            // HLSL defining surface()
    std::vector<SurfaceParameter> parameters;
    std::vector<std::string> textures;           // sampled in the code as sample_<name>(uv)
    // textures mods replace in game (Material.set_resource): bound as plain textures like the game's render target
    // slots, since a texture made at runtime has no streaming data for the bindless path
    std::vector<std::string> runtime_textures;
    bool alpha_clip = false;                     // discard where s.opacity < alpha_cutoff (G-buffer and shadow)
    float alpha_cutoff = 0.5f;
    bool double_sided = false;                   // no back-face culling; back faces see the flipped normal
    bool decal = false;                          // a projection decal (no shadow pass, double_sided unused)
};

// The complete HLSL every stage of the shader is compiled from (the author's code included as is).
std::string surface_shader_hlsl(const SurfaceShaderSpec& spec);

// The shader material stream: shader section with the compiled stages, the sampler names it binds and its default
// variable values. own_name is the material resource's name hash (the section names itself by its high 32 bits).
bool build_surface_shader(const SurfaceShaderSpec& spec, std::uint64_t own_name, MaterialStream& out, std::string& error);

}
