#include "compiler/loose_textures.h"

#include "compiler/particles_builder.h"
#include "gltf/json_value.h"
#include "stingray/resource_name.h"
#include "stingray/texture/texture_writer.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace dtglb::compiler {

bool build_loose_textures(const Scene& scene, const CompilationContext& context, ResourceGraph& graph,
                          std::vector<ResourceKey>& textures, std::string& error) {
    const auto& text = scene.asset_definition.loose_textures;
    if (text.empty()) return true;
    json::Value list;
    if (!json::parse(text, list, error) || !list.is_array()) {
        error = "loose textures: " + (error.empty() ? std::string("not a list") : error);
        return false;
    }
    for (const auto& item : list.items) {
        const auto string = [&](const char* key) {
            const auto* value = item.find(key);
            return value && value->is_string() ? value->string : std::string();
        };
        const auto name = string("name"), image_path = string("image"), kind = string("kind");
        const auto where = "loose texture '" + name + "'";
        if (name.empty() || !std::all_of(name.begin(), name.end(), [](unsigned char c) {
                return std::islower(c) || std::isdigit(c) || c == '_'; })) {
            error = where + ": the name must be lowercase letters, digits and _";
            return false;
        }
        std::ifstream in(std::filesystem::u8path(image_path), std::ios::binary);
        const std::vector<std::uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        stingray::texture::ImageRGBA image;
        if (file.empty() || !stingray::texture::decode_image_rgba(file, "", image, error)) {
            error = where + ": cannot read the image " + image_path + (error.empty() ? "" : ": " + error);
            return false;
        }
        const auto stem = context.file_base() + "_" + name;
        const auto key = context.generated_key("texture", stem);
        if (std::any_of(graph.owned.begin(), graph.owned.end(), [&](const auto& node) { return node.key == key; })) {
            error = where + ": the asset already has a texture named " + key.name;
            return false;
        }
        if (kind == "like") {
            // a stand-in for one of the game's textures: its size, mips, pixel format and colour space
            const auto like = string("like");
            std::uint64_t hash = 0;
            if (like.size() == 17 && like[0] == '#') {
                try { hash = std::stoull(like.substr(1), nullptr, 16); } catch (const std::exception&) {}
            } else if (!like.empty()) {
                hash = stingray::resource_name_hash(like);
            }
            if (!hash) { error = where + ": name the game texture it stands in for (resource path or # and 16 hex digits)"; return false; }
            std::vector<std::uint8_t> game_body, body, stream;
            std::string game_stream;
            if (!read_game_resource(string("extract"), hash, "texture", game_body, game_stream, error) ||
                !stingray::texture::encode_texture_like(image, game_body, body, stream, error)) {
                error = where + ": " + error;
                return false;
            }
            graph.owned.push_back({key, stem + ".texture", PreservedTexture{std::move(body), std::move(stream)}, {}});
        } else {
            const auto* profile = kind == "color" ? &stingray::texture::substance_basic_bc_profile()
                                : kind == "color_alpha" ? &stingray::texture::substance_basic_bca_profile()
                                : kind == "normal" ? &stingray::texture::substance_basic_nm_profile()
                                : kind == "data" ? &stingray::texture::substance_basic_orm_profile() : nullptr;
            if (!profile) { error = where + ": kind must be color, color_alpha, normal, data or like"; return false; }
            graph.owned.push_back({key, stem + ".texture", TextureResource{std::move(image), *profile}, {}});
        }
        textures.push_back(key);
    }
    return true;
}
}
