#include "stingray/dynamic_data.h"

#include <cstring>

#include "stingray/murmur_hash.h"

namespace dtglb::stingray {

bool read_dynamic_data(const std::vector<std::uint8_t>& data, std::size_t at, std::vector<DynamicEntry>& out,
                       std::size_t& size, std::string& error) {
    const std::size_t start = at;
    const auto u32 = [&](std::uint32_t& v) {
        if (at + 4 > data.size()) return false;
        std::memcpy(&v, data.data() + at, 4);
        at += 4;
        return true;
    };
    std::uint32_t count = 0;
    if (!u32(count) || count > (1u << 20)) { error = "dynamic data runs past its end"; return false; }
    out.clear();
    for (std::uint32_t i = 0; i < count; ++i) {
        DynamicEntry e;
        std::uint32_t n = 0;
        if (!u32(n) || n > 64) { error = "dynamic data path is implausible"; return false; }
        e.path.resize(n);
        for (auto& p : e.path)
            if (!u32(p)) { error = "dynamic data runs past its end"; return false; }
        if (!u32(e.type)) { error = "dynamic data runs past its end"; return false; }
        if (e.type == 1) {
            if (!u32(e.number)) { error = "dynamic data runs past its end"; return false; }
        } else if (e.type == 2) {
            std::uint32_t bits = 0;
            if (!u32(bits)) { error = "dynamic data runs past its end"; return false; }
            std::memcpy(&e.scalar, &bits, 4);
        } else if (e.type == 3) {
            std::uint32_t length = 0;
            if (!u32(length) || at + length > data.size()) { error = "dynamic data runs past its end"; return false; }
            e.bytes.assign(data.begin() + static_cast<std::ptrdiff_t>(at), data.begin() + static_cast<std::ptrdiff_t>(at + length));
            at += (static_cast<std::size_t>(length) + 3) & ~std::size_t{3};
        } else {
            error = "dynamic data value type " + std::to_string(e.type);
            return false;
        }
        out.push_back(std::move(e));
    }
    size = at - start;
    return true;
}

void write_dynamic_data(const std::vector<DynamicEntry>& entries, std::vector<std::uint8_t>& out) {
    out.clear();
    const auto u32 = [&](std::uint32_t v) {
        const std::size_t at = out.size();
        out.resize(at + 4);
        std::memcpy(out.data() + at, &v, 4);
    };
    u32(static_cast<std::uint32_t>(entries.size()));
    for (const auto& e : entries) {
        u32(static_cast<std::uint32_t>(e.path.size()));
        for (const auto p : e.path) u32(p);
        u32(e.type);
        if (e.type == 1) {
            u32(e.number);
        } else if (e.type == 2) {
            std::uint32_t bits = 0;
            std::memcpy(&bits, &e.scalar, 4);
            u32(bits);
        } else if (e.type == 3) {
            u32(static_cast<std::uint32_t>(e.bytes.size()));
            out.insert(out.end(), e.bytes.begin(), e.bytes.end());
            out.resize((out.size() + 3) & ~std::size_t{3}, 0);
        }
    }
}

const DynamicEntry* find_dynamic(const std::vector<DynamicEntry>& entries, std::initializer_list<const char*> path) {
    std::vector<std::uint32_t> ids;
    for (const char* p : path) ids.push_back(id32_from_id64(p));
    for (const auto& e : entries)
        if (e.path == ids) return &e;
    return nullptr;
}

}  // namespace dtglb::stingray
