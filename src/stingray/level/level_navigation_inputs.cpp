#include "stingray/level/level_navigation.h"
#include "stingray/level/level_package.h"

#include "stingray/dynamic_data.h"
#include "stingray/murmur_hash.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>

namespace dtglb::stingray::level {
namespace fs = std::filesystem;

namespace {

std::vector<std::uint8_t> load(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

// ---- a Lua table constructor, as volume data scripts are written ------------------------------------------------
struct LuaValue {
    enum Kind { Nil, Number, String, Table } kind = Nil;
    double number = 0;
    std::string text;
    std::vector<LuaValue> items;                          // array part
    std::vector<std::pair<std::string, LuaValue>> fields;  // keyed part
    const LuaValue* field(const char* name) const {
        for (const auto& [k, v] : fields)
            if (k == name) return &v;
        return nullptr;
    }
};

struct LuaReader {
    const std::string& s;
    std::size_t at = 0;
    void skip() {
        for (;;) {
            while (at < s.size() && std::isspace(static_cast<unsigned char>(s[at]))) ++at;
            if (s.compare(at, 2, "--") == 0) {
                while (at < s.size() && s[at] != '\n') ++at;
                continue;
            }
            return;
        }
    }
    bool name(std::string& out) {
        skip();
        const std::size_t start = at;
        while (at < s.size() && (std::isalnum(static_cast<unsigned char>(s[at])) || s[at] == '_')) ++at;
        out = s.substr(start, at - start);
        return !out.empty() && !std::isdigit(static_cast<unsigned char>(out[0]));
    }
    bool value(LuaValue& out) {
        skip();
        if (at >= s.size()) return false;
        if (s[at] == '"' || s[at] == '\'') {
            const char quote = s[at++];
            out.kind = LuaValue::String;
            while (at < s.size() && s[at] != quote) {
                if (s[at] == '\\' && at + 1 < s.size()) ++at;
                out.text += s[at++];
            }
            ++at;
            return true;
        }
        if (s[at] == '{') {
            ++at;
            out.kind = LuaValue::Table;
            for (;;) {
                skip();
                if (at < s.size() && s[at] == '}') { ++at; return true; }
                const std::size_t mark = at;
                std::string key;
                LuaValue item;
                if (name(key)) {
                    skip();
                    if (at < s.size() && s[at] == '=') {
                        ++at;
                        if (!value(item)) return false;
                        out.fields.emplace_back(key, std::move(item));
                    } else {
                        at = mark;
                        if (!value(item)) return false;
                        out.items.push_back(std::move(item));
                    }
                } else {
                    at = mark;
                    if (!value(item)) return false;
                    out.items.push_back(std::move(item));
                }
                skip();
                if (at < s.size() && (s[at] == ',' || s[at] == ';')) ++at;
            }
        }
        char* end = nullptr;
        out.number = std::strtod(s.c_str() + at, &end);
        if (end == s.c_str() + at) return false;
        out.kind = LuaValue::Number;
        at = static_cast<std::size_t>(end - s.c_str());
        return true;
    }
};

bool vec3(const LuaValue* v, std::array<float, 3>& out) {
    if (!v || v->kind != LuaValue::Table || v->items.size() < 3) return false;
    for (int i = 0; i < 3; ++i) out[i] = static_cast<float>(v->items[i].number);
    return true;
}

// ---- level data -------------------------------------------------------------------------------------------------
std::vector<DynamicEntry> level_entries(const LevelDocument& d) {
    std::vector<DynamicEntry> entries;
    std::size_t size = 0;
    std::string problem;
    if (!d.level_data.empty() && !read_dynamic_data(d.level_data, 0, entries, size, problem)) entries.clear();
    return entries;
}

}  // namespace

void level_navigation_settings(const LevelDocument& d, navigation::NavSettings& settings) {
    const auto entries = level_entries(d);
    const auto set = [&](const char* key, float& out) {
        if (const auto* e = find_dynamic(entries, {"gwnavgen", key}); e && e->type == 2) out = e->scalar;
    };
    set("entity_radius", settings.entity_radius);
    set("entity_height", settings.entity_height);
    set("slope_max", settings.slope_max);
    set("step_max", settings.step_max);
}

std::string stored_navigation_inputs(const LevelDocument& d) {
    const auto entries = level_entries(d);
    const auto* e = find_dynamic(entries, {"gwnavgen", "inputs"});
    return e && e->type == 3 ? std::string(e->bytes.begin(), e->bytes.end()) : std::string();
}

std::string navigation_inputs(const LevelDocument& d, const std::string& volume_data_script, const std::vector<fs::path>& custom_folders) {
    std::string all;
    const auto put = [&](const void* data, std::size_t size) { all.append(static_cast<const char*>(data), size); };
    put(d.prototype_unit.data(), d.prototype_unit.size());
    const auto own = mod_unit_files(custom_folders);
    std::map<std::uint64_t, std::uint64_t> files;  // a mod unit's file, hashed once
    for (const auto& u : d.units) {
        put(&u.resource, 8);
        put(u.position.data(), 12);
        put(u.rotation.data(), 16);
        put(u.scale.data(), 12);
        put(u.data.data(), u.data.size());
        if (const auto found = own.find(u.resource); found != own.end()) {
            auto& hash = files[u.resource];
            if (!hash) {
                const auto bytes = load(found->second);
                hash = id64(std::string(bytes.begin(), bytes.end())) | 1;
            }
            put(&hash, 8);
        }
    }
    for (const auto& v : d.volumes) put(v.record.data(), v.record.size());
    for (const auto& m : d.volume_matrices) put(m.data(), m.size());
    all += volume_data_script;
    const auto entries = level_entries(d);
    const std::uint32_t group = id32_from_id64("gwnavgen"), inputs = id32_from_id64("inputs");
    for (const auto& e : entries)
        if (!e.path.empty() && e.path[0] == group && !(e.path.size() == 2 && e.path[1] == inputs)) {
            put(e.path.data(), e.path.size() * 4);
            put(&e.scalar, 4);
            put(&e.number, 4);
        }
    char text[24];
    std::snprintf(text, sizeof text, "%016llx", static_cast<unsigned long long>(id64(all)));
    return text;
}

std::vector<navigation::NavTagVolume> navigation_tag_volumes(const std::string& script) {
    std::vector<navigation::NavTagVolume> out;
    const auto start = script.find("volume_data");
    if (start == std::string::npos) return out;
    const auto brace = script.find('{', start);
    if (brace == std::string::npos) return out;
    LuaReader reader{script, brace};
    LuaValue list;
    if (!reader.value(list) || list.kind != LuaValue::Table) return out;
    for (const auto& v : list.items) {
        const auto* type = v.field("type");
        if (!type || type->kind != LuaValue::String) continue;
        navigation::NavTagVolume volume;
        const std::string& t = type->text;
        const auto suffix = [&](const char* kind) {
            const auto at = t.find(kind);
            return at == std::string::npos ? -2 : std::atoi(t.c_str() + at + std::strlen(kind)) ;
        };
        if (t.find("gwnavexclusivetagvolume") != std::string::npos) {
            volume.exclusive = true;
        } else if (t.find("gwnavtagvolume_layer") != std::string::npos) {
            volume.exclusive = false;
            volume.layer = suffix("gwnavtagvolume_layer");
        } else if (t.find("gwnavtagvolume_smartobject") != std::string::npos) {
            volume.exclusive = false;
            volume.smart_object = suffix("gwnavtagvolume_smartobject");
        } else {
            continue;
        }
        std::array<float, 3> low{}, high{};
        const auto* points = v.field("bottom_points");
        if (!vec3(v.field("alt_min_vector"), low) || !vec3(v.field("alt_max_vector"), high) || !points || points->kind != LuaValue::Table)
            continue;
        for (const auto& p : points->items) {
            std::array<float, 3> point{};
            if (vec3(&p, point)) volume.outline.push_back(point);
        }
        if (const auto* c = v.field("color"); c && c->kind == LuaValue::Table && c->items.size() >= 4)
            volume.color = {static_cast<float>(c->items[3].number), static_cast<float>(c->items[0].number),
                            static_cast<float>(c->items[1].number), static_cast<float>(c->items[2].number)};
        volume.bottom = low[2];
        volume.top = high[2];
        if (volume.outline.size() >= 3) out.push_back(std::move(volume));
    }
    return out;
}

}  // namespace dtglb::stingray::level
