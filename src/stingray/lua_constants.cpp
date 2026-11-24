#include "stingray/lua_constants.h"

#include <cstring>

namespace dtglb::stingray {

namespace {

struct Reader {
    const std::uint8_t* p;
    const std::uint8_t* end;
    bool ok = true;
    std::uint8_t byte() {
        if (p >= end) { ok = false; return 0; }
        return *p++;
    }
    std::uint64_t uleb() {
        std::uint64_t value = 0;
        for (int shift = 0; shift < 64; shift += 7) {
            const std::uint8_t b = byte();
            value |= static_cast<std::uint64_t>(b & 0x7f) << shift;
            if (!(b & 0x80) || !ok) return value;
        }
        ok = false;
        return value;
    }
    std::string text(std::uint64_t n) {
        if (n > static_cast<std::uint64_t>(end - p)) { ok = false; p = end; return {}; }
        std::string s(reinterpret_cast<const char*>(p), n);
        p += n;
        return s;
    }
    void skip(std::uint64_t n) {
        if (n > static_cast<std::uint64_t>(end - p)) { ok = false; p = end; return; }
        p += n;
    }
    static double as_double(std::uint32_t lo, std::uint32_t hi) {
        const std::uint64_t bits = (static_cast<std::uint64_t>(hi) << 32) | lo;
        double d;
        std::memcpy(&d, &bits, 8);
        return d;
    }
    LuaValue value() {
        const auto type = uleb();
        if (type >= 5) return text(type - 5);
        if (type == 3) return static_cast<std::int64_t>(static_cast<std::int32_t>(uleb()));
        if (type == 4) {
            const auto lo = static_cast<std::uint32_t>(uleb()), hi = static_cast<std::uint32_t>(uleb());
            return as_double(lo, hi);
        }
        if (type == 1) return false;
        if (type == 2) return true;
        return std::monostate{};
    }
    LuaValue number() {
        std::uint8_t b = byte();
        const bool is_double = b & 1;
        std::uint64_t v = b >> 1;
        if (b >= 0x80) {
            v = (b & 0x7f) >> 1;
            for (int shift = 6; ok; shift += 7) {
                b = byte();
                v |= static_cast<std::uint64_t>(b & 0x7f) << shift;
                if (b < 0x80) break;
            }
        }
        if (is_double) return as_double(static_cast<std::uint32_t>(v), static_cast<std::uint32_t>(uleb()));
        return static_cast<std::int64_t>(static_cast<std::int32_t>(static_cast<std::uint32_t>(v)));
    }
};

}  // namespace

const LuaValue* LuaTable::field(const std::string& key) const {
    for (const auto& [k, v] : hash)
        if (const auto* s = std::get_if<std::string>(&k); s && *s == key) return &v;
    return nullptr;
}

bool read_lua_prototypes(const std::vector<std::uint8_t>& body, std::vector<LuaPrototype>& prototypes, std::string& error) {
    prototypes.clear();
    std::uint32_t size = 0;
    if (body.size() >= 8) std::memcpy(&size, body.data() + 4, 4);
    if (size < 5 || size > body.size()) { error = "not a compiled Lua resource"; return false; }
    Reader r{body.data() + body.size() - size, body.data() + body.size()};
    if (r.byte() != 0x1b || r.byte() != 'F' || r.byte() != 'S') { error = "the Lua resource holds no bytecode dump"; return false; }
    r.byte();  // version
    const auto flags = r.uleb();
    const bool stripped = flags & 2;
    if (!stripped) r.skip(r.uleb());  // chunk name
    while (r.ok) {
        const auto length = r.uleb();
        if (length == 0) break;
        if (length > static_cast<std::uint64_t>(r.end - r.p)) { error = "Lua prototype runs past the dump"; return false; }
        const std::uint8_t* proto_end = r.p + length;
        LuaPrototype proto;
        r.skip(4);  // flags, parameters, frame size, upvalue count
        const std::uint8_t upvalues = r.p[-1];
        const auto gc = r.uleb(), numbers = r.uleb(), instructions = r.uleb();
        if (!stripped) {
            const auto debug = r.uleb();
            if (debug) { r.uleb(); r.uleb(); }
        }
        if (instructions * 4 > static_cast<std::uint64_t>(r.end - r.p)) { error = "Lua bytecode runs past the dump"; return false; }
        proto.bytecode.resize(instructions);
        if (instructions) std::memcpy(proto.bytecode.data(), r.p, instructions * 4);
        r.skip(instructions * 4 + upvalues * 2ull);
        for (std::uint64_t k = 0; k < gc && r.ok; ++k) {
            const auto type = r.uleb();
            if (type == 1) {
                LuaTable table;
                const auto array = r.uleb(), hash = r.uleb();
                for (std::uint64_t i = 0; i < array && r.ok; ++i) table.array.push_back(r.value());
                for (std::uint64_t i = 0; i < hash && r.ok; ++i) {
                    LuaValue key = r.value();
                    table.hash.emplace_back(std::move(key), r.value());
                }
                proto.gc.emplace_back(std::move(table));
            } else if (type >= 5) {
                proto.gc.emplace_back(r.text(type - 5));
            } else {
                if (type == 2 || type == 3) { r.uleb(); r.uleb(); }
                else if (type == 4) for (int i = 0; i < 4; ++i) r.uleb();
                proto.gc.emplace_back(std::monostate{});
            }
        }
        for (std::uint64_t k = 0; k < numbers && r.ok; ++k) proto.numbers.push_back(r.number());
        if (r.ok && r.p > proto_end) { error = "Lua prototype constants run past the prototype"; return false; }
        r.p = proto_end;
        prototypes.push_back(std::move(proto));
    }
    if (!r.ok) { error = "the Lua bytecode dump is truncated"; return false; }
    return true;
}

bool read_lua_tables(const std::vector<std::uint8_t>& body, std::vector<LuaTable>& tables, std::string& error) {
    tables.clear();
    std::vector<LuaPrototype> prototypes;
    if (!read_lua_prototypes(body, prototypes, error)) return false;
    for (auto& proto : prototypes)
        for (auto& constant : proto.gc)
            if (auto* table = std::get_if<LuaTable>(&constant)) tables.push_back(std::move(*table));
    return true;
}

}  // namespace dtglb::stingray
