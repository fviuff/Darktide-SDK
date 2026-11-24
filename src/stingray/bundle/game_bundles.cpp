#include "stingray/bundle/game_bundles.h"

#include "stingray/murmur_hash.h"
#include "stingray/oodle.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <regex>

namespace dtglb::stingray::bundle {
namespace {

constexpr std::size_t kHeader = 12 + 256;
constexpr std::size_t kIndexRecord = 20;
constexpr std::size_t kChunk = 0x80000;
constexpr std::size_t kCacheSize = 8;

template <typename T> T get(const std::vector<std::uint8_t>& data, std::size_t at) {
    T value{};
    if (at + sizeof(T) <= data.size()) std::memcpy(&value, data.data() + at, sizeof(T));
    return value;
}

std::size_t align16(std::size_t at) { return (at + 15) & ~static_cast<std::size_t>(15); }

bool read_file(const std::filesystem::path& path, std::vector<std::uint8_t>& data, std::size_t limit = SIZE_MAX) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    file.seekg(0, std::ios::end);
    const auto size = static_cast<std::size_t>(file.tellg());
    file.seekg(0);
    data.resize(std::min(size, limit));
    return static_cast<bool>(file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size())));
}

bool valid_version(std::uint64_t version) { return version == 0x00000003f0000007ull || version == 0x00000003f0000008ull; }

}  // namespace

std::uint64_t type_id(const std::string& type) { return id64(type); }

std::uint64_t name_id(const std::string& name_or_hash) {
    static const std::regex hex("^[0-9a-fA-F]{16}$");
    if (std::regex_match(name_or_hash, hex)) return std::stoull(name_or_hash, nullptr, 16);
    return id64(name_or_hash);
}

bool GameBundles::open(const std::filesystem::path& bundle_folder, std::string& error) {
    folder_ = bundle_folder;
    bundles_.clear();
    where_.clear();
    cache_.clear();
    layouts_.clear();
    // base bundles first, then patches in ascending order: a later one replaces what an earlier one holds
    static const std::regex base("^[0-9a-f]{16}$"), patch("^[0-9a-f]{16}\\.patch_([0-9]{3})$");
    std::vector<std::pair<int, std::filesystem::path>> files;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(bundle_folder, ec)) {
        if (!entry.is_regular_file()) continue;
        const auto name = entry.path().filename().string();
        std::smatch m;
        if (std::regex_match(name, base)) files.emplace_back(-1, entry.path());
        else if (std::regex_match(name, m, patch)) files.emplace_back(std::stoi(m[1].str()), entry.path());
    }
    if (ec || files.empty()) {
        error = "no Darktide bundles in " + bundle_folder.string();
        return false;
    }
    std::sort(files.begin(), files.end());
    std::vector<std::uint8_t> head;
    for (const auto& [order, path] : files) {
        if (!read_file(path, head, kHeader)) continue;
        if (head.size() < kHeader || !valid_version(get<std::uint64_t>(head, 0))) continue;
        const auto count = get<std::uint32_t>(head, 8);
        std::vector<std::uint8_t> index;
        if (!read_file(path, index, kHeader + static_cast<std::size_t>(count) * kIndexRecord) ||
            index.size() < kHeader + static_cast<std::size_t>(count) * kIndexRecord)
            continue;
        const std::size_t bundle = bundles_.size();
        bundles_.push_back(path);
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::size_t at = kHeader + static_cast<std::size_t>(i) * kIndexRecord;
            where_[ResourceId{get<std::uint64_t>(index, at), get<std::uint64_t>(index, at + 8)}] = bundle;
        }
    }
    if (bundles_.empty()) {
        error = "no readable Darktide bundles in " + bundle_folder.string();
        return false;
    }
    return true;
}

std::vector<ResourceId> GameBundles::resources() const {
    std::vector<ResourceId> out;
    out.reserve(where_.size());
    for (const auto& [id, bundle] : where_) out.push_back(id);
    return out;
}

std::vector<std::uint64_t> GameBundles::names(std::uint64_t type) const {
    std::vector<std::uint64_t> out;
    for (const auto& [id, bundle] : where_)
        if (id.type == type) out.push_back(id.name);
    std::sort(out.begin(), out.end());
    return out;
}

bool GameBundles::unpack(std::size_t bundle, Unpacked& out, std::string& error) const {
    std::vector<std::uint8_t> raw;
    const auto& path = bundles_[bundle];
    if (!read_file(path, raw)) { error = "cannot read " + path.string(); return false; }
    const auto count = get<std::uint32_t>(raw, 8);
    std::size_t at = kHeader + static_cast<std::size_t>(count) * kIndexRecord;
    const auto chunks = get<std::uint32_t>(raw, at);
    at += 4;
    std::vector<std::uint32_t> sizes(chunks);
    for (auto& size : sizes) { size = get<std::uint32_t>(raw, at); at += 4; }
    at = align16(at);
    const auto unpacked_size = get<std::uint32_t>(raw, at);
    at += 8;
    out.payload.clear();
    out.payload.reserve(unpacked_size);
    out.chunks.clear();
    std::vector<std::uint8_t> chunk;
    for (std::uint32_t i = 0; i < chunks; ++i) {
        const auto size = get<std::uint32_t>(raw, at);
        at = align16(at + 4);
        if (size != sizes[i] || at + size > raw.size()) { error = path.string() + ": chunk table doesn't match the chunks"; return false; }
        out.chunks.push_back({at, size});
        if (size == kChunk) {
            out.payload.insert(out.payload.end(), raw.begin() + static_cast<std::ptrdiff_t>(at), raw.begin() + static_cast<std::ptrdiff_t>(at + size));
        } else {
            // every chunk unpacks to at most 0x80000 bytes (the last one may be padded); the payload is cut below
            chunk.assign(kChunk, 0);
            if (!texture::oodle_decompress_up_to(raw.data() + at, size, chunk, error)) { error = path.string() + ": " + error; return false; }
            out.payload.insert(out.payload.end(), chunk.begin(), chunk.end());
        }
        at += size;
    }
    if (out.payload.size() < unpacked_size) { error = path.string() + ": unpacked data is short"; return false; }
    out.payload.resize(unpacked_size);

    out.spans.clear();
    std::size_t pos = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::size_t index_at = kHeader + static_cast<std::size_t>(i) * kIndexRecord;
        const ResourceId id{get<std::uint64_t>(raw, index_at), get<std::uint64_t>(raw, index_at + 8)};
        if (get<std::uint64_t>(out.payload, pos) != id.type || get<std::uint64_t>(out.payload, pos + 8) != id.name) {
            error = path.string() + ": resources are not in index order";
            return false;
        }
        const auto variants = get<std::uint32_t>(out.payload, pos + 16);
        std::size_t header = 24 + static_cast<std::size_t>(variants) * 14, data = 0;
        for (std::uint32_t v = 0; v < variants; ++v) {
            const std::size_t variant = pos + 24 + static_cast<std::size_t>(v) * 14;
            data += get<std::uint32_t>(out.payload, variant + 5) + static_cast<std::size_t>(get<std::uint32_t>(out.payload, variant + 10));
        }
        if (pos + header + data > out.payload.size()) { error = path.string() + ": a resource runs past the bundle"; return false; }
        out.spans[id] = {pos, header + data};
        pos += header + data;
    }
    // bytes after the last resource (bundles some tools write carry them) are not read, as in the game
    return true;
}

bool GameBundles::read_chunks(std::size_t bundle, const Layout& layout, std::size_t at, std::size_t size,
                              std::vector<std::uint8_t>& out, std::string& error) const {
    const auto& path = bundles_[bundle];
    std::ifstream file(path, std::ios::binary);
    if (!file) { error = "cannot read " + path.string(); return false; }
    out.clear();
    out.reserve(size);
    std::vector<std::uint8_t> packed, chunk;
    for (std::size_t c = at / kChunk; c <= (at + size - 1) / kChunk; ++c) {
        if (c >= layout.chunks.size()) { error = path.string() + ": a resource runs past the chunks"; return false; }
        const auto& stored = layout.chunks[c];
        packed.resize(stored.size);
        file.seekg(static_cast<std::streamoff>(stored.offset));
        if (!file.read(reinterpret_cast<char*>(packed.data()), stored.size)) { error = "cannot read " + path.string(); return false; }
        if (stored.size == kChunk) {
            chunk = packed;
        } else {
            chunk.assign(kChunk, 0);
            if (!texture::oodle_decompress_up_to(packed.data(), stored.size, chunk, error)) { error = path.string() + ": " + error; return false; }
        }
        const std::size_t from = std::max(at, c * kChunk), to = std::min(at + size, (c + 1) * kChunk);
        out.insert(out.end(), chunk.begin() + static_cast<std::ptrdiff_t>(from - c * kChunk),
                   chunk.begin() + static_cast<std::ptrdiff_t>(to - c * kChunk));
    }
    return true;
}

bool GameBundles::read(ResourceId id, std::vector<std::uint8_t>& resource, std::string& error) {
    const auto found = where_.find(id);
    if (found == where_.end()) { error = "the game has no such resource"; return false; }
    const auto layout = layouts_.find(found->second);
    const bool in_cache = std::any_of(cache_.begin(), cache_.end(), [&](const auto& entry) { return entry.first == found->second; });
    if (layout != layouts_.end() && !in_cache) {
        const auto span = layout->second.spans.find(id);
        if (span == layout->second.spans.end()) { error = "the bundle's index lists the resource but its data doesn't"; return false; }
        return read_chunks(found->second, layout->second, span->second.first, span->second.second, resource, error);
    }
    auto cached = std::find_if(cache_.begin(), cache_.end(), [&](const auto& entry) { return entry.first == found->second; });
    if (cached == cache_.end()) {
        Unpacked unpacked;
        if (!unpack(found->second, unpacked, error)) return false;
        layouts_[found->second] = Layout{unpacked.spans, unpacked.chunks};
        cache_.emplace_front(found->second, std::move(unpacked));
        if (cache_.size() > kCacheSize) cache_.pop_back();
        cached = cache_.begin();
    } else if (cached != cache_.begin()) {
        cache_.splice(cache_.begin(), cache_, cached);
        cached = cache_.begin();
    }
    const auto span = cached->second.spans.find(id);
    if (span == cached->second.spans.end()) { error = "the bundle's index lists the resource but its data doesn't"; return false; }
    const auto& payload = cached->second.payload;
    resource.assign(payload.begin() + static_cast<std::ptrdiff_t>(span->second.first),
                    payload.begin() + static_cast<std::ptrdiff_t>(span->second.first + span->second.second));
    return true;
}

bool GameBundles::for_each(std::uint64_t type,
                           const std::function<void(std::uint64_t, const std::vector<std::uint8_t>&)>& visit,
                           std::string& error) const {
    std::vector<std::vector<std::uint64_t>> per_bundle(bundles_.size());
    for (const auto& [id, bundle] : where_)
        if (id.type == type) per_bundle[bundle].push_back(id.name);
    std::vector<std::uint8_t> resource;
    for (std::size_t bundle = 0; bundle < bundles_.size(); ++bundle) {
        if (per_bundle[bundle].empty()) continue;
        Unpacked unpacked;
        if (!unpack(bundle, unpacked, error)) return false;
        std::sort(per_bundle[bundle].begin(), per_bundle[bundle].end());
        for (const auto name : per_bundle[bundle]) {
            const auto span = unpacked.spans.find(ResourceId{type, name});
            if (span == unpacked.spans.end()) { error = bundles_[bundle].string() + ": the index lists a resource its data doesn't"; return false; }
            resource.assign(unpacked.payload.begin() + static_cast<std::ptrdiff_t>(span->second.first),
                            unpacked.payload.begin() + static_cast<std::ptrdiff_t>(span->second.first + span->second.second));
            visit(name, resource);
        }
    }
    return true;
}

bool GameBundles::read_loose(const std::string& relative, std::vector<std::uint8_t>& data, std::string& error) const {
    if (relative.find("..") != std::string::npos) { error = "not a bundle data path: " + relative; return false; }
    if (!read_file(folder_ / relative, data)) { error = "cannot read " + (folder_ / relative).string(); return false; }
    return true;
}

}  // namespace dtglb::stingray::bundle
