#include "app/compiler.h"
#include "app/asset_settings.h"
#include "app/source_inspection.h"
#include "app/terrain_commands.h"
#include "validation/unit_validator.h"
#include "stingray/texture/texture_writer.h"
#include "stingray/cooked_resource.h"
#include "stingray/lua_data_compiler.h"
#include "stingray/particles/particles_resource.h"
#include "stingray/flow/flow_resource.h"
#include "stingray/flow/flow_authoring.h"
#include "stingray/flow/flow_graph.h"
#include "stingray/flow/flow_names.h"
#include "stingray/unit/script_data.h"
#include "stingray/unit/unit_reader.h"
#include "stingray/bundle/game_bundles.h"
#include "stingray/texture/texture_reader.h"
#include "stingray/animation/animation_reader.h"
#include "stingray/state_machine/state_machine_reader.h"
#include "stingray/level/level_reader.h"
#include "stingray/level/level_document.h"
#include "stingray/level/level_camera.h"
#include "stingray/level/level_light.h"
#include "stingray/level/level_package.h"
#include "stingray/level/level_navigation.h"
#include "stingray/navigation/navdata_reader.h"
#include "stingray/level/level_prototypes.h"
#include "stingray/level/level_prototype_writer.h"
#include "stingray/shading_environment/shading_environment.h"
#include "stingray/volume_record.h"
#include "stingray/lua_constants.h"
#include "stingray/lua_components.h"
#include "stingray/dynamic_data.h"
#include "stingray/shader/dxil_module.h"
#include "stingray/material/shader_compiler.h"
#include "stingray/material/shader_section.h"
#include "stingray/material/surface_shader.h"
#include "stingray/resource_name.h"
#include "stingray/physics/physics_collection.h"
#include "third_party_licenses.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#endif

namespace fs = std::filesystem;

static void usage() {
    std::cout <<
        "DarktideGLBCompiler - GLB/glTF 2.0 to Darktide Stingray UNIT v115\n\n"
        "Usage:\n"
        "  DarktideGLBCompiler <input.glb|input.gltf> [--inspect] [-o output_dir] [--asset-path PATH] [--output-kind all|model|animations] [--scene INDEX] [--clip INDEX] [--simple-clip INDEX|--no-simple-animation] [--loop-clip INDEX|--once-clip INDEX] [--sm-state NAME CLIP_INDEX loop|once]... [--sm-variable NAME INITIAL MIN MAX]... [--sm-transition FROM_INDEX TO_INDEX EVENT_NAME BLEND_SECONDS]... [--sm-range-transition FROM_INDEX TO_INDEX EVENT_NAME BLEND_SECONDS VARIABLE_INDEX LOWER UPPER inclusive|exclusive inclusive|exclusive]... [--material-variant NAME] [--scale FACTOR] [--animation-translation-tolerance VALUE] [--animation-scale-tolerance VALUE] [--animation-rotation-tolerance-radians VALUE] [--animation-max-controls COUNT] [--animation-max-refinements COUNT] [--animation-max-evaluations COUNT] [--material RESOURCE_PATH] [--reference-bones FILE.bones --skeleton-resource RESOURCE_PATH] [--material-layer NAME MATERIAL]... [--physics-shape geometry|convex|box|sphere|capsule] [--physics-actor static|dynamic|keyframed] [--physics-mass KG] [--physics-material default|iron|rubber] [--no-materials] [--no-images] [--no-physics] [--no-validate] [--manifest]\n"
        "  DarktideGLBCompiler --validate <file.unit>\n"
        "  DarktideGLBCompiler --particles <file.particles> [out.json]   (a particle effect as its JSON description)\n"
        "  DarktideGLBCompiler --particles-schema   (the particle components and their fields, as JSON)\n"
        "  DarktideGLBCompiler --flow <unit.flow> [out.json]   (a unit flow graph as JSON; reads <unit>.flowdyn beside it)\n"
        "  DarktideGLBCompiler --flow-schema   (the flow node types with their inputs, outputs and events, as JSON)\n"
        "  DarktideGLBCompiler --flow-graph <file.unit|file.level|file.flow> [game folder]   (its flow with every node named: ports, event links; with the game folder, event names from the game's Lua)\n"
        "  DarktideGLBCompiler --flow-graph-check <game folder>   (every flow the game ships read node by node and written back)\n"
        "  DarktideGLBCompiler --physics-joints <file.unit>   (the D6 joints of a unit's physics collection, one per line)\n"
        "  DarktideGLBCompiler --physics-collection <file.unit>   (a unit's physics bodies, shapes and joints, as JSON)\n"
        "  DarktideGLBCompiler --physics-meshes <file>   (cooked PhysX shapes as vertices and triangles, JSON; the file holds\n"
        "      records of u32 type 3 triangle mesh / 4 convex, u32 size, cooked bytes)\n"
        "  DarktideGLBCompiler --flow-build <flow.json>   (an authored flow graph as the --flow description)\n"
        "  DarktideGLBCompiler --unit-data <file>   (a unit's script data bytes as JSON)\n"
        "  DarktideGLBCompiler --shader <material stream>   (the compiled shaders of a shader material, summarized as JSON)\n"
        "  DarktideGLBCompiler --shader-reflect <stage.dxbc>...   (binding records of compiled stages of one shader group, in order)\n"
        "  DarktideGLBCompiler --shader-stages <material stream> <out dir>   (unpacked vertex and pixel stages of a shader material, with index.json)\n"
        "  DarktideGLBCompiler --texture-image <file.texture> <out.png>   (a game texture at full size, its streamed mips included, as a PNG; prints its size and srgb or linear)\n"
        "  DarktideGLBCompiler --texture-read <file.texture>...   (each texture read back at full size: format, size, mips; one JSON line each)\n"
        "  DarktideGLBCompiler --animation-read <file.animation>...   (each animation read back and sampled; one JSON line each)\n"
        "  DarktideGLBCompiler --state-machine-read <file.state_machine>...   (layers, states, clips; one JSON line each)\n"
        "  DarktideGLBCompiler --lua-tables <file.lua>...   (the constant tables of compiled Lua resources, volume entries counted)\n"
        "  DarktideGLBCompiler --lua-returned <file.lua>...   (the value compiled data scripts return, as JSON)\n"
        "  DarktideGLBCompiler --lua-components <file.lua>...   (the gameplay components compiled component scripts declare, with their fields)\n"
        "  DarktideGLBCompiler --shader-library-read <file.shader_library_group>...   (the shaders of each library, decoded and written back identically)\n"
        "  DarktideGLBCompiler --shading-environment-read <file.shading_environment>...   (variables and settings of each, and whether it writes back identically)\n"
        "  DarktideGLBCompiler --level-roundtrip <file.level>...   (each level read as a document and written back; identical or where it differs)\n"
        "  DarktideGLBCompiler --level-new <resource path> <out.level>   (an empty level)\n"
        "  DarktideGLBCompiler --lua-data <in.lua> <out> --name <resource path> [--luajit]   (a data script - locals holding tables, then return - as the game's Lua resource)\n"
        "  DarktideGLBCompiler --level-folder <file.level> <out folder> [Custom folder]... [--names <dictionary>]   (a level as a Custom Assets folder; the dictionary names the game resources it uses)\n"
        "  DarktideGLBCompiler --level-navigation <file.level> <game directory> [Custom folder]... [--seed x,y,z]...   (a level's navigation, by the game's generator)\n"
        "  DarktideGLBCompiler --navdata-read <file>... [--obj <out.obj>]   (NavData: cells, floors, triangles; the walkable triangles as OBJ)\n"
        "  DarktideGLBCompiler --level-prototypes <file.level>...   (a level's blockout boxes, checked by writing them again)\n"
        "  DarktideGLBCompiler --level-prototype-unit <boxes.txt> <out>   (blockout boxes as a level's embedded UNIT)\n"
        "  DarktideGLBCompiler --terrain-read <unit>...   (each unit's terrains as one JSON line, checked against the game's data)\n"
        "  DarktideGLBCompiler --terrain-unit <spec.json> <out folder>   (a terrain unit, its height map, blend mask and material copies)\n"
        "  DarktideGLBCompiler --level-read <file.level>...   (units, nested levels, volumes, scatter, stories; one JSON line each)\n"
        "  DarktideGLBCompiler --game-read-check <type> [bundle dir]   (every resource of a type read one by one, compared with whole-bundle unpacking)\n"
        "  DarktideGLBCompiler --configure-oodle <game directory or oo2core_9_win64.dll>\n"
        "  DarktideGLBCompiler --capabilities\n\n"
        "  DarktideGLBCompiler --licenses\n\n"
        "--inspect reads and summarizes source content without compiling or writing output; only --scene may be combined with it.\n"
        "The compiler converts what has a direct Darktide equivalent and records every approximation it makes\n"
        "in build.log and compile_manifest.json. Output is UNIT, BONES, ANIMATION, STATE_MACHINE, MATERIAL and\n"
        "TEXTURE resources plus a build.json for the Custom Assets patcher.\n"
        "--output-kind selects a resource closure; all is the default. Model excludes animation clips;\n"
        "animations emits clips and their BONES dependency without model/material resources.\n"
        "--material explicitly records an existing Darktide material resource as an approximation for GLB materials.\n"
        "--material-variant selects a named KHR_materials_variants variant from the source glTF.\n"
        "Without a state machine, the first emitted clip (or --simple-clip INDEX) is embedded as the UNIT's\n"
        "simple animation, played from Lua with Unit.play_simple_animation; --no-simple-animation omits it.\n"
        "--loop-clip emits a minimal looping STATE_MACHINE for an emitted clip in all-output mode.\n"
        "--once-clip emits a minimal one-shot STATE_MACHINE for an emitted clip in all-output mode.\n"
        "--sm-state and --sm-transition author direct-event STATE_MACHINE states and transitions in all-output mode.\n"
        "@FILE anywhere stands for the arguments in FILE, one per line.\n"
        "--sm-declare-events A,B,... lists further events scripts may send that no transition reacts to (the engine\n"
        "stops on an event its state machine doesn't list); #1234abcd names an event by its hash.\n"
        "--in-place removes the horizontal root travel from every clip so walk/run cycles loop on the spot;\n"
        "build.log notes each clip's speed for moving the unit from Lua.\n"
        "--root-motion moves that travel onto the skeleton root (root_point) instead, where the engine reads it\n"
        "as root motion (Unit.animation_wanted_root_pose), like the game's character clips.\n"
        "--package-name NAME names the package the asset is loaded with (default: Custom Assets picks one). Give an\n"
        "enemy body its own resource path: the game loads a breed's base_unit as a package of that name.\n"
        "--ragdoll-event NAME adds a ragdoll state entered on NAME from every state; dynamic node bodies stay\n"
        "uncreated until then (retail minion ragdoll setup).\n"
        "--scale uniformly converts all spatial data before Stingray coordinate conversion (1 keeps glTF units).\n"
        "--asset-path sets a stable extensionless resource identity, e.g. content/mods/my_mod/trolley.\n"
        "It also names generated dependencies; keep one asset build per output directory.\n"
        "Copies of game resources an asset needs (shader materials, effect materials, textures, units) are named\n"
        "content/mods/custom_assets/game/<kind>_<game hash>_<digest of the copy>, the same in every asset and mod,\n"
        "so the game loads one copy however many assets carry it.\n"        "A plain glTF without authored colliders or a physics shape builds without a physics actor.\n"
        "When requested, geometry physics uses the exact triangle surface for static/keyframed actors;\n"
        "dynamic actors use a PhysX convex hull per source object/instance. Convex, fitted\n"
        "box, sphere, and capsule remain explicit fitted alternatives.\n"
        "Actor, mass, and material controls require a shape. --no-physics wins regardless of argument order.\n"
        "--no-images is accepted as a legacy no-op; resource-only output is unconditional.\n"
        "--allow-fixed-morph-bake consents to keeping only the initial morph pose.\n"
        "Continuous animation is adaptively fitted to measured tolerances; animation tolerance and work-limit flags configure the fit.\n"
        "--allow-animation-resampling and --allow-native-animation-interpolation are accepted legacy flags; they do not bypass fitting.\n";
}

static void capabilities() {
    // Keep this machine-readable so front ends do not duplicate compiler policy.
    // other tools might parse this so dont go renaming keys
    std::cout << "{\"schema\":1,\"output_kinds\":[\"all\",\"model\",\"animations\"],\"inspect\":true,\"inspect_options\":[\"--scene INDEX\"],"
                 "\"asset_path\":true,"
                 "\"simple_animation\":{\"options\":[\"--simple-clip INDEX\",\"--no-simple-animation\"],\"default\":\"first emitted clip when no state machine\",\"lua\":\"Unit.play_simple_animation(unit, from, to, loop, speed)\"},"
                 "\"material_variants\":{\"option\":\"--material-variant NAME\",\"settings_key\":\"material_variant\",\"extension\":\"KHR_materials_variants\"},"
                 "\"reference_bones\":{\"options\":[\"--reference-bones\",\"--skeleton-resource\"],\"mapping\":\"exact_joint_set\"},"
                 "\"physics_shapes\":[\"geometry\",\"convex\",\"box\",\"sphere\",\"capsule\"],"
                 "\"physics_actors\":[\"static\",\"dynamic\",\"keyframed\"],"
                 "\"physics_materials\":[\"default\",\"iron\",\"rubber\"],"
                 "\"physics_rules\":{\"geometry\":\"exact triangle mesh for static/keyframed; per-object/instance convex hull for dynamic\",\"convex\":\"per-object/instance PhysX convex hull\",\"box\":\"single fitted box\",\"sphere\":\"single fitted sphere\",\"capsule\":\"single fitted capsule along longest bounds axis\"},"
                 "\"actor_rules\":{\"static\":\"fixed world collision; mass is ignored\",\"dynamic\":\"simulated rigid body; geometry is cooked as convex hulls\",\"keyframed\":\"externally moved collision; exact geometry is allowed\"},"
                 "\"material_modes\":[\"automatic\",\"external\",\"none\"],"
                 "\"material_slot_extras\":{\"name\":\"darktide_material\",\"version\":1,\"modes\":[\"generated\",\"external\"],\"template\":\"compiler_gap\"},"
                 "\"material_surface_options\":[\"default\",\"metal_solid\",\"metal_sheet\",\"cloth\",\"concrete\",\"brick\",\"bone\",\"plastic\"],"
                 "\"material_rules\":{\"automatic\":\"convert each glTF material and its referenced textures\",\"external\":\"bind every mesh slot to an existing Darktide material resource\",\"none\":\"emit geometry without material resources\"},"
                 "\"scale_presets\":[\"1\",\"0.1\",\"0.01\",\"0.001\"],"
                 "\"external_material_presets\":[\"content/parent_materials/substance_basic\",\"content/parent_materials/substance_basic_transparent\"]}\n";
}

// A particles resource as its JSON description (stdout or a file); the description is written back to binary
// and must give the same bytes.
// A unit flow blob (and its dynamic data, <name>.flowdyn) as its JSON description; it must encode back to the
// same bytes.
static bool flow_description(const fs::path& input, const fs::path& output, std::string& error) {
    const auto read = [](const fs::path& path, std::vector<std::uint8_t>& out) {
        std::ifstream file(path, std::ios::binary);
        if (!file) return false;
        out.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        return true;
    };
    std::vector<std::uint8_t> flow, dynamic_data, flow_again, dynamic_again;
    if (!read(input, flow)) { error = "cannot read the file"; return false; }
    auto dynamic_path = input;
    dynamic_path.replace_extension(".flowdyn");
    if (fs::exists(dynamic_path) && !read(dynamic_path, dynamic_data)) { error = "cannot read " + dynamic_path.string(); return false; }
    dtglb::json::Value graph;
    if (!dtglb::stingray::flow::decode(flow, dynamic_data, graph, error)) return false;
    if (!dtglb::stingray::flow::encode(graph, flow_again, dynamic_again, error)) { error = "description does not encode: " + error; return false; }
    if (flow_again != flow || dynamic_again != dynamic_data) {
        std::size_t at = 0;
        while (at < flow_again.size() && at < flow.size() && flow_again[at] == flow[at]) ++at;
        error = "description encodes to different bytes (flow first difference at byte " + std::to_string(at) + ")";
        return false;
    }
    const auto text = dtglb::json::dump(graph) + "\n";
    if (output.empty()) { std::cout << text; return true; }
    std::ofstream out(output, std::ios::binary);
    out << text;
    if (!out) { error = "cannot write " + output.string(); return false; }
    return true;
}

static bool particles_description(const fs::path& input, const fs::path& output, std::string& error) {
    std::ifstream file(input, std::ios::binary);
    const std::vector<std::uint8_t> blob((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::vector<std::uint8_t> body, again;
    std::string stream;
    dtglb::json::Value effect;
    if (!file.good() && !file.eof()) { error = "cannot read the file"; return false; }
    if (!dtglb::stingray::parse_cooked_resource_envelope(blob, "particles", body, stream, error) ||
        !dtglb::stingray::particles::decode(body, effect, error)) return false;
    if (!dtglb::stingray::particles::encode(effect, again, error)) { error = "description does not encode: " + error; return false; }
    if (again != body) {
        std::size_t at = 0;
        while (at < again.size() && at < body.size() && again[at] == body[at]) ++at;
        error = "description encodes to different bytes (first difference at byte " + std::to_string(at) + ", " +
            std::to_string(again.size()) + " bytes instead of " + std::to_string(body.size()) + ")";
        return false;
    }
    const auto text = dtglb::json::dump(effect) + "\n";
    if (output.empty()) { std::cout << text; return true; }
    std::ofstream out(output, std::ios::binary);
    out << text;
    if (!out) { error = "cannot write " + output.string(); return false; }
    return true;
}

static bool parse_finite_float(const std::string& value, float& result, bool allow_zero) {
    if (value.empty()) return false;
    double parsed = 0.0;
    const auto converted = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (converted.ec != std::errc{} || converted.ptr != value.data() + value.size() ||
        !std::isfinite(parsed) || parsed > std::numeric_limits<float>::max() ||
        (allow_zero ? parsed < 0.0 : parsed <= 0.0)) return false;
    result = static_cast<float>(parsed);
    return true;
}

static bool parse_signed_finite_float(const std::string& value, float& result) {
    if (value.empty()) return false;
    double parsed = 0.0;
    const auto converted = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (converted.ec != std::errc{} || converted.ptr != value.data() + value.size() ||
        !std::isfinite(parsed) || parsed < std::numeric_limits<float>::lowest() ||
        parsed > std::numeric_limits<float>::max()) return false;
    result = static_cast<float>(parsed);
    return true;
}

static bool parse_positive_double(const std::string& value, double& result) {
    if (value.empty()) return false;
    double parsed = 0.0;
    const auto converted = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (converted.ec != std::errc{} || converted.ptr != value.data() + value.size() ||
        !std::isfinite(parsed) || parsed <= 0.0) return false;
    result = parsed;
    return true;
}

static bool parse_scene_index(const std::string& value, int& result) {
    if (value.empty()) return false;
    unsigned long long parsed = 0;
    for (const char c : value) {
        if (c < '0' || c > '9') return false;
        const auto digit = static_cast<unsigned>(c - '0');
        if (parsed > (static_cast<unsigned long long>(std::numeric_limits<int>::max()) - digit) / 10u) return false;
        parsed = parsed * 10u + digit;
    }
    result = static_cast<int>(parsed);
    return true;
}

static bool parse_positive_int(const std::string& value, int& result) {
    if (!parse_scene_index(value, result)) return false;
    return result > 0;
}

int main(int argc, char** argv) {
    // @file stands for the arguments in that file, one per line (a big state machine doesn't fit a command line)
    std::vector<std::string> expanded;
    for (int i = 0; i < argc; ++i) {
        const std::string arg = argv[i];
        if (i == 0 || arg.size() < 2 || arg[0] != '@') { expanded.push_back(arg); continue; }
        std::ifstream file(fs::u8path(arg.substr(1)));
        if (!file) { std::cerr << "cannot read argument file " << arg.substr(1) << "\n"; return 1; }
        for (std::string line; std::getline(file, line);) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            expanded.push_back(line);
        }
    }
    std::vector<char*> expanded_argv;
    for (auto& arg : expanded) expanded_argv.push_back(arg.data());
    expanded_argv.push_back(nullptr);
    argc = static_cast<int>(expanded.size());
    argv = expanded_argv.data();
    if (argc < 2) { usage(); return 1; }
    if (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h") { usage(); return 0; }
    if (int code = 0; dtglb::app::run_terrain_command(argc, argv, code)) return code;
    if (std::string(argv[1]) == "--licenses") { if (argc != 2) { usage(); return 1; } std::cout << darktide_third_party_licenses; return 0; }
    if (std::string(argv[1]) == "--capabilities") { if (argc != 2) { usage(); return 1; } capabilities(); return 0; }
    if (std::string(argv[1]) == "--configure-oodle") {
        fs::path dll_or_game_directory;
#ifdef _WIN32
        int wide_argc = 0;
        LPWSTR* wide_argv = CommandLineToArgvW(GetCommandLineW(), &wide_argc);
        if (!wide_argv || wide_argc != 3) {
            if (wide_argv) LocalFree(wide_argv);
            usage();
            return 1;
        }
        dll_or_game_directory = fs::path(wide_argv[2]);
        LocalFree(wide_argv);
#else
        if (argc != 3) { usage(); return 1; }
        dll_or_game_directory = fs::u8path(argv[2]);
#endif
        std::string error;
        if (!dtglb::stingray::texture::configure_oodle(dll_or_game_directory, error)) {
            std::cerr << "Oodle configuration failed: " << error << '\n';
            return 2;
        }
        std::cout << "Oodle configured.\n";
        return 0;
    }
    if (std::string(argv[1]) == "--validate") {
        if (argc < 3) { usage(); return 1; }
        std::string report;
        const bool ok = dtglb::validation::validate_unit_v115(argv[2], report);
        std::cout << report << "\n";
        return ok ? 0 : 2;
    }
    if (std::string(argv[1]) == "--particles-schema") {
        if (argc != 2) { usage(); return 1; }
        std::cout << dtglb::json::dump(dtglb::stingray::particles::schema()) << '\n';
        return 0;
    }
    if (std::string(argv[1]) == "--game-read-check") {
        // every resource of a type read one by one (in an order that defeats the unpacked-bundle cache), compared
        // with the same resources unpacked bundle by bundle
        if (argc != 3 && argc != 4) { usage(); return 1; }
        namespace bundle = dtglb::stingray::bundle;
        const std::filesystem::path folder = argc == 4 ? std::filesystem::path(argv[3]) : dtglb::stingray::texture::game_root() / "bundle";
        bundle::GameBundles game;
        std::string error;
        if (!game.open(folder, error)) { std::cerr << error << '\n'; return 2; }
        const auto type = bundle::type_id(argv[2]);
        auto names = game.names(type);
        std::reverse(names.begin(), names.end());
        std::size_t mismatches = 0, failures = 0;
        std::vector<std::uint8_t> read;
        for (const auto name : names)
            if (!game.read({type, name}, read, error)) ++failures;
        if (!game.for_each(type, [&](std::uint64_t name, const std::vector<std::uint8_t>& data) {
                std::string e;
                std::vector<std::uint8_t> one;
                if (!game.read({type, name}, one, e) || one != data) ++mismatches;
            }, error)) { std::cerr << error << '\n'; return 2; }
        std::cout << names.size() << " " << argv[2] << " resources, " << failures << " reads failed, " << mismatches << " differ\n";
        return failures || mismatches ? 2 : 0;
    }
    if (std::string(argv[1]) == "--game-extract") {
        if (argc != 4 && argc != 5) { usage(); return 1; }
        namespace bundle = dtglb::stingray::bundle;
        const std::filesystem::path folder = argc == 5 ? std::filesystem::path(argv[4]) : dtglb::stingray::texture::game_root() / "bundle";
        bundle::GameBundles game;
        std::string error;
        if (!game.open(folder, error)) { std::cerr << error << '\n'; return 2; }
        const std::string type = argv[2];
        const std::filesystem::path out = argv[3];
        std::filesystem::create_directories(out);
        int written = 0;
        const bool ok = game.for_each(bundle::type_id(type), [&](std::uint64_t name, const std::vector<std::uint8_t>& data) {
            char file[64];
            std::snprintf(file, sizeof file, "%016llx.%s", static_cast<unsigned long long>(name), type.c_str());
            std::ofstream(out / file, std::ios::binary).write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
            ++written;
        }, error);
        std::cout << written << " " << type << " resources\n";
        if (!ok) { std::cerr << error << '\n'; return 2; }
        return 0;
    }
    if (std::string(argv[1]) == "--unit-geometry") {
        std::filesystem::path bundle;
        int first = 2;
        if (argc > 3 && std::string(argv[2]) == "--bundle") { bundle = argv[3]; first = 4; }
        if (first >= argc) { usage(); return 1; }
        const auto load = [](const std::filesystem::path& path) {
            std::ifstream file(path, std::ios::binary);
            return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        };
        int failures = 0;
        for (int i = first; i < argc; ++i) {
            namespace unit = dtglb::stingray::unit;
            unit::Unit u;
            std::string error;
            std::size_t vertices = 0, triangles = 0, skinned = 0;
            if (unit::read_unit(load(argv[i]), u, error) && !u.stream_name.empty()) {
                if (bundle.empty()) error = "streamed geometry: pass --bundle";
                else unit::apply_unit_stream(u, load(bundle / u.stream_name), error);
            }
            for (std::size_t g = 0; error.empty() && g < u.geometries.size(); ++g) {
                unit::DecodedGeometry decoded;
                if (!unit::decode_geometry(u, u.geometries[g], decoded, error)) error = "geometry " + std::to_string(g) + ": " + error;
                vertices += decoded.positions.size() / 3;
                triangles += decoded.indices.size() / 3;
                skinned += !decoded.joints.empty();
            }
            dtglb::json::Value line = dtglb::json::Value::object();
            line.set("unit", dtglb::json::Value::of(std::filesystem::path(argv[i]).filename().string()));
            if (!error.empty()) {
                line.set("error", dtglb::json::Value::of(error));
                ++failures;
            } else {
                line.set("geometries", dtglb::json::Value::of(static_cast<double>(u.geometries.size())));
                line.set("meshes", dtglb::json::Value::of(static_cast<double>(u.meshes.size())));
                {
                    // mesh render flags (0x1 drawn, 0x10 occluder) as counts
                    std::map<std::uint32_t, int> flag_counts;
                    for (const auto& m : u.meshes) ++flag_counts[m.flags & 0xffu];
                    auto flags = dtglb::json::Value::object();
                    for (const auto& [f, n] : flag_counts) flags.set(std::to_string(f), dtglb::json::Value::of(static_cast<double>(n)));
                    line.set("mesh_flags", std::move(flags));
                }
                if (!u.cameras.empty()) {
                    auto cameras = dtglb::json::Value::array();
                    for (const auto& camera : u.cameras) {
                        auto c = dtglb::json::Value::array();
                        c.push(dtglb::json::Value::of(static_cast<double>(camera.near_range)));
                        c.push(dtglb::json::Value::of(static_cast<double>(camera.far_range)));
                        c.push(dtglb::json::Value::of(static_cast<double>(camera.vertical_fov)));
                        cameras.push(std::move(c));
                    }
                    line.set("cameras", std::move(cameras));
                }
                line.set("nodes", dtglb::json::Value::of(static_cast<double>(u.nodes.size())));
                line.set("vertices", dtglb::json::Value::of(static_cast<double>(vertices)));
                line.set("triangles", dtglb::json::Value::of(static_cast<double>(triangles)));
                std::uint32_t most = 0, wide = 0;  // the most vertices one geometry has; geometries with 32-bit indices
                for (const auto& g : u.geometries) {
                    most = std::max(most, g.vertices);
                    wide += g.index32 ? 1 : 0;
                }
                {
                    auto words = dtglb::json::Value::array();  // index validity / stream type words, each pair seen once
                    std::set<std::pair<std::uint32_t, std::uint32_t>> seen;
                    for (const auto& g : u.geometries)
                        if (seen.insert({g.index_words[0] * 2 + (g.index32 ? 1 : 0), g.index_words[1]}).second) {
                            auto pair = dtglb::json::Value::array();
                            pair.push(dtglb::json::Value::of(static_cast<double>(g.index_words[0])));
                            pair.push(dtglb::json::Value::of(static_cast<double>(g.index_words[1])));
                            pair.push(dtglb::json::Value::of(g.index32));
                            words.push(std::move(pair));
                        }
                    line.set("index_words", std::move(words));
                }
                line.set("most_geometry_vertices", dtglb::json::Value::of(static_cast<double>(most)));
                line.set("index32_geometries", dtglb::json::Value::of(static_cast<double>(wide)));
                line.set("skinned_geometries", dtglb::json::Value::of(static_cast<double>(skinned)));
                line.set("materials", dtglb::json::Value::of(static_cast<double>(u.materials.size())));
                line.set("streamed", dtglb::json::Value::of(!u.stream_name.empty()));
                // each node's world matrix rebuilt from the local transforms (row vectors: the rotation's rows are the
                // scaled axes, world = local * parent world) against the stored one
                std::vector<std::array<float, 16>> world(u.nodes.size());
                double deviation = 0;
                for (std::size_t n = 0; n < u.nodes.size(); ++n) {
                    const auto& node = u.nodes[n];
                    std::array<float, 16> local{};
                    for (int r = 0; r < 3; ++r)
                        for (int c = 0; c < 3; ++c) local[r * 4 + c] = node.rotation[r * 3 + c] * node.scale[r];
                    for (int c = 0; c < 3; ++c) local[12 + c] = node.position[c];
                    local[15] = 1;
                    if (node.parent_type == 1 && node.parent < n) {
                        const auto& p = world[node.parent];
                        for (int r = 0; r < 4; ++r)
                            for (int c = 0; c < 4; ++c) {
                                float v = 0;
                                for (int k = 0; k < 4; ++k) v += local[r * 4 + k] * p[k * 4 + c];
                                world[n][r * 4 + c] = v;
                            }
                    } else {
                        world[n] = local;
                    }
                    for (int k = 0; k < 16; ++k) deviation = std::max(deviation, static_cast<double>(std::fabs(world[n][k] - node.world[k])));
                }
                line.set("node_world_deviation", dtglb::json::Value::of(deviation));
                // a skin's inverse bind times its node's stored world matrix: identity at the rest pose
                // and how far those products differ between the joints of one skin (a skin bound with its geometry
                // somewhere else than the unit's origin gives one common offset)
                double bind = 0, spread = 0;
                for (const auto& skin : u.skins) {
                    std::array<float, 16> first{};
                    for (std::size_t j = 0; j < skin.inverse_binds.size() && j < skin.nodes.size(); ++j) {
                        if (skin.nodes[j] >= u.nodes.size()) { bind = 1e9; continue; }
                        const auto& a = skin.inverse_binds[j];
                        const auto& w = u.nodes[skin.nodes[j]].world;
                        std::array<float, 16> product{};
                        for (int r = 0; r < 4; ++r)
                            for (int c = 0; c < 4; ++c) {
                                float v = 0;
                                for (int k = 0; k < 4; ++k) v += a[r * 4 + k] * w[k * 4 + c];
                                product[r * 4 + c] = v;
                                bind = std::max(bind, static_cast<double>(std::fabs(v - (r == c ? 1.0f : 0.0f))));
                            }
                        if (j == 0) first = product;
                        for (int k = 0; k < 16; ++k) spread = std::max(spread, static_cast<double>(std::fabs(product[k] - first[k])));
                    }
                }
                line.set("bind_deviation", dtglb::json::Value::of(bind));
                line.set("bind_spread", dtglb::json::Value::of(spread));
                auto lods = dtglb::json::Value::array();
                for (const auto& lod : u.lods) {
                    auto steps = dtglb::json::Value::array();
                    for (const auto& step : lod.steps) {
                        auto one = dtglb::json::Value::array();
                        one.push(dtglb::json::Value::of(static_cast<double>(step.height_from)));
                        one.push(dtglb::json::Value::of(static_cast<double>(step.height_to)));
                        one.push(dtglb::json::Value::of(static_cast<double>(step.meshes.size())));
                        steps.push(std::move(one));
                    }
                    lods.push(std::move(steps));
                }
                line.set("lods", std::move(lods));
                if (!u.lights.empty()) {
                    auto lights = dtglb::json::Value::array();
                    for (const auto& l : u.lights) {
                        auto light = dtglb::json::Value::object();
                        light.set("type", dtglb::json::Value::of(static_cast<double>(l.type)));
                        light.set("intensity", dtglb::json::Value::of(l.intensity));
                        light.set("temperature", dtglb::json::Value::of(l.temperature));
                        light.set("falloff_end", dtglb::json::Value::of(l.parameters[0]));
                        light.set("flags", dtglb::json::Value::of(static_cast<double>(l.flags)));
                        lights.push(std::move(light));
                    }
                    line.set("lights", std::move(lights));
                }
                line.set("state_machine", dtglb::json::Value::of(u.state_machine));
                if (!u.terrains.empty()) line.set("terrains", dtglb::json::Value::of(static_cast<double>(u.terrains.size())));
                auto groups = dtglb::json::Value::array();
                for (const auto& g : u.visibility_groups) {
                    char hex[16];
                    std::snprintf(hex, sizeof hex, "%08x", g.name);
                    groups.push(dtglb::json::Value::of(std::string(hex)));
                }
                line.set("visibility_groups", std::move(groups));
                dtglb::json::Value data;
                std::string data_error;
                if (!u.script_data.empty() && dtglb::stingray::unit::decode_script_data(u.script_data, data, data_error)) {
                    // LevelEditor.is_gizmo_unit: shown by the editor, hidden in the game (level_object_set_manager.lua)
                    // keys are kept as "#<id32>" (only hashes are stored)
                    const auto key = [](const char* name) {
                        char hex[16];
                        std::snprintf(hex, sizeof hex, "#%08x", dtglb::stingray::id32_from_id64(name));
                        return std::string(hex);
                    };
                    const auto* editor = data.find(key("LevelEditor"));
                    const auto* gizmo = editor ? editor->find(key("is_gizmo_unit")) : nullptr;
                    line.set("gizmo_unit", dtglb::json::Value::of(gizmo && gizmo->kind == dtglb::json::Value::Kind::Bool && gizmo->boolean));
                    line.set("script_data", data);
                } else if (!u.script_data.empty()) {
                    line.set("script_error", dtglb::json::Value::of(data_error));
                }
            }
            std::cout << dtglb::json::dump(line, 0) << '\n';
        }
        return failures ? 2 : 0;
    }
    if (std::string(argv[1]) == "--unit-data") {
        if (argc != 3) { usage(); return 1; }
        std::ifstream file(argv[2], std::ios::binary);
        const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        dtglb::json::Value data;
        std::vector<std::uint8_t> again;
        std::string error;
        if (!file.good() && !file.eof()) error = "cannot read the file";
        else if (dtglb::stingray::unit::decode_script_data(bytes, data, error) &&
                 dtglb::stingray::unit::encode_script_data(data, again, error) && again != bytes)
            error = "data encodes to different bytes";
        if (!error.empty()) { std::cerr << argv[2] << ": " << error << '\n'; return 2; }
        std::cout << dtglb::json::dump(data) << '\n';
        return 0;
    }
    if (std::string(argv[1]) == "--shader") {
        if (argc != 3) { usage(); return 1; }
        std::ifstream file(argv[2], std::ios::binary);
        const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        std::string error;
        std::uint32_t header[7] = {};
        if (bytes.size() >= sizeof(header)) std::memcpy(header, bytes.data(), sizeof(header));
        dtglb::stingray::material::ShaderSection section;
        if (bytes.size() < sizeof(header)) error = "not a material stream";
        else if (!header[4] || header[3] > bytes.size() || header[4] > bytes.size() - header[3]) error = "the stream has no shader section";
        else dtglb::stingray::material::decode_shader_section(
            std::vector<std::uint8_t>(bytes.begin() + header[3], bytes.begin() + header[3] + header[4]), section, error);
        if (!error.empty()) { std::cerr << argv[2] << ": " << error << '\n'; return 2; }
        std::cout << dtglb::json::dump(dtglb::stingray::material::describe_shader_section(section)) << '\n';
        return 0;
    }
    if (std::string(argv[1]) == "--dxil-check") {
        // every compiled stage of each material stream read as DXIL (code and names): one JSON line per stream
        if (argc < 3) { usage(); return 1; }
        int failures = 0;
        for (int i = 2; i < argc; ++i) {
            std::ifstream file(argv[i], std::ios::binary);
            const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            std::string error;
            std::uint32_t header[7] = {};
            if (bytes.size() >= sizeof(header)) std::memcpy(header, bytes.data(), sizeof(header));
            dtglb::stingray::material::ShaderSection section;
            if (bytes.size() < sizeof(header)) error = "not a material stream";
            else if (!header[4] || header[3] > bytes.size() || header[4] > bytes.size() - header[3]) error = "the stream has no shader section";
            else dtglb::stingray::material::decode_shader_section(
                std::vector<std::uint8_t>(bytes.begin() + header[3], bytes.begin() + header[3] + header[4]), section, error);
            std::size_t stages = 0, instructions = 0;
            for (const auto& device : section.device_data) {
                for (const auto& group : device.groups)
                    for (const auto& pass : group)
                        for (const auto& stage : pass.stages)
                            for (const auto& variant : stage) {
                                if (!error.empty()) break;
                                std::vector<std::uint8_t> dxbc(variant.size);
                                if (!dtglb::stingray::texture::oodle_decompress(variant.bytecode, dxbc, error)) break;
                                const auto parts = dtglb::stingray::shader::parse_container(dxbc, error);
                                for (const char* part : {"DXIL", "STAT"}) {
                                    const auto found = parts.find(part);
                                    if (found == parts.end()) continue;
                                    std::uint32_t kind = 0;
                                    std::vector<std::uint8_t> bitcode;
                                    dtglb::stingray::shader::Module module;
                                    if (!dtglb::stingray::shader::program_bitcode(found->second, kind, bitcode, error) ||
                                        !module.read(bitcode, error)) {
                                        error = std::string(part) + ": " + error;
                                        break;
                                    }
                                    for (const auto* f : module.functions)
                                        for (const auto& b : f->blocks) instructions += b.size();
                                }
                                ++stages;
                            }
                break;  // the first device (D3D12) is the one the game uses
            }
            dtglb::json::Value line = dtglb::json::Value::object();
            line.set("stream", dtglb::json::Value::of(fs::u8path(argv[i]).filename().string()));
            line.set("stages", dtglb::json::Value::of(static_cast<double>(stages)));
            line.set("instructions", dtglb::json::Value::of(static_cast<double>(instructions)));
            if (!error.empty()) { line.set("error", dtglb::json::Value::of(error)); ++failures; }
            std::cout << dtglb::json::dump(line, 0) << '\n';
        }
        return failures ? 2 : 0;
    }
    if (std::string(argv[1]) == "--shader-stages") {
        if (argc != 4) { usage(); return 1; }
        std::ifstream file(argv[2], std::ios::binary);
        const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        std::string error;
        std::uint32_t header[7] = {};
        if (bytes.size() >= sizeof(header)) std::memcpy(header, bytes.data(), sizeof(header));
        dtglb::stingray::material::ShaderSection section;
        if (bytes.size() < sizeof(header)) error = "not a material stream";
        else if (!header[4] || header[3] > bytes.size() || header[4] > bytes.size() - header[3]) error = "the stream has no shader section";
        else dtglb::stingray::material::decode_shader_section(
            std::vector<std::uint8_t>(bytes.begin() + header[3], bytes.begin() + header[3] + header[4]), section, error);
        if (!error.empty()) { std::cerr << argv[2] << ": " << error << '\n'; return 2; }
        // index.json: contexts (shader names + condition offsets), the condition programs, and per shader its
        // passes with layer, render states, sampler states and the stage files written next to it
        using dtglb::json::Value;
        const fs::path out = fs::u8path(argv[3]);
        std::error_code ec;
        fs::create_directories(out, ec);
        auto root = Value::object();
        auto contexts = Value::array();
        for (const auto& c : section.contexts) {
            auto context = Value::object();
            context.set("name", Value::of(static_cast<double>(c.name)));
            auto shaders = Value::array();
            for (const auto& [shader, condition] : c.shaders) {
                auto pair = Value::array();
                pair.push(Value::of(static_cast<double>(shader)));
                pair.push(Value::of(static_cast<double>(condition)));
                shaders.push(std::move(pair));
            }
            context.set("shaders", std::move(shaders));
            contexts.push(std::move(context));
        }
        root.set("contexts", std::move(contexts));
        static const char digits[] = "0123456789abcdef";
        std::string conditions;
        for (const auto byte : section.conditions) { conditions += digits[byte >> 4]; conditions += digits[byte & 15]; }
        root.set("conditions", Value::of(conditions));
        auto states = [](const std::vector<dtglb::stingray::material::RenderState>& list) {
            auto array = Value::array();
            for (const auto& state : list) {
                auto pair = Value::array();
                pair.push(Value::of(static_cast<double>(state.key)));
                pair.push(Value::of(static_cast<double>(state.value)));
                array.push(std::move(pair));
            }
            return array;
        };
        static const char* stage_names[] = {"vertex", "domain", "hull", "geometry", "pixel", "compute"};
        auto shaders = Value::array();
        for (std::size_t i = 0; i < section.shader_data.size() && error.empty(); ++i) {
            const auto& data = section.shader_data[i];
            auto shader = Value::object();
            shader.set("name", Value::of(static_cast<double>(data.name)));
            auto passes = Value::array();
            for (std::size_t p = 0; p < data.passes.size() && error.empty(); ++p) {
                auto pass = Value::object();
                pass.set("layer", Value::of(static_cast<double>(data.passes[p].layer)));
                const dtglb::stingray::material::ShaderPass* device = nullptr;
                for (const auto& d : section.device_data)
                    if (i < d.groups.size() && p < d.groups[i].size()) device = &d.groups[i][p];
                if (device) {
                    pass.set("render_states", states(device->render_states));
                    auto samplers = Value::object();
                    for (const auto& sampler : device->sampler_states) samplers.set(std::to_string(sampler.name), states(sampler.states));
                    pass.set("sampler_states", std::move(samplers));
                    for (const std::size_t k : {std::size_t{0}, std::size_t{4}}) {
                        if (device->stages[k].empty()) continue;
                        const auto& variant = device->stages[k].front();
                        std::vector<std::uint8_t> unpacked(variant.size);
                        if (!dtglb::stingray::texture::oodle_decompress(variant.bytecode, unpacked, error)) break;
                        const auto name = "s" + std::to_string(i) + "_p" + std::to_string(p) + "_" + stage_names[k] + ".dxbc";
                        std::ofstream stage(out / name, std::ios::binary);
                        stage.write(reinterpret_cast<const char*>(unpacked.data()), static_cast<std::streamsize>(unpacked.size()));
                        if (!stage) { error = "cannot write " + (out / name).string(); break; }
                        pass.set(stage_names[k], Value::of(name));
                    }
                }
                passes.push(std::move(pass));
            }
            shader.set("passes", std::move(passes));
            shaders.push(std::move(shader));
        }
        if (!error.empty()) { std::cerr << argv[2] << ": " << error << '\n'; return 2; }
        root.set("shaders", std::move(shaders));
        std::ofstream index(out / "index.json", std::ios::binary);
        index << dtglb::json::dump(root) << '\n';
        if (!index) { std::cerr << "cannot write " << (out / "index.json").string() << '\n'; return 2; }
        return 0;
    }
    if (std::string(argv[1]) == "--texture-image" || std::string(argv[1]) == "--texture-read") {
        // --texture-image: the full-size mip as a PNG; --texture-read: every listed texture read back (one JSON line each)
        namespace texture = dtglb::stingray::texture;
        const bool image_mode = std::string(argv[1]) == "--texture-image";
        if (image_mode ? argc != 4 : argc < 3) { usage(); return 1; }
        const fs::path bundle = texture::game_root() / "bundle";
        const texture::ReadLoose read_loose = [&](const std::string& relative, std::vector<std::uint8_t>& data, std::string& error) {
            std::ifstream source(bundle / relative, std::ios::binary);
            if (!source) { error = "names " + relative + ", which is not in the game folder"; return false; }
            data.assign(std::istreambuf_iterator<char>(source), std::istreambuf_iterator<char>());
            return true;
        };
        int failures = 0;
        for (int i = 2; i < (image_mode ? 3 : argc); ++i) {
            std::ifstream file(fs::u8path(argv[i]), std::ios::binary);
            const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            texture::GameTexture read;
            std::string error;
            texture::read_game_texture(bytes, read_loose, true, read, error);
            if (image_mode && !error.empty()) {
                // an extract older than the game's .stream files: the mips the texture holds itself still read
                error.clear();
                texture::read_game_texture(bytes, read_loose, false, read, error);
            }
            if (image_mode) {
                texture::ImageRGBA image;
                if (error.empty() && texture::mip_image(read.dxgi_format, read.width, read.height, read.mips.front(), image, error))
                    texture::write_png(image, fs::u8path(argv[3]), error);
                if (!error.empty()) { std::cerr << argv[2] << ": " << error << '\n'; return 2; }
                std::cout << read.width << "x" << read.height << (texture::dxgi_is_srgb(read.dxgi_format) ? " srgb" : " linear") << '\n';
                return 0;
            }
            dtglb::json::Value line = dtglb::json::Value::object();
            line.set("texture", dtglb::json::Value::of(fs::u8path(argv[i]).filename().string()));
            if (!error.empty()) {
                line.set("error", dtglb::json::Value::of(error));
                ++failures;
            } else {
                line.set("format", dtglb::json::Value::of(static_cast<double>(read.dxgi_format)));
                line.set("width", dtglb::json::Value::of(static_cast<double>(read.width)));
                line.set("height", dtglb::json::Value::of(static_cast<double>(read.height)));
                if (read.faces > 1) line.set(read.cube ? "cube_faces" : "slices", dtglb::json::Value::of(static_cast<double>(read.faces)));
                line.set("mips", dtglb::json::Value::of(static_cast<double>(read.mips.size())));
                line.set("streamed", dtglb::json::Value::of(static_cast<double>(read.streamed_mips)));
            }
            std::cout << dtglb::json::dump(line, 0) << '\n';
        }
        return failures ? 2 : 0;
    }
    if (std::string(argv[1]) == "--lua-tables") {
        // the constant tables of compiled Lua resources: how many, and the volume entries among them (name + type)
        if (argc < 3) { usage(); return 1; }
        int failures = 0;
        for (int i = 2; i < argc; ++i) {
            std::ifstream file(fs::u8path(argv[i]), std::ios::binary);
            const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            std::vector<std::uint8_t> body;
            std::string stream, error;
            std::vector<dtglb::stingray::LuaTable> tables;
            dtglb::json::Value line = dtglb::json::Value::object();
            line.set("lua", dtglb::json::Value::of(fs::u8path(argv[i]).filename().string()));
            if (!dtglb::stingray::parse_cooked_resource_envelope(bytes, "lua", body, stream, error) ||
                !dtglb::stingray::read_lua_tables(body, tables, error)) {
                line.set("error", dtglb::json::Value::of(error));
                ++failures;
            } else {
                std::size_t volumes = 0;
                for (const auto& t : tables) volumes += t.field("name") && t.field("type") && t.field("bottom_points");
                line.set("tables", dtglb::json::Value::of(static_cast<double>(tables.size())));
                line.set("volume_entries", dtglb::json::Value::of(static_cast<double>(volumes)));
            }
            std::cout << dtglb::json::dump(line, 0) << '\n';
        }
        return failures ? 2 : 0;
    }
    if (std::string(argv[1]) == "--lua-returned") {
        // the value compiled data scripts return (a level's _main_path, _volume_data), as JSON: tables with keys 1..n as
        // arrays, other tables as objects, calls as {"call": name, "arguments": [...]}
        if (argc < 3) { usage(); return 1; }
        int failures = 0;
        std::function<dtglb::json::Value(const dtglb::stingray::LuaData&)> to_json = [&](const dtglb::stingray::LuaData& v) {
            using J = dtglb::json::Value;
            if (const auto* b = std::get_if<bool>(&v)) return J::of(*b);
            if (const auto* i = std::get_if<std::int64_t>(&v)) return J::of(static_cast<double>(*i));
            if (const auto* d = std::get_if<double>(&v)) return J::of(*d);
            if (const auto* s = std::get_if<std::string>(&v)) return J::of(*s);
            const auto* o = std::get_if<std::shared_ptr<dtglb::stingray::LuaObject>>(&v);
            if (!o || !*o) return J();
            if ((*o)->kind != dtglb::stingray::LuaObject::table) {
                J out = J::object();
                out.set("call", J::of((*o)->function));
                J arguments = J::array();
                for (const auto& a : (*o)->arguments) arguments.push(to_json(a));
                out.set("arguments", std::move(arguments));
                return out;
            }
            bool list = true;
            for (std::size_t k = 0; k < (*o)->fields.size() && list; ++k) {
                const auto* i = std::get_if<std::int64_t>(&(*o)->fields[k].first);
                list = i && *i == static_cast<std::int64_t>(k + 1);
            }
            J out = list ? J::array() : J::object();
            for (const auto& [key, value] : (*o)->fields) {
                if (list) { out.push(to_json(value)); continue; }
                std::string name;
                if (const auto* s = std::get_if<std::string>(&key)) name = *s;
                else if (const auto* i = std::get_if<std::int64_t>(&key)) name = std::to_string(*i);
                else name = "?";
                out.set(name, to_json(value));
            }
            return out;
        };
        for (int i = 2; i < argc; ++i) {
            std::ifstream file(fs::u8path(argv[i]), std::ios::binary);
            const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            std::vector<std::uint8_t> body;
            std::string stream, error;
            dtglb::stingray::LuaData returned;
            dtglb::json::Value line = dtglb::json::Value::object();
            line.set("lua", dtglb::json::Value::of(fs::u8path(argv[i]).filename().string()));
            if (!dtglb::stingray::parse_cooked_resource_envelope(bytes, "lua", body, stream, error) ||
                !dtglb::stingray::read_lua_returned(body, returned, error)) {
                line.set("error", dtglb::json::Value::of(error));
                ++failures;
            } else {
                line.set("returned", to_json(returned));
            }
            std::cout << dtglb::json::dump(line, 0) << '\n';
        }
        return failures ? 2 : 0;
    }
    if (std::string(argv[1]) == "--lua-components") {
        // the components compiled component scripts declare, each field with its ui_type; fields whose definition
        // holds a value that isn't a constant (or a call of a global with constants) are listed as open
        if (argc < 3) { usage(); return 1; }
        int failures = 0;
        for (int i = 2; i < argc; ++i) {
            std::ifstream file(fs::u8path(argv[i]), std::ios::binary);
            const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            std::vector<std::uint8_t> body;
            std::string stream, error;
            std::vector<dtglb::stingray::Component> components;
            dtglb::json::Value line = dtglb::json::Value::object();
            line.set("lua", dtglb::json::Value::of(fs::u8path(argv[i]).filename().string()));
            if (!dtglb::stingray::parse_cooked_resource_envelope(bytes, "lua", body, stream, error) ||
                !dtglb::stingray::read_lua_components(body, components, error)) {
                line.set("error", dtglb::json::Value::of(error));
                ++failures;
                std::cout << dtglb::json::dump(line, 0) << '\n';
                continue;
            }
            // a definition value that is unknown, anywhere inside it
            std::function<bool(const dtglb::stingray::LuaData&)> open = [&](const dtglb::stingray::LuaData& v) {
                const auto* o = std::get_if<std::shared_ptr<dtglb::stingray::LuaObject>>(&v);
                if (!o || !*o) return false;
                if ((*o)->kind == dtglb::stingray::LuaObject::unknown) return true;
                for (const auto& [k, x] : (*o)->fields) if (open(x)) return true;
                for (const auto& x : (*o)->arguments) if (open(x)) return true;
                return false;
            };
            auto list = dtglb::json::Value::array();
            for (const auto& c : components) {
                auto entry = dtglb::json::Value::object();
                entry.set("name", dtglb::json::Value::of(c.name));
                auto fields = dtglb::json::Value::array();
                auto open_fields = dtglb::json::Value::array();
                if (c.data)
                    for (const auto& [key, spec] : c.data->fields) {
                        const auto* name = std::get_if<std::string>(&key);
                        const auto* def = std::get_if<std::shared_ptr<dtglb::stingray::LuaObject>>(&spec);
                        const dtglb::stingray::LuaData* type = def && *def ? (*def)->get("ui_type") : nullptr;
                        const auto* type_name = type ? std::get_if<std::string>(type) : nullptr;
                        fields.push(dtglb::json::Value::of((name ? *name : std::string("?")) + ":" + (type_name ? *type_name : std::string("?"))));
                        if (open(spec)) open_fields.push(dtglb::json::Value::of(name ? *name : std::string("?")));
                    }
                entry.set("fields", std::move(fields));
                if (!open_fields.items.empty()) entry.set("open", std::move(open_fields));
                list.push(std::move(entry));
            }
            line.set("components", std::move(list));
            std::cout << dtglb::json::dump(line, 0) << '\n';
        }
        return failures ? 2 : 0;
    }
    if (std::string(argv[1]) == "--shader-library-read") {
        namespace material = dtglb::stingray::material;
        if (argc < 3) { usage(); return 1; }
        int failures = 0;
        for (int i = 2; i < argc; ++i) {
            std::ifstream file(fs::u8path(argv[i]), std::ios::binary);
            const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            std::vector<std::uint8_t> body;
            std::string stream, error;
            material::ShaderLibraryGroup group;
            dtglb::json::Value line = dtglb::json::Value::object();
            line.set("shader_library_group", dtglb::json::Value::of(fs::u8path(argv[i]).filename().string()));
            if (!dtglb::stingray::parse_cooked_resource_envelope(bytes, "shader_library_group", body, stream, error) ||
                !material::decode_shader_library_group(body, group, error)) {
                line.set("error", dtglb::json::Value::of(error));
                ++failures;
            } else {
                auto libraries = dtglb::json::Value::array();
                for (const auto& library : group.libraries) {
                    auto entry = dtglb::json::Value::object();
                    auto shaders = dtglb::json::Value::array();
                    for (const auto& data : library.shader_data) {
                        char name[16];
                        std::snprintf(name, sizeof name, "#%08x", data.name);
                        shaders.push(dtglb::json::Value::of(std::string(name)));
                    }
                    entry.set("shaders", std::move(shaders));
                    auto templates = dtglb::json::Value::array();
                    for (const auto& t : library.templates) {
                        auto item = dtglb::json::Value::object();
                        char name[16];
                        std::snprintf(name, sizeof name, "#%08x", t.name);
                        item.set("name", dtglb::json::Value::of(std::string(name)));
                        auto items = dtglb::json::Value::array();
                        for (const auto& it : t.items) {
                            std::snprintf(name, sizeof name, "#%08x", it.name);
                            auto words = dtglb::json::Value::array();
                            words.push(dtglb::json::Value::of(std::string(name)));
                            for (const auto& [word, bytes] : it.entries) {
                                std::snprintf(name, sizeof name, "#%08x", word);
                                words.push(dtglb::json::Value::of(std::string(name) + "/" + std::to_string(bytes.size())));
                            }
                            items.push(std::move(words));
                        }
                        item.set("items", std::move(items));
                        templates.push(std::move(item));
                    }
                    entry.set("templates", std::move(templates));
                    entry.set("lists", dtglb::json::Value::of(static_cast<double>(library.lists.size())));
                    entry.set("records", dtglb::json::Value::of(static_cast<double>(library.records.size())));
                    libraries.push(std::move(entry));
                }
                line.set("libraries", std::move(libraries));
            }
            std::cout << dtglb::json::dump(line, 0) << '\n';
        }
        return failures ? 2 : 0;
    }
    if (std::string(argv[1]) == "--shading-environment-read") {
        namespace se = dtglb::stingray::shading_environment;
        if (argc < 3) { usage(); return 1; }
        int failures = 0;
        for (int i = 2; i < argc; ++i) {
            std::ifstream file(fs::u8path(argv[i]), std::ios::binary);
            const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            std::vector<std::uint8_t> body, written;
            std::string stream, error;
            se::ShadingEnvironment environment;
            dtglb::json::Value line = dtglb::json::Value::object();
            line.set("shading_environment", dtglb::json::Value::of(fs::u8path(argv[i]).filename().string()));
            if (!dtglb::stingray::parse_cooked_resource_envelope(bytes, "shading_environment", body, stream, error) ||
                !se::read_shading_environment(body, environment, error)) {
                line.set("error", dtglb::json::Value::of(error));
                ++failures;
            } else {
                se::write_shading_environment(environment, written);
                line.set("variables", dtglb::json::Value::of(static_cast<double>(environment.variables.size())));
                line.set("settings", dtglb::json::Value::of(static_cast<double>(environment.settings.size())));
                line.set("engine_textures", dtglb::json::Value::of(static_cast<double>(environment.engine_textures.size())));
                line.set("identical", dtglb::json::Value::of(written == body));
                if (written != body) ++failures;
            }
            std::cout << dtglb::json::dump(line, 0) << '\n';
        }
        return failures ? 2 : 0;
    }
    if (std::string(argv[1]) == "--level-edit-check") {
        // each level: remove a unit nothing uses and add a copy of the first one; every unit reference of the level
        // flow and the stories must still name the same unit (by its id), and the edited level must read back
        namespace level = dtglb::stingray::level;
        if (argc < 3) { usage(); return 1; }
        int failures = 0;
        const auto references = [](const level::LevelDocument& d) {
            std::vector<std::uint64_t> ids;
            for (const auto& n : d.unit_nodes) ids.push_back(n[0] < d.units.size() ? d.units[n[0]].id : ~0ull);
            for (std::size_t u = 0; u < d.units.size(); ++u)
                for (const auto& user : level::unit_users(d, u))
                    if (user != "level flow") ids.push_back(d.units[u].id ^ dtglb::stingray::id64(user));
            std::sort(ids.begin(), ids.end());
            return ids;
        };
        for (int i = 2; i < argc; ++i) {
            std::ifstream file(fs::u8path(argv[i]), std::ios::binary);
            const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            level::LevelDocument d, back;
            std::string error;
            dtglb::json::Value line = dtglb::json::Value::object();
            line.set("level", dtglb::json::Value::of(fs::u8path(argv[i]).filename().string()));
            if (!level::read_level_document(bytes, d, error) || d.units.empty()) {
                line.set("skipped", dtglb::json::Value::of(error.empty() ? std::string("no units") : error));
                std::cout << dtglb::json::dump(line, 0) << '\n';
                continue;
            }
            const auto before = references(d);
            // every volume made again from its outline and height: the same bytes as stored
            std::size_t volumes_same = 0;
            for (const auto& v : d.volumes) {
                dtglb::stingray::VolumeShape shape;
                std::vector<std::uint8_t> again;
                std::string problem;
                if (dtglb::stingray::read_volume_record(v.record, shape, problem) &&
                    dtglb::stingray::make_volume_record(shape, false, again, problem) && again == v.record)
                    ++volumes_same;
                else if (!line.find("volume_differs_at")) {
                    std::size_t at = 0;
                    while (at < again.size() && at < v.record.size() && again[at] == v.record[at]) ++at;
                    line.set("volume_differs_at", dtglb::json::Value::of(static_cast<double>(at)));
                    line.set("volume_sizes", dtglb::json::Value::of(std::to_string(again.size()) + "/" + std::to_string(v.record.size()) + " " + problem));
                }
            }
            line.set("volumes", dtglb::json::Value::of(static_cast<double>(d.volumes.size())));
            line.set("volumes_rebuilt_identically", dtglb::json::Value::of(static_cast<double>(volumes_same)));
            // every unit's dynamic data read and written again: the same bytes
            std::size_t data_same = 0, data_units = 0;
            for (const auto& u : d.units) {
                if (u.data.empty()) continue;
                ++data_units;
                std::vector<dtglb::stingray::DynamicEntry> entries;
                std::vector<std::uint8_t> again;
                std::size_t size = 0;
                std::string problem;
                if (dtglb::stingray::read_dynamic_data(u.data, 0, entries, size, problem)) {
                    dtglb::stingray::write_dynamic_data(entries, again);
                    data_same += again == u.data;
                }
            }
            // the level's own flow through the flow codec: the same bytes back
            if (!d.flow.empty()) {
                dtglb::json::Value graph;
                std::vector<std::uint8_t> flow_again, data_again;
                std::string problem;
                const bool same = dtglb::stingray::flow::decode(d.flow, d.flow_data, graph, problem) &&
                                  dtglb::stingray::flow::encode(graph, flow_again, data_again, problem) && flow_again == d.flow &&
                                  data_again == d.flow_data;
                line.set("level_flow_identical", dtglb::json::Value::of(same));
                std::map<int, int> types;
                if (const auto* nodes = graph.find("nodes"); nodes && nodes->is_array())
                    for (const auto& node : nodes->items)
                        if (const auto* type = node.find("type"); type && type->is_number()) ++types[static_cast<int>(type->number)];
                dtglb::json::Value used = dtglb::json::Value::object();
                for (const auto& [type, count] : types) used.set(std::to_string(type), dtglb::json::Value::of(static_cast<double>(count)));
                line.set("level_flow_types", std::move(used));
                if (!same) line.set("level_flow_problem", dtglb::json::Value::of(problem.empty() ? std::string("bytes differ") : problem));
            }
            // every light record read and written again
            std::size_t lights_same = 0;
            for (const auto& record : d.lights) {
                level::LevelLight light;
                lights_same += level::read_level_light(record, light) && level::write_level_light(light) == record;
            }
            // every camera record too
            std::size_t cameras_same = 0;
            for (const auto& record : d.cameras) {
                level::LevelCamera camera;
                cameras_same += level::read_level_camera(record, camera) && level::write_level_camera(camera) == record;
            }
            line.set("cameras", dtglb::json::Value::of(static_cast<double>(d.cameras.size())));
            line.set("cameras_rewritten_identically", dtglb::json::Value::of(static_cast<double>(cameras_same)));
            if (cameras_same != d.cameras.size()) error = "a camera record writes back differently";
            line.set("lights", dtglb::json::Value::of(static_cast<double>(d.lights.size())));
            line.set("lights_rewritten_identically", dtglb::json::Value::of(static_cast<double>(lights_same)));
            if (lights_same != d.lights.size()) error = "a light record writes back differently";
            line.set("unit_data", dtglb::json::Value::of(static_cast<double>(data_units)));
            line.set("unit_data_rewritten_identically", dtglb::json::Value::of(static_cast<double>(data_same)));
            if (data_same != data_units) error = "unit dynamic data writes back differently";
            std::size_t removed = d.units.size();
            for (std::size_t u = d.units.size() / 2; u < d.units.size() && removed == d.units.size(); ++u)
                if (level::unit_users(d, u).empty()) removed = u;
            // who owns each camera record: only the removed unit's may go
            const auto camera_owners = [](const level::LevelDocument& doc) {
                std::vector<std::pair<std::uint64_t, level::Bytes>> owners;
                for (auto record : doc.cameras) {
                    std::uint32_t number = 0;
                    std::memcpy(&number, record.data(), 4);
                    std::memset(record.data(), 0, 4);
                    owners.emplace_back(number < doc.units.size() ? doc.units[number].id : 0, std::move(record));
                }
                std::sort(owners.begin(), owners.end());
                return owners;
            };
            auto cameras_kept = camera_owners(d);
            if (removed < d.units.size()) {
                const auto gone = d.units[removed].id;
                cameras_kept.erase(std::remove_if(cameras_kept.begin(), cameras_kept.end(), [&](const auto& o) { return o.first == gone; }), cameras_kept.end());
            }
            // the edited list: without that unit, with a copy of the first one (same id: it gets its own) before the
            // background units
            std::vector<level::DocumentUnit> units;
            for (std::size_t u = 0; u < d.units.size(); ++u)
                if (u != removed) units.push_back(d.units[u]);
            units.insert(units.end() - std::min<std::size_t>(d.background_units, units.size()), d.units.front());
            if (error.empty() && !level::set_units(d, std::move(units), d.background_units, error)) error = "set_units: " + error;
            std::vector<std::uint8_t> body;
            if (error.empty() && level::write_level_document(d, body, error) && level::read_level_document(body, back, error)) {
                if (references(back) != before) error = "a unit reference changed";
                else if (back.units.size() != d.units.size()) error = "the unit count reads back differently";
                else if (camera_owners(back) != cameras_kept) error = "a camera record lost its unit";
            }
            if (!error.empty()) {
                line.set("error", dtglb::json::Value::of(error));
                ++failures;
            } else {
                line.set("ok", dtglb::json::Value::of(true));
                line.set("references", dtglb::json::Value::of(static_cast<double>(before.size())));
            }
            std::cout << dtglb::json::dump(line, 0) << '\n';
        }
        return failures ? 2 : 0;
    }
    if (std::string(argv[1]) == "--level-roundtrip") {
        namespace level = dtglb::stingray::level;
        if (argc < 3) { usage(); return 1; }
        int failures = 0;
        for (int i = 2; i < argc; ++i) {
            std::ifstream file(fs::u8path(argv[i]), std::ios::binary);
            const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            std::vector<std::uint8_t> body, written;
            std::string stream, error;
            level::LevelDocument document;
            dtglb::json::Value line = dtglb::json::Value::object();
            line.set("level", dtglb::json::Value::of(fs::u8path(argv[i]).filename().string()));
            if (!dtglb::stingray::parse_cooked_resource_envelope(bytes, "level", body, stream, error)) body = bytes;
            error.clear();
            if (!level::read_level_document(bytes, document, error) || !level::write_level_document(document, written, error)) {
                line.set("error", dtglb::json::Value::of(error));
                ++failures;
            } else if (written != body) {
                std::size_t at = 0;
                while (at < written.size() && at < body.size() && written[at] == body[at]) ++at;
                // the header word whose section holds the first difference: the largest offset at or below it
                std::uint32_t section = 0, best = 0;
                for (std::uint32_t h = 0x10; h < 0xc0 && at >= 0xc0; h += 4) {
                    std::uint32_t v = 0;
                    std::memcpy(&v, body.data() + h, 4);
                    if (v <= at && v >= best && v > 0xc0) { best = v; section = h; }
                }
                line.set("differs_at", dtglb::json::Value::of(static_cast<double>(at)));
                line.set("section", dtglb::json::Value::of(static_cast<double>(at < 0xc0 ? at & ~3u : section)));
                line.set("size", dtglb::json::Value::of(static_cast<double>(body.size())));
                line.set("written", dtglb::json::Value::of(static_cast<double>(written.size())));
                ++failures;
            } else {
                line.set("identical", dtglb::json::Value::of(true));
            }
            std::cout << dtglb::json::dump(line, 0) << '\n';
        }
        return failures ? 2 : 0;
    }
    if (std::string(argv[1]) == "--level-new") {
        // an empty level: --level-new <resource path> <out.level>
        namespace level = dtglb::stingray::level;
        if (argc != 4) { usage(); return 1; }
        std::vector<std::uint8_t> body;
        std::string error;
        if (!level::write_level_document(level::new_level_document(argv[2]), body, error)) { std::cerr << error << '\n'; return 1; }
        const auto cooked = dtglb::stingray::wrap_cooked_resource("level", argv[2], body);
        std::ofstream(fs::u8path(argv[3]), std::ios::binary).write(reinterpret_cast<const char*>(cooked.data()), static_cast<std::streamsize>(cooked.size()));
        return 0;
    }
    if (std::string(argv[1]) == "--level-folder") {
        // a level as a Custom Assets folder: --level-folder <file.level> <out folder> [Custom folder]... [--names <dictionary>]
        namespace level = dtglb::stingray::level;
        if (argc < 4) { usage(); return 1; }
        std::vector<fs::path> custom;
        std::unordered_map<std::uint64_t, std::string> names;
        for (int i = 4; i < argc; ++i) {
            if (std::string(argv[i]) == "--names" && i + 1 < argc) names = level::read_name_dictionary(fs::u8path(argv[++i]));
            else custom.push_back(fs::u8path(argv[i]));
        }
        std::string summary, error;
        if (!level::write_level_folder(fs::u8path(argv[2]), fs::u8path(argv[3]), custom, summary, error, names)) { std::cerr << error << '\n'; return 1; }
        std::cout << summary << '\n';
        return 0;
    }
    if (std::string(argv[1]) == "--lua-data") {
        // a data script as the game's Lua resource: --lua-data <in.lua> <out> --name <resource path> [--luajit]
        // (--luajit: the bare dump in stock LuaJIT's form, for checking)
        if (argc < 4) { usage(); return 1; }
        std::string name;
        bool luajit = false;
        for (int i = 4; i < argc; ++i) {
            if (std::string(argv[i]) == "--name" && i + 1 < argc) name = argv[++i];
            else if (std::string(argv[i]) == "--luajit") luajit = true;
        }
        if (name.empty()) { std::cerr << "--lua-data needs --name <resource path>\n"; return 1; }
        std::ifstream in(fs::u8path(argv[2]), std::ios::binary);
        const std::string source((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::vector<std::uint8_t> dump;
        std::string error;
        if (!dtglb::stingray::compile_lua_data(source, "@" + name + ".lua",
                                               luajit ? dtglb::stingray::LuaDumpForm::luajit : dtglb::stingray::LuaDumpForm::game, dump, error)) {
            std::cerr << argv[2] << ": " << error << '\n';
            return 1;
        }
        const auto out = luajit ? dump : dtglb::stingray::wrap_cooked_resource("lua", name, dtglb::stingray::lua_resource_body(dump));
        std::ofstream(fs::u8path(argv[3]), std::ios::binary).write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
        std::cout << name << ": " << dump.size() << " bytes of bytecode\n";
        return 0;
    }
    if (std::string(argv[1]) == "--level-navigation") {
        // a level's navigation, made by the game's generator: --level-navigation <file.level> <game folder>
        // [Custom folder]... [--seed x,y,z]...
        namespace level = dtglb::stingray::level;
        if (argc < 4) { usage(); return 1; }
        std::vector<fs::path> custom;
        std::vector<std::array<float, 3>> seeds;
        bool report = false;
        dtglb::stingray::navigation::NavSettings settings;
        for (int i = 4; i < argc; ++i) {
            if (std::string(argv[i]) == "--report") {
                report = true;
            } else if (std::string(argv[i]) == "--physics-meshes" || std::string(argv[i]) == "--render-meshes") {
                settings.consume_physics_meshes = std::string(argv[i]) == "--physics-meshes";
            } else if (std::string(argv[i]) == "--seed" && i + 1 < argc) {
                std::array<float, 3> p{};
                if (std::sscanf(argv[++i], "%f,%f,%f", &p[0], &p[1], &p[2]) != 3) { std::cerr << "--seed takes x,y,z\n"; return 1; }
                seeds.push_back(p);
            } else {
                custom.push_back(fs::u8path(argv[i]));
            }
        }
        std::string summary, error, units;
        if (!level::build_level_navigation(fs::u8path(argv[2]), fs::u8path(argv[3]), custom, seeds, settings, summary, error,
                                           report ? &units : nullptr)) {
            std::cerr << error << '\n';
            return 1;
        }
        std::cout << units << summary << '\n';
        return 0;
    }
    if (std::string(argv[1]) == "--navdata-read") {
        // NavData resources: one JSON line each (sector, meshes with cells, floors, triangles, settings; graph points);
        // with --obj <out.obj> the walkable triangles of the first file as an OBJ
        namespace navigation = dtglb::stingray::navigation;
        if (argc < 3) { usage(); return 1; }
        int failures = 0;
        std::string obj;
        for (int i = 2; i + 1 < argc; ++i)
            if (std::string(argv[i]) == "--obj") obj = argv[i + 1];
        for (int i = 2; i < argc; ++i) {
            if (std::string(argv[i]) == "--obj") { ++i; continue; }
            std::ifstream file(fs::u8path(argv[i]), std::ios::binary);
            const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            navigation::NavData nav;
            std::string error;
            auto line = dtglb::json::Value::object();
            line.set("file", dtglb::json::Value::of(fs::u8path(argv[i]).filename().string()));
            if (!navigation::read_navdata(bytes, nav, error)) {
                line.set("error", dtglb::json::Value::of(error));
                ++failures;
            } else {
                line.set("version", dtglb::json::Value::of(nav.version));
                line.set("sector", dtglb::json::Value::of(nav.sector));
                auto meshes = dtglb::json::Value::array();
                for (const auto& m : nav.meshes) {
                    auto v = dtglb::json::Value::object();
                    v.set("cells", dtglb::json::Value::of(static_cast<double>(m.cells)));
                    v.set("floors", dtglb::json::Value::of(static_cast<double>(m.floors)));
                    v.set("triangles", dtglb::json::Value::of(static_cast<double>(m.vertices.size() / 3)));
                    auto s = dtglb::json::Value::array();
                    for (const float f : m.settings) s.push(dtglb::json::Value::of(static_cast<double>(f)));
                    s.push(dtglb::json::Value::of(static_cast<double>(m.cell_size)));
                    v.set("settings", std::move(s));
                    meshes.push(std::move(v));
                }
                line.set("meshes", std::move(meshes));
                line.set("graph_points", dtglb::json::Value::of(static_cast<double>(nav.graph_points.size())));
                if (!obj.empty()) {
                    std::ofstream out(fs::u8path(obj));
                    std::size_t n = 0;
                    for (const auto& m : nav.meshes)
                        for (const auto& v : m.vertices) out << "v " << v[0] << ' ' << v[1] << ' ' << v[2] << '\n';
                    for (const auto& m : nav.meshes)
                        for (std::size_t t = 0; t < m.vertices.size(); t += 3, n += 3) out << "f " << n + 1 << ' ' << n + 2 << ' ' << n + 3 << '\n';
                    obj.clear();
                }
            }
            std::cout << dtglb::json::dump(line, 0) << '\n';
        }
        return failures ? 1 : 0;
    }
    if (std::string(argv[1]) == "--level-prototype-unit") {
        // a level's boxes (one a line: centre, three axes, half sizes, colour, visible, solid, object set, physics
        // material, shape template) as its embedded UNIT
        namespace level = dtglb::stingray::level;
        if (argc != 4) { usage(); return 1; }
        std::ifstream in(fs::u8path(argv[2]));
        std::vector<level::LevelPrototype> boxes;
        for (level::LevelPrototype box; in;) {
            for (auto& v : box.center) in >> v;
            for (auto& axis : box.axes) for (auto& v : axis) in >> v;
            for (auto& v : box.half_size) in >> v;
            for (auto& v : box.colour) in >> v;
            in >> box.visible >> box.physical >> box.object_set >> box.material >> box.shape_template;
            if (in) boxes.push_back(box);
        }
        std::vector<std::uint8_t> body;
        std::string error;
        if (!level::write_level_prototype_unit(boxes, body, error)) {
            std::cerr << error << '\n';
            return 1;
        }
        std::ofstream(fs::u8path(argv[3]), std::ios::binary).write(reinterpret_cast<const char*>(body.data()), static_cast<std::streamsize>(body.size()));
        return 0;
    }
    if (std::string(argv[1]) == "--level-prototypes") {
        // a level's blockout boxes, checked against the vertices they came from
        namespace level = dtglb::stingray::level;
        namespace unit = dtglb::stingray::unit;
        namespace json = dtglb::json;
        if (argc < 3) { usage(); return 1; }
        int failures = 0;
        const auto number = [](auto v) { return json::Value::of(static_cast<double>(v)); };
        for (int i = 2; i < argc; ++i) {
            std::ifstream file(fs::u8path(argv[i]), std::ios::binary);
            const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            level::Level read;
            std::string error;
            auto line = json::Value::object();
            line.set("level", json::Value::of(fs::u8path(argv[i]).filename().string()));
            unit::Unit prototype;
            std::vector<level::LevelPrototype> boxes;
            if (level::read_level(bytes, read, error) && !read.prototype_unit.empty() &&
                unit::read_unit(read.prototype_unit, prototype, error))
                level::read_level_prototypes(prototype, boxes, error);
            if (error.empty() && read.prototype_unit.empty()) continue;
            double deviation = 0;
            std::size_t at = 0;
            for (const auto& mesh : prototype.meshes) {
                unit::DecodedGeometry geometry;
                if (!error.empty() || !unit::decode_geometry(prototype, prototype.geometries[mesh.geometry - 1], geometry, error)) break;
                for (std::size_t first = 0; first < geometry.positions.size() / 3; first += 36, ++at) {
                    const auto corners = level::prototype_vertices(boxes[at]);
                    for (std::size_t v = 0; v < 36; ++v)
                        for (std::size_t k = 0; k < 3; ++k)
                            deviation = std::max(deviation, std::fabs(double(corners[v][k]) - geometry.positions[(first + v) * 3 + k]));
                }
            }
            if (!error.empty()) {
                line.set("error", json::Value::of(error));
                ++failures;
            }
            // written from the boxes and read back: the same boxes, meshes and actors
            std::vector<std::uint8_t> rewritten;
            unit::Unit again;
            std::vector<level::LevelPrototype> boxes_again;
            if (error.empty() && level::write_level_prototype_unit(boxes, rewritten, error) && unit::read_unit(rewritten, again, error) &&
                level::read_level_prototypes(again, boxes_again, error)) {
                bool same = boxes.size() == boxes_again.size() && prototype.meshes.size() == again.meshes.size() &&
                            prototype.actors.size() == again.actors.size();
                for (std::size_t b = 0; same && b < boxes.size(); ++b)
                    same = level::same_prototype(boxes[b], boxes_again[b]) && boxes[b].colour == boxes_again[b].colour &&
                           boxes[b].visible == boxes_again[b].visible && boxes[b].object_set == boxes_again[b].object_set &&
                           boxes[b].material == boxes_again[b].material && boxes[b].shape_template == boxes_again[b].shape_template;
                for (std::size_t m = 0; same && m < prototype.meshes.size(); ++m)
                    same = prototype.meshes[m].name == again.meshes[m].name && prototype.meshes[m].flags == again.meshes[m].flags;
                line.set("rewritten_same", json::Value::of(same));
                if (!same) ++failures;
            }
            line.set("merged", number(level::merged_prototypes(boxes).size()));
            auto meshes = json::Value::array();
            for (const auto& mesh : prototype.meshes) {
                auto m = json::Value::array();
                m.push(number(mesh.name));
                m.push(number(mesh.flags));
                meshes.push(std::move(m));
            }
            auto actors = json::Value::array();
            for (const auto& actor : prototype.actors) {
                auto a = json::Value::object();
                a.set("name", number(actor.name));
                a.set("template", number(actor.actor_template));
                auto shapes = json::Value::array();
                for (const auto& shape : actor.shapes) {
                    auto v = json::Value::array();
                    v.push(number(shape.type));
                    v.push(number(shape.material));
                    v.push(number(shape.shape_template));
                    shapes.push(std::move(v));
                }
                a.set("shapes", std::move(shapes));
                actors.push(std::move(a));
            }
            line.set("boxes", number(boxes.size()));
            line.set("deviation", json::Value::of(deviation));
            line.set("meshes", std::move(meshes));
            line.set("actors", std::move(actors));
            std::cout << json::dump(line, 0) << '\n';
        }
        return failures ? 1 : 0;
    }
    if (std::string(argv[1]) == "--level-read") {
        namespace level = dtglb::stingray::level;
        if (argc < 3) { usage(); return 1; }
        int failures = 0;
        for (int i = 2; i < argc; ++i) {
            std::ifstream file(fs::u8path(argv[i]), std::ios::binary);
            const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            level::Level read;
            std::string error;
            dtglb::json::Value line = dtglb::json::Value::object();
            line.set("level", dtglb::json::Value::of(fs::u8path(argv[i]).filename().string()));
            if (!level::read_level(bytes, read, error)) {
                line.set("error", dtglb::json::Value::of(error));
                ++failures;
            } else {
                std::size_t poses = 0, scattered = 0;
                for (const auto& u : read.units) poses += u.node_poses.size();
                for (const auto& s : read.scatter) scattered += s.instances.size();
                line.set("name", dtglb::json::Value::of(read.name));
                line.set("units", dtglb::json::Value::of(static_cast<double>(read.units.size())));
                line.set("node_poses", dtglb::json::Value::of(static_cast<double>(poses)));
                line.set("nested", dtglb::json::Value::of(static_cast<double>(read.nested.size())));
                line.set("effects", dtglb::json::Value::of(static_cast<double>(read.effects.size())));
                line.set("volumes", dtglb::json::Value::of(static_cast<double>(read.volumes.size())));
                line.set("object_sets", dtglb::json::Value::of(static_cast<double>(read.object_sets.size())));
                line.set("nav_data", dtglb::json::Value::of(static_cast<double>(read.nav_data.size())));
                line.set("scatter_brushes", dtglb::json::Value::of(static_cast<double>(read.scatter.size())));
                line.set("scattered", dtglb::json::Value::of(static_cast<double>(scattered)));
                line.set("stories", dtglb::json::Value::of(static_cast<double>(read.stories.size())));
                line.set("prototypes", dtglb::json::Value::of(!read.prototype_unit.empty()));
            }
            std::cout << dtglb::json::dump(line, 0) << '\n';
        }
        return failures ? 2 : 0;
    }
    if (std::string(argv[1]) == "--state-machine-read") {
        namespace sm = dtglb::stingray::state_machine;
        if (argc < 3) { usage(); return 1; }
        int failures = 0;
        for (int i = 2; i < argc; ++i) {
            std::ifstream file(fs::u8path(argv[i]), std::ios::binary);
            const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            sm::StateMachine read;
            std::string error;
            dtglb::json::Value line = dtglb::json::Value::object();
            line.set("state_machine", dtglb::json::Value::of(fs::u8path(argv[i]).filename().string()));
            if (!sm::read_state_machine(bytes, read, error)) {
                line.set("error", dtglb::json::Value::of(error));
                ++failures;
            } else {
                std::size_t states = 0, clips = 0;
                for (const auto& layer : read.layers) {
                    states += layer.size();
                    for (const auto& state : layer) clips += state.animations.size();
                }
                line.set("layers", dtglb::json::Value::of(static_cast<double>(read.layers.size())));
                line.set("states", dtglb::json::Value::of(static_cast<double>(states)));
                line.set("clips", dtglb::json::Value::of(static_cast<double>(clips)));
            }
            std::cout << dtglb::json::dump(line, 0) << '\n';
        }
        return failures ? 2 : 0;
    }
    if (std::string(argv[1]) == "--animation-read") {
        namespace animation = dtglb::stingray::animation;
        if (argc < 3) { usage(); return 1; }
        int failures = 0;
        for (int i = 2; i < argc; ++i) {
            std::ifstream file(fs::u8path(argv[i]), std::ios::binary);
            const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            animation::GameAnimation read;
            std::string error;
            double rotation_norm = 0;  // largest |1 - |q|| over the stored rotation keys
            if (animation::read_game_animation(bytes, read, error)) {
                for (const auto& key : read.keys)
                    if (key.channel == 1) {
                        double n = 0;
                        for (float v : key.value) n += double(v) * v;
                        rotation_norm = std::max(rotation_norm, std::abs(1.0 - std::sqrt(n)));
                    }
                std::vector<animation::BonePose> pose;
                for (float t : {0.0f, read.length * 0.5f, read.length})
                    if (error.empty()) animation::sample_game_animation(read, t, pose, error);
            }
            dtglb::json::Value line = dtglb::json::Value::object();
            line.set("animation", dtglb::json::Value::of(fs::u8path(argv[i]).filename().string()));
            if (!error.empty()) {
                line.set("error", dtglb::json::Value::of(error));
                ++failures;
            } else {
                line.set("bones", dtglb::json::Value::of(static_cast<double>(read.start.size())));
                line.set("length", dtglb::json::Value::of(static_cast<double>(read.length)));
                line.set("keys", dtglb::json::Value::of(static_cast<double>(read.keys.size())));
                line.set("packed_keys", dtglb::json::Value::of(static_cast<double>(read.packed_keys)));
                line.set("packed_start", dtglb::json::Value::of(read.packed_start));
                line.set("beats", dtglb::json::Value::of(static_cast<double>(read.beats.size())));
                line.set("triggers", dtglb::json::Value::of(static_cast<double>(read.triggers.size())));
                line.set("size", dtglb::json::Value::of(static_cast<double>(read.size)));
                line.set("body", dtglb::json::Value::of(static_cast<double>(read.body_size)));
                line.set("end", dtglb::json::Value::of(static_cast<double>(read.end)));
                line.set("rotation_norm", dtglb::json::Value::of(rotation_norm));
            }
            std::cout << dtglb::json::dump(line, 0) << '\n';
        }
        return failures ? 2 : 0;
    }
    if (std::string(argv[1]) == "--physics-meshes") {
        // records of u32 type (3 triangle mesh, 4 convex), u32 size, cooked bytes
        if (argc != 3) { usage(); return 1; }
        std::ifstream file(fs::u8path(argv[2]), std::ios::binary);
        const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        std::vector<std::pair<std::uint32_t, std::vector<std::uint8_t>>> meshes;
        std::string error, text;
        for (std::size_t at = 0; at < bytes.size();) {
            std::uint32_t type = 0, size = 0;
            if (bytes.size() - at < 8) { error = "the mesh file is truncated"; break; }
            std::memcpy(&type, bytes.data() + at, 4);
            std::memcpy(&size, bytes.data() + at + 4, 4);
            at += 8;
            if (size > bytes.size() - at) { error = "the mesh file is truncated"; break; }
            meshes.emplace_back(type, std::vector<std::uint8_t>(bytes.begin() + at, bytes.begin() + at + size));
            at += size;
        }
        if (error.empty()) dtglb::stingray::physics::describe_cooked_meshes(meshes, text, error);
        if (!error.empty()) { std::cerr << argv[2] << ": " << error << '\n'; return 2; }
        std::cout << text;
        return 0;
    }
    if (std::string(argv[1]) == "--physics-joints" || std::string(argv[1]) == "--physics-collection") {
        // the unit's physics scene: u32 size, then the dependency and object collections (each starting "SEBD")
        // with a 128-byte header at the end giving their offsets and sizes
        if (argc != 3) { usage(); return 1; }
        std::ifstream file(argv[2], std::ios::binary);
        const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        const char magic[] = {'S', 'E', 'B', 'D'};
        const auto start = std::search(bytes.begin(), bytes.end(), std::begin(magic), std::end(magic));
        std::string error, text;
        const std::size_t at = static_cast<std::size_t>(start - bytes.begin());
        std::uint32_t size = 0;
        if (start == bytes.end() || at < 4) error = "the unit has no physics collections";
        else {
            std::memcpy(&size, bytes.data() + at - 4, 4);
            if (size < 128 || size > bytes.size() - at) error = "the physics scene is truncated";
        }
        if (error.empty()) {
            const auto* scene = bytes.data() + at;
            const auto word = [&](std::size_t offset) { std::uint32_t v; std::memcpy(&v, scene + size - 128 + offset, 4); return v; };
            const auto slice = [&](std::uint32_t offset, std::uint32_t length) {
                return offset <= size && length <= size - offset ? std::vector<std::uint8_t>(scene + offset, scene + offset + length)
                                                                 : std::vector<std::uint8_t>();
            };
            const auto dependencies = slice(word(8), word(12)), objects = slice(word(80), word(84));
            if (std::string(argv[1]) == "--physics-collection")
                dtglb::stingray::physics::describe_physics_collection(std::vector<std::uint8_t>(scene, scene + size), text, error);
            else if (dependencies.empty() || objects.empty()) error = "the physics scene header does not match";
            else dtglb::stingray::physics::describe_physics_joints(dependencies, objects, text, error);
        }
        if (!error.empty()) { std::cerr << argv[2] << ": " << error << '\n'; return 2; }
        std::cout << text;
        return 0;
    }
    if (std::string(argv[1]) == "--shader-reflect") {
        if (argc < 3) { usage(); return 1; }
        dtglb::stingray::material::RootTable roots;
        auto out = dtglb::json::Value::array();
        for (int i = 2; i < argc; ++i) {
            std::ifstream file(argv[i], std::ios::binary);
            const std::vector<std::uint8_t> dxbc((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            dtglb::stingray::material::ShaderVariant variant;
            std::string error;
            if (dxbc.empty()) error = "cannot read the file";
            else dtglb::stingray::material::make_shader_variant(dxbc, roots, variant, error);
            if (!error.empty()) { std::cerr << argv[i] << ": " << error << '\n'; return 2; }
            out.push(dtglb::stingray::material::describe_shader_variant(variant));
        }
        std::cout << dtglb::json::dump(out) << '\n';
        return 0;
    }
    if (std::string(argv[1]) == "--surface-shader") {
        // builds an own shader material stream from {code | code_file, parameters {name: [floats]}, textures [names],
        // runtime_textures [names], alpha_clip, alpha_cutoff, double_sided} and prints its shader section summary
        if (argc != 4) { usage(); return 1; }
        std::ifstream file(argv[2], std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        dtglb::json::Value spec_json;
        std::string error;
        dtglb::stingray::material::SurfaceShaderSpec spec;
        if (!file || !dtglb::json::parse(text, spec_json, error)) error = error.empty() ? "cannot read the file" : error;
        if (error.empty()) {
            if (const auto* code = spec_json.find("code")) spec.code = code->string;
            if (const auto* code_file = spec_json.find("code_file")) {
                std::ifstream source(fs::path(argv[2]).parent_path() / fs::u8path(code_file->string), std::ios::binary);
                spec.code.assign((std::istreambuf_iterator<char>(source)), std::istreambuf_iterator<char>());
                if (!source) error = "cannot read " + code_file->string;
            }
            if (const auto* parameters = spec_json.find("parameters"))
                for (const auto& [name, value] : parameters->members) {
                    auto& p = spec.parameters.emplace_back();
                    p.name = name;
                    for (const auto& item : value.items) p.value.push_back(static_cast<float>(item.number));
                }
            if (const auto* textures = spec_json.find("textures"))
                for (const auto& item : textures->items) spec.textures.push_back(item.string);
            if (const auto* runtime = spec_json.find("runtime_textures"))
                for (const auto& item : runtime->items) spec.runtime_textures.push_back(item.string);
            if (const auto* clip = spec_json.find("alpha_clip")) spec.alpha_clip = clip->boolean;
            if (const auto* cutoff = spec_json.find("alpha_cutoff")) spec.alpha_cutoff = static_cast<float>(cutoff->number);
            if (const auto* sides = spec_json.find("double_sided")) spec.double_sided = sides->boolean;
            if (const auto* decal = spec_json.find("decal")) spec.decal = decal->boolean;
        }
        dtglb::stingray::material::MaterialStream stream;
        const std::uint64_t own_name = dtglb::stingray::resource_name_hash("content/mods/surface_shader_test");
        if (error.empty()) dtglb::stingray::material::build_surface_shader(spec, own_name, stream, error);
        std::vector<std::uint8_t> bytes;
        if (error.empty()) bytes = dtglb::stingray::material::serialize_material_stream(stream, error);
        dtglb::stingray::material::ShaderSection section;
        if (error.empty()) dtglb::stingray::material::decode_shader_section(stream.shader_blob, section, error);
        if (!error.empty()) { std::cerr << argv[2] << ": " << error << '\n'; return 2; }
        std::ofstream output(fs::u8path(argv[3]), std::ios::binary);
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!output) { std::cerr << "cannot write " << argv[3] << '\n'; return 2; }
        std::cout << dtglb::json::dump(dtglb::stingray::material::describe_shader_section(section)) << '\n';
        return 0;
    }
    if (std::string(argv[1]) == "--shader-data-check") {
        // rebuilds every shader_data entry of each material stream from its passes' stages and lists what differs
        if (argc < 3) { usage(); return 1; }
        namespace m = dtglb::stingray::material;
        std::size_t shaders = 0, matched = 0;
        const auto hex = [](std::uint32_t v) { char b[11]; std::snprintf(b, sizeof b, "%08x", v); return std::string(b); };
        for (int a = 2; a < argc; ++a) {
            std::ifstream file(argv[a], std::ios::binary);
            const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            std::uint32_t header[7] = {};
            if (bytes.size() >= sizeof(header)) std::memcpy(header, bytes.data(), sizeof(header));
            if (bytes.size() < sizeof(header) || !header[4] || header[3] > bytes.size() || header[4] > bytes.size() - header[3]) continue;
            m::ShaderSection section;
            std::string error;
            if (!m::decode_shader_section(std::vector<std::uint8_t>(bytes.begin() + header[3], bytes.begin() + header[3] + header[4]), section, error)) {
                std::cout << argv[a] << " decode: " << error << '\n';
                continue;
            }
            for (std::size_t i = 0; i < section.shader_data.size(); ++i) {
                const auto& want = section.shader_data[i];
                if (section.device_data.empty() || i >= section.device_data[0].groups.size()) continue;
                std::vector<std::array<std::vector<std::uint8_t>, 6>> passes;
                for (const auto& pass : section.device_data[0].groups[i]) {
                    auto& stages = passes.emplace_back();
                    for (std::size_t k = 0; k < 6 && error.empty(); ++k) {
                        if (pass.stages[k].empty()) continue;
                        stages[k].resize(pass.stages[k].front().size);
                        dtglb::stingray::texture::oodle_decompress(pass.stages[k].front().bytecode, stages[k], error);
                    }
                }
                m::ShaderData got;
                if (error.empty()) m::make_shader_data(passes, got, error);
                ++shaders;
                std::string diff;
                if (!error.empty()) diff = " error " + error;
                else {
                    if (got.resources != want.resources) {
                        diff += " resources";
                        for (std::size_t r = 0; r < std::max(got.resources.size(), want.resources.size()); ++r) {
                            const auto show = [&](const auto& list) {
                                return r < list.size() ? hex(list[r][0]) + "/" + hex(list[r][1]) + "/" + std::to_string(list[r][2]) + "/" + std::to_string(list[r][3]) : std::string("-");
                            };
                            if (r >= got.resources.size() || r >= want.resources.size() || got.resources[r] != want.resources[r])
                                diff += " [" + std::to_string(r) + " want " + show(want.resources) + " got " + show(got.resources) + "]";
                        }
                    }
                    if (got.resource_size != want.resource_size)
                        diff += " size want " + std::to_string(want.resource_size) + " got " + std::to_string(got.resource_size);
                    if (got.constant_buffers.size() != want.constant_buffers.size()) diff += " cbuffer count";
                    else for (std::size_t c = 0; c < got.constant_buffers.size(); ++c) {
                        const auto& g = got.constant_buffers[c];
                        const auto& w = want.constant_buffers[c];
                        if (g.size != w.size || g.offset != w.offset)
                            diff += " cb" + std::to_string(c) + " size/offset want " + std::to_string(w.size) + "/" + std::to_string(w.offset) +
                                    " got " + std::to_string(g.size) + "/" + std::to_string(g.offset);
                        for (std::size_t v = 0; v < std::max(g.variables.size(), w.variables.size()); ++v) {
                            if (v < g.variables.size() && v < w.variables.size() && g.variables[v] == w.variables[v]) continue;
                            const auto show = [&](const auto& list) {
                                if (v >= list.size()) return std::string("-");
                                std::string s;
                                for (const auto x : list[v]) s += std::to_string(x) + "/";
                                return s;
                            };
                            diff += " cb" + std::to_string(c) + "v" + std::to_string(v) + " want " + show(w.variables) + " got " + show(g.variables);
                            break;
                        }
                    }
                    if (got.records != want.records) {
                        diff += " records";
                        for (std::size_t r = 0; r < std::max(got.records.size(), want.records.size()); ++r) {
                            const auto show = [&](const auto& list) {
                                if (r >= list.size()) return std::string("-");
                                std::string s;
                                for (const auto x : list[r]) s += std::to_string(x) + "/";
                                return s;
                            };
                            if (r >= got.records.size() || r >= want.records.size() || got.records[r] != want.records[r]) {
                                diff += " [" + std::to_string(r) + " want " + show(want.records) + " got " + show(got.records) + "]";
                                break;
                            }
                        }
                    }
                }
                if (diff.empty()) ++matched;
                else std::cout << argv[a] << " shader " << i << ":" << diff << '\n';
            }
        }
        std::cout << "shaders " << shaders << " matched " << matched << '\n';
        return 0;
    }
    if (std::string(argv[1]) == "--flow-build") {
        if (argc != 3) { usage(); return 1; }
        std::ifstream file(argv[2], std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        dtglb::json::Value authored, graph;
        std::string error;
        if (!file || !dtglb::json::parse(text, authored, error) || !dtglb::stingray::flow::build_graph(authored, graph, error)) {
            std::cerr << argv[2] << ": " << (error.empty() ? "cannot read the file" : error) << '\n';
            return 2;
        }
        std::cout << dtglb::json::dump(graph) << '\n';
        return 0;
    }
    if (std::string(argv[1]) == "--flow-schema") {
        if (argc != 2) { usage(); return 1; }
        std::cout << dtglb::json::dump(dtglb::stingray::flow::authoring_schema()) << '\n';
        return 0;
    }
    if (std::string(argv[1]) == "--flow-graph") {
        // a unit's, level's or standalone flow's graph with every node named: type, ports (slot, value), event links
        namespace flow = dtglb::stingray::flow;
        if (argc != 3 && argc != 4) { usage(); return 1; }
        std::unordered_map<std::uint32_t, std::string> names;  // with the game folder: hashes named from the game's Lua
        if (argc == 4) {
            std::string problem;
            if (!flow::lua_string_names(fs::u8path(argv[3]) / "bundle", names, problem)) { std::cerr << problem << '\n'; return 2; }
        }
        const auto named = [&](std::uint32_t id) {
            if (const char* known = flow::known_flow_name(id); *known) return std::string(known);
            if (const auto found = names.find(id); found != names.end()) return found->second;
            char text[12];
            std::snprintf(text, sizeof(text), "#%08x", id);
            return std::string(text);
        };
        std::ifstream file(fs::u8path(argv[2]), std::ios::binary);
        const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        const std::string ext = fs::u8path(argv[2]).extension().string();
        std::vector<std::uint8_t> blob, dynamic;
        std::string error;
        if (ext == ".unit") {
            dtglb::stingray::unit::Unit unit;
            if (!dtglb::stingray::unit::read_unit(bytes, unit, error)) { std::cerr << error << '\n'; return 2; }
            blob = unit.flow;
            dynamic = unit.flow_dynamic_data;
        } else if (ext == ".level") {
            dtglb::stingray::level::LevelDocument d;
            if (!dtglb::stingray::level::read_level_document(bytes, d, error)) { std::cerr << error << '\n'; return 2; }
            blob = d.flow;
            dynamic = d.flow_data;
        } else {
            std::vector<std::uint8_t> body;
            std::string stream;
            if (!dtglb::stingray::parse_cooked_resource_envelope(bytes, "flow", body, stream, error)) body = bytes;
            std::uint32_t size = 0;
            if (body.size() >= 0x5c) std::memcpy(&size, body.data() + 0x54, 4);
            if (size > body.size()) { std::cerr << "not a flow\n"; return 2; }
            blob.assign(body.begin(), body.begin() + size);
            dynamic.assign(body.begin() + size, body.end());
        }
        if (blob.empty()) { std::cout << "{\"nodes\": []}\n"; return 0; }
        flow::Graph graph;
        if (!flow::read_graph(blob, dynamic, graph, error)) { std::cerr << error << '\n'; return 2; }
        using dtglb::json::Value;
        const auto num = [](double v) { return Value::of(v); };
        auto nodes = Value::array();
        for (std::size_t i = 0; i < graph.nodes.size(); ++i) {
            const auto& node = graph.nodes[i];
            if (node.empty) continue;
            const flow::NodeType* type = flow::node_type(node.type);
            auto out = Value::object();
            out.set("node", num(static_cast<double>(i)));
            out.set("type", Value::of(type && !type->name.empty() ? type->name : std::to_string(node.type)));
            auto ports = Value::object();
            for (const auto& port : flow::node_ports(graph, node)) {
                if (port.direction == "static" || port.slot == flow::kNoSlot) continue;
                const auto value = port.direction == "in" ? flow::value_text(port.kind, graph.dynamic, port.slot) : std::string();
                ports.set(port.name, Value::of(port.direction + " " + port.kind + " @" + std::to_string(port.slot) +
                                               (value.empty() ? "" : " = " + value)));
            }
            out.set("ports", std::move(ports));
            auto events = Value::object();
            const bool nested = type && type->codec == flow::Codec::kNested;
            for (const auto& list : node.outputs) {
                if (list.links.empty()) continue;
                auto links = Value::array();
                for (const auto& link : list.links) links.push(Value::of(std::to_string(link.node) + "." + std::to_string(link.event)));
                events.set(nested ? named(list.id) : list.name, std::move(links));
            }
            if (!node.input_events.empty()) {
                auto ins = Value::array();
                for (const auto id : node.input_events) ins.push(Value::of(named(id)));
                out.set("in", std::move(ins));
            }
            out.set("out", std::move(events));
            if (!node.callbacks.empty()) out.set("lua", Value::of(node.callbacks.front().module + "." + node.callbacks.front().function));
            nodes.push(std::move(out));
        }
        auto root = Value::object();
        root.set("nodes", std::move(nodes));
        auto external = Value::array();
        for (const auto& e : graph.external_events)
            external.push(Value::of(named(e.id) + " -> " + std::to_string(e.node) + "." + std::to_string(e.in)));
        root.set("external_events", std::move(external));
        root.set("dynamic_size", num(graph.dynamic_size));
        std::cout << dtglb::json::dump(root) << '\n';
        return 0;
    }
    if (std::string(argv[1]) == "--flow-query-survey") {
        // per node type output: how many inputs reading it pull it with a query link and how many don't
        namespace flow = dtglb::stingray::flow;
        if (argc != 3) { usage(); return 1; }
        dtglb::stingray::bundle::GameBundles bundles;
        std::string error;
        if (!bundles.open(fs::u8path(argv[2]) / "bundle", error)) { std::cerr << error << '\n'; return 2; }
        std::map<std::string, std::array<std::size_t, 2>> counts;  // "type port" -> {pulled, read without pulling}
        const auto survey = [&](const std::vector<std::uint8_t>& blob, const std::vector<std::uint8_t>& dynamic) {
            flow::Graph graph;
            std::string problem;
            if (!flow::read_graph(blob, dynamic, graph, problem)) return;
            std::map<std::uint32_t, std::vector<std::pair<std::size_t, std::string>>> writers;
            for (std::size_t i = 0; i < graph.nodes.size(); ++i) {
                const auto& node = graph.nodes[i];
                if (node.empty) continue;
                const flow::NodeType* type = flow::node_type(node.type);
                for (const auto& port : flow::node_ports(graph, node))
                    if (port.direction == "out" && port.slot != flow::kNoSlot)
                        writers[port.slot].push_back({i, (type ? type->name : std::to_string(node.type)) + " " +
                                                             (port.name[0] == '#' ? std::string("#variable") : port.name)});
            }
            for (std::size_t i = 0; i < graph.nodes.size(); ++i) {
                const auto& node = graph.nodes[i];
                if (node.empty) continue;
                for (const auto& port : flow::node_ports(graph, node)) {
                    if (port.direction != "in" || port.slot == flow::kNoSlot) continue;
                    const auto w = writers.find(port.slot);
                    if (w == writers.end()) continue;
                    for (const auto& [from, key] : w->second) {
                        if (from == i) continue;
                        const bool pulled = std::any_of(node.query.begin(), node.query.end(), [&](const flow::Link& l) { return l.node == from; });
                        ++counts[key][pulled ? 0 : 1];
                    }
                }
            }
        };
        bool ok = bundles.for_each(dtglb::stingray::bundle::type_id("unit"), [&](std::uint64_t, const std::vector<std::uint8_t>& bytes) {
            dtglb::stingray::unit::Unit unit;
            std::string problem;
            if (dtglb::stingray::unit::read_unit(bytes, unit, problem) && !unit.flow.empty()) survey(unit.flow, unit.flow_dynamic_data);
        }, error);
        ok = ok && bundles.for_each(dtglb::stingray::bundle::type_id("level"), [&](std::uint64_t, const std::vector<std::uint8_t>& bytes) {
            dtglb::stingray::level::LevelDocument d;
            std::string problem;
            if (dtglb::stingray::level::read_level_document(bytes, d, problem) && !d.flow.empty()) survey(d.flow, d.flow_data);
        }, error);
        ok = ok && bundles.for_each(dtglb::stingray::bundle::type_id("flow"), [&](std::uint64_t, const std::vector<std::uint8_t>& bytes) {
            if (bytes.size() < 38 + 0x5c) return;
            const std::vector<std::uint8_t> body(bytes.begin() + 38, bytes.end());
            std::uint32_t size = 0;
            std::memcpy(&size, body.data() + 0x54, 4);
            if (size <= body.size()) survey({body.begin(), body.begin() + size}, {body.begin() + size, body.end()});
        }, error);
        if (!ok) { std::cerr << error << '\n'; return 2; }
        for (const auto& [key, c] : counts) std::cout << key << '\t' << c[0] << '\t' << c[1] << '\n';
        return 0;
    }
    if (std::string(argv[1]) == "--flow-graph-check") {
        // every flow the game ships (units, levels, standalone flows) read typed and written back
        namespace flow = dtglb::stingray::flow;
        if (argc != 3) { usage(); return 1; }
        dtglb::stingray::bundle::GameBundles bundles;
        std::string error;
        if (!bundles.open(fs::u8path(argv[2]) / "bundle", error)) { std::cerr << error << '\n'; return 2; }
        std::map<std::string, int> graphs, failures;
        std::size_t nodes = 0, links = 0, queries_derived = 0;
        std::vector<std::string> problems, query_problems;
        const auto check = [&](const std::string& kind, std::uint64_t name, const std::vector<std::uint8_t>& blob,
                               const std::vector<std::uint8_t>& dynamic) {
            ++graphs[kind];
            flow::Graph graph;
            std::vector<std::uint8_t> again;
            std::string problem;
            char id[20];
            std::snprintf(id, sizeof(id), "%016llx", static_cast<unsigned long long>(name));
            if (flow::read_graph(blob, dynamic, graph, problem) && flow::write_graph(graph, again, problem) && again == blob) {
                const auto derived = flow::derived_queries(graph);
                for (std::size_t i = 0; i < graph.nodes.size(); ++i) {
                    const auto& node = graph.nodes[i];
                    if (node.empty) continue;
                    ++nodes;
                    for (const auto& list : node.outputs) links += list.links.size();
                    const auto& d = derived[i];
                    const bool same = d.size() == node.query.size() &&
                                      std::equal(d.begin(), d.end(), node.query.begin(), [](const flow::Link& a, const flow::Link& b) {
                                          return a.node == b.node && a.event == b.event;
                                      });
                    if (same) ++queries_derived;
                    else if (query_problems.size() < 10)
                        query_problems.push_back(kind + " " + id + " node " + std::to_string(i) + ": query list " +
                                                 std::to_string(node.query.size()) + " links, derived " + std::to_string(d.size()));
                }
                return;
            }
            if (problem.empty()) {
                std::size_t at = 0;
                while (at < again.size() && at < blob.size() && again[at] == blob[at]) ++at;
                std::uint32_t count = 0, index = 0;
                std::memcpy(&count, blob.data() + 4, 4);
                std::memcpy(&index, blob.data() + 8, 4);
                std::uint32_t node = 0;
                for (std::uint32_t i = 0; i < count; ++i) {
                    std::uint32_t offset = 0;
                    std::memcpy(&offset, blob.data() + index + 4 * i, 4);
                    if (offset <= at) node = i;
                }
                std::uint32_t start = 0, type = 0;
                std::memcpy(&start, blob.data() + index + 4 * node, 4);
                if (start + 4 <= blob.size()) std::memcpy(&type, blob.data() + start, 4);
                problem = "differs at byte " + std::to_string(at) + " (node " + std::to_string(node) + ", type " +
                          std::to_string(type) + ", record byte " + std::to_string(at - start) + "), sizes " +
                          std::to_string(blob.size()) + " / " + std::to_string(again.size());
            }
            ++failures[kind];
            if (problems.size() < 40) problems.push_back(kind + " " + id + ": " + problem);
        };
        bool ok = bundles.for_each(dtglb::stingray::bundle::type_id("unit"), [&](std::uint64_t name, const std::vector<std::uint8_t>& bytes) {
            dtglb::stingray::unit::Unit unit;
            std::string problem;
            if (!dtglb::stingray::unit::read_unit(bytes, unit, problem)) return;
            if (!unit.flow.empty()) check("unit", name, unit.flow, unit.flow_dynamic_data);
        }, error);
        ok = ok && bundles.for_each(dtglb::stingray::bundle::type_id("level"), [&](std::uint64_t name, const std::vector<std::uint8_t>& bytes) {
            dtglb::stingray::level::LevelDocument d;
            std::string problem;
            if (!dtglb::stingray::level::read_level_document(bytes, d, problem)) { ++failures["level"]; problems.push_back("level read: " + problem); return; }
            if (!d.flow.empty()) check("level", name, d.flow, d.flow_data);
        }, error);
        ok = ok && bundles.for_each(dtglb::stingray::bundle::type_id("flow"), [&](std::uint64_t name, const std::vector<std::uint8_t>& bytes) {
            if (bytes.size() < 38 + 0x5c) { ++failures["flow"]; return; }
            const std::vector<std::uint8_t> body(bytes.begin() + 38, bytes.end());
            std::uint32_t size = 0;
            std::memcpy(&size, body.data() + 0x54, 4);
            if (size > body.size()) { ++failures["flow"]; return; }
            check("flow", name, std::vector<std::uint8_t>(body.begin(), body.begin() + size),
                  std::vector<std::uint8_t>(body.begin() + size, body.end()));
        }, error);
        if (!ok) { std::cerr << error << '\n'; return 2; }
        int failed = 0;
        for (const auto& [kind, count] : graphs) {
            std::cout << kind << ": " << count << " graphs, " << failures[kind] << " differ\n";
            failed += failures[kind];
        }
        std::cout << nodes << " nodes, " << links << " event links\n";
        std::cout << queries_derived << " of " << nodes << " node query lists derived from their inputs\n";
        for (const auto& problem : problems) std::cout << problem << '\n';
        for (const auto& problem : query_problems) std::cout << problem << '\n';
        return failed ? 3 : 0;
    }
    if (std::string(argv[1]) == "--flow") {
        if (argc < 3 || argc > 4) { usage(); return 1; }
        std::string error;
        if (!flow_description(fs::path(argv[2]), argc == 4 ? fs::path(argv[3]) : fs::path(), error)) {
            std::cerr << argv[2] << ": " << error << '\n';
            return 2;
        }
        return 0;
    }
    if (std::string(argv[1]) == "--particles") {
        if (argc < 3 || argc > 4) { usage(); return 1; }
        std::string error;
        if (!particles_description(fs::path(argv[2]), argc == 4 ? fs::path(argv[3]) : fs::path(), error)) {
            std::cerr << argv[2] << ": " << error << '\n';
            return 2;
        }
        return 0;
    }

    dtglb::app::CompileOptions opt;
    opt.input = fs::path(argv[1]);
    opt.output_dir = opt.input.parent_path() / (opt.input.stem().string() + "_compiled");
    bool inspect_mode = false;
    for (int i = 2; i < argc; ++i) if (std::string(argv[i]) == "--inspect") inspect_mode = true;
    if (inspect_mode) {
        for (int i = 2; i < argc; ++i) {
            const std::string option = argv[i];
            if (option == "--inspect") continue;
            if (option == "--scene") {
                if (i + 1 < argc) ++i;
                continue;
            }
            std::cerr << "--inspect supports only --scene; compile option cannot be used with inspection: "
                      << option << '\n';
            return 1;
        }
    }
    bool physics_shape_set = false;
    bool physics_modifier_set = false;
    bool no_physics_requested = false;
    bool physics_actor_set = false;
    bool physics_mass_set = false;
    bool physics_material_set = false;
    const auto fitted_physics = [&]() -> dtglb::stingray::physics::FittedActorOptions& {
        opt.physics = true;
        if (!opt.fitted_physics) opt.fitted_physics.emplace();
        return *opt.fitted_physics;
    };
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--inspect") { /* handled above */ }
        else if ((a == "-o" || a == "--output") && i + 1 < argc) opt.output_dir = fs::path(argv[++i]);
        else if (a == "--material" && i + 1 < argc) opt.material_override = argv[++i];
        else if (a == "--material-variant") {
            if (i + 1 >= argc) { std::cerr << "Missing material variant name\n"; return 1; }
            opt.material_variant = argv[++i];
            if (opt.material_variant->empty()) { std::cerr << "Material variant name must not be empty\n"; return 1; }
        }
        else if (a == "--settings" && i + 1 < argc) opt.settings_path = fs::path(argv[++i]);
        else if (a == "--asset-path") {
            if (i + 1 >= argc) { std::cerr << "Missing asset path\n"; return 1; }
            opt.asset_path = argv[++i];
        }
        else if (a == "--reference-bones") {
            if (i + 1 >= argc) { std::cerr << "Missing reference BONES file\n"; return 1; }
            opt.reference_bones_file = fs::path(argv[++i]);
        }
        else if (a == "--material-layer") {
            if (i + 2 >= argc) { std::cerr << "--material-layer needs a layer name and a material resource\n"; return 1; }
            const std::string layer = argv[++i];
            opt.material_layers.emplace_back(layer, argv[++i]);
        }
        else if (a == "--skeleton-resource") {
            if (i + 1 >= argc) { std::cerr << "Missing skeleton resource identity\n"; return 1; }
            opt.skeleton_resource = argv[++i];
        }
        else if (a == "--output-kind" && i + 1 < argc) {
            const std::string kind = argv[++i];
            if (kind == "all") opt.output_kind = dtglb::app::OutputKind::All;
            else if (kind == "model") opt.output_kind = dtglb::app::OutputKind::Model;
            else if (kind == "animations") opt.output_kind = dtglb::app::OutputKind::Animations;
            else { std::cerr << "Invalid output kind: " << kind << "\n"; return 1; }
        }
        else if (a == "--no-materials") opt.auto_materials = false;
        else if (a == "--no-images") { /* legacy compatibility; images are never emitted */ }
        else if (a == "--no-physics") { no_physics_requested = true; }
        else if (a == "--no-validate") opt.validate = false;
        else if (a == "--manifest") opt.write_manifest = true;
        else if (a == "--allow-fixed-morph-bake") opt.allow_fixed_morph_bake = true;
        else if (a == "--allow-animation-resampling") opt.allow_animation_resampling = true;
        else if (a == "--allow-native-animation-interpolation") opt.allow_native_animation_interpolation = true;
        else if (a == "--scale") {
            if (i + 1 >= argc) { std::cerr << "Missing scale factor\n"; return 1; }
            float value = 0.0f;
            if (!parse_finite_float(argv[++i], value, false)) { std::cerr << "Invalid scale factor: " << argv[i] << "\n"; return 1; }
            opt.asset_scale = value;
        }
        else if (a == "--animation-translation-tolerance" || a == "--animation-scale-tolerance" ||
                 a == "--animation-rotation-tolerance-radians") {
            if (i + 1 >= argc) { std::cerr << "Missing value for " << a << "\n"; return 1; }
            double value = 0.0;
            if (!parse_positive_double(argv[++i], value)) { std::cerr << "Invalid positive finite value for " << a << ": " << argv[i] << "\n"; return 1; }
            if (a == "--animation-translation-tolerance") opt.animation_fit_overrides.translation_tolerance = value;
            else if (a == "--animation-scale-tolerance") opt.animation_fit_overrides.scale_tolerance = value;
            else opt.animation_fit_overrides.rotation_tolerance_radians = value;
        }
        else if (a == "--animation-max-controls" || a == "--animation-max-refinements" ||
                 a == "--animation-max-evaluations") {
            if (i + 1 >= argc) { std::cerr << "Missing count for " << a << "\n"; return 1; }
            int value = 0;
            if (!parse_positive_int(argv[++i], value)) { std::cerr << "Invalid positive integer for " << a << ": " << argv[i] << "\n"; return 1; }
            if (a == "--animation-max-controls") opt.animation_fit_overrides.max_controls = static_cast<std::size_t>(value);
            else if (a == "--animation-max-refinements") opt.animation_fit_overrides.max_refinements = static_cast<std::size_t>(value);
            else opt.animation_fit_overrides.max_evaluations = static_cast<std::size_t>(value);
        }
        else if (a == "--physics-shape") {
            if (i + 1 >= argc) { std::cerr << "Missing physics shape\n"; return 1; }
            const std::string shape = argv[++i];
            if (shape == "geometry" || shape == "mesh") fitted_physics().shape = dtglb::stingray::physics::PhysicsShapeType::TriangleMesh;
            else if (shape == "convex") fitted_physics().shape = dtglb::stingray::physics::PhysicsShapeType::Convex;
            else if (shape == "box") fitted_physics().shape = dtglb::stingray::physics::PhysicsShapeType::Box;
            else if (shape == "sphere") fitted_physics().shape = dtglb::stingray::physics::PhysicsShapeType::Sphere;
            else if (shape == "capsule") fitted_physics().shape = dtglb::stingray::physics::PhysicsShapeType::Capsule;
            else { std::cerr << "Invalid physics shape: " << shape << "\n"; return 1; }
            physics_shape_set = true;
            physics_modifier_set = true;
        }
        else if (a == "--physics-actor") {
            if (i + 1 >= argc) { std::cerr << "Missing physics actor type\n"; return 1; }
            const std::string actor = argv[++i];
            if (actor != "static" && actor != "dynamic" && actor != "keyframed") {
                std::cerr << "Invalid physics actor type: " << actor << "\n"; return 1;
            }
            fitted_physics().actor_template = actor;
            physics_actor_set = true;
            physics_modifier_set = true;
        }
        else if (a == "--physics-mass") {
            if (i + 1 >= argc) { std::cerr << "Missing physics mass\n"; return 1; }
            float value = 0.0f;
            if (!parse_finite_float(argv[++i], value, true)) { std::cerr << "Invalid physics mass: " << argv[i] << "\n"; return 1; }
            fitted_physics().mass = value;
            physics_mass_set = true;
            physics_modifier_set = true;
        }
        else if (a == "--physics-material") {
            if (i + 1 >= argc) { std::cerr << "Missing physics material\n"; return 1; }
            const std::string material = argv[++i];
            if (material != "default" && material != "iron" && material != "rubber") {
                std::cerr << "Invalid physics material: " << material << "\n"; return 1;
            }
            fitted_physics().material = material;
            physics_material_set = true;
            physics_modifier_set = true;
        }
        else if (a == "--scene") {
            if (i + 1 >= argc) { std::cerr << "Missing scene index\n"; return 1; }
            int index = 0;
            if (!parse_scene_index(argv[++i], index)) { std::cerr << "Invalid scene index: " << argv[i] << "\n"; return 1; }
            opt.scene_index = index;
        }
        else if (a == "--clip") {
            if (i + 1 >= argc) { std::cerr << "Missing clip index\n"; return 1; }
            int index = 0;
            if (!parse_scene_index(argv[++i], index)) { std::cerr << "Invalid clip index: " << argv[i] << "\n"; return 1; }
            opt.animation_index = index;
        }
        else if (a == "--loop-clip") {
            if (i + 1 >= argc) { std::cerr << "Missing loop clip index\n"; return 1; }
            int index = 0;
            if (!parse_scene_index(argv[++i], index)) { std::cerr << "Invalid loop clip index: " << argv[i] << "\n"; return 1; }
            opt.loop_clip_index = index;
        }
        else if (a == "--simple-clip") {
            if (i + 1 >= argc) { std::cerr << "Missing simple-animation clip index\n"; return 1; }
            int index = 0;
            if (!parse_scene_index(argv[++i], index)) { std::cerr << "Invalid simple-animation clip index: " << argv[i] << "\n"; return 1; }
            opt.simple_clip_index = index;
        }
        else if (a == "--no-simple-animation") { opt.simple_animation = false; }
        else if (a == "--in-place") { opt.in_place = true; }
        else if (a == "--root-motion") { opt.root_motion = true; }
        else if (a == "--package-name") {
            if (i + 1 >= argc || !*argv[i + 1]) { std::cerr << "--package-name requires a resource name\n"; return 1; }
            opt.package_name = argv[++i];
        }
        else if (a == "--ragdoll-event") {
            if (i + 1 >= argc || !*argv[i + 1]) { std::cerr << "--ragdoll-event requires an event name\n"; return 1; }
            opt.ragdoll_event = argv[++i];
        }
        else if (a == "--once-clip") {
            if (i + 1 >= argc) { std::cerr << "Missing one-shot clip index\n"; return 1; }
            int index = 0;
            if (!parse_scene_index(argv[++i], index)) { std::cerr << "Invalid one-shot clip index: " << argv[i] << "\n"; return 1; }
            opt.once_clip_index = index;
        }
        else if (a == "--sm-state") {
            if (i + 3 >= argc) { std::cerr << "--sm-state requires NAME CLIP_INDEX loop|once\n"; return 1; }
            dtglb::app::StateMachineStateOption state;
            state.name = argv[++i];
            if (!parse_scene_index(argv[++i], state.clip_index)) {
                std::cerr << "Invalid --sm-state clip index: " << argv[i] << "\n"; return 1;
            }
            const std::string mode = argv[++i];
            if (mode != "loop" && mode != "once") {
                std::cerr << "Invalid --sm-state mode: " << mode << " (expected loop or once)\n"; return 1;
            }
            state.looping = mode == "loop";
            opt.state_machine_states.push_back(std::move(state));
        }
        else if (a == "--sm-blend") {
            if (i + 4 >= argc) { std::cerr << "--sm-blend requires NAME VARIABLE_INDEX[,VARIABLE_INDEX] loop|once CLIP:VALUE[:VALUE2][,...]\n"; return 1; }
            dtglb::app::StateMachineStateOption state;
            state.name = argv[++i];
            // one variable index, or two (X,Y) for a 2D blend whose clips are CLIP:X:Y
            const std::string variables = argv[++i];
            const auto comma = variables.find(',');
            if (!parse_scene_index(variables.substr(0, comma), state.blend_variable) ||
                (comma != std::string::npos && !parse_scene_index(variables.substr(comma + 1), state.blend_variable2))) {
                std::cerr << "Invalid --sm-blend variable index: " << variables << "\n"; return 1;
            }
            const std::string mode = argv[++i];
            if (mode != "loop" && mode != "once") {
                std::cerr << "Invalid --sm-blend mode: " << mode << " (expected loop or once)\n"; return 1;
            }
            state.looping = mode == "loop";
            std::stringstream list(argv[++i]);
            std::string entry;
            while (std::getline(list, entry, ',')) {
                const auto colon = entry.find(':');
                const auto second = colon == std::string::npos ? colon : entry.find(':', colon + 1);
                const bool two_d = state.blend_variable2 >= 0;
                int clip = -1;
                float value = 0.0f, value2 = 0.0f;
                if (colon == std::string::npos || (second != std::string::npos) != two_d ||
                    !parse_scene_index(entry.substr(0, colon), clip) ||
                    !parse_signed_finite_float(entry.substr(colon + 1, second == std::string::npos ? std::string::npos : second - colon - 1), value) ||
                    (two_d && !parse_signed_finite_float(entry.substr(second + 1), value2))) {
                    std::cerr << "Invalid --sm-blend clip entry: " << entry << (two_d ? " (expected CLIP:X:Y)\n" : " (expected CLIP:VALUE)\n"); return 1;
                }
                state.blend.push_back({clip, value});
                state.blend_value2.push_back(value2);
            }
            if (state.blend.empty()) { std::cerr << "--sm-blend needs at least one CLIP:VALUE\n"; return 1; }
            opt.state_machine_states.push_back(std::move(state));
        }
        else if (a == "--sm-random") {
            if (i + 4 >= argc) { std::cerr << "--sm-random requires NAME on_entry|every_loop|no_repeat loop|once CLIP:WEIGHT[,CLIP:WEIGHT...]\n"; return 1; }
            dtglb::app::StateMachineStateOption state;
            state.name = argv[++i];
            const std::string pick = argv[++i];
            if (pick == "on_entry") state.randomization = 0;
            else if (pick == "every_loop") state.randomization = 1;
            else if (pick == "no_repeat") state.randomization = 2;
            else { std::cerr << "Invalid --sm-random pick mode: " << pick << " (on_entry, every_loop or no_repeat)\n"; return 1; }
            const std::string mode = argv[++i];
            if (mode != "loop" && mode != "once") { std::cerr << "Invalid --sm-random mode: " << mode << " (expected loop or once)\n"; return 1; }
            state.looping = mode == "loop";
            state.random = true;
            std::stringstream list(argv[++i]);
            std::string entry;
            while (std::getline(list, entry, ',')) {
                const auto colon = entry.find(':');
                int clip = -1;
                float weight = 1.0f;
                if (!parse_scene_index(entry.substr(0, colon), clip) ||
                    (colon != std::string::npos && !parse_signed_finite_float(entry.substr(colon + 1), weight)) || !(weight > 0.0f)) {
                    std::cerr << "Invalid --sm-random clip entry: " << entry << " (expected CLIP or CLIP:WEIGHT, weight above 0)\n"; return 1;
                }
                state.blend.push_back({clip, weight});
            }
            if (state.blend.size() < 2) { std::cerr << "--sm-random needs at least two clips\n"; return 1; }
            opt.state_machine_states.push_back(std::move(state));
        }
        else if (a == "--sm-speed") {
            if (i + 2 >= argc) { std::cerr << "--sm-speed requires STATE_NAME SPEED|var:VARIABLE_INDEX\n"; return 1; }
            const std::string name = argv[++i];
            const std::string value = argv[++i];
            const auto state = std::find_if(opt.state_machine_states.begin(), opt.state_machine_states.end(),
                [&](const auto& s) { return s.name == name; });
            if (state == opt.state_machine_states.end()) { std::cerr << "--sm-speed names no earlier --sm-state or --sm-blend: " << name << "\n"; return 1; }
            if (value.rfind("var:", 0) == 0) {
                if (!parse_scene_index(value.substr(4), state->speed_variable)) { std::cerr << "Invalid --sm-speed variable index: " << value << "\n"; return 1; }
            } else if (!parse_signed_finite_float(value, state->speed)) {
                std::cerr << "Invalid --sm-speed value: " << value << "\n"; return 1;
            }
        }
        else if (a == "--sm-empty") {
            if (i + 2 >= argc) { std::cerr << "--sm-empty requires NAME LAYER\n"; return 1; }
            dtglb::app::StateMachineStateOption state;
            state.name = argv[++i];
            state.empty = true;
            if (!parse_scene_index(argv[++i], state.layer)) { std::cerr << "Invalid --sm-empty layer: " << argv[i] << "\n"; return 1; }
            opt.state_machine_states.push_back(std::move(state));
        }
        else if (a == "--sm-events" || a == "--sm-exit") {
            const bool exit = a == "--sm-exit";
            if (i + (exit ? 3 : 2) >= argc) {
                std::cerr << a << (exit ? " requires STATE_NAME EVENT SECONDS_LEFT\n" : " requires STATE_NAME SECONDS:EVENT[,SECONDS:EVENT...]\n");
                return 1;
            }
            const std::string name = argv[++i];
            const auto state = std::find_if(opt.state_machine_states.begin(), opt.state_machine_states.end(),
                [&](const auto& s) { return s.name == name; });
            if (state == opt.state_machine_states.end()) { std::cerr << a << " names no earlier state: " << name << "\n"; return 1; }
            if (exit) {
                state->exit_event = argv[++i];
                if (state->exit_event.empty() || !parse_signed_finite_float(argv[++i], state->exit_blend) || state->exit_blend < 0.0f) {
                    std::cerr << "Invalid --sm-exit event or time\n"; return 1;
                }
                continue;
            }
            std::stringstream list(argv[++i]);
            std::string entry;
            while (std::getline(list, entry, ',')) {
                const auto colon = entry.find(':');
                float time = 0.0f;
                if (colon == std::string::npos || colon + 1 == entry.size() ||
                    !parse_signed_finite_float(entry.substr(0, colon), time) || time < 0.0f) {
                    std::cerr << "Invalid --sm-events entry: " << entry << " (expected SECONDS:EVENT)\n"; return 1;
                }
                state->events_at.push_back({time, entry.substr(colon + 1)});
            }
        }
        else if (a == "--sm-layer" || a == "--sm-mask" || a == "--sm-additive") {
            const bool takes_value = a != "--sm-additive";
            if (i + (takes_value ? 2 : 1) >= argc) {
                std::cerr << a << (a == "--sm-layer" ? " requires STATE_NAME LAYER\n" : a == "--sm-mask" ? " requires STATE_NAME BONE[:WEIGHT][,BONE[:WEIGHT]...]\n" : " requires STATE_NAME\n");
                return 1;
            }
            const std::string name = argv[++i];
            const auto state = std::find_if(opt.state_machine_states.begin(), opt.state_machine_states.end(),
                [&](const auto& s) { return s.name == name; });
            if (state == opt.state_machine_states.end()) { std::cerr << a << " names no earlier state: " << name << "\n"; return 1; }
            if (a == "--sm-additive") { state->additive = true; continue; }
            const std::string value = argv[++i];
            if (a == "--sm-layer") {
                if (!parse_scene_index(value, state->layer)) { std::cerr << "Invalid --sm-layer layer: " << value << "\n"; return 1; }
                continue;
            }
            std::stringstream list(value);
            std::string entry;
            while (std::getline(list, entry, ',')) {
                const auto colon = entry.find(':');
                float weight = 1.0f;
                const auto bone = entry.substr(0, colon);
                if (bone.empty() || (colon != std::string::npos && !parse_signed_finite_float(entry.substr(colon + 1), weight)) ||
                    weight < 0.0f || weight > 1.0f) {
                    std::cerr << "Invalid --sm-mask entry: " << entry << " (expected BONE or BONE:WEIGHT, weight 0 to 1)\n"; return 1;
                }
                state->mask.push_back({bone, weight});
            }
            if (state->mask.empty()) { std::cerr << "--sm-mask needs at least one bone\n"; return 1; }
        }
        else if (a == "--sm-transition") {
            if (i + 4 >= argc) { std::cerr << "--sm-transition requires FROM_INDEX TO_INDEX EVENT_NAME BLEND_SECONDS\n"; return 1; }
            dtglb::app::StateMachineTransitionOption transition;
            int from = 0, to = 0;
            if (!parse_scene_index(argv[++i], from)) {
                std::cerr << "Invalid --sm-transition source state index: " << argv[i] << "\n"; return 1;
            }
            if (!parse_scene_index(argv[++i], to)) {
                std::cerr << "Invalid --sm-transition target state index: " << argv[i] << "\n"; return 1;
            }
            transition.from_state = static_cast<std::size_t>(from);
            transition.to_state = static_cast<std::size_t>(to);
            transition.event_name = argv[++i];
            if (!parse_finite_float(argv[++i], transition.blend_seconds, true)) {
                std::cerr << "Invalid --sm-transition blend seconds: " << argv[i] << "\n"; return 1;
            }
            opt.state_machine_transitions.push_back(std::move(transition));
        }
        else if (a == "--sm-declare-events") {
            if (i + 1 >= argc) { std::cerr << "--sm-declare-events requires a comma-separated list\n"; return 1; }
            const std::string list = argv[++i];
            std::size_t start = 0;
            while (start <= list.size()) {
                const auto comma = list.find(',', start);
                const auto name = list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
                if (!name.empty()) opt.state_machine_declared_events.push_back(name);
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
        }
        else if (a == "--sm-variable") {
            if (i + 4 >= argc) { std::cerr << "--sm-variable requires NAME INITIAL MIN MAX\n"; return 1; }
            dtglb::app::StateMachineVariableOption variable;
            variable.name = argv[++i];
            if (!parse_signed_finite_float(argv[++i], variable.initial_value) ||
                !parse_signed_finite_float(argv[++i], variable.minimum) ||
                !parse_signed_finite_float(argv[++i], variable.maximum)) {
                std::cerr << "Invalid --sm-variable value or bound\n"; return 1;
            }
            opt.state_machine_variables.push_back(std::move(variable));
        }
        else if (a == "--sm-range-transition") {
            if (i + 9 >= argc) {
                std::cerr << "--sm-range-transition requires FROM TO EVENT BLEND VARIABLE LOWER UPPER LOWER_EDGE UPPER_EDGE\n";
                return 1;
            }
            dtglb::app::StateMachineTransitionOption transition;
            int from = 0, to = 0, variable = 0;
            if (!parse_scene_index(argv[++i], from) || !parse_scene_index(argv[++i], to)) {
                std::cerr << "Invalid --sm-range-transition state index\n"; return 1;
            }
            transition.from_state = static_cast<std::size_t>(from);
            transition.to_state = static_cast<std::size_t>(to);
            transition.event_name = argv[++i];
            if (!parse_finite_float(argv[++i], transition.blend_seconds, true) ||
                !parse_scene_index(argv[++i], variable) ||
                !parse_signed_finite_float(argv[++i], transition.lower) ||
                !parse_signed_finite_float(argv[++i], transition.upper)) {
                std::cerr << "Invalid --sm-range-transition blend, variable, or bounds\n"; return 1;
            }
            transition.variable_index = static_cast<std::size_t>(variable);
            const std::string lower_edge = argv[++i], upper_edge = argv[++i];
            if ((lower_edge != "inclusive" && lower_edge != "exclusive") ||
                (upper_edge != "inclusive" && upper_edge != "exclusive")) {
                std::cerr << "Range edges must be inclusive or exclusive\n"; return 1;
            }
            transition.lower_exclusive = lower_edge == "exclusive";
            transition.upper_exclusive = upper_edge == "exclusive";
            opt.state_machine_transitions.push_back(std::move(transition));
        }
        else { std::cerr << "Unknown option: " << a << "\n"; return 1; }
    }
    if (opt.loop_clip_index && opt.once_clip_index) {
        std::cerr << "--loop-clip and --once-clip are mutually exclusive\n";
        return 1;
    }
    if ((!opt.state_machine_states.empty() || !opt.state_machine_variables.empty() ||
         !opt.state_machine_transitions.empty()) &&
        (opt.loop_clip_index || opt.once_clip_index)) {
        std::cerr << "--sm-state/--sm-transition cannot be combined with --loop-clip or --once-clip\n";
        return 1;
    }
    if (inspect_mode) return dtglb::app::inspect_source(opt.input, opt.scene_index);
    if (no_physics_requested) {
        opt.physics = false;
        opt.fitted_physics.reset();
    } else if (physics_modifier_set) {
        // CLI modifiers are an overlay on a settings-file actor. Keep the
        // explicit shape requirement for CLI-only invocations.
        bool settings_shape_available = false;
        if (!opt.settings_path.empty()) {
            dtglb::app::AssetSettings settings;
            std::string error;
            if (!dtglb::app::load_asset_settings(opt.settings_path, settings, error)) {
                std::cerr << "settings failed: " << error << "\n";
                return 2;
            }
            if (settings.fitted_physics) {
                settings_shape_available = true;
                auto merged = *settings.fitted_physics;
                if (physics_shape_set) merged.shape = opt.fitted_physics->shape;
                if (physics_actor_set) merged.actor_template = opt.fitted_physics->actor_template;
                if (physics_mass_set) merged.mass = opt.fitted_physics->mass;
                if (physics_material_set) merged.material = opt.fitted_physics->material;
                opt.fitted_physics = merged;
            }
        }
        if (!physics_shape_set && !settings_shape_available) {
            std::cerr << "A physics shape must be provided by --physics-shape or the settings file when using physics actor, mass, or material controls\n";
            return 1;
        }
    }
    if (opt.reference_bones_file.has_value() != opt.skeleton_resource.has_value()) {
        std::cerr << "--reference-bones and --skeleton-resource must be supplied together\n";
        return 1;
    }
    return dtglb::app::compile(opt);
}
