#include "stingray/shading_environment/shading_environment.h"

#include "stingray/murmur_hash.h"

#include <algorithm>
#include <cstring>

namespace dtglb::stingray::shading_environment {
namespace {

struct Reader {
    const std::vector<std::uint8_t>& data;
    std::size_t at = 0;
    bool ok = true;
    template <typename T>
    T get() {
        T value{};
        if (at + sizeof(T) > data.size()) { ok = false; return value; }
        std::memcpy(&value, data.data() + at, sizeof(T));
        at += sizeof(T);
        return value;
    }
    // a count that can't be larger than what is left at `each` bytes per entry
    std::uint32_t count(std::size_t each) {
        const auto n = get<std::uint32_t>();
        if (ok && n * each > data.size() - at) ok = false;
        return ok ? n : 0;
    }
};

template <typename T>
void put(std::vector<std::uint8_t>& out, T value) {
    const auto at = out.size();
    out.resize(at + sizeof(T));
    std::memcpy(out.data() + at, &value, sizeof(T));
}

}  // namespace

const Variable* ShadingEnvironment::find(std::uint32_t name) const {
    for (const auto& v : variables)
        if (v.name == name) return &v;
    return nullptr;
}
const Variable* ShadingEnvironment::find(const char* name) const { return find(id32_from_id64(name)); }
std::optional<std::vector<float>> ShadingEnvironment::floats(const char* name) const { return floats(id32_from_id64(name)); }
std::uint64_t ShadingEnvironment::texture(const char* name) const { return texture(id32_from_id64(name)); }

std::optional<std::vector<float>> ShadingEnvironment::floats(std::uint32_t name) const {
    const Variable* v = find(name);
    if (!v || settings.empty() || v->type > 3) return std::nullopt;
    const auto& values = settings.front().values;
    std::vector<float> out(v->type + 1);
    if (v->offset + out.size() * 4 > values.size()) return std::nullopt;
    std::memcpy(out.data(), values.data() + v->offset, out.size() * 4);
    return out;
}

std::uint64_t ShadingEnvironment::texture(std::uint32_t name) const {
    const Variable* v = find(name);
    if (!v || settings.empty() || (v->type != 10 && v->type != 11)) return 0;
    const auto& values = settings.front().values;
    std::uint64_t id = 0;
    if (v->offset + 8 <= values.size()) std::memcpy(&id, values.data() + v->offset, 8);
    return id;
}

bool read_shading_environment(const std::vector<std::uint8_t>& body, ShadingEnvironment& out, std::string& error) {
    out = {};
    Reader r{body};
    out.version = r.get<std::uint32_t>();
    if (r.ok && out.version != 22) { error = "shading environment version " + std::to_string(out.version) + ", not 22"; return false; }
    for (std::uint32_t i = 0, n = r.count(12); i < n; ++i) {
        const auto name = r.get<std::uint32_t>();
        out.engine_textures.emplace_back(name, r.get<std::uint64_t>());
    }
    for (std::uint32_t i = 0, n = r.count(16); i < n; ++i) {
        Variable v;
        v.type = r.get<std::uint32_t>();
        v.flags = r.get<std::uint32_t>();
        v.name = r.get<std::uint32_t>();
        v.offset = r.get<std::uint32_t>();
        out.variables.push_back(v);
    }
    for (std::uint32_t i = 0, n = r.count(8); i < n; ++i) {
        Setting s;
        s.name = r.get<std::uint32_t>();
        const auto size = r.count(1);
        s.values.assign(body.begin() + static_cast<std::ptrdiff_t>(std::min(r.at, body.size())),
                        body.begin() + static_cast<std::ptrdiff_t>(std::min(r.at + size, body.size())));
        r.at += size;
        out.settings.push_back(std::move(s));
    }
    for (std::uint32_t i = 0, n = r.count(4); i < n; ++i) out.weights.push_back(r.get<float>());
    if (!r.ok) { error = "shading environment is truncated"; return false; }
    out.mapping.assign(body.begin() + static_cast<std::ptrdiff_t>(r.at), body.end());
    // each variable runs to the next one (or the end of the values)
    const std::size_t values = out.settings.empty() ? 0 : out.settings.front().values.size();
    for (auto& v : out.variables) {
        std::size_t end = values;
        for (const auto& w : out.variables)
            if (w.offset > v.offset) end = std::min<std::size_t>(end, w.offset);
        if (v.offset > values) { error = "shading environment variable lies outside its values"; return false; }
        v.size = static_cast<std::uint32_t>(end - v.offset);
    }
    return true;
}

void write_shading_environment(const ShadingEnvironment& environment, std::vector<std::uint8_t>& body) {
    body.clear();
    put<std::uint32_t>(body, environment.version);
    put<std::uint32_t>(body, static_cast<std::uint32_t>(environment.engine_textures.size()));
    for (const auto& [name, texture] : environment.engine_textures) {
        put(body, name);
        put(body, texture);
    }
    put<std::uint32_t>(body, static_cast<std::uint32_t>(environment.variables.size()));
    for (const auto& v : environment.variables) {
        put(body, v.type);
        put(body, v.flags);
        put(body, v.name);
        put(body, v.offset);
    }
    put<std::uint32_t>(body, static_cast<std::uint32_t>(environment.settings.size()));
    for (const auto& s : environment.settings) {
        put(body, s.name);
        put<std::uint32_t>(body, static_cast<std::uint32_t>(s.values.size()));
        body.insert(body.end(), s.values.begin(), s.values.end());
    }
    put<std::uint32_t>(body, static_cast<std::uint32_t>(environment.weights.size()));
    for (const float w : environment.weights) put(body, w);
    body.insert(body.end(), environment.mapping.begin(), environment.mapping.end());
}

}  // namespace dtglb::stingray::shading_environment
