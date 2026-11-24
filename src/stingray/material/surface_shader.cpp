#include "stingray/material/surface_shader.h"

#include "stingray/material/shader_compiler.h"
#include "stingray/material/shader_section.h"
#include "stingray/murmur_hash.h"
#include "stingray/texture/texture_writer.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <set>
#include <sstream>

namespace dtglb::stingray::material {
namespace {

constexpr std::uint32_t kDefaultContext = 0xf2760503u;        // "default"
constexpr std::uint32_t kShadowCasterContext = 0x3100c3d2u;   // "shadow_caster"
constexpr std::uint32_t kGbufferLayer = 0xc0d9fff4u;          // "gbuffer"
constexpr std::uint32_t kDecalsLayer = 0x86c8c90fu;           // "decals"
constexpr std::uint32_t kGbufferBranchKey = 0x45cf47cfu;      // the branch key every retail gbuffer pass carries
constexpr std::uint32_t kSurfaceMaterialContext = 0x0254a04bu;
constexpr std::uint32_t kSurfaceMaterialDefault = 0xc7380305u;
constexpr std::uint64_t kRenderConfig = 0x209fb8c3c0a8c3a4ull; // the game's one render config
constexpr std::uint32_t kNumSkinWeights = 0x7f9e89fdu;        // condition intrinsic num_skin_weights

// The render states every retail pass starts from (Stingray's default block), then the gbuffer and shadow_caster
// overrides of substance_basic's passes: {key, kind, value, extra}; later entries win.
const std::vector<RenderState> kBaseStates = {
    {30, 0, 0, 0}, {3, 0, 0, 0}, {29, 0, 0, 0}, {8, 0, 1, 0}, {28, 0, 0, 0}, {1, 0, 3, 0}, {5, 0, 0, 0},
    {15, 0, 0, 0}, {18, 0, 1, 0}, {22, 0, 1, 0}, {21, 0, 8, 0}, {25, 0, 8, 0}, {16, 0, 255, 0}, {20, 0, 1, 0},
    {24, 0, 1, 0}, {26, 0, 1, 0}, {17, 0, 255, 0}, {19, 0, 1, 0}, {23, 0, 1, 0}, {94, 0, 15, 0}, {95, 0, 15, 0},
    {96, 0, 15, 0}, {97, 0, 15, 0}, {98, 0, 15, 0}, {99, 0, 15, 0}, {100, 0, 15, 0}, {101, 0, 15, 0},
    {12, 0, 1, 0}, {14, 0, 4, 0}, {13, 0, 1, 0},
};
// independent blend, stencil class 0 written through mask 0x78, per-target write masks (RT4 x, RT6 xyz, RT7 off)
const std::vector<RenderState> kGbufferStates = {
    {29, 0, 1, 0}, {26, 0, 0, 0}, {15, 0, 1, 0}, {18, 0, 1, 0}, {22, 0, 1, 0}, {21, 0, 8, 0}, {25, 0, 8, 0},
    {16, 0, 120, 0}, {20, 0, 3, 0}, {24, 0, 3, 0}, {17, 0, 120, 0}, {19, 0, 1, 0}, {23, 0, 1, 0},
    {98, 0, 1, 0}, {99, 0, 15, 0}, {100, 0, 7, 0}, {101, 0, 0, 0}, {1, 0, 3, 0},
};
// no colour writes, depth bias 255 and slope bias 2.5
const std::vector<RenderState> kShadowStates = {
    {1, 0, 3, 0}, {94, 0, 0, 0}, {95, 0, 0, 0}, {96, 0, 0, 0}, {97, 0, 0, 0}, {3, 0, 255, 0}, {5, 0, 0x40200000u, 0},
};
// the decals pass of the game's blood decals with only the base colour written: alpha blended into RT0's rgb (the
// surface's ao stays), RT1 and RT2 off, front faces culled so the box draws from inside too, depth test greater
// or equal without writes, and the stencil test that keeps decals off weapons and characters (decal group bits 0x60)
const std::vector<RenderState> kDecalStates = {
    {30, 0, 1, 0}, {54, 0, 1, 0}, {46, 0, 6, 0}, {38, 0, 5, 0}, {13, 0, 0, 0}, {21, 0, 3, 0}, {25, 0, 3, 0},
    {16, 0, 96, 0}, {26, 0, 0, 0}, {15, 0, 1, 0}, {18, 0, 1, 0}, {22, 0, 1, 0}, {20, 0, 1, 0}, {24, 0, 1, 0},
    {17, 0, 0, 0}, {19, 0, 1, 0}, {23, 0, 1, 0}, {95, 0, 0, 0}, {96, 0, 0, 0}, {1, 0, 2, 0}, {94, 0, 7, 0},
    {29, 0, 1, 0}, {14, 0, 7, 0}, {13, 0, 0, 0},
};
// a material texture's sampler: anisotropic, wrapping in u, v and w
const std::vector<RenderState> kTextureSampler = {{1, 0, 1, 0}, {2, 0, 1, 0}, {3, 0, 1, 0}, {0, 0, 21, 0}, {4, 0, 0, 0}};
// a decal's texture clamps, so its edges never wrap around
const std::vector<RenderState> kDecalTextureSampler = {{1, 0, 3, 0}, {2, 0, 3, 0}, {3, 0, 3, 0}, {0, 0, 21, 0}, {4, 0, 0, 0}};
// the engine textures a decal reads, point sampled and clamped like the game's own decals
const std::vector<RenderState> kEngineSampler = {{1, 0, 3, 0}, {2, 0, 3, 0}, {3, 0, 3, 0}, {0, 0, 0, 0}};

// condition_language v1: num_skin_weights() == 4 (retail's skinned / static pick)
std::vector<std::uint8_t> skinned_condition() {
    std::vector<std::uint8_t> out;
    const auto u16 = [&](std::uint16_t v) { out.push_back(v & 0xff); out.push_back(v >> 8); };
    u16(1); u16(8); u16(12); u16(1);
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(kNumSkinWeights >> (8 * i)));
    for (const std::uint16_t word : {0x3001, 0x7007, 0x2000, 0x1000, 0x3004, 0x5005, 0x5007, 0x9000}) u16(word);
    return out;
}

const char* kEngineInterface = R"(
cbuffer global_viewport
{
    float3 camera_unprojection; float3 camera_pos;
    float4x4 camera_view; float4x4 camera_inv_view; float4x4 camera_world; float4x4 camera_last_world;
    float4x4 camera_last_view; float4x4 camera_last_inv_view;
    float4x4 camera_projection[2]; float4x4 camera_inv_projection[2]; float4x4 camera_last_projection[2];
    float4x4 camera_last_inv_projection[2]; float4x4 camera_last_view_projection[2];
    float4x4 camera_last_inv_view_projection[2]; float4x4 camera_view_projection[2];
    float4x4 camera_inv_view_projection[2];
    float time; float delta_time; float2 streamer_write_feedback_threshold; float2 sampler_lod_bias;
    float frame_number; float2 back_buffer_size; float2 output_rt_size; float4x4 hdr_content_to_monitor_rec;
    float taa_enabled; float jitter_enabled; float upscaling_enabled; float debug_rendering; float gamma;
    float lens_quality_color_fringe_enabled; float lens_quality_distortion_enabled;
    float volumetric_reprojection_amount; float volumetric_volumes_enabled; float sun_shadows;
    float capture_cubemap; float capture_ddgi; float hair_wrapped_diffuse; float4 viewport; float is_editor;
    float2 mouse_uv; float rt_reflections_enabled; float rt_particle_reflections_enabled;
    float rt_relax_denoiser_enabled; float dlss_rr_enabled; float gtao_enabled; float cacao_enabled;
    float rt_mixed_reflections; float rt_shadow_ray_multiplier; float rtxgi_enabled; float baked_ddgi; float dxr;
    float rt_checkerboard_reflections; float3 camera_near_far; float direct_diffuse_enabled;
    float direct_specular_enabled; float indirect_diffuse_enabled; float indirect_specular_enabled;
    float occlusion_debug_opacity; float occlusion_debug_far; float occlusion_debug_near;
    float occluder_debug_opacity; float occluder_debug_far; float hdr_paper_white_nits; float debug_hdr_compare;
    float reverse_z; float terrain_displacement_min_distance; float terrain_displacement_max_distance;
    float terrain_tesselation_min_distance; float terrain_tesselation_max_distance; float fsr_rr_enabled;
    float fsr_rr_mirror_passthrough_depth_enabled; float fsr_rr_mirror_passthrough_roughness_enabled;
    float fsr_rr_mirror_passthrough_threshold;
};

cbuffer global_environment_settings
{
    float minion_rimlight; float minion_glowing_eyes; float minion_environment_mask; float emissive_intensity;
    float global_roughness_multiplier; float metallic_roughness_multiplier; float non_metallic_roughness_multiplier;
    float global_reflectance_multiplier; float reflection_boost; float reflection_smooth_bias; float ambient_enabled;
    float3 global_ambient_tint; float3 local_ambient_tint; float ambient_depth_occlusion_enabled;
    float ambient_normal_weight_enabled; float2 specular_ao_intensity; float specular_ao_horizon_occlusion;
    float sun_enabled; float3 sun_direction; float3 sun_color; float sun_disk_enabled; float sun_disk_size;
    float3 local_shadow_map_bias; float3 sun_shadow_map_bias; float3 ssm_shadow_map_bias;
    float sun_shadows_enabled; float ssm_enabled; float skydome_u_offset; float skydome_intensity;
    float3 skydome_tint_color; float3 skydome_flow_direction; float skydome_flow_speed; float skydome_flow_tiling;
    float2 skydome_cloud_speed_scale; float3 wind_amount; float eye_intensity; float fog_enabled; float3 fog_color;
    float3 fog0_settings; float3 fog1_settings; float2 skydome_fog_height_falloff; float volumetric_lighting_enabled;
    float volumetric_global_light_multiplier; float volumetric_local_light_multiplier; float volumetric_distance;
    float volumetric_phase; float volumetric_extinction; float volumetric_ambient_multiplier;
    float world_interaction_window_size; float world_interaction_water_window_size; float tonemap_use_custom;
    float tonemap_slope; float tonemap_toe; float tonemap_shoulder; float tonemap_black_clip; float tonemap_white_clip;
    float pathtraced_accumulation_frames; float4 ies_atlas_size;
};

// bindless resources the engine binds for every material
Texture2D<float4> global_texture2D[] : register(t0, space2);
SamplerState global_samplers[] : register(s0, space2);
SamplerState static_minlod_sampler : register(s0, space31);
RWBuffer<uint> global_feedback_buffers[] : register(u0, space31);

static const float2 halton[32] = {
    float2(0, -0.166667014), float2(-0.25, 0.166666985), float2(0.25, -0.388889015), float2(-0.375, -0.0555559993),
    float2(0.125, 0.277778029), float2(-0.125, -0.277778), float2(0.375, 0.0555559993), float2(-0.4375, 0.388889015),
    float2(0.0625, -0.462962985), float2(-0.1875, -0.129629999), float2(0.3125, 0.203704), float2(-0.3125, -0.351852),
    float2(0.1875, -0.0185190141), float2(-0.0625, 0.314814985), float2(0.4375, -0.240741014), float2(-0.46875, 0.0925930142),
    float2(0.03125, 0.42592603), float2(-0.21875, -0.425926), float2(0.28125, -0.0925930142), float2(-0.34375, 0.240741014),
    float2(0.15625, -0.314814985), float2(-0.09375, 0.0185189843), float2(0.40625, 0.351852), float2(-0.40625, -0.203704),
    float2(0.09375, 0.129630029), float2(-0.15625, 0.462962985), float2(0.34375, -0.487654001), float2(-0.28125, -0.154321015),
    float2(0.21875, 0.179012001), float2(-0.03125, -0.376542985), float2(0.46875, -0.0432099998), float2(-0.484375, 0.290122986),
};

// TAA jitter in NDC, as the game's own vertex shaders add it
float2 jitter()
{
    float enabled = jitter_enabled * (debug_rendering == 0) * (capture_cubemap == 0);
    float2 h = halton[(uint)frame_number & 31];
    return 2.0 * h / (viewport.xy * output_rt_size) * enabled;
}

float2 sign_not_zero(float2 v)
{
    return float2(v.x >= 0 ? 1.0 : -1.0, v.y >= 0 ? 1.0 : -1.0);
}

float3 oct_decode(float2 e)
{
    e = e * 2.0 - 1.0;
    float3 n = float3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0) n.xy = (1.0 - abs(n.yx)) * sign_not_zero(n.xy);
    return normalize(n);
}

float2 oct_encode(float3 n)
{
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    float2 e = n.xy;
    if (n.z < 0) e = (1.0 - abs(n.yx)) * sign_not_zero(n.xy);
    return e * 0.5 + 0.5;
}

static uint2 dt_pixel;

// a material texture: its bindless sampler / texture, the resident-mip map it is clamped to, and the streaming
// feedback the game's shaders write (one quad in 8x8, rotating per frame)
float4 dt_sample(uint sampler_index, uint2 texture_index, uint2 minlod, float2 uv)
{
    SamplerState s = global_samplers[sampler_index];
    Texture2D<float4> t = global_texture2D[NonUniformResourceIndex(texture_index.x)];
    Texture2D<float4> resident = global_texture2D[NonUniformResourceIndex(minlod.x)];
    float clamp_lod = resident.Sample(static_minlod_sampler, uv).x * 32.0;
    float4 value = t.SampleBias(s, uv, sampler_lod_bias.x, int2(0, 0), clamp_lod);
#ifndef DT_NO_FEEDBACK
    uint frame = (uint)frame_number;
    uint mine = (((frame ^ dt_pixel.x) | ((frame >> 3) ^ dt_pixel.y)) & 7) == 0 ? 1 : 0;
    if ((mine | QuadReadAcrossX(mine) | QuadReadAcrossY(mine) | QuadReadAcrossDiagonal(mine)) != 0) {
        uint lod = (uint)(max(t.CalculateLevelOfDetail(s, uv) + sampler_lod_bias.x, 0.0) * 8.0);
        uint width = minlod.y >> 16, height = minlod.y & 65535;
        uint cell = (uint)(frac(uv.x) * width) + (uint)(frac(uv.y) * height) * width;
        uint previous;
        InterlockedMin(global_feedback_buffers[minlod.x + 1][cell], lod, previous);
    }
#endif
    return value;
}

struct SurfaceInput
{
    float2 uv;              // first UV set
    float3 world_position;
    float3 normal;          // world space, from the mesh
    float3 tangent;         // world space tangent frame for normal maps
    float3 bitangent;
    float3 view_direction;  // towards the camera
    float time;             // seconds
    bool front_face;
};

struct Surface
{
    float3 base_color;  // linear
    float opacity;      // compared with the alpha cutoff when the material clips
    float3 normal;      // world space
    float roughness;
    float metallic;
    float ao;
    float3 emissive;    // linear, scaled by the level's emissive intensity
};

// a tangent space normal (e.g. sample_normal(uv).xy unpacked) to world space
float3 tangent_to_world(SurfaceInput i, float3 n)
{
    return normalize(i.tangent * n.x + i.bitangent * n.y + i.normal * n.z);
}

// a two-channel normal map sample (the compiler stores normal maps as BC5) to a tangent space normal
float3 unpack_normal(float4 value)
{
    float2 xy = value.xy * 2.0 - 1.0;
    return float3(xy, sqrt(saturate(1.0 - dot(xy, xy))));
}
)";

const char* kStages = R"(
StructuredBuffer<float4x4> bones : register(t0);
StructuredBuffer<float4x4> last_bones : register(t1);

// an opaque material's shadow pass only needs the position (and the skin), like the game's own
#if defined(DEPTH_ONLY) && !defined(ALPHA_CLIP)
#define POSITION_ONLY
#endif

struct VsIn
{
    float4 position : POSITION;
#ifndef POSITION_ONLY
    float2 uv : TEXCOORD0;
    float2 normal : NORMAL;
    float4 tangent_frame : TANGENT_BINORMAL;
#endif
#ifdef SKINNED
    uint bone_indices : BLENDINDICES;
    float4 bone_weights : BLENDWEIGHTS;
#endif
};

struct Varyings
{
    float4 position : SV_Position;
#ifndef POSITION_ONLY
    float3 world_position : TEXCOORD0;
    float2 uv : TEXCOORD1;
    float3 normal : TEXCOORD2;
    float3 tangent : TEXCOORD3;
    float3 bitangent : TEXCOORD4;
    float3 last_clip : TEXCOORD5;
    float3 last_world_position : TEXCOORD6;
#endif
};

#ifdef SKINNED
float3 skin_point(StructuredBuffer<float4x4> palette, uint indices, float4 weights, float4 p)
{
    float3 r = 0;
    [unroll] for (uint k = 0; k < 4; ++k) r += weights[k] * mul(p, palette[(indices >> (8 * k)) & 255]).xyz;
    return r;
}
float3 skin_vector(uint indices, float4 weights, float3 v)
{
    float3 r = 0;
    [unroll] for (uint k = 0; k < 4; ++k) r += weights[k] * mul(v, (float3x3)bones[(indices >> (8 * k)) & 255]);
    return r;
}
#endif

#ifdef POSITION_ONLY
Varyings vs_main(VsIn input)
{
    float4 p = input.position;
#ifdef SKINNED
    p = float4(skin_point(bones, input.bone_indices, input.bone_weights, input.position), 1.0);
#endif
    Varyings o;
    o.position = mul(p, world_view_proj);
    return o;
}

float4 ps_main() : SV_Target0
{
    return 1.0;
}
#else
Varyings vs_main(VsIn input)
{
    float4 p = input.position;
    float4 last_p = input.position;
    float3 n = oct_decode(input.normal);
    float3 t = oct_decode(input.tangent_frame.xy);
    float3 b = -oct_decode(input.tangent_frame.zw);
#ifdef SKINNED
    p = float4(skin_point(bones, input.bone_indices, input.bone_weights, input.position), 1.0);
    n = skin_vector(input.bone_indices, input.bone_weights, n);
    t = skin_vector(input.bone_indices, input.bone_weights, t);
    b = skin_vector(input.bone_indices, input.bone_weights, b);
#ifndef DEPTH_ONLY
    last_p = float4(skin_point(last_bones, input.bone_indices, input.bone_weights, input.position), 1.0);
#endif
#endif
    Varyings o = (Varyings)0;
    o.uv = input.uv;
    o.world_position = mul(p, world).xyz;
    o.normal = mul(n, (float3x3)world);
    o.tangent = mul(t, (float3x3)world);
    o.bitangent = mul(b, (float3x3)world);
#ifdef DEPTH_ONLY
    o.position = mul(p, world_view_proj);
#else
    float4 clip = mul(p, world_view_proj);
    float2 offset = jitter();
    o.position = float4((clip.xy / clip.w + offset) * clip.w, clip.z, clip.w);
    float4 last_world_position = mul(last_p, last_world);
    float4 last_clip = mul(last_world_position, last_view_proj);
    float2 last_ndc = last_clip.xy / last_clip.w + offset;
    o.last_clip = float3((last_ndc.x * 0.5 + 0.5) * last_clip.w, (0.5 - last_ndc.y * 0.5) * last_clip.w, last_clip.w);
    o.last_world_position = last_world_position.xyz;
#endif
    return o;
}

Surface run_surface(Varyings input, bool front_face)
{
    SurfaceInput i;
    i.uv = input.uv;
    i.world_position = input.world_position;
    // back faces of a double-sided material see the whole tangent frame reversed (as glTF renders them)
    float flip = front_face ? 1.0 : -1.0;
    i.normal = normalize(input.normal) * flip;
    i.tangent = normalize(input.tangent) * flip;
    i.bitangent = normalize(input.bitangent) * flip;
    i.view_direction = normalize(camera_pos - input.world_position);
    i.time = time;
    i.front_face = front_face;
    Surface s;
    s.base_color = 1.0;
    s.opacity = 1.0;
    s.normal = i.normal;
    s.roughness = 0.5;
    s.metallic = 0.0;
    s.ao = 1.0;
    s.emissive = 0.0;
    surface(i, s);
#ifdef ALPHA_CLIP
    clip(s.opacity - ALPHA_CUTOFF);
#endif
    return s;
}

#ifdef DEPTH_ONLY
float4 ps_main(Varyings input, bool front_face : SV_IsFrontFace) : SV_Target0
{
    dt_pixel = uint2(input.position.xy);
    run_surface(input, front_face);
    return 1.0;
}
#else
struct GBuffer
{
    float4 base_color_ao : SV_Target0;
    float4 normal_roughness : SV_Target1;
    float4 reflectance_metallic : SV_Target2;
    float4 screen_motion : SV_Target3;
    float4 camera : SV_Target4;
    float4 emission : SV_Target5;
    float4 world_motion : SV_Target6;
};

GBuffer ps_main(Varyings input, bool front_face : SV_IsFrontFace)
{
    dt_pixel = uint2(input.position.xy);
    Surface s = run_surface(input, front_face);
    float metallic = saturate(s.metallic);
    float roughness = saturate(s.roughness) * global_roughness_multiplier *
        (metallic > 0.5 ? metallic_roughness_multiplier : 1.0) * (metallic < 0.5 ? non_metallic_roughness_multiplier : 1.0);
    float2 uv = ((input.position.xy / output_rt_size) - viewport.zw) / viewport.xy;
    float2 last_uv = input.last_clip.xy / input.last_clip.z;
    GBuffer o;
    o.base_color_ao = float4(saturate(s.base_color), saturate(s.ao));
    o.normal_roughness = float4(oct_encode(normalize(s.normal)), roughness, 0.0);
    o.reflectance_metallic = float4(global_reflectance_multiplier * 0.5, metallic, 0.0, 0.0);
    o.screen_motion = float4(viewport.xy * (uv - last_uv), 0.0, 0.0);
    o.camera = float4(camera_index / 255.0, 0.0, 0.0, 0.0);
    o.emission = float4(max(s.emissive, 0.0) * emissive_intensity, 0.0);
    o.world_motion = float4(input.world_position - input.last_world_position, 0.0);
    return o;
}
#endif
#endif
)";

// The game's projection decal: the mesh's box rasterized, each pixel's scene position rebuilt from the linear depth
// and kept when it lies inside the box, the box's x/z giving the uv (projection along local Y)
const char* kDecalStages = R"(
Texture2D<float4> __tex_linear_depth : register(t0);
SamplerState __samp_linear_depth : register(s0);
Texture2D<float4> __tex_temp_gbuffer1 : register(t1);
SamplerState __samp_temp_gbuffer1 : register(s1);
Texture2D<float4> __tex_temp_gbuffer2 : register(t2);
SamplerState __samp_temp_gbuffer2 : register(s2);

struct VsIn
{
    float4 position : POSITION;
};

// the box's projection direction (out of the surface it lands on) and one view ray per camera matrix; the depth
// buffer's sign bit says which camera drew a pixel
struct Varyings
{
    float4 position : SV_Position;
    float3 box_y : TEXCOORD0;
    float4 ray0 : TEXCOORD1;
    float4 ray1 : TEXCOORD2;
};

Varyings vs_main(VsIn input)
{
    float4 clip = mul(input.position, world_view_proj);
    float2 offset = jitter();
    Varyings o;
    o.position = float4((clip.xy / clip.w + offset) * clip.w, clip.z, clip.w);
    o.box_y = -normalize(world[1].xyz);
    o.ray0 = float4(mul(o.position, camera_inv_view_projection[0]).xyz, clip.w);
    o.ray1 = float4(mul(o.position, camera_inv_view_projection[1]).xyz, clip.w);
    return o;
}

// the normal the G-buffer holds: octahedral in gbuffer1.xy, or a tangent frame quaternion spread over gbuffer1.xy
// and gbuffer2.z when gbuffer1.w marks it (gbuffer2.w names the component left out)
float3 decal_surface_normal(float2 screen)
{
    float4 g1 = __tex_temp_gbuffer1.Sample(__samp_temp_gbuffer1, screen);
    if ((uint)max(g1.w * 3.0, 0.0) != 2)
        return oct_decode(g1.xy);
    float4 g2 = __tex_temp_gbuffer2.Sample(__samp_temp_gbuffer2, screen);
    float3 abc = float3(g1.x, g1.y, g2.z) * 1.41421354 - 0.707106769;
    float d = sqrt(1.0 - min(dot(abc, abc), 1.0));
    uint left_out = (uint)max(g2.w * 3.0, 0.0);
    float q0 = left_out == 0 ? d : abc.x;
    float q1 = left_out == 0 ? abc.x : abc.y;
    float q2 = left_out == 0 ? abc.y : abc.z;
    float q3 = left_out == 0 ? abc.z : d;
    float r1 = left_out == 1 ? q3 : q1;
    float r2 = left_out == 1 ? q1 : q2;
    float r3 = left_out == 1 ? q2 : q3;
    float s2 = left_out == 2 ? r3 : r2;
    float s3 = left_out == 2 ? r2 : r3;
    return float3(2.0 * (s2 * q0 - s3 * r1), 2.0 * (s3 * q0 + s2 * r1), 1.0 - 2.0 * q0 * q0 - 2.0 * r1 * r1);
}

struct DecalOutput
{
    float4 base_color : SV_Target0;
    float4 normal_roughness : SV_Target1;
    float4 reflectance_metallic : SV_Target2;
};

DecalOutput ps_main(Varyings input)
{
    dt_pixel = uint2(input.position.xy);
    float2 screen = input.position.xy / output_rt_size;
    uint depth_bits = asuint(__tex_linear_depth.Sample(__samp_linear_depth, screen).x);
    float depth = asfloat(depth_bits & 0x7fffffff);
    float4 ray = (depth_bits >> 31) == 0 ? input.ray0 : input.ray1;
    float3 world_position = depth * (ray.xyz / ray.w) + camera_pos;
    float3 box = mul(float4(world_position, 1.0), inv_world).xyz;
    if (!(all(box > bounding_volume[0].xyz) && all(box < bounding_volume[1].xyz)))
        discard;
    float3 surface_normal = decal_surface_normal(screen);
    // surfaces turned away from the projection stay clean
    if (dot(input.box_y, surface_normal) < 0.25)
        discard;
    SurfaceInput i;
    i.uv = float2(box.x / (bounding_volume[2].x * 0.5) * 0.5 + 0.5, 0.5 - box.z / (bounding_volume[2].z * 0.5) * 0.5);
    i.world_position = world_position;
    i.normal = surface_normal;
    i.tangent = normalize(world[0].xyz);
    i.bitangent = -normalize(world[2].xyz);
    i.view_direction = normalize(camera_pos - world_position);
    i.time = time;
    i.front_face = true;
    Surface s;
    s.base_color = 1.0;
    s.opacity = 1.0;
    s.normal = i.normal;
    s.roughness = 0.5;
    s.metallic = 0.0;
    s.ao = 1.0;
    s.emissive = 0.0;
    surface(i, s);
#ifdef ALPHA_CLIP
    clip(s.opacity - ALPHA_CUTOFF);
#endif
    DecalOutput o;
    o.base_color = float4(saturate(s.base_color), saturate(s.opacity));
    o.normal_roughness = 0.0;
    o.reflectance_metallic = 0.0;
    return o;
}
)";

bool identifier(const std::string& name) {
    if (name.empty() || !(std::isalpha(static_cast<unsigned char>(name[0])) || name[0] == '_')) return false;
    return std::all_of(name.begin(), name.end(), [](unsigned char c) { return std::isalnum(c) || c == '_'; });
}

std::string float_text(float v) {
    std::ostringstream s;
    s.precision(9);
    s << v;
    const auto text = s.str();
    return text.find_first_of(".eEn") == std::string::npos ? text + ".0" : text;
}

// the per-object constant buffer of a pass: bindless texture members, the engine's matrices, the material's
// parameters (the engine fills each member by name)
bool runtime_texture(const SurfaceShaderSpec& spec, const std::string& name) {
    return std::find(spec.runtime_textures.begin(), spec.runtime_textures.end(), name) != spec.runtime_textures.end();
}

std::string object_cbuffer(const SurfaceShaderSpec& spec, bool depth_only) {
    std::ostringstream s;
    s << "cbuffer " << (depth_only ? "c_depth_only" : "c_per_object") << "\n{\n";
    if (!depth_only || spec.alpha_clip)
        for (const auto& t : spec.textures)
            if (!runtime_texture(spec, t))
                s << "    uint __BINDLESS_SAMPLER_" << t << ";\n    uint2 __BINDLESS_TEX2D_" << t
                  << ";\n    uint2 __BINDLESS_MINLOD_" << t << ";\n";
    if (spec.decal) {
        // the engine fills inv_world and the box (rows: min, max, size) for every decal
        s << "    float4x4 world_view_proj;\n    float4x4 world;\n    float4x4 inv_world;\n    float4x4 bounding_volume;\n";
    } else {
        s << "    float4x4 view_proj;\n    float4x4 world_view_proj;\n    float4x4 world;\n";
        if (!depth_only) s << "    float4x4 last_world;\n    float4x4 last_view_proj;\n    uint camera_index;\n";
    }
    if (!depth_only || spec.alpha_clip)
        for (const auto& p : spec.parameters)
            s << "    float" << (p.value.size() > 1 ? std::to_string(p.value.size()) : std::string()) << " " << p.name << ";\n";
    s << "};\n";
    if (depth_only && !spec.alpha_clip) return s.str();
    // plain registers after the ones the stages use (bones t0-t1, the decal's engine textures t0-t2 / s0-s2)
    int slot = 3;
    for (const auto& t : spec.textures) {
        if (runtime_texture(spec, t)) {
            s << "Texture2D<float4> __tex_" << t << " : register(t" << slot << ");\nSamplerState __samp_" << t
              << " : register(s" << slot << ");\nfloat4 sample_" << t << "(float2 uv) { return __tex_" << t << ".Sample(__samp_"
              << t << ", uv); }\n";
            ++slot;
        } else {
            s << "float4 sample_" << t << "(float2 uv) { return dt_sample(__BINDLESS_SAMPLER_" << t << ", __BINDLESS_TEX2D_"
              << t << ", __BINDLESS_MINLOD_" << t << ", uv); }\n";
        }
    }
    return s.str();
}

std::string stage_source(const SurfaceShaderSpec& spec, bool skinned, bool depth_only) {
    std::ostringstream s;
    if (skinned) s << "#define SKINNED\n";
    if (depth_only) s << "#define DEPTH_ONLY\n#define DT_NO_FEEDBACK\n";
    if (spec.alpha_clip) s << "#define ALPHA_CLIP\n#define ALPHA_CUTOFF " << float_text(spec.alpha_cutoff) << "\n";
    s << kEngineInterface;
    if (!depth_only || spec.alpha_clip) {
        s << object_cbuffer(spec, depth_only) << "\n#line 1 \"surface\"\n" << spec.code << "\n";
    } else {
        // the shadow pass of an opaque material never runs the surface function
        s << object_cbuffer(spec, depth_only) << "void surface(SurfaceInput i, inout Surface s) {}\n";
    }
    s << (spec.decal ? kDecalStages : kStages);
    return s.str();
}

} // namespace

std::string surface_shader_hlsl(const SurfaceShaderSpec& spec) {
    return stage_source(spec, false, false);
}

bool build_surface_shader(const SurfaceShaderSpec& spec, std::uint64_t own_name, MaterialStream& out, std::string& error) {
    // names the engine's constant buffers and the shader around the code already use
    std::set<std::string> reserved = {
        "view_proj", "world_view_proj", "world", "last_world", "last_view_proj", "camera_index", "bones", "last_bones",
        "surface", "halton", "jitter", "dt_pixel", "dt_sample", "oct_decode", "oct_encode", "sign_not_zero",
        "unpack_normal", "tangent_to_world", "skin_point", "skin_vector", "run_surface", "vs_main", "ps_main",
        "VsIn", "Varyings", "GBuffer", "Surface", "SurfaceInput", "global_viewport", "global_environment_settings",
        "c_per_object", "c_depth_only", "global_texture2D", "global_samplers", "static_minlod_sampler",
        "global_feedback_buffers", "inv_world", "bounding_volume", "decal_surface_normal", "DecalOutput"};
    {
        // every member of the two global constant buffers
        const std::string_view text(kEngineInterface);
        const auto end = text.find("// bindless resources");
        std::string word;
        for (std::size_t at = 0; at < end; ++at) {
            const char c = text[at];
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') { word += c; continue; }
            if ((c == ';' || c == '[') && !word.empty()) reserved.insert(word);
            word.clear();
        }
    }
    std::set<std::string> names;
    for (const auto& p : spec.parameters) {
        if (!identifier(p.name) || reserved.count(p.name) || p.name.rfind("__", 0) == 0)
            { error = "shader parameter '" + p.name + "' needs a plain name (letters, digits, _) not used by the game"; return false; }
        if (p.value.empty() || p.value.size() > 4) { error = "shader parameter " + p.name + " takes 1 to 4 numbers"; return false; }
        if (!names.insert(p.name).second) { error = "shader parameter " + p.name + " is listed twice"; return false; }
    }
    for (const auto& t : spec.textures) {
        if (!identifier(t) || reserved.count(t) || reserved.count("sample_" + t) || !names.insert(t).second)
            { error = "shader texture '" + t + "' needs its own plain name (letters, digits, _) not used by the game"; return false; }
    }
    for (const auto& t : spec.runtime_textures)
        if (std::find(spec.textures.begin(), spec.textures.end(), t) == spec.textures.end())
            { error = "runtime texture " + t + " is not one of the shader's textures"; return false; }
    if (spec.code.find("surface") == std::string::npos) { error = "the shader code has no surface function"; return false; }

    struct Variant { std::uint32_t name; std::uint32_t context; bool skinned; bool depth_only; };
    const auto own_hex = [&] { char b[17]; std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(own_name)); return std::string(b); }();
    std::vector<Variant> variants = {
        {id32_from_id64(own_hex + ":gbuffer_skinned"), kDefaultContext, true, false},
        {id32_from_id64(own_hex + ":gbuffer_static"), kDefaultContext, false, false},
        {id32_from_id64(own_hex + ":shadow_skinned"), kShadowCasterContext, true, true},
        {id32_from_id64(own_hex + ":shadow_static"), kShadowCasterContext, false, true},
    };

    ShaderSection section;
    section.name = static_cast<std::uint32_t>(own_name >> 32);
    if (spec.decal) {
        // one shader in the default context, like the game's decal materials
        variants = {{id32_from_id64(own_hex + ":decal"), kDefaultContext, false, false}};
        section.contexts = {{kDefaultContext, 0, {{variants[0].name, 0xffffffffu}}}};
    } else {
        const auto condition = skinned_condition();
        section.conditions = condition;
        section.conditions.insert(section.conditions.end(), condition.begin(), condition.end());
        section.contexts = {
            {kDefaultContext, 0, {{variants[0].name, 0u}, {variants[1].name, 0xffffffffu}}},
            {kShadowCasterContext, 0, {{variants[2].name, static_cast<std::uint32_t>(condition.size())}, {variants[3].name, 0xffffffffu}}},
        };
    }
    section.render_config = {kRenderConfig};
    DeviceShader device;
    for (const auto& v : variants) {
        const auto source = stage_source(spec, v.skinned, v.depth_only);
        std::array<std::vector<std::uint8_t>, 6> stages;
        for (const auto& [index, profile] : {std::pair<std::size_t, const char*>{0, "vs_6_0"}, {4, "ps_6_0"}}) {
            const auto entry = index == 0 ? "vs_main" : "ps_main";
            if (!compile_hlsl(source, "surface_shader.hlsl", entry, profile, stages[index], error)) {
                error = std::string(v.depth_only ? "shadow" : "G-buffer") + (v.skinned ? " skinned " : " static ") +
                        (index == 0 ? "vertex" : "pixel") + " shader: " + error;
                return false;
            }
        }
        ShaderData data;
        if (!make_shader_data({stages}, data, error)) return false;
        data.name = v.name;
        data.passes = {spec.decal ? PassInfo{kDecalsLayer, 0, 0, 0}
                       : v.depth_only ? PassInfo{} : PassInfo{kGbufferLayer, 1, 0, kGbufferBranchKey}};
        data.tail = {v.depth_only ? 0u : 1u, 0u}; // shadow passes draw in submission order (passes_sort_mode immediate)
        ShaderPass pass;
        pass.render_states = kBaseStates;
        const auto& overrides = spec.decal ? kDecalStates : v.depth_only ? kShadowStates : kGbufferStates;
        pass.render_states.insert(pass.render_states.end(), overrides.begin(), overrides.end());
        if (spec.double_sided && !spec.decal) pass.render_states.push_back({1, 0, 1, 0});
        if (spec.decal)
            for (const char* engine : {"linear_depth", "temp_gbuffer1", "temp_gbuffer2"})
                pass.sampler_states.push_back({id32_from_id64(engine), kEngineSampler});
        if (!v.depth_only || spec.alpha_clip)
            for (const auto& t : spec.textures)
                pass.sampler_states.push_back({id32_from_id64(t), spec.decal ? kDecalTextureSampler : kTextureSampler});
        RootTable roots;
        for (const std::size_t index : {std::size_t{0}, std::size_t{4}}) {
            ShaderVariant variant;
            if (!make_shader_variant(stages[index], roots, variant, error)) return false;
            pass.stages[index].push_back(std::move(variant));
        }
        section.shader_data.push_back(std::move(data));
        device.groups.push_back({std::move(pass)});
    }
    section.device_data = {std::move(device)};

    // default data: u32 0, count, {name, float count, offset from the block start}, the values
    std::vector<std::uint8_t> defaults;
    const auto u32 = [&](std::uint32_t value) { for (int i = 0; i < 4; ++i) defaults.push_back(static_cast<std::uint8_t>(value >> (8 * i))); };
    u32(0);
    u32(static_cast<std::uint32_t>(spec.parameters.size()));
    std::uint32_t at = 8 + 12 * static_cast<std::uint32_t>(spec.parameters.size());
    for (const auto& p : spec.parameters) {
        u32(id32_from_id64(p.name));
        u32(static_cast<std::uint32_t>(p.value.size()));
        u32(at);
        at += 4 * static_cast<std::uint32_t>(p.value.size());
    }
    for (const auto& p : spec.parameters)
        for (const float v : p.value) { std::uint32_t bits; std::memcpy(&bits, &v, 4); u32(bits); }
    if (spec.parameters.empty()) defaults.resize(16, 0); // retail writes 16 zero bytes when there are none
    section.default_data = std::move(defaults);

    MaterialStream stream;
    stream.version = 62;
    for (const auto& t : spec.textures) stream.unknown_u32.push_back(id32_from_id64(t)); // its sampler names
    stream.contexts = {{kSurfaceMaterialContext, kSurfaceMaterialDefault}};
    stream.shader_blob = encode_shader_section(section);
    out = std::move(stream);
    return true;
}

}
