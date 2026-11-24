#pragma once
// Reads a UNIT v115 resource (the game's or ours) back: mesh geometry, skins, scene graph, mesh objects, LOD objects,
// material bindings and the skeleton. Geometry the unit streams lives in its .stream file (a sequence of records, one
// per streamed mesh in the LOD objects' stream order: the mesh's vertex stream count, then the data of each stream the
// unit leaves empty, then the indices if the unit has none); apply_unit_stream fills it in. decode_geometry turns a geometry's packed channels into plain float arrays.
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace dtglb::stingray::unit {

// Vertex components and channel types as the game writes them (UNIT MeshGeometry channel records).
enum Component : std::uint32_t {
    kPosition = 0, kNormal = 1, kTangentFrame = 4, kTexcoord = 5, kColor = 6, kBlendIndices = 7, kBlendWeights = 8,
};
enum ChannelType : std::uint32_t { kFloat2 = 1, kFloat3 = 2, kHalf2 = 15, kHalf4 = 17, kUbyte4Norm = 18, kUbyte4 = 19 };

struct UnitChannel {
    std::uint32_t component = 0, type = 0, set = 0, stream = 0;
};

struct UnitBatch {
    std::uint32_t material = 0;  // index into UnitGeometry::materials
    std::uint32_t first_triangle = 0, triangles = 0, bone_set = 0;
};

struct UnitGeometry {
    std::vector<std::vector<std::uint8_t>> streams;  // empty when streamed and not yet applied
    std::vector<std::uint32_t> strides;
    std::uint32_t vertices = 0;
    std::vector<UnitChannel> channels;
    bool index32 = false;
    std::uint32_t index_words[2] = {0, 0};  // the two words before the index format (validity, stream type)
    std::uint32_t index_count = 0;
    std::vector<std::uint8_t> indices;
    std::vector<UnitBatch> batches;
    std::array<float, 10> bounds{};  // min xyz, max xyz, centre xyz, radius
    std::vector<std::uint32_t> materials;  // material slot names (IdString32)
};

struct UnitSkin {
    std::vector<std::array<float, 16>> inverse_binds;
    std::vector<std::uint32_t> nodes;                  // scene graph node per inverse bind
    std::vector<std::vector<std::uint32_t>> bone_sets;  // a batch's blend indices index its set; entries index nodes
};

struct UnitNode {
    std::array<float, 9> rotation{};  // local 3x3, rows
    std::array<float, 3> position{}, scale{};
    std::array<float, 16> world{};
    std::uint16_t parent_type = 0, parent = 0;  // parent_type 1: parent is a node index
    std::uint32_t name = 0;                     // IdString32
};

struct UnitMesh {
    std::uint32_t name = 0, node = 0;
    std::uint32_t geometry = 0;  // 1-based index into geometries
    std::uint32_t skin = 0;      // 1-based index into skins, 0 = none (stored after the geometries: geometry count + n)
    std::uint32_t flags = 0;
    std::array<float, 10> bounds{};
};

// a lamp (light record, 160 bytes): the fields the engine's Light object takes
struct UnitLight {
    std::uint32_t name = 0, node = 0;
    std::array<float, 3> colour{1, 1, 1};  // filter, multiplied with the black body of the temperature
    float temperature = 6500;               // kelvin
    float intensity = 600;
    float volumetric = 0, particle = 0;     // fog and particle lighting strength
    // falloff end; falloff start (omni, spot) / radius (sphere, disk, tube) / half width (rectangle); spot angle start
    // (full cone) / half height (rectangle) / half length (tube); spot angle end (full cone, pi otherwise); box max y, z
    std::array<float, 6> parameters{};
    std::uint32_t flags = 0;  // 1 casts shadows, 2 disabled, 4 baked, 8 dynamic, 0x10 spot reflector, 0x20 lens flare, 0x40 force static
    std::uint32_t type = 0;   // 0 omni, 1 spot, 2 box, 3 directional, 4 directional box, 5 sphere, 6 disk, 7 rectangle, 8 tube, 9 ddgi probe
    std::uint64_t material = 0, ies = 0;
    std::uint32_t shadow_resolution = 128;
};

// a camera (camera record, 32 bytes)
struct UnitCamera {
    std::uint32_t name = 0, node = 0;
    float near_range = 0, far_range = 0, vertical_fov = 0;
};

struct UnitLodStep {
    float height_from = 0, height_to = 0;
    std::vector<std::uint32_t> meshes;
    std::uint32_t stream_offset = 0;  // where this step's records start in the .stream file
};

struct UnitLod {
    std::uint32_t name = 0, node = 0;
    std::array<float, 10> bounds{};  // min xyz, max xyz, centre xyz, radius: what the engine projects to pick a step
    std::vector<UnitLodStep> steps;
    std::vector<std::uint32_t> stream_mesh_order;  // meshes whose geometry is in the .stream file, in file order
};

struct UnitVisibilityGroup {
    std::uint32_t name = 0;              // IdString32
    std::vector<std::uint32_t> meshes;   // mesh object indices
};

// a physics actor (UNIT actor record); names are IdString32
struct UnitActorShape {
    std::uint32_t type = 0;  // 0 sphere, 1 box, 2 capsule, 3 mesh, 4 convex, 5 height field, 7 a cloth's
    std::uint32_t material = 0, shape_template = 0;
    std::uint32_t node = 0;  // the node it sits on; a level prototype's: the name of the mesh it was cooked from
    std::array<float, 16> local{};          // in the node's frame (row vectors, translation in 12..14)
    std::vector<std::uint8_t> cooked;       // mesh / convex: the cooked PhysX mesh
    std::vector<std::uint8_t> second;       // the second byte array (type 7's own data)
    float radius = 0, height = 0;           // sphere, capsule (along local +X; height = the cylinder's length)
    std::array<float, 3> half_extents{};    // box
    // height field (type 5): `cooked` holds rows x columns PhysX samples {i16 height, u8 material 0 (bit 7 tessellation
    // flag), u8 material 1}, row-major; local x = row * row_scale, local z = column * column_scale, local y = height *
    // height_scale (the shape's local matrix turns that into the node's frame)
    std::uint32_t columns = 0, rows = 0;
    float height_scale = 0, row_scale = 0, column_scale = 0;
    bool multiple_materials = false;
};
struct UnitActor {
    std::uint32_t name = 0, actor_template = 0, node = 0;
    float mass = 0;
    std::vector<UnitActorShape> shapes;
    std::array<std::uint32_t, 6> callbacks{};  // touch start/stay/end, trigger enter/leave/stay: flow node << 16 | event
    bool enabled = true;  // created when the unit spawns
    std::array<std::uint8_t, 3> flag_bytes{};
};

// a terrain (stingray::TerrainData): a height map over the bounding box's xy rectangle in its node's frame, drawn by the
// terrain material (the unit's material slot base_material); the unit's height field actor collides with it.
// Layers: type 0 the height map (map = an R16 texture, rows from +y down), type 1 the blend mask (an RGBA texture whose
// channels weigh the material's blend layers). Quadtree nodes: the root covers the box, children {-x-y, +x-y, +x+y,
// -x+y} quarters down to 32 x 32 height texels, ~0u when a leaf; heights are max sample / 65535 x box max z (and min
// sample x box min z, as the Stingray compiler computes it).
struct UnitTerrainDecoration {
    std::uint64_t unit = 0;   // decoration unit resource
    std::uint32_t mesh = 0;   // its mesh (IdString32)
    float density = 0;
};
struct UnitTerrainLayer {
    std::uint64_t map = 0;  // texture resource
    std::uint32_t resolution = 0, type = 0;
    // per blend channel: a material (IdString32) and up to 4 decoration units
    std::array<std::uint32_t, 4> materials{};
    std::array<std::uint32_t, 4> decoration_counts{};
    std::array<std::array<UnitTerrainDecoration, 4>, 4> decorations{};
    // surface contexts: {name, 5 surface materials (IdString32): the base and one per mask channel}
    std::vector<std::array<std::uint32_t, 6>> contexts;
};
struct UnitTerrainNode {
    float height_min = 0, height_max = 0;
    std::array<std::uint32_t, 4> children{};
};
struct UnitTerrain {
    std::uint32_t name = 0, node = 0;
    std::array<float, 10> bounds{};  // min xyz, max xyz, centre xyz, radius
    std::uint32_t flags = 0;
    std::vector<UnitTerrainNode> nodes;
    std::uint32_t base_material = 0;  // material slot (IdString32)
    std::vector<UnitTerrainLayer> layers;
    std::vector<std::uint8_t> height_data;  // empty in every retail terrain
};

struct Unit {
    std::vector<UnitGeometry> geometries;
    std::vector<UnitSkin> skins;
    std::vector<UnitNode> nodes;
    std::vector<UnitMesh> meshes;
    std::vector<UnitActor> actors;
    std::vector<UnitLod> lods;
    std::vector<UnitLight> lights;  // along the node's +Y for spots
    std::vector<UnitCamera> cameras;
    std::uint64_t default_material = 0;
    std::vector<std::pair<std::uint32_t, std::uint64_t>> materials;  // slot name (IdString32) -> material resource
    std::uint64_t skeleton = 0;                                        // BONES resource name, 0 = none
    std::string stream_name;                                           // "data/xx/<hash>.stream", empty = not streamed
    std::vector<UnitVisibilityGroup> visibility_groups;
    std::vector<std::uint8_t> script_data;  // Unit.get_data values (stingray/unit/script_data.h), empty = none
    std::string state_machine;                                         // state machine resource path, empty = none
    std::vector<UnitTerrain> terrains;
    // kept as stored
    std::vector<std::uint8_t> flow, flow_dynamic_data, triangle_finder, physics_scene;
    std::uint8_t animation_blender = 0;
    // sections read past without keeping them held something (8-byte node list, cloth actors, volumes, skeleton entries)
    bool unkept_sections = false;
};

// file: a cooked resource (with the 38-byte envelope) or a bare v115 body
bool read_unit(const std::vector<std::uint8_t>& file, Unit& unit, std::string& error);
bool apply_unit_stream(Unit& unit, const std::vector<std::uint8_t>& stream, std::string& error);

struct DecodedGeometry {
    std::vector<float> positions;              // xyz
    std::vector<float> normals;                // xyz
    std::vector<float> tangents;               // xyz + handedness
    std::vector<std::vector<float>> uvs;       // per set, uv
    std::vector<float> colors;                 // rgba
    std::vector<std::uint32_t> joints;         // 4 per vertex, skin entries (already through the batch's bone set)
    std::vector<float> weights;                // 4 per vertex
    std::vector<std::uint32_t> indices;
};
bool decode_geometry(const Unit& unit, const UnitGeometry& geometry, DecodedGeometry& out, std::string& error);

}  // namespace dtglb::stingray::unit
