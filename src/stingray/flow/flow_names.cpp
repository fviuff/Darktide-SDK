#include "stingray/flow/flow_names.h"

#include <algorithm>
#include <iterator>

#include "stingray/bundle/game_bundles.h"
#include "stingray/cooked_resource.h"
#include "stingray/lua_constants.h"
#include "stingray/murmur_hash.h"

namespace dtglb::stingray::flow {

const char* known_flow_name(std::uint32_t id) {
    struct Row {
        std::uint32_t id;
        const char* name;
    };
    static const Row rows[] = {
#include "stingray/flow/flow_event_names.inc"
    };
    const auto* end = std::end(rows);
    const auto* found = std::lower_bound(std::begin(rows), end, id, [](const Row& r, std::uint32_t v) { return r.id < v; });
    return found != end && found->id == id ? found->name : "";
}

bool lua_string_names(const std::filesystem::path& bundle_folder, std::unordered_map<std::uint32_t, std::string>& names,
                      std::string& error, const std::vector<std::uint64_t>& only) {
    bundle::GameBundles bundles;
    if (!bundles.open(bundle_folder, error)) return false;
    const auto add = [&](const std::string& s) {
        if (s.empty() || s.size() > 128) return;
        for (const char c : s)
            if (static_cast<unsigned char>(c) < 0x20) return;
        names.emplace(id32_from_id64(s), s);
    };
    const auto add_value = [&](const LuaValue& v) {
        if (const auto* s = std::get_if<std::string>(&v)) add(*s);
    };
    const auto visit = [&](std::uint64_t, const std::vector<std::uint8_t>& resource) {
        std::vector<std::uint8_t> body;
        std::string stream, problem;
        if (!parse_cooked_resource_envelope(resource, "lua", body, stream, problem)) return;
        std::vector<LuaPrototype> prototypes;
        if (!read_lua_prototypes(body, prototypes, problem)) return;
        for (const auto& prototype : prototypes)
            for (const auto& constant : prototype.gc) {
                if (const auto* s = std::get_if<std::string>(&constant)) add(*s);
                if (const auto* t = std::get_if<LuaTable>(&constant)) {
                    for (const auto& v : t->array) add_value(v);
                    for (const auto& [k, v] : t->hash) { add_value(k); add_value(v); }
                }
            }
    };
    if (only.empty()) return bundles.for_each(bundle::type_id("lua"), visit, error);
    std::vector<std::uint8_t> resource;
    for (const auto name : only)
        if (bundles.read({bundle::type_id("lua"), name}, resource, error)) visit(name, resource);
    error.clear();
    return true;
}

}  // namespace dtglb::stingray::flow
