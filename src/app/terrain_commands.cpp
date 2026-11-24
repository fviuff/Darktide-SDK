#include "app/terrain_commands.h"

#include "compiler/resource_output.h"
#include "compiler/terrain_builder.h"
#include "gltf/json_value.h"
#include "stingray/murmur_hash.h"
#include "stingray/resource_name.h"
#include "stingray/bundle/game_bundles.h"
#include "stingray/cooked_resource.h"
#include "stingray/terrain/terrain.h"
#include "stingray/texture/texture_reader.h"
#include "stingray/texture/texture_writer.h"
#include "stingray/unit/unit_reader.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>

namespace dtglb::app {
namespace {

namespace fs = std::filesystem;
namespace terrain = stingray::terrain;
namespace texture = stingray::texture;
namespace unit = stingray::unit;
using json::Value;

std::vector<std::uint8_t> load(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

std::string hex(std::uint64_t v, int digits) {
    char text[20];
    std::snprintf(text, sizeof text, "%0*llx", digits, static_cast<unsigned long long>(v));
    return text;
}

Value number(double v) { return Value::of(v); }

// the game's bundles, opened on first use
struct Game {
    std::unique_ptr<stingray::bundle::GameBundles> bundles;
    std::string error;
    bool open() {
        if (bundles) return true;
        if (!error.empty()) return false;
        bundles = std::make_unique<stingray::bundle::GameBundles>();
        if (!bundles->open(texture::game_root() / "bundle", error)) { bundles.reset(); return false; }
        return true;
    }
    bool texture(std::uint64_t name, texture::GameTexture& out, std::string& problem) {
        if (!open()) { problem = error; return false; }
        std::vector<std::uint8_t> file;
        if (!bundles->read({stingray::bundle::type_id("texture"), name}, file, problem)) return false;
        const texture::ReadLoose loose = [&](const std::string& relative, std::vector<std::uint8_t>& data, std::string& e) {
            return bundles->read_loose(relative, data, e);
        };
        return texture::read_game_texture(file, loose, true, out, problem);
    }
    // the body of a texture resource (the loose file's when the resource names one)
    bool texture_body(std::uint64_t name, std::vector<std::uint8_t>& body, std::string& problem) {
        if (!open()) { problem = error; return false; }
        std::vector<std::uint8_t> file;
        if (!bundles->read({stingray::bundle::type_id("texture"), name}, file, problem)) return false;
        std::uint32_t size = 0;
        if (file.size() >= 38) std::memcpy(&size, file.data() + 29, 4);
        if (file.size() < 38ull + size) { problem = "the texture resource is cut short"; return false; }
        std::vector<std::uint8_t> inner(file.begin() + 38, file.begin() + 38 + size);
        if (file[28] == 1) {  // the body is the name of a loose file holding it
            const std::string relative(inner.begin(), std::find(inner.begin(), inner.end(), std::uint8_t{0}));
            return bundles->read_loose(relative, body, problem);
        }
        body = std::move(inner);
        return true;
    }
};

bool same_nodes(const std::vector<unit::UnitTerrainNode>& a, const std::vector<unit::UnitTerrainNode>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i].height_min != b[i].height_min || a[i].height_max != b[i].height_max || a[i].children != b[i].children) return false;
    return true;
}

// --terrain-read <unit>...: each unit's terrains as one JSON line, with what was checked against the game's data
int terrain_read(int argc, char** argv) {
    Game game;
    int failures = 0;
    for (int i = 2; i < argc; ++i) {
        const auto bytes = load(fs::u8path(argv[i]));
        Value line = Value::object();
        line.set("unit", Value::of(fs::u8path(argv[i]).filename().string()));
        unit::Unit u;
        std::string error;
        std::vector<std::uint8_t> body;
        std::string stream;
        if (!unit::read_unit(bytes, u, error) || !stingray::parse_cooked_resource_envelope(bytes, "unit", body, stream, error)) {
            line.set("error", Value::of(error));
            std::cout << json::dump(line, 0) << '\n';
            ++failures;
            continue;
        }
        bool ok = true;
        auto list = Value::array();
        for (const auto& t : u.terrains) {
            Value out = Value::object();
            out.set("name", Value::of(hex(t.name, 8)));
            out.set("node", number(t.node));
            auto bounds = Value::array();
            for (const float v : t.bounds) bounds.push(number(v));
            out.set("bounds", std::move(bounds));
            out.set("bounds_rebuilt", Value::of(terrain::terrain_bounds(t.bounds[3] - t.bounds[0], t.bounds[4] - t.bounds[1], t.bounds[2],
                                                                        t.bounds[5]) == t.bounds));
            out.set("flags", number(t.flags));
            out.set("quadtree_nodes", number(static_cast<double>(t.nodes.size())));
            out.set("base_material", Value::of(hex(t.base_material, 8)));
            out.set("height_data", number(static_cast<double>(t.height_data.size())));
            auto layers = Value::array();
            for (const auto& l : t.layers) {
                Value layer = Value::object();
                layer.set("map", Value::of(hex(l.map, 16)));
                layer.set("resolution", number(l.resolution));
                layer.set("type", number(l.type));
                auto materials = Value::array();
                for (std::size_t k = 0; k < 4; ++k) {
                    Value m = Value::object();
                    m.set("material", Value::of(hex(l.materials[k], 8)));
                    m.set("decorations", number(l.decoration_counts[k]));
                    materials.push(std::move(m));
                }
                layer.set("channels", std::move(materials));
                auto contexts = Value::array();
                for (const auto& c : l.contexts) {
                    auto row = Value::array();
                    for (const auto v : c) row.push(Value::of(hex(v, 8)));
                    contexts.push(std::move(row));
                }
                layer.set("contexts", std::move(contexts));
                layers.push(std::move(layer));
            }
            out.set("layers", std::move(layers));

            // the height field shape: its samples, and the shape set_height_field makes from them
            terrain::HeightGrid grid;
            const auto* shape = terrain::height_field(u, t);
            std::string problem;
            if (!shape) problem = "no height field shape";
            else if (terrain::grid_from_height_field(*shape, grid, problem)) {
                out.set("samples", number(static_cast<double>(grid.columns) * grid.rows));
                unit::UnitActorShape made = *shape;
                terrain::set_height_field(t, grid, made);
                const bool same = made.cooked == shape->cooked && made.local == shape->local && made.columns == shape->columns &&
                                  made.rows == shape->rows && made.height_scale == shape->height_scale && made.row_scale == shape->row_scale &&
                                  made.column_scale == shape->column_scale && made.multiple_materials == shape->multiple_materials;
                out.set("height_field_rebuilt", Value::of(same));
                out.set("quadtree_rebuilt", Value::of(same_nodes(terrain::build_quadtree(t, grid), t.nodes)));
                ok = ok && same;
                std::uint32_t tessellated = 0, holes = 0, two_materials = 0;
                for (std::size_t s = 0; s < grid.materials.size(); ++s) {
                    tessellated += (grid.materials[s] & 0x80) != 0;
                    holes += (grid.materials[s] & 0x7f) == terrain::kHole;
                    two_materials += (grid.materials[s] & 0x7f) != shape->cooked[s * 4 + 3];
                }
                out.set("tessellated", number(tessellated));
                out.set("holes", number(holes));
                out.set("two_materials", number(two_materials));
                terrain::Surface surface;
                if (terrain::terrain_surface(u, t, grid, 0, false, surface, problem))
                    out.set("triangles", number(static_cast<double>(surface.indices.size() / 3)));
            }
            if (!problem.empty()) { out.set("height_field_error", Value::of(problem)); ok = false; problem.clear(); }

            // the height map: the same samples, and its body as the writer makes it
            if (const auto* layer = terrain::terrain_layer(t, 0)) {
                texture::GameTexture map;
                std::vector<std::uint8_t> game_body;
                terrain::HeightGrid from_map;
                if (game.texture(layer->map, map, problem) && terrain::grid_from_height_map(map.width, map.height, map.mips.front(), from_map, problem)) {
                    out.set("height_map_format", number(map.dxgi_format));
                    out.set("height_map_matches", Value::of(from_map.heights == grid.heights));
                    out.set("height_map_rebuilt", Value::of(game.texture_body(layer->map, game_body, problem) &&
                                                            terrain::height_map_body(from_map) == game_body));
                    // which orientation the quadtree's leaves read (shown when the samples' own order doesn't match)
                    if (!same_nodes(terrain::build_quadtree(t, grid), t.nodes)) {
                        terrain::HeightGrid flipped = grid;
                        for (std::uint32_t r = 0; r < grid.rows; ++r)
                            std::copy_n(grid.heights.begin() + static_cast<std::ptrdiff_t>(r) * grid.columns, grid.columns,
                                        flipped.heights.begin() + static_cast<std::ptrdiff_t>(grid.rows - 1 - r) * grid.columns);
                        out.set("quadtree_rebuilt_flipped", Value::of(same_nodes(terrain::build_quadtree(t, flipped), t.nodes)));
                    }
                }
                if (!problem.empty()) { out.set("height_map_error", Value::of(problem)); problem.clear(); }
            }
            // the blend mask: how far the height field's materials follow it
            if (const auto* layer = terrain::terrain_layer(t, 1); layer && !grid.heights.empty()) {
                texture::GameTexture mask;
                texture::ImageRGBA image;
                if (game.texture(layer->map, mask, problem) &&
                    texture::mip_image(mask.dxgi_format, mask.width, mask.height, mask.mips.front(), image, problem)) {
                    // the uncompressed retail masks (B8G8R8A8) hold their channels in r, g, b, a byte order: the height
                    // field's materials follow the bytes, the shader sees r and b swapped
                    if (mask.dxgi_format == 87)
                        for (std::size_t p = 0; p + 3 < image.pixels.size(); p += 4) std::swap(image.pixels[p], image.pixels[p + 2]);
                    terrain::HeightGrid painted = grid;
                    terrain::materials_from_mask(image.pixels, image.width, image.height, painted);
                    std::size_t same = 0;
                    for (std::size_t s = 0; s < grid.materials.size(); ++s) same += (grid.materials[s] & 0x7f) == painted.materials[s];
                    out.set("blend_mask_format", number(mask.dxgi_format));
                    out.set("blend_mask_size", number(mask.width));
                    out.set("materials_from_mask", number(grid.materials.empty() ? 0.0 : static_cast<double>(same) / grid.materials.size()));
                }
                if (!problem.empty()) { out.set("blend_mask_error", Value::of(problem)); problem.clear(); }
            }
            list.push(std::move(out));
        }
        line.set("terrains", std::move(list));
        std::vector<std::uint8_t> written;
        if (terrain::write_terrain_unit(u, written, error)) line.set("rewritten", Value::of(written == body));
        else line.set("rewrite_error", Value::of(error));
        ok = ok && written == body;
        if (!ok) ++failures;
        std::cout << json::dump(line, 0) << '\n';
    }
    return failures ? 2 : 0;
}

// a resource name as the spec gives it: "#ID[<16 hex>]" or a path
std::uint64_t resource_id(const std::string& text) {
    if (text.size() == 21 && text.rfind("#ID[", 0) == 0) return std::strtoull(text.substr(4, 16).c_str(), nullptr, 16);
    return stingray::resource_name_hash(text);
}
std::uint32_t name_id32(const std::string& text) {
    if (text.size() == 9 && text[0] == '#') return static_cast<std::uint32_t>(std::strtoul(text.c_str() + 1, nullptr, 16));
    return stingray::id32_from_id64(text);
}

// --terrain-unit <spec.json> <out folder>: a terrain unit with its textures and material copies as a Custom Assets folder
int terrain_unit(const fs::path& spec_file, const fs::path& folder) {
    const auto text = load(spec_file);
    Value spec;
    std::string error;
    if (text.empty() || !json::parse(std::string_view(reinterpret_cast<const char*>(text.data()), text.size()), spec, error)) {
        std::cerr << spec_file.string() << ": " << (error.empty() ? "cannot read it" : error) << '\n';
        return 1;
    }
    compiler::TerrainSpec t;
    const auto string = [&](const char* key) { const auto* v = spec.find(key); return v && v->is_string() ? v->string : std::string(); };
    const auto pair = [&](const char* key, std::array<float, 2>& out) {
        if (const auto* v = spec.find(key); v && v->is_array() && v->items.size() == 2)
            for (std::size_t i = 0; i < 2; ++i) out[i] = static_cast<float>(v->items[i].number);
    };
    t.name = string("name");
    pair("size", t.size);
    pair("height", t.height);
    if (const auto* v = spec.find("resolution"); v && v->is_number()) t.resolution = static_cast<std::uint32_t>(v->number);
    if (!string("material").empty()) t.material = resource_id(string("material"));
    if (!string("source").empty()) t.source = resource_id(string("source"));
    if (const auto* v = spec.find("surface_materials"); v && v->is_array())
        for (std::size_t i = 0; i < v->items.size() && i < 5; ++i)
            if (v->items[i].is_string() && !v->items[i].string.empty()) t.surface_materials[i] = name_id32(v->items[i].string);
    const fs::path base = spec_file.parent_path();
    if (const auto file = string("heights"); !file.empty()) {
        const auto bytes = load(base / fs::u8path(file));
        t.heights.resize(bytes.size() / 2);
        std::memcpy(t.heights.data(), bytes.data(), t.heights.size() * 2);
    }
    if (const auto file = string("mask"); !file.empty()) t.mask = load(base / fs::u8path(file));
    if (t.name.empty()) { std::cerr << "the terrain spec names no unit (\"name\")\n"; return 1; }

    compiler::ResourceGraph graph;
    compiler::SerializedResources serialized;
    if (!compiler::build_terrain_resources(t, graph, error) || !compiler::serialize_graph(graph, folder, serialized, error) ||
        !compiler::write_build_manifest(graph, serialized, folder, error)) {
        std::cerr << "terrain: " << error << '\n';
        return 2;
    }
    std::cout << t.name << ": " << t.resolution << " x " << t.resolution << " samples over " << t.size[0] << " x " << t.size[1]
              << " m, " << serialized.files.size() << " files\n";
    return 0;
}

}  // namespace

bool run_terrain_command(int argc, char** argv, int& exit_code) {
    if (argc < 2) return false;
    const std::string command = argv[1];
    if (command == "--terrain-read") {
        if (argc < 3) { std::cerr << "--terrain-read <unit>...\n"; exit_code = 1; return true; }
        exit_code = terrain_read(argc, argv);
        return true;
    }
    if (command == "--terrain-unit") {
        if (argc != 4) { std::cerr << "--terrain-unit <spec.json> <out folder>\n"; exit_code = 1; return true; }
        exit_code = terrain_unit(fs::u8path(argv[2]), fs::u8path(argv[3]));
        return true;
    }
    return false;
}

}  // namespace dtglb::app
