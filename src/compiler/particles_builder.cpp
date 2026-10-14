#include "compiler/particles_builder.h"

#include "compiler/material_builder.h"
#include "stingray/cooked_resource.h"
#include "stingray/material/material_v61.h"
#include "stingray/material/shader_section.h"
#include "stingray/murmur_hash.h"
#include "stingray/particles/particles_resource.h"
#include "stingray/resource_name.h"
#include "stingray/texture/texture_writer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <tuple>

namespace dtglb::compiler {
namespace {
namespace fs = std::filesystem;

std::string hex16(std::uint64_t value) {
    std::ostringstream text;
    text << std::hex << std::nouppercase << std::setfill('0') << std::setw(16) << value;
    return text.str();
}

bool read_file(const fs::path& path, std::vector<std::uint8_t>& out, std::string& error) {
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    std::ifstream input(path, std::ios::binary);
    out.resize(ec ? 0 : static_cast<std::size_t>(size));
    if (ec || !input || !input.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()))) {
        error = "cannot read " + path.string();
        return false;
    }
    return true;
}

std::uint32_t u32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    std::uint32_t value; std::memcpy(&value, bytes.data() + offset, 4); return value;
}

// The game's resources as a limn extract holds them (<extract>/<16 hex>.<type>). Materials and most textures
// there are stubs whose body names a loose file under <game>/bundle.
struct GameFiles {
    fs::path extract, bundle;
    std::set<std::uint64_t> materials, units, other; // other: resource types a copied unit cannot bring along

    bool load(const std::string& folder, std::string& error) {
        extract = fs::path(folder);
        bundle = stingray::texture::game_root() / "bundle";
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(extract, ec)) {
            const auto name = entry.path().filename().string();
            if (name.size() < 18 || name[16] != '.') continue;
            const auto type = name.substr(17);
            auto& target = type == "material" ? materials : type == "unit" ? units : other;
            if (&target == &other && type != "bones" && type != "state_machine" && type != "particles" &&
                type != "texture" && type != "animation") continue;
            try {
                target.insert(std::stoull(name.substr(0, 16), nullptr, 16));
            } catch (const std::exception&) {}
        }
        if (ec) { error = "cannot list the extract folder " + folder; return false; }
        if (materials.empty()) {
            error = "the extract folder has no material files; extract with limn's 'particles material texture' filter";
            return false;
        }
        return true;
    }

    // body of a cooked resource; a stub's body is replaced by the loose file it names
    bool resource(std::uint64_t hash, const char* type, std::vector<std::uint8_t>& body, std::string& stream,
                  std::string& error) const {
        std::vector<std::uint8_t> blob;
        if (!read_file(extract / (hex16(hash) + "." + type), blob, error)) return false;
        if (blob.size() < 38) { error = std::string("the game's ") + type + " " + hex16(hash) + " is truncated"; return false; }
        const auto body_size = u32(blob, 29), stream_size = u32(blob, 34);
        if (38ull + body_size + stream_size != blob.size()) {
            error = std::string("the game's ") + type + " " + hex16(hash) + " has an unknown layout";
            return false;
        }
        body.assign(blob.begin() + 38, blob.begin() + 38 + body_size);
        stream.assign(reinterpret_cast<const char*>(blob.data()) + 38 + body_size, stream_size);
        if (body.size() >= 5 && std::memcmp(body.data(), "data/", 5) == 0) {
            const std::string loose(reinterpret_cast<const char*>(body.data()),
                                    strnlen(reinterpret_cast<const char*>(body.data()), body.size()));
            stream.clear();
            if (!fs::is_regular_file(bundle / loose)) {
                error = std::string("the game's ") + type + " " + hex16(hash) + " names " + loose +
                    ", which this game version does not have; extract the game files again";
                return false;
            }
            return read_file(bundle / loose, body, error);
        }
        return true;
    }

    // path of a material's loose stream
    bool material_stream(std::uint64_t hash, fs::path& path, std::string& error) const {
        std::vector<std::uint8_t> blob;
        if (!read_file(extract / (hex16(hash) + ".material"), blob, error)) return false;
        if (blob.size() < 43 || std::memcmp(blob.data() + 38, "data/", 5) != 0) {
            error = "the game's material " + hex16(hash) + " is not a stub naming its stream";
            return false;
        }
        const std::string loose(reinterpret_cast<const char*>(blob.data()) + 38,
                                strnlen(reinterpret_cast<const char*>(blob.data()) + 38, blob.size() - 38));
        path = bundle / loose;
        if (!fs::is_regular_file(path)) {
            error = "the game's material " + hex16(hash) + " names " + loose +
                ", which this game version does not have; extract the game files again";
            return false;
        }
        return true;
    }
};

bool own_texture(const CompilationContext&, ResourceGraph& graph, const GameFiles& game, std::uint64_t hash,
                 ResourceKey& out, std::string& error) {
    std::vector<std::uint8_t> body, kind1, stream_bytes;
    std::string stream;
    if (!game.resource(hash, "texture", body, stream, error)) return false;
    if (!stream.empty() && !read_file(game.bundle / stream, stream_bytes, error)) return false;
    if (!stingray::texture::texture_body_as_kind1(body, kind1, error)) {
        error = "the game's texture " + hex16(hash) + ": " + error;
        return false;
    }
    std::vector<std::uint8_t> content = kind1;
    content.insert(content.end(), stream_bytes.begin(), stream_bytes.end());
    const auto stem = game_copy_stem("fx_tex_" + hex16(hash), content);
    out = game_copy_key("texture", stem);
    if (std::any_of(graph.owned.begin(), graph.owned.end(), [&](const auto& node) { return node.key == out; })) return true;
    graph.owned.push_back({out, stem + ".texture", PreservedTexture{std::move(kind1), std::move(stream_bytes)}, {}});
    return true;
}

// How a billboard visualizer draws, as the shader conditions see it: its vertex channels by the names conditions use
// (FUN_14057fbc0) and its mode flags (FUN_140580480). Same as the add-on's particle preview (shader_preview.py).
struct BillboardUse {
    std::set<std::uint32_t> channels;
    std::uint32_t mode = 0;
    bool operator<(const BillboardUse& other) const {
        return std::tie(channels, mode) < std::tie(other.channels, other.mode);
    }
};
using BillboardUses = std::set<BillboardUse>;

BillboardUse billboard_use(const json::Value& visualizer) {
    BillboardUse use;
    if (const auto* mode = visualizer.find("mode"); mode && mode->is_number()) use.mode = static_cast<std::uint32_t>(mode->number);
    const auto* vertex = visualizer.find("vertex");
    if (!vertex || !vertex->is_array()) return use;
    for (const auto& channel : vertex->items) {
        const auto* component = channel.find("component");
        const auto* type = channel.find("type");
        const auto* set_value = channel.find("set");
        if (!component || !component->is_string()) continue;
        const std::string kind = type && type->is_string() ? type->string : std::string();
        const int set = set_value && set_value->is_number() ? static_cast<int>(set_value->number) : 0;
        const auto& name = component->string;
        std::string id;
        if (name == "texcoord") {
            if (set == 1) id = "rotation";
            else if (set == 6) id = "pivot";
            else if (set == 0 && (kind == "float1" || kind == "float3")) id = "uv_animation";
            else if (set == 0 && kind == "float2") id = "uv_scale";
            else if (set == 5 && kind == "float1") id = "particle_id";
            else if (set == 8 && kind == "float1") id = "particle_age";
        } else if (name == "tangent" || name == "binormal") {
            id = name;
        } else if (name == "color") {
            id = "vertex_color";
        }
        if (!id.empty()) use.channels.insert(stingray::name_id32(id));
    }
    return use;
}

std::optional<bool> billboard_intrinsic(const BillboardUse& use, std::uint32_t name, const std::vector<std::uint32_t>& args) {
    static const auto visualizer_channels = stingray::name_id32("has_visualizer_channels");
    static const auto object_type = stingray::name_id32("object_type");
    static const auto billboard_particle = stingray::name_id32("billboard_particle");
    static const std::map<std::uint32_t, std::uint32_t> flags = {
        {stingray::name_id32("lighting"), 1u}, {stingray::name_id32("pivot"), 2u}, {stingray::name_id32("raytracing"), 4u},
        {stingray::name_id32("billboard_cross"), 8u}, {stingray::name_id32("billboard_random"), 16u}};
    static const std::set<std::uint32_t> flag_tests = {
        stingray::name_id32("has_intrinsic"), stingray::name_id32("billboard_cross"), stingray::name_id32("billboard_random")};
    if (name == visualizer_channels)
        return std::all_of(args.begin(), args.end(), [&](std::uint32_t a) { return use.channels.count(a) != 0; });
    if (flag_tests.count(name))
        return std::any_of(args.begin(), args.end(), [&](std::uint32_t a) {
            const auto flag = flags.find(a);
            return flag != flags.end() && (use.mode & flag->second) != 0;
        });
    if (name == object_type)
        return std::all_of(args.begin(), args.end(), [&](std::uint32_t a) { return a == billboard_particle; });
    return std::nullopt;   // anything else (render settings, runtime permutations...) can't be known here
}

// A shader material's section holds every shader its conditions choose between; in the default context the first
// shader whose condition passes is drawn. Keep, for each way the asset's billboards draw with it, that one (a
// condition that can't be judged here keeps its shader and the search goes on; when none
// passes the whole context stays). Other contexts stay complete.
ShaderTrim billboard_trim(const BillboardUses* uses) {
    if (!uses || uses->empty()) return {};
    return [uses](std::vector<std::uint8_t>& stream, std::string& error) {
        if (stream.size() < 28) return true;
        const std::uint32_t offset = u32(stream, 12), size = u32(stream, 16), next = u32(stream, 20);
        if (!size || offset == 0xffffffffu || offset + static_cast<std::uint64_t>(size) > stream.size()) return true;
        const std::vector<std::uint8_t> bytes(stream.begin() + offset, stream.begin() + offset + size);
        stingray::material::ShaderSection section;
        if (!stingray::material::decode_shader_section(bytes, section, error)) return false;
        static const auto default_context = stingray::name_id32("default");
        std::set<std::uint32_t> keep;
        for (const auto& context : section.contexts) {
            const auto keep_all = [&] { for (const auto& shader : context.shaders) keep.insert(shader[0]); };
            if (context.name != default_context) { keep_all(); continue; }
            for (const auto& use : *uses) {
                bool drawn = false;
                for (const auto& shader : context.shaders) {
                    const auto passes = shader[1] == 0xffffffffu ? std::optional<bool>(true) :
                        stingray::material::evaluate_condition(section.conditions, shader[1],
                            [&](std::uint32_t intrinsic, const std::vector<std::uint32_t>& args) {
                                return billboard_intrinsic(use, intrinsic, args); });
                    if (passes && !*passes) continue;   // never drawn for this billboard
                    keep.insert(shader[0]);
                    if (passes) { drawn = true; break; }
                }
                if (!drawn) keep_all();
            }
        }
        bool drops = false;
        for (const auto& data : section.shader_data) drops = drops || !keep.count(data.name);
        if (!drops || !stingray::material::keep_shaders(section, keep)) return true;
        const auto encoded = stingray::material::encode_shader_section(section);
        if (encoded.size() % 4 != size % 4) return true;   // the block after it keeps its alignment
        std::vector<std::uint8_t> out(stream.begin(), stream.begin() + offset);
        out.insert(out.end(), encoded.begin(), encoded.end());
        out.insert(out.end(), stream.begin() + offset + size, stream.end());
        const auto put = [&](std::size_t at, std::uint32_t value) { std::memcpy(out.data() + at, &value, 4); };
        put(16, static_cast<std::uint32_t>(encoded.size()));
        if (next != 0xffffffffu && next >= offset + size) put(20, next - size + static_cast<std::uint32_t>(encoded.size()));
        stream = std::move(out);
        return true;
    };
}

// a shader provider/parent and the parents above it, as owned copies; trimmed to the billboards that use them
bool own_shader_chain(const CompilationContext& context, ResourceGraph& graph, const GameFiles& game,
                      std::uint64_t hash, ResourceKey& out, std::string& error, const BillboardUses* uses) {
    fs::path path;
    std::vector<std::uint8_t> bytes;
    if (!game.material_stream(hash, path, error) || !read_file(path, bytes, error)) return false;
    if (bytes.size() < 28 || u32(bytes, 4) + 20 > bytes.size()) { error = "the game's shader material " + hex16(hash) + " is truncated"; return false; }
    std::uint64_t parent = 0;
    std::memcpy(&parent, bytes.data() + u32(bytes, 4) + 12, 8);
    ResourceKey owned_parent;
    if (parent && !own_shader_chain(context, graph, game, parent, owned_parent, error, uses)) return false;
    return own_game_shader_material(context, graph, hash, path, parent, parent ? &owned_parent : nullptr, out, error,
        [&](std::uint64_t texture, ResourceKey& key, std::string& texture_error) {
            return own_texture(context, graph, game, texture, key, texture_error);
        }, billboard_trim(uses));
}

bool own_particle_material(const CompilationContext& context, ResourceGraph& graph, const GameFiles& game,
                           std::uint64_t hash, const ParticleMaterialEdit* edit, ResourceKey& out, std::string& error,
                           const BillboardUses* uses = nullptr) {
    fs::path path;
    std::vector<std::uint8_t> bytes;
    if (!game.material_stream(hash, path, error) || !read_file(path, bytes, error)) return false;
    stingray::material::MaterialStream parsed;
    if (!stingray::material::parse_material_stream(bytes, parsed, error)) {
        error = "the game's material " + hex16(hash) + " is not a v61 material stream: " + error;
        return false;
    }
    const auto where = "particle material " + hex16(hash);
    if (edit) {
        for (const auto& [name, values] : edit->variables) {
            const auto name_hash = stingray::name_id32(name);
            const auto found = std::find_if(parsed.variables.begin(), parsed.variables.end(),
                                            [&](const auto& variable) { return variable.name_hash == name_hash; });
            if (found == parsed.variables.end()) { error = where + " has no variable '" + name + "'"; return false; }
            const std::size_t floats = found->klass + 1u;
            if (found->klass > 3 || values.size() != floats || found->data_offset + floats * 4 > parsed.variable_data.size()) {
                error = where + ": variable '" + name + "' takes " + std::to_string(floats) + " number(s)";
                return false;
            }
            std::vector<std::uint8_t> expected(floats * 4), replacement(floats * 4), edited;
            std::memcpy(expected.data(), parsed.variable_data.data() + found->data_offset, floats * 4);
            for (std::size_t i = 0; i < floats; ++i) {
                if (!std::isfinite(values[i])) { error = where + ": variable '" + name + "' must be finite"; return false; }
                std::memcpy(replacement.data() + i * 4, &values[i], 4);
            }
            if (expected == replacement) continue;
            if (!stingray::material::clone_material_v61_with_variable_data_edit(
                    bytes, found->data_offset, expected, replacement, edited, error)) return false;
            bytes = std::move(edited);
            if (!stingray::material::parse_material_stream(bytes, parsed, error)) return false;
        }
        for (const auto& entry : edit->textures) {
            const auto channel = stingray::name_id32(entry.first);
            if (std::none_of(parsed.textures.begin(), parsed.textures.end(),
                             [&](const auto& texture) { return texture.channel_hash == channel; })) {
                error = where + " has no texture channel '" + entry.first + "'";
                return false;
            }
        }
    }
    std::vector<ResourceKey> dependencies;
    std::vector<stingray::material::TextureResourceHashEdit> texture_edits;
    for (const auto& texture : parsed.textures) {
        if (!texture.resource_hash) continue;
        ResourceKey key;
        const std::pair<const std::string, std::string>* replaced = nullptr;
        if (edit)
            for (const auto& entry : edit->textures)
                if (stingray::name_id32(entry.first) == texture.channel_hash) replaced = &entry;
        if (replaced) {
            // the image takes the place of the game's texture, so it is written the way that one is
            std::vector<std::uint8_t> file, game_body, body, stream;
            std::string game_stream;
            stingray::texture::ImageRGBA image;
            if (!read_file(replaced->second, file, error) || !stingray::texture::decode_image_rgba(file, "", image, error) ||
                !game.resource(texture.resource_hash, "texture", game_body, game_stream, error) ||
                !stingray::texture::encode_texture_like(image, game_body, body, stream, error)) {
                error = where + ", texture '" + replaced->first + "': " + error;
                return false;
            }
            std::vector<std::uint8_t> content = body;
            content.insert(content.end(), stream.begin(), stream.end());
            const auto texture_stem = game_copy_stem("fx_tex_" + hex16(texture.resource_hash), content);
            key = game_copy_key("texture", texture_stem);
            if (std::none_of(graph.owned.begin(), graph.owned.end(), [&](const auto& node) { return node.key == key; }))
                graph.owned.push_back({key, texture_stem + ".texture", PreservedTexture{std::move(body), std::move(stream)}, {}});
        } else if (!own_texture(context, graph, game, texture.resource_hash, key, error)) {
            return false;
        }
        texture_edits.push_back({texture.channel_hash, texture.resource_hash, stingray::resource_name_hash(key.name)});
        dependencies.push_back(key);
    }
    if (!texture_edits.empty()) {
        std::vector<std::uint8_t> edited;
        if (!stingray::material::clone_material_v61_with_texture_hash_edits(bytes, texture_edits, edited, error)) return false;
        bytes = std::move(edited);
    }
    if (const auto trim = billboard_trim(uses); trim && !trim(bytes, error)) return false;   // a section of its own
    stingray::material::MaterialDependencyHashes hashes;
    if (!stingray::material::validate_preserved_material_v61(bytes, hashes, error)) return false;
    ResourceKey owned_provider, owned_parent;
    if (hashes.shader_provider_material_hash &&
        !own_shader_chain(context, graph, game, hashes.shader_provider_material_hash, owned_provider, error, uses)) return false;
    if (hashes.parent_material_hash &&
        !own_shader_chain(context, graph, game, hashes.parent_material_hash, owned_parent, error, uses)) return false;
    const std::size_t section = u32(bytes, 4);
    const std::uint64_t provider = hashes.shader_provider_material_hash ? stingray::resource_name_hash(owned_provider.name) : 0;
    const std::uint64_t parent = hashes.parent_material_hash ? stingray::resource_name_hash(owned_parent.name) : 0;
    std::memcpy(bytes.data() + section + 4, &provider, 8);
    std::memcpy(bytes.data() + section + 12, &parent, 8);
    if (hashes.shader_provider_material_hash) dependencies.push_back(owned_provider);
    if (hashes.parent_material_hash) dependencies.push_back(owned_parent);
    const auto stem = game_copy_stem("fx_" + hex16(hash), bytes);
    out = game_copy_key("material", stem);
    if (std::any_of(graph.owned.begin(), graph.owned.end(), [&](const auto& node) { return node.key == out; })) return true;
    graph.owned.push_back({out, stem + ".material", stingray::material::PreservedMaterialStream{std::move(bytes)},
                           std::move(dependencies)});
    return true;
}
// the unit a mesh particle draws: its body copied, its materials pointed at owned copies (with the effect's edits)
bool own_mesh_unit(const CompilationContext& context, ResourceGraph& graph, const GameFiles& game, std::uint64_t hash,
                   const std::map<std::uint64_t, ParticleMaterialEdit>& edits, ResourceKey& out, std::string& error) {
    std::vector<std::uint8_t> body;
    std::string stream;
    if (!game.resource(hash, "unit", body, stream, error)) return false;
    if (!stream.empty()) { error = "the game's unit " + hex16(hash) + " streams its meshes; those are not supported yet"; return false; }
    std::map<std::uint64_t, std::vector<std::size_t>> references;
    for (std::size_t offset = 0; offset + 8 <= body.size(); ++offset) {
        std::uint64_t value;
        std::memcpy(&value, body.data() + offset, 8);
        if (game.units.count(value) || game.other.count(value)) {
            error = "the game's unit " + hex16(hash) + " uses more than materials (bones, animation, other units...); not supported yet";
            return false;
        }
        if (game.materials.count(value)) references[value].push_back(offset);
    }
    std::vector<ResourceKey> dependencies;
    for (const auto& [material_hash, offsets] : references) {
        ResourceKey material;
        const auto edit = edits.find(material_hash);
        if (!own_particle_material(context, graph, game, material_hash, edit == edits.end() ? nullptr : &edit->second,
                                   material, error)) return false;
        const auto owned = stingray::resource_name_hash(material.name);
        for (const auto offset : offsets) std::memcpy(body.data() + offset, &owned, 8);
        dependencies.push_back(material);
    }
    // one copy per distinct set of material edits: those change the material names it holds
    const auto stem = game_copy_stem("fx_unit_" + hex16(hash), body);
    out = game_copy_key("unit", stem);
    if (std::any_of(graph.owned.begin(), graph.owned.end(), [&](const auto& node) { return node.key == out; })) return true;
    graph.owned.push_back({out, stem + ".unit", PreservedUnit{std::move(body)}, std::move(dependencies)});
    return true;
}

// materials and units may be named by resource path ("content/fx/materials/..."); use their ids
void reference_ids(json::Value& value) {
    if (value.is_object()) {
        for (auto& [name, member] : value.members) {
            if (member.is_string() && !member.string.empty() && member.string[0] != '#' &&
                (name == "unit" || name == "material" || name.rfind("material_", 0) == 0))
                member.string = "#" + hex16(stingray::resource_name_hash(member.string));
            reference_ids(member);
        }
    } else if (value.is_array()) {
        for (auto& item : value.items) reference_ids(item);
    }
}

// point the description's material and unit ids at the owned copies
void retarget_references(json::Value& value, const std::map<std::uint64_t, ResourceKey>& owned) {
    if (value.is_object()) {
        for (auto& [name, member] : value.members) {
            if (member.is_string() && member.string.size() == 17 && member.string[0] == '#' &&
                (name == "unit" || name.rfind("material", 0) == 0)) {
                const auto found = owned.find(std::stoull(member.string.substr(1), nullptr, 16));
                if (found != owned.end()) member.string = "#" + hex16(stingray::resource_name_hash(found->second.name));
            }
            retarget_references(member, owned);
        }
    } else if (value.is_array()) {
        for (auto& item : value.items) retarget_references(item, owned);
    }
}
} // namespace

bool read_game_resource(const std::string& extract, std::uint64_t hash, const char* type,
                        std::vector<std::uint8_t>& body, std::string& stream, std::string& error) {
    if (extract.empty()) { error = "needs the game files extract"; return false; }
    GameFiles game;
    game.extract = fs::u8path(extract);
    game.bundle = stingray::texture::game_root() / "bundle";
    return game.resource(hash, type, body, stream, error);
}

bool build_particle_effects(const Scene& scene, const CompilationContext& context, ResourceGraph& graph,
                            std::vector<ResourceKey>& effects, std::string& error) {
    std::map<std::string, GameFiles> extracts;
    std::set<std::string> names;
    struct Decoded {
        const ParticleEffectDefinition* effect;
        const GameFiles* game;
        json::Value description;
    };
    std::vector<Decoded> decoded;
    for (const auto& effect : scene.particle_effects) {
        const auto where = "particle effect '" + effect.name + "'";
        if (effect.name.empty() || !std::all_of(effect.name.begin(), effect.name.end(), [](char c) {
                return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'; }) ||
            !names.insert(effect.name).second) {
            error = where + ": names are unique and use lowercase letters, digits, _ or -";
            return false;
        }
        auto [found, added] = extracts.try_emplace(effect.extract);
        if (added && !found->second.load(effect.extract, error)) return false;
        const auto& game = found->second;
        json::Value description;
        if (!effect.description.empty()) {
            if (!json::parse(effect.description, description, error)) {
                error = where + ": the effect description is not valid JSON: " + error;
                return false;
            }
        } else {
            std::vector<std::uint8_t> blob, cooked;
            std::string stream;
            if (!read_file(effect.source, blob, error) ||
                !stingray::parse_cooked_resource_envelope(blob, "particles", cooked, stream, error) ||
                !stingray::particles::decode(cooked, description, error)) {
                error = where + ": " + error;
                return false;
            }
        }
        reference_ids(description);
        decoded.push_back({&effect, &game, std::move(description)});
    }
    // every way the asset's billboards draw with a material: its shader copies keep only what those can pick. A
    // material something else draws too (gpu particles, ribbons) keeps its whole shaders
    std::map<std::uint64_t, BillboardUses> uses;
    std::map<std::uint64_t, std::size_t> billboards, references;
    for (const auto& item : decoded) {
        for (const auto& reference : stingray::particles::resource_references(item.description))
            if (reference.type == "material") ++references[reference.id];
        const auto* systems = item.description.find("systems");
        for (const auto& system : systems && systems->is_array() ? systems->items : std::vector<json::Value>{}) {
            const auto* visualizers = system.find("visualizers");
            for (const auto& visualizer : visualizers && visualizers->is_array() ? visualizers->items : std::vector<json::Value>{}) {
                const auto* type = visualizer.find("type");
                const auto* material = visualizer.find("material");
                if (!type || !type->is_string() || type->string != "billboard" || !material || !material->is_string() ||
                    material->string.size() != 17 || material->string[0] != '#') continue;
                const auto id = std::stoull(material->string.substr(1), nullptr, 16);
                uses[id].insert(billboard_use(visualizer));
                ++billboards[id];
            }
        }
    }
    for (const auto& [id, count] : references)
        if (billboards[id] != count) uses.erase(id);
    for (auto& item : decoded) {
        const auto& effect = *item.effect;
        const auto& game = *item.game;
        auto& description = item.description;
        const auto where = "particle effect '" + effect.name + "'";
        // every game material and unit the effect names is shipped as an owned copy
        std::map<std::uint64_t, ResourceKey> owned;
        std::vector<ResourceKey> dependencies;
        for (const auto& reference : stingray::particles::resource_references(description)) {
            if (owned.count(reference.id)) continue;
            ResourceKey key;
            if (reference.type == "unit") {
                if (!game.units.count(reference.id) || !own_mesh_unit(context, graph, game, reference.id, effect.materials, key, error)) {
                    if (!game.units.count(reference.id)) error = "the game has no unit " + hex16(reference.id);
                    error = where + ": " + error;
                    return false;
                }
            } else {
                const auto edit = effect.materials.find(reference.id);
                const auto use = uses.find(reference.id);
                if (!game.materials.count(reference.id) ||
                    !own_particle_material(context, graph, game, reference.id,
                                           edit == effect.materials.end() ? nullptr : &edit->second, key, error,
                                           use == uses.end() ? nullptr : &use->second)) {
                    if (!game.materials.count(reference.id)) error = "the game has no material " + hex16(reference.id);
                    error = where + ": " + error;
                    return false;
                }
            }
            owned.emplace(reference.id, key);
            dependencies.push_back(key);
        }
        for (const auto& edited : effect.materials)
            if (!owned.count(edited.first)) {
                error = where + " does not use material " + hex16(edited.first);
                return false;
            }
        retarget_references(description, owned);
        std::vector<std::uint8_t> body;
        if (!stingray::particles::encode(description, body, error)) {
            error = where + ": " + error;
            return false;
        }
        const auto key = context.generated_key("particles", effect.name);
        graph.owned.push_back({key, effect.name + ".particles", ParticlesResource{std::move(body)}, std::move(dependencies)});
        effects.push_back(key);
    }
    return true;
}
}
