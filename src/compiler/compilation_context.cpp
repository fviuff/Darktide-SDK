#include "compiler/compilation_context.h"
#include "compiler/resource_graph.h"

#include "stingray/murmur_hash.h"

#include <cctype>
#include <iomanip>
#include <sstream>
#include <utility>

namespace dtglb::compiler {
namespace {

std::string safe_name(std::string value) {
    for (char& c : value) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_' && c != '.') c = '_';
    }
    if (value.empty()) value = "asset";
    return value;
}

bool valid_resource_path(const std::string& path) {
    bool valid = portable_payload_path(path);
    for (const char c : path)
        valid = valid && ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '/' || c == '_' || c == '-');
    return valid;
}
} // namespace

bool make_compilation_context(const std::string& input_stem,
                              CompilationContext& out,
                              std::string& error,
                              const std::optional<std::string>& asset_path) {
    error.clear();
    if (asset_path) {
        // Native names are persisted identities: never silently sanitize one.
        if (!valid_resource_path(*asset_path)) {
            error = "asset path must be a relative, extensionless path with lowercase letters, digits, /, _ or - and no empty/reserved components";
            return false;
        }
        const auto slash = asset_path->find_last_of('/');
        out = CompilationContext(asset_path->substr(slash == std::string::npos ? 0 : slash + 1));
        out.resource_directory_ = slash == std::string::npos ? "" : asset_path->substr(0, slash + 1);
        return true;
    }
    out = CompilationContext(safe_name(input_stem));
    return true;
}

ResourceKey CompilationContext::generated_key(std::string type, std::string local_name) const {
    ResourceKey key{type, std::move(local_name)};
    if (resource_directory_) {
        key.name = *resource_directory_ + key.name;
    } else if (key.type == "material") {
        key.name = "content/mods/custom_assets/materials/" + key.name;
    } else if (key.type == "texture") {
        key.name = "content/mods/custom_assets/textures/" + key.name;
    }
    key.validate();
    return key;
}

std::string game_copy_stem(const std::string& local_name, const std::vector<std::uint8_t>& bytes) {
    std::ostringstream digest;
    digest << std::hex << std::setfill('0') << std::setw(16) << stingray::murmur64(bytes.data(), bytes.size());
    return local_name + "_" + digest.str().substr(0, 8);
}

ResourceKey game_copy_key(std::string type, const std::string& stem) {
    ResourceKey key{std::move(type), "content/mods/custom_assets/game/" + stem};
    key.validate();
    return key;
}

} // namespace dtglb::compiler
