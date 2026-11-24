#include "stingray/level/level_navigation.h"
#include "stingray/level/level_package.h"

#include "gltf/json_value.h"
#include "stingray/bundle/game_bundles.h"
#include "stingray/cooked_resource.h"
#include "stingray/dynamic_data.h"
#include "stingray/level/level_document.h"
#include "stingray/level/level_prototypes.h"
#include "stingray/murmur_hash.h"
#include "stingray/terrain/terrain.h"
#include "stingray/unit/unit_collision.h"
#include "stingray/unit/script_data.h"
#include "stingray/unit/unit_reader.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>

namespace dtglb::stingray::level {
namespace fs = std::filesystem;

namespace {

std::vector<std::uint8_t> load(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

bool save(const fs::path& path, const std::vector<std::uint8_t>& bytes, std::string& error) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file) { error = "cannot write " + path.string(); return false; }
    return true;
}

std::string level_name(const LevelDocument& d) {
    std::string name = d.name;
    if (name.size() > 6 && name.compare(name.size() - 6, 6, ".level") == 0) name.resize(name.size() - 6);
    return name;
}


std::array<std::uint8_t, 16> nav_row(std::uint64_t resource) {
    std::array<std::uint8_t, 16> row{};
    std::memcpy(row.data(), &resource, 8);  // then database index 0 and a zero word, as in every retail level
    return row;
}

}  // namespace

bool build_level_navigation(const fs::path& level_file, const fs::path& game_folder, const std::vector<fs::path>& custom_folders,
                            const std::vector<std::array<float, 3>>& seeds, const navigation::NavSettings& given_settings,
                            std::string& summary, std::string& error, std::string* report) {
    const auto cooked = load(level_file);
    if (cooked.empty()) { error = "cannot read " + level_file.string(); return false; }
    LevelDocument d;
    if (!read_level_document(cooked, d, error)) return false;
    const std::string name = level_name(d);
    if (name.empty()) { error = "the level has no name"; return false; }

    navigation::NavSettings settings = given_settings;
    level_navigation_settings(d, settings);
    std::string volume_script;
    {
        const fs::path script = level_file.parent_path() / fs::path(level_file.stem().wstring() + L"_volume_data.lua");
        const auto bytes = load(script);
        volume_script.assign(bytes.begin(), bytes.end());
    }
    const std::string inputs = navigation_inputs(d, volume_script, custom_folders);
    navigation::NavInput input;
    input.tag_volumes = navigation_tag_volumes(volume_script);
    auto add = [&](const unit::CollisionMesh& mesh) {
        const auto first = static_cast<std::uint32_t>(input.vertices.size());
        input.vertices.insert(input.vertices.end(), mesh.vertices.begin(), mesh.vertices.end());
        for (const auto i : mesh.indices) input.indices.push_back(first + i);
    };
    // The level's own geometry as the Stingray editor's navigation generation takes it (editor plugin gwnavgen.lua).
    // Every blockout box, shown or solid, as its six faces (GwNavGen.push_prototype).
    std::size_t box_count = 0;
    if (!d.prototype_unit.empty()) {
        unit::Unit prototype;
        std::vector<LevelPrototype> boxes;
        if (!unit::read_unit(d.prototype_unit, prototype, error) || !read_level_prototypes(prototype, boxes, error)) {
            error = "the level's blockout: " + error;
            return false;
        }
        for (const auto& box : merged_prototypes(boxes)) {
            unit::CollisionMesh mesh;
            for (int i = 0; i < 8; ++i) {
                std::array<float, 3> p = box.center;
                for (int axis = 0; axis < 3; ++axis) {
                    const float s = (i >> axis & 1) ? box.half_size[axis] : -box.half_size[axis];
                    for (int k = 0; k < 3; ++k) p[k] += box.axes[axis][k] * s;
                }
                mesh.vertices.push_back(p);
            }
            static const std::uint32_t faces[12][3] = {{0, 2, 1}, {1, 2, 3}, {4, 5, 6}, {5, 7, 6}, {0, 1, 4}, {1, 5, 4},
                                                       {2, 6, 3}, {3, 6, 7}, {0, 4, 2}, {2, 4, 6}, {1, 3, 5}, {3, 7, 5}};
            for (const auto& f : faces) mesh.indices.insert(mesh.indices.end(), {f[0], f[1], f[2]});
            add(mesh);
            ++box_count;
        }
    }
    // The placed units (default_unit_filter and push_unit): a seed point unit gives its position; editor gizmos, units
    // that are navigation obstacles, tag boxes or bots, and units marked gwnavgenexcluded give nothing; the others their
    // static collision or their render meshes (script data gwnavgen_fromphysicsmesh / gwnavgen_fromrendermesh, else
    // the settings' default).
    struct Read {
        unit::Unit unit;
        json::Value data;
        bool found = false, meshes_decoded = false;
        std::vector<unit::DecodedGeometry> geometry;  // per mesh object (empty: skinned or not decoded)
    };
    const auto own = mod_unit_files(custom_folders);
    std::unique_ptr<bundle::GameBundles> game;
    std::map<std::uint64_t, std::shared_ptr<Read>> read_units;
    const std::uint64_t unit_type = bundle::type_id("unit"), seed_unit = id64("core/gwnav/units/seedpoint/seedpoint");
    const auto key = [](const char* text) {
        char hex[16];
        std::snprintf(hex, sizeof hex, "#%08x", id32_from_id64(text));
        return std::string(hex);
    };
    const auto flag = [&](const json::Value& data, const char* name) {
        const auto* v = data.find(key(name));
        return v && v->kind == json::Value::Kind::Bool && v->boolean;
    };
    std::size_t physics_units = 0, render_units = 0, terrain_units = 0, skipped = 0, missing = 0;
    std::vector<std::array<float, 3>> all_seeds = seeds;
    for (const auto& placed : d.units) {
        if (placed.resource == seed_unit) { all_seeds.push_back(placed.position); continue; }
        auto& slot = read_units[placed.resource];
        if (!slot) {
            slot = std::make_shared<Read>();
            std::vector<std::uint8_t> bytes, stream;
            fs::path stream_folder;
            std::string problem;
            if (const auto found = own.find(placed.resource); found != own.end()) {
                bytes = load(found->second);
                stream_folder = found->second.parent_path();
            } else {
                if (!game) {
                    game = std::make_unique<bundle::GameBundles>();
                    if (!game->open(game_folder / "bundle", error)) return false;
                }
                if (!game->read({unit_type, placed.resource}, bytes, problem)) bytes.clear();
            }
            slot->found = !bytes.empty() && unit::read_unit(bytes, slot->unit, problem);
            if (slot->found && !slot->unit.stream_name.empty()) {
                const bool streamed = stream_folder.empty() ? game->read_loose(slot->unit.stream_name, stream, problem)
                                                            : !(stream = load(stream_folder / fs::u8path(slot->unit.stream_name))).empty();
                if (!streamed || !unit::apply_unit_stream(slot->unit, stream, problem)) slot->unit.stream_name.clear();
            }
            if (slot->found && !slot->unit.script_data.empty() && !unit::decode_script_data(slot->unit.script_data, slot->data, problem))
                slot->data = json::Value::object();
        }
        if (!slot->found) { ++missing; continue; }
        const json::Value& data = slot->data;
        // the placement's own level data first (the keys a unit's script data has), then the unit's
        std::vector<DynamicEntry> own_data;
        {
            std::size_t size = 0;
            std::string problem;
            if (!placed.data.empty() && !read_dynamic_data(placed.data, 0, own_data, size, problem)) own_data.clear();
        }
        const auto setting = [&](const char* name) -> int {
            const auto* e = find_dynamic(own_data, {name});
            return e && e->type == 1 ? (e->number ? 1 : 0) : -1;
        };
        const auto said = [&](const char* name) { const int own = setting(name); return own >= 0 ? own == 1 : flag(data, name); };
        if (said("gwnavseedpoint")) { all_seeds.push_back(placed.position); continue; }
        const auto* editor = data.find(key("LevelEditor"));
        const bool gizmo = editor && flag(*editor, "is_gizmo_unit");
        if (gizmo || said("gwnavgenexcluded") || data.find(key("GwNavBoxObstacle")) || data.find(key("GwNavCylinderObstacle")) ||
            data.find(key("GwNavTagBox")) || data.find(key("GwNavBot"))) {
            ++skipped;
            continue;
        }
        bool physics = said("gwnavgen_fromphysicsmesh"), render = said("gwnavgen_fromrendermesh");
        if (!physics && !render) (settings.consume_physics_meshes ? physics : render) = true;
        const auto placement = unit::pose_matrix(placed.position, placed.rotation, placed.scale);
        unit::CollisionMesh mesh;
        if (physics && !unit::static_collision(slot->unit, placement, mesh, error)) return false;
        if (render) {
            if (!slot->meshes_decoded) {
                slot->meshes_decoded = true;
                slot->geometry.resize(slot->unit.meshes.size());
                for (std::size_t m = 0; m < slot->unit.meshes.size(); ++m) {
                    const auto& object = slot->unit.meshes[m];
                    std::string problem;
                    if (object.skin || !object.geometry || object.geometry > slot->unit.geometries.size() ||
                        !unit::decode_geometry(slot->unit, slot->unit.geometries[object.geometry - 1], slot->geometry[m], problem))
                        slot->geometry[m] = {};
                }
            }
            for (std::size_t m = 0; m < slot->unit.meshes.size(); ++m) {
                const auto& g = slot->geometry[m];
                const auto node = slot->unit.meshes[m].node;
                if (g.indices.empty() || node >= slot->unit.nodes.size()) continue;
                std::array<float, 16> frame{};
                const auto& w = slot->unit.nodes[node].world;
                for (int r = 0; r < 4; ++r)
                    for (int c = 0; c < 4; ++c)
                        for (int k = 0; k < 4; ++k) frame[r * 4 + c] += w[r * 4 + k] * placement[k * 4 + c];
                const auto first = static_cast<std::uint32_t>(mesh.vertices.size());
                for (std::size_t v = 0; v + 2 < g.positions.size(); v += 3) {
                    const float x = g.positions[v], y = g.positions[v + 1], z = g.positions[v + 2];
                    mesh.vertices.push_back({x * frame[0] + y * frame[4] + z * frame[8] + frame[12],
                                             x * frame[1] + y * frame[5] + z * frame[9] + frame[13],
                                             x * frame[2] + y * frame[6] + z * frame[10] + frame[14]});
                }
                for (const auto i : g.indices) mesh.indices.push_back(first + i);
            }
        }
        // its terrains (the generator's consume_terrain, as the Stingray editor hands terrains over): the surface where
        // the height field collides, sampled every height_field_sampling metres
        std::size_t terrain_triangles = 0;
        if (settings.consume_terrain)
            for (const auto& t : slot->unit.terrains) {
                const auto* shape = terrain::height_field(slot->unit, t);
                terrain::HeightGrid grid;
                terrain::Surface surface;
                std::string problem;
                if (!shape || !terrain::grid_from_height_field(*shape, grid, problem) ||
                    !terrain::terrain_surface(slot->unit, t, grid, settings.height_field_sampling, false, surface, problem))
                    continue;
                const auto first = static_cast<std::uint32_t>(mesh.vertices.size());
                for (const auto& p : surface.positions)
                    mesh.vertices.push_back({p[0] * placement[0] + p[1] * placement[4] + p[2] * placement[8] + placement[12],
                                             p[0] * placement[1] + p[1] * placement[5] + p[2] * placement[9] + placement[13],
                                             p[0] * placement[2] + p[1] * placement[6] + p[2] * placement[10] + placement[14]});
                for (const auto i : surface.indices) mesh.indices.push_back(first + i);
                terrain_triangles += surface.indices.size() / 3;
            }
        if (mesh.indices.empty()) continue;
        if (terrain_triangles) ++terrain_units;
        if (mesh.indices.size() / 3 > terrain_triangles) ++(physics ? physics_units : render_units);
        if (report) {
            char line[128];
            std::snprintf(line, sizeof line, "%016llx %s %zu triangles\n", static_cast<unsigned long long>(placed.resource),
                          terrain_triangles ? "terrain" : physics ? "collision" : "render meshes", mesh.indices.size() / 3);
            *report += line;
        }
        add(mesh);
    }
    input.seeds = all_seeds;
    if (input.indices.empty()) { error = "the level has nothing to walk on (no blockout boxes, terrains or units with geometry)"; return false; }

    const auto names = navigation_names(level_file, name);
    const std::string sector = fs::u8path(names.mesh).filename().string();
    navigation::NavOutput out;
    if (!navigation::generate_navdata(game_folder, input, settings, sector, navigation::sector_guid(names.mesh), out, error)) return false;
    // as cooked NavData resources (the 38-byte envelope, then the BlobAggFile the game loads)
    if (!save(names.mesh_file, wrap_cooked_resource("NavData", names.mesh, out.mesh), error) ||
        !save(names.sparse_file, wrap_cooked_resource("NavData", names.sparse, out.sparse), error))
        return false;

    d.nav_data = {nav_row(id64(names.mesh)), nav_row(id64(names.sparse))};
    {
        // what it was built from, for the editor to tell when the level has changed since
        std::vector<DynamicEntry> entries;
        std::size_t size = 0;
        std::string problem;
        if (!d.level_data.empty() && !read_dynamic_data(d.level_data, 0, entries, size, problem)) entries.clear();
        const std::vector<std::uint32_t> path{id32_from_id64("gwnavgen"), id32_from_id64("inputs")};
        entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const DynamicEntry& e) { return e.path == path; }), entries.end());
        DynamicEntry e;
        e.path = path;
        e.type = 3;
        e.bytes.assign(inputs.begin(), inputs.end());
        entries.push_back(std::move(e));
        write_dynamic_data(entries, d.level_data);
    }
    std::vector<std::uint8_t> body;
    if (!write_level_document(d, body, error)) return false;
    // written back in the form it came in: a cooked resource (the editor's) or a bare body
    std::vector<std::uint8_t> inner;
    std::string stream, problem;
    if (parse_cooked_resource_envelope(cooked, "level", inner, stream, problem)) body = wrap_cooked_resource("level", name, body);
    if (!save(level_file, body, error)) return false;
    const auto fixed = [](float v) {
        char text[32];
        std::snprintf(text, sizeof text, "%.2f", static_cast<double>(v));
        return std::string(text);
    };
    summary = name + ": navigation from " + std::to_string(input.indices.size() / 3) + " triangles (" + std::to_string(box_count) +
              " blockout boxes, " + std::to_string(physics_units) + " units by their collision, " + std::to_string(render_units) +
              " by their render meshes, " + std::to_string(terrain_units) + " terrains, " + std::to_string(skipped) + " left out" +
              (missing ? ", " + std::to_string(missing) + " not found" : std::string()) + "), " + std::to_string(all_seeds.size()) +
              " seed points, " + std::to_string(input.tag_volumes.size()) + " navigation volumes, enemy radius " + fixed(settings.entity_radius) + " m, height " + fixed(settings.entity_height) + " m; " + names.mesh_file.filename().string() + " " + std::to_string(out.mesh.size()) + " bytes";
    return true;
}

}  // namespace dtglb::stingray::level
