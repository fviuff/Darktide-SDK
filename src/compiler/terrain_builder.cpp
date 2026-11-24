#include "compiler/terrain_builder.h"

#include "compiler/material_builder.h"
#include "stingray/bundle/game_bundles.h"
#include "stingray/material/material_v61.h"
#include "stingray/murmur_hash.h"
#include "stingray/oodle.h"
#include "stingray/resource_name.h"
#include "stingray/terrain/terrain.h"
#include "stingray/texture/texture_writer.h"
#include "stingray/unit/unit_reader.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>

namespace dtglb::compiler {
namespace {

namespace fs = std::filesystem;
namespace terrain = stingray::terrain;
namespace unit = stingray::unit;

constexpr std::uint64_t kTerrainMaterial = 0xdba8ff9dde3e8a4aull;  // the terrain material 60 of the 75 retail terrains use
constexpr std::uint32_t kMaskFooter = 0x0e736796u;                  // what every retail blend mask body ends with

std::string hex16(std::uint64_t v) {
    char text[20];
    std::snprintf(text, sizeof text, "%016llx", static_cast<unsigned long long>(v));
    return text;
}
std::uint32_t u32(const std::vector<std::uint8_t>& b, std::size_t at) {
    std::uint32_t v = 0;
    if (at + 4 <= b.size()) std::memcpy(&v, b.data() + at, 4);
    return v;
}
std::uint32_t id32(const char* name) { return stingray::id32_from_id64(name); }

struct GameData {
    stingray::bundle::GameBundles bundles;
    bool opened = false;
    bool open(std::string& error) {
        if (opened) return true;
        opened = bundles.open(stingray::texture::game_root() / "bundle", error);
        return opened;
    }
    // a resource's body (after the 38-byte envelope) and the stream it names
    bool resource(const char* type, std::uint64_t name, std::vector<std::uint8_t>& body, std::string& stream, std::string& error) {
        std::vector<std::uint8_t> file;
        if (!open(error) || !bundles.read({stingray::bundle::type_id(type), name}, file, error)) return false;
        const auto size = u32(file, 29), stream_size = u32(file, 34);
        if (file.size() < 38ull + size + stream_size) { error = "the game's " + std::string(type) + " " + hex16(name) + " is cut short"; return false; }
        body.assign(file.begin() + 38, file.begin() + 38 + size);
        stream.assign(file.begin() + 38 + size, file.begin() + 38 + size + stream_size);
        if (file[28] == 1) {  // the body names a loose file holding it
            const std::string loose(body.begin(), std::find(body.begin(), body.end(), std::uint8_t{0}));
            std::vector<std::uint8_t> real;
            if (!bundles.read_loose(loose, real, error)) return false;
            body = std::move(real);
        }
        return true;
    }
    // a material's stream file (the material resource names it)
    bool material_stream(std::uint64_t name, fs::path& path, std::vector<std::uint8_t>& bytes, std::string& error) {
        std::vector<std::uint8_t> body;
        std::string stream;
        std::vector<std::uint8_t> file;
        if (!open(error) || !bundles.read({stingray::bundle::type_id("material"), name}, file, error)) return false;
        const auto size = u32(file, 29);
        if (file.size() < 38ull + size || size < 5 || std::memcmp(file.data() + 38, "data/", 5) != 0) {
            error = "the game's material " + hex16(name) + " doesn't name its stream";
            return false;
        }
        const std::string loose(reinterpret_cast<const char*>(file.data()) + 38, strnlen(reinterpret_cast<const char*>(file.data()) + 38, size));
        path = bundles.folder() / loose;
        return bundles.read_loose(loose, bytes, error);
    }
};

// a copy of one of the game's textures under its content name
bool own_texture(GameData& game, ResourceGraph& graph, std::uint64_t hash, ResourceKey& out, std::string& error) {
    std::vector<std::uint8_t> body, kind1, stream_bytes;
    std::string stream;
    if (!game.resource("texture", hash, body, stream, error)) return false;
    if (!stream.empty() && !game.bundles.read_loose(stream, stream_bytes, error)) return false;
    if (!stingray::texture::texture_body_as_kind1(body, kind1, error)) { error = "the game's texture " + hex16(hash) + ": " + error; return false; }
    std::vector<std::uint8_t> content = kind1;
    content.insert(content.end(), stream_bytes.begin(), stream_bytes.end());
    const auto stem = game_copy_stem("terrain_tex_" + hex16(hash), content);
    out = game_copy_key("texture", stem);
    if (std::any_of(graph.owned.begin(), graph.owned.end(), [&](const auto& node) { return node.key == out; })) return true;
    graph.owned.push_back({out, stem + ".texture", PreservedTexture{std::move(kind1), std::move(stream_bytes)}, {}});
    return true;
}

// a shader material and the parents above it, as owned copies with owned textures
bool own_shader_chain(GameData& game, const CompilationContext& context, ResourceGraph& graph, std::uint64_t hash, ResourceKey& out,
                      std::string& error) {
    fs::path path;
    std::vector<std::uint8_t> bytes;
    if (!game.material_stream(hash, path, bytes, error)) return false;
    const auto section = u32(bytes, 4);
    if (section + 20 > bytes.size()) { error = "the game's material " + hex16(hash) + " is cut short"; return false; }
    std::uint64_t parent = 0;
    std::memcpy(&parent, bytes.data() + section + 12, 8);
    ResourceKey owned_parent;
    if (parent && !own_shader_chain(game, context, graph, parent, owned_parent, error)) return false;
    return own_game_shader_material(context, graph, hash, path, parent, parent ? &owned_parent : nullptr, out, error,
                                    [&](std::uint64_t texture, ResourceKey& key, std::string& e) { return own_texture(game, graph, texture, key, e); });
}

// the terrain material as an owned copy: a shader material itself, or a child material over one
bool own_terrain_material(GameData& game, const CompilationContext& context, ResourceGraph& graph, std::uint64_t hash, ResourceKey& out,
                          std::string& error) {
    fs::path path;
    std::vector<std::uint8_t> bytes;
    if (!game.material_stream(hash, path, bytes, error)) return false;
    const auto section = u32(bytes, 4);
    if (section + 20 > bytes.size()) { error = "the game's material " + hex16(hash) + " is cut short"; return false; }
    std::uint64_t provider = 0, parent = 0;
    std::memcpy(&provider, bytes.data() + section + 4, 8);
    std::memcpy(&parent, bytes.data() + section + 12, 8);
    if (!provider) return own_shader_chain(game, context, graph, hash, out, error);
    std::vector<ResourceKey> dependencies;
    ResourceKey owned_provider, owned_parent;
    if (!own_shader_chain(game, context, graph, provider, owned_provider, error)) return false;
    dependencies.push_back(owned_provider);
    if (parent) {
        if (!own_shader_chain(game, context, graph, parent, owned_parent, error)) return false;
        dependencies.push_back(owned_parent);
    }
    const std::uint64_t provider_name = stingray::resource_name_hash(owned_provider.name);
    const std::uint64_t parent_name = parent ? stingray::resource_name_hash(owned_parent.name) : 0;
    std::memcpy(bytes.data() + section + 4, &provider_name, 8);
    std::memcpy(bytes.data() + section + 12, &parent_name, 8);
    stingray::material::MaterialStream parsed;
    if (!stingray::material::parse_material_stream(bytes, parsed, error)) return false;
    std::vector<stingray::material::TextureResourceHashEdit> edits;
    for (const auto& texture : parsed.textures) {
        if (!texture.resource_hash) continue;
        ResourceKey owned;
        if (!own_texture(game, graph, texture.resource_hash, owned, error)) return false;
        edits.push_back({texture.channel_hash, texture.resource_hash, stingray::resource_name_hash(owned.name)});
        dependencies.push_back(std::move(owned));
    }
    if (!edits.empty()) {
        std::vector<std::uint8_t> edited;
        if (!stingray::material::clone_material_v61_with_texture_hash_edits(bytes, edits, edited, error)) return false;
        bytes = std::move(edited);
    }
    stingray::material::MaterialDependencyHashes hashes;
    if (!stingray::material::validate_preserved_material_v61(bytes, hashes, error)) return false;
    const auto stem = game_copy_stem("terrain_material_" + hex16(hash), bytes);
    out = game_copy_key("material", stem);
    if (std::none_of(graph.owned.begin(), graph.owned.end(), [&](const auto& node) { return node.key == out; }))
        graph.owned.push_back({out, stem + ".material", stingray::material::PreservedMaterialStream{std::move(bytes)}, std::move(dependencies)});
    return true;
}

// a kind 0 body that only describes a texture (DX10 DDS header, the mip block, the footer): what encode_texture_like
// takes the size, format and mip count from
std::vector<std::uint8_t> texture_template(std::uint32_t width, std::uint32_t height, std::uint32_t format, std::uint32_t mips,
                                           std::uint32_t footer) {
    std::vector<std::uint8_t> b(12 + 148, 0);
    const auto put = [&](std::size_t at, std::uint32_t v) { std::memcpy(b.data() + at, &v, 4); };
    std::memcpy(b.data() + 12, "DDS ", 4);
    put(12 + 4, 124);
    put(12 + 12, height);
    put(12 + 16, width);
    put(12 + 28, mips);
    put(12 + 76, 32);
    put(12 + 80, 4);
    std::memcpy(b.data() + 12 + 84, "DX10", 4);
    put(12 + 128, format);
    put(12 + 132, 3);
    put(12 + 140, 1);
    const std::size_t tail = b.size();
    b.resize(tail + 156, 0);
    put(tail, 67);
    put(tail + 12, width);
    put(tail + 16, height);
    put(b.size() - 4, footer);
    return b;
}

}  // namespace

bool build_terrain_resources(const TerrainSpec& spec, ResourceGraph& graph, std::string& error) {
    if (spec.resolution < 2 || spec.resolution > 4096) { error = "the terrain's resolution must be 2 to 4096 samples"; return false; }
    if (!(spec.size[0] > 0 && spec.size[1] > 0) || !(spec.height[1] > spec.height[0])) {
        error = "the terrain needs a size above zero and a max height above its min";
        return false;
    }
    const std::size_t samples = static_cast<std::size_t>(spec.resolution) * spec.resolution;
    if (!spec.heights.empty() && spec.heights.size() != samples) { error = "the terrain's heights don't match its resolution"; return false; }
    if (!spec.mask.empty() && spec.mask.size() != samples * 4) { error = "the terrain's blend mask doesn't match its resolution"; return false; }
    CompilationContext context;
    if (!make_compilation_context("terrain", context, error, spec.name)) return false;
    const ResourceKey unit_key{"unit", spec.name};
    const ResourceKey height_key{"texture", spec.name + "_height"}, mask_key{"texture", spec.name + "_mask"};
    GameData game;

    // the unit: a retail terrain's when it starts from one, else made here like the Stingray terrain compiler's
    unit::Unit u;
    if (spec.source) {
        std::vector<std::uint8_t> file;
        if (!game.open(error) || !game.bundles.read({stingray::bundle::type_id("unit"), spec.source}, file, error) ||
            !unit::read_unit(file, u, error))
            return false;
        if (u.terrains.size() != 1 || !terrain::height_field(u, u.terrains.front())) {
            error = "the source unit is not a terrain unit";
            return false;
        }
    } else {
        unit::UnitNode root;
        root.rotation = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        root.scale = {1, 1, 1};
        root.world = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        root.name = 0x270ebbaau;  // every retail terrain unit's root node
        u.nodes.push_back(root);
        unit::UnitActor actor;
        actor.name = id32("terrain");
        actor.actor_template = id32("static");
        actor.node = root.name;
        actor.callbacks.fill(~0u);
        unit::UnitActorShape shape;
        shape.type = 5;
        shape.material = id32("default");
        shape.shape_template = id32("default");
        shape.node = id32("terrain");
        actor.shapes.push_back(shape);
        u.actors.push_back(actor);
        unit::UnitTerrain t;
        t.name = id32("terrain");
        t.flags = 3;
        t.base_material = id32("default");
        unit::UnitTerrainLayer heights, mask;
        heights.type = 0;
        mask.type = 1;
        std::array<std::uint32_t, 6> surfaces{id32("surface_material")};
        for (std::size_t i = 0; i < 5; ++i) surfaces[i + 1] = id32("brick");
        mask.contexts.push_back(surfaces);
        t.layers = {heights, mask};
        u.terrains.push_back(t);
        u.script_data = {0xff, 0xff, 0xff, 0xff, 0, 0, 0, 0};  // empty script data
        u.triangle_finder = {0, 0, 0, 0};
        u.materials.push_back({id32("default"), 0});
    }
    auto& t = u.terrains.front();
    t.bounds = terrain::terrain_bounds(spec.size[0], spec.size[1], spec.height[0], spec.height[1]);
    for (auto& l : t.layers) {
        l.resolution = spec.resolution;
        l.map = stingray::resource_name_hash(l.type == 0 ? height_key.name : mask_key.name);
        if (l.type == 1)
            for (auto& c : l.contexts)
                for (std::size_t i = 0; i < 5; ++i)
                    if (spec.surface_materials[i]) c[i + 1] = spec.surface_materials[i];
    }

    terrain::HeightGrid grid;
    grid.columns = grid.rows = spec.resolution;
    grid.heights = spec.heights.empty() ? std::vector<std::uint16_t>(samples, 0) : spec.heights;
    stingray::texture::ImageRGBA mask{spec.resolution, spec.resolution, spec.mask};
    if (mask.pixels.empty()) mask.pixels.assign(samples * 4, 0);
    terrain::materials_from_mask(mask.pixels, mask.width, mask.height, grid);
    t.nodes = terrain::build_quadtree(t, grid);
    const auto* field = terrain::height_field(u, t);
    for (auto& actor : u.actors)
        for (auto& shape : actor.shapes)
            if (&shape == field) {
                const auto material = shape.material, shape_template = shape.shape_template, node = shape.node;
                terrain::set_height_field(t, grid, shape);
                shape.material = material;
                shape.shape_template = shape_template;
                shape.node = node;
            }

    // the material: an owned copy of the game's
    std::uint64_t material = spec.material;
    if (!material) {
        for (const auto& [slot, m] : u.materials)
            if (slot == t.base_material) material = m;
        if (!material) material = kTerrainMaterial;
    }
    ResourceKey owned_material;
    if (!own_terrain_material(game, context, graph, material, owned_material, error)) return false;
    for (auto& [slot, m] : u.materials)
        if (slot == t.base_material) m = stingray::resource_name_hash(owned_material.name);

    // the textures: the height map as the game ships them (R16, one mip), the blend mask block compressed (BC7) with
    // mips down to 4 x 4
    {
        std::vector<std::uint8_t> kind1;
        if (!stingray::texture::texture_body_as_kind1(terrain::height_map_body(grid), kind1, error)) return false;
        graph.owned.push_back({height_key, fs::u8path(spec.name).filename().string() + "_height.texture", PreservedTexture{std::move(kind1), {}}, {}});
    }
    {
        std::uint32_t mips = 1;
        while ((spec.resolution >> mips) >= 4) ++mips;
        const auto shape = texture_template(spec.resolution, spec.resolution, 98, mips, kMaskFooter);
        std::vector<std::uint8_t> body, stream;
        if (!stingray::texture::encode_texture_like(mask, shape, body, stream, error)) return false;
        graph.owned.push_back({mask_key, fs::u8path(spec.name).filename().string() + "_mask.texture", PreservedTexture{std::move(body), std::move(stream)}, {}});
    }

    std::vector<std::uint8_t> body;
    if (!terrain::write_terrain_unit(u, body, error)) return false;
    {
        unit::Unit check;
        if (!unit::read_unit(body, check, error)) { error = "the written terrain unit doesn't read back: " + error; return false; }
    }
    graph.owned.push_back({unit_key, fs::u8path(spec.name).filename().string() + ".unit", PreservedUnit{std::move(body)},
                           {owned_material, height_key, mask_key}});
    graph.roots.push_back(unit_key);
    return true;
}

}  // namespace dtglb::compiler
