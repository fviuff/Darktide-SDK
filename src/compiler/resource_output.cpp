#include "stingray/resource_name.h"
#include "compiler/resource_output.h"
#include "gltf/json_value.h"
#include "stingray/cooked_resource.h"
#include "stingray/murmur_hash.h"
#include "validation/unit_validator.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <set>
#include <string_view>
#include <utility>

namespace dtglb::compiler {
namespace {
std::filesystem::path artifact_path(const std::filesystem::path& directory, const std::string& relative) {
    auto path = directory / relative;
#ifdef _WIN32
    path.make_preferred();
#endif
    return path;
}

bool save(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes, std::string& error) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) { error = "cannot create payload directory: " + ec.message(); return false; }
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file) { error = "cannot write resource payload: " + path.string(); return false; }
    return true;
}

bool artifact_size(const std::filesystem::path& path, std::uint64_t& size, std::string& error) {
    std::error_code ec;
    const auto value = std::filesystem::file_size(path, ec);
    if (ec) { error = "cannot inspect resource artifact: " + path.string() + ": " + ec.message(); return false; }
    size = static_cast<std::uint64_t>(value);
    return true;
}
std::string quote(std::string_view value) {
    std::ostringstream out; out << '"';
    for (const unsigned char c : value) {
        if (c == '"' || c == '\\') out << '\\' << c;
        else if (c < 32) out << "\\u" << std::hex << std::setfill('0') << std::setw(4) << static_cast<unsigned>(c);
        else out << c;
    }
    out << '"'; return out.str();
}
std::string key_json(const ResourceKey& key) {
    return "{\"type\":" + quote(key.type) + ",\"name\":" + quote(key.name) + "}";
}
const ResourceArtifact* find_artifact(const SerializedResources& resources,
                                      const ResourceKey& resource, ArtifactRole role) {
    const auto found = std::find_if(resources.artifacts.begin(), resources.artifacts.end(),
        [&](const ResourceArtifact& artifact) {
            return artifact.logical_resource == resource && artifact.role == role;
        });
    return found == resources.artifacts.end() ? nullptr : &*found;
}
}

bool read_build_artifact_paths(const std::filesystem::path& directory,
                               std::vector<std::string>& paths, std::string& error) {
    std::ifstream input(directory / "build.json", std::ios::binary);
    if (!input) { error = "cannot open prior build manifest"; return false; }
    const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (input.bad()) { error = "cannot read prior build manifest"; return false; }
    json::Value build;
    if (!json::parse(text, build, error)) return false;
    paths.clear();
    const auto add = [&](const json::Value* file) {
        const auto* path = file ? file->find("path") : nullptr;
        if (path && path->is_string()) paths.push_back(path->string);
    };
    // the files of its resources (and, from builds that listed them separately, its artifacts)
    if (const auto* list = build.find("resources"); list && list->is_array())
        for (const auto& r : list->items) {
            add(r.find("file"));
            if (const auto* stream = r.find("stream"); stream && stream->is_object()) add(stream->find("file"));
        }
    if (const auto* list = build.find("artifacts"); list && list->is_array())
        for (const auto& a : list->items) add(&a);
    std::set<std::string> unique_paths;
    paths.erase(std::remove_if(paths.begin(), paths.end(), [&](const std::string& path) {
        return !unique_paths.insert(path).second;
    }), paths.end());
    return true;
}

bool serialize_graph(const ResourceGraph& graph, const std::filesystem::path& directory,
                     SerializedResources& out, std::string& error, const TargetProfile& target_profile) {
    out = {};
    if (!validate_graph(graph, out.closure, error, target_profile)) return false;
    std::set<std::string> artifact_paths;
    for (const auto index : out.closure.owned) {
        const auto& node = graph.owned[index];
        const auto stream = resource_stream_name(node, target_profile);
        if (!artifact_paths.insert(node.file).second || (!stream.empty() && !artifact_paths.insert(stream).second)) {
            error = "resource artifact path collision";
            return false;
        }
    }
    for (const auto index : out.closure.owned) {
        const auto& node = graph.owned[index];
        const auto path = artifact_path(directory, node.file);
        const auto stream = resource_stream_name(node, target_profile);
        const auto stream_path = artifact_path(directory, stream);
        bool ok = false;
        if (const auto* unit = std::get_if<stingray::unit::UnitResource>(&node.resource)) {
            ok = save(path, stingray::wrap_cooked_resource("unit", node.key.name, unit->body), error);
            std::string report;
            std::string state_machine_resource;
            for (const auto& dependency : node.dependencies)
                if (dependency.type == "state_machine") state_machine_resource = dependency.name;
            if (ok && !validation::validate_unit_v115(path, report, state_machine_resource)) { error = report; ok = false; }
        } else if (const auto* state_machine = std::get_if<std::vector<std::uint8_t>>(&node.resource)) {
            ok = save(path, *state_machine, error);
            if (ok) {
                std::vector<std::string> animations;
                for (const auto& dependency : node.dependencies)
                    if (dependency.type == "animation") animations.push_back(dependency.name);
                std::string validation_error;
                bool valid = animations.size() == 1 &&
                    stingray::validate_minimal_single_clip_state_machine(
                        *state_machine, node.key.name, animations.front(), validation_error);
                if (!valid) valid = stingray::validate_direct_event_state_machine_dependencies(
                    *state_machine, node.key.name, animations, validation_error);
                if (!valid) {
                    error = validation_error;
                    ok = false;
                }
            }
        } else if (const auto* bones = std::get_if<stingray::bones::BonesResource>(&node.resource)) {
            ok = stingray::bones::write_cooked_bones(*bones, path, error);
        } else if (const auto* animation = std::get_if<stingray::animation::BuiltAnimation>(&node.resource)) {
            std::vector<std::uint8_t> body; std::string stream_name;
            ok = stingray::parse_cooked_resource_envelope(animation->cooked, "animation", body, stream_name, error);
            std::uint64_t name_hash = 0; std::uint32_t bone_count = 0;
            if (ok && body.size() >= 12) {
                std::memcpy(&name_hash, animation->cooked.data() + 8, 8);
                std::memcpy(&bone_count, body.data() + 8, 4);
                ok = name_hash == stingray::resource_name_hash(node.key.name) && stingray::animation::validate_skeletal_animation(animation->cooked, bone_count, error);
            } else ok = false;
            if (!ok && error.empty()) error = "ANIMATION body or identity mismatch";
            if (ok) ok = stingray::animation::write_skeletal_animation(*animation, path, error);
        } else if (const auto* material = std::get_if<stingray::material::MaterialStream>(&node.resource)) {
            ok = stingray::material::write_material_pair(node.key.name, *material, path, stream_path, stream, error);
        } else if (const auto* preserved = std::get_if<stingray::material::PreservedMaterialStream>(&node.resource)) {
            ok = stingray::material::write_preserved_material_pair(node.key.name, *preserved, path, stream_path, stream, error);
        } else if (const auto* texture = std::get_if<PreservedTexture>(&node.resource)) {
            ok = save(path, stingray::wrap_cooked_resource("texture", node.key.name, texture->body, stream), error) &&
                 (stream.empty() || save(stream_path, texture->stream, error));
        } else if (const auto* unit = std::get_if<PreservedUnit>(&node.resource)) {
            ok = save(path, stingray::wrap_cooked_resource("unit", node.key.name, unit->body), error);
        } else if (const auto* ies = std::get_if<IesResource>(&node.resource)) {
            ok = save(path, stingray::wrap_cooked_resource("ies", node.key.name, ies->body), error);
        } else if (const auto* particles = std::get_if<ParticlesResource>(&node.resource)) {
            ok = save(path, stingray::wrap_cooked_resource("particles", node.key.name, particles->body), error);
        } else if (const auto* texture = std::get_if<TextureResource>(&node.resource)) {
            stingray::texture::CookedTexture cooked;
            ok = stingray::texture::write_native_texture_rgba(texture->image, texture->profile, node.key.name,
                path, stream_path, cooked, error);
            if (ok && cooked.dimensions_normalized) {
                out.texture_normalizations.push_back({node.key, cooked.profile, cooked.source_width,
                    cooked.source_height, cooked.width, cooked.height});
            }
        }
        if (!ok) return false;
        out.files.push_back(node.file);
        std::uint64_t primary_size = 0;
        if (!artifact_size(path, primary_size, error)) return false;
        out.artifacts.push_back({node.key, ArtifactRole::Primary, node.file, primary_size});
        std::uint64_t loaded = primary_size;
        if (node.key.type == "texture") {
            // kind 1 body (after the 38-byte envelope): 1, packed size, unpacked resident DDS size
            std::ifstream file(path, std::ios::binary);
            std::uint32_t head[3] = {};
            if (file.seekg(38) && file.read(reinterpret_cast<char*>(head), sizeof(head)) && head[0] == 1) loaded = head[2];
        }
        if (!stream.empty()) {
            out.files.push_back(stream);
            std::uint64_t stream_size = 0;
            if (!artifact_size(stream_path, stream_size, error)) return false;
            out.artifacts.push_back({node.key, ArtifactRole::Stream, stream, stream_size});
            if (node.key.type == "texture") out.memory.streamed += stream_size;
            else loaded += stream_size;
        }
        out.memory.loaded += loaded;
        if (node.key.name.rfind("content/mods/custom_assets/game/", 0) == 0) out.memory.game_copies += loaded;
    }
    return true;
}

bool write_build_manifest(const ResourceGraph& graph, const SerializedResources& resources,
                          const std::filesystem::path& directory,
                          std::string& error, const BuildMetadata& metadata) {
    // what Custom Assets reads: the compiler and game it is for, the package's name, the roots, every resource with its
    // file (and stream), and the game's resources to load first. Build reports go to compile_manifest.json (--manifest).
    std::ostringstream json;
    json << "{\n  \"schema\":1,"
         << "\n  \"compiler\":{\"name\":\"DarktideGLBCompiler\",\"version\":\"0.3.0-beta\"},"
         << "\n  \"target\":{\"id\":" << quote(metadata.target_profile.id) << '}';
    if (!metadata.package_name.empty())
        json << ",\n  \"package\":{\"key\":{\"type\":\"package\",\"name\":" << quote(metadata.package_name) << "}}";
    json << ",\n  \"roots\":[";
    bool first = true;
    for (const auto& root : graph.roots) { if (!first) json << ','; first = false; json << key_json(root); }
    json << "],\n  \"resources\":[";
    first = true;
    for (const auto index : resources.closure.owned) {
        const auto& node = graph.owned[index];
        const auto* primary = find_artifact(resources, node.key, ArtifactRole::Primary);
        if (!primary) { error = "resource has no primary artifact: " + node.key.type + ":" + node.key.name; return false; }
        if (!first) json << ','; first = false;
        json << "\n    {\"key\":" << key_json(node.key) << ",\"file\":{\"path\":" << quote(primary->path)
             << ",\"size\":" << primary->size << '}';
        if (const auto* stream = find_artifact(resources, node.key, ArtifactRole::Stream))
            json << ",\"stream\":{\"file\":{\"path\":" << quote(stream->path) << ",\"size\":" << stream->size << "}}";
        json << '}';
    }
    json << "\n  ],\n  \"external_resources\":[";
    first = true;
    for (const auto& ref : resources.closure.external) {
        if (!first) json << ','; first = false;
        json << "{\"key\":" << key_json(ref.key) << ",\"package_member\":" << (ref.package_member ? "true" : "false") << '}';
    }
    json << "]\n}\n";
    const auto text = json.str();
    return save(directory / "build.json", {text.begin(), text.end()}, error);
}
}
