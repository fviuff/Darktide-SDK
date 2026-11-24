#pragma once
// The game's resources straight from its bundles (<game>/bundle/<16 hex>[.patch_NNN]).
//
// A bundle: u64 version (0x3f0000007/8), u32 resource count, 256 bytes of type data, then per resource an index
// record {u64 type, u64 name, u32 mode}; then u32 chunk count, the chunk sizes, (16-aligned) u32 unpacked size, u32 0,
// and the chunks: u32 size, 16-aligned data, Oodle-packed unless exactly 0x80000 bytes (then stored as is). Unpacked,
// the resources follow each other in index order: u64 type, u64 name, u32 variant count, 4 zero bytes, per variant
// {u32 kind, u8, u32 body size, u8 1, u32 tail size} (14 bytes), then the bodies and tails. A resource is returned whole, header
// included (the same bytes limn's --dump-raw writes), so the cooked-resource readers take it as is.
#include <cstdint>
#include <filesystem>
#include <functional>
#include <list>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dtglb::stingray::bundle {

struct ResourceId {
    std::uint64_t type = 0, name = 0;
    bool operator==(const ResourceId& o) const { return type == o.type && name == o.name; }
};
struct ResourceIdHash {
    std::size_t operator()(const ResourceId& id) const { return static_cast<std::size_t>(id.type * 0x9e3779b97f4a7c15ull ^ id.name); }
};

class GameBundles {
public:
    // indexes every bundle in the folder (headers only); patch bundles override the base ones
    bool open(const std::filesystem::path& bundle_folder, std::string& error);
    const std::filesystem::path& folder() const { return folder_; }

    bool contains(ResourceId id) const { return where_.count(id) != 0; }
    std::vector<ResourceId> resources() const;          // every resource the game has
    std::vector<std::uint64_t> names(std::uint64_t type) const;  // every resource of one type

    bool read(ResourceId id, std::vector<std::uint8_t>& resource, std::string& error);
    // every resource of a type, bundle by bundle (each bundle unpacked once); stops at the first error
    bool for_each(std::uint64_t type, const std::function<void(std::uint64_t name, const std::vector<std::uint8_t>&)>& visit,
                  std::string& error) const;
    // a loose file the resources refer to (data/xx/<hash>[.stream], under the bundle folder)
    bool read_loose(const std::string& relative, std::vector<std::uint8_t>& data, std::string& error) const;

private:
    using Spans = std::unordered_map<ResourceId, std::pair<std::size_t, std::size_t>, ResourceIdHash>;  // offset, size
    struct Chunk {
        std::size_t offset = 0;  // of the chunk's data in the bundle file
        std::uint32_t size = 0;  // stored size (0x80000 = not packed)
    };
    struct Unpacked {
        std::vector<std::uint8_t> payload;
        Spans spans;
        std::vector<Chunk> chunks;
    };
    // where a bundle's resources lie in its unpacked data, and its chunks: kept once a bundle was unpacked, so later
    // reads unpack only the chunks a resource covers
    struct Layout {
        Spans spans;
        std::vector<Chunk> chunks;
    };
    bool unpack(std::size_t bundle, Unpacked& out, std::string& error) const;
    bool read_chunks(std::size_t bundle, const Layout& layout, std::size_t at, std::size_t size,
                     std::vector<std::uint8_t>& out, std::string& error) const;

    std::filesystem::path folder_;
    std::vector<std::filesystem::path> bundles_;
    std::unordered_map<ResourceId, std::size_t, ResourceIdHash> where_;
    std::list<std::pair<std::size_t, Unpacked>> cache_;  // most recently used first
    std::unordered_map<std::size_t, Layout> layouts_;
};

// "unit" -> type id; a name is a resource path or its 16-hex hash
std::uint64_t type_id(const std::string& type);
std::uint64_t name_id(const std::string& name_or_hash);

}  // namespace dtglb::stingray::bundle
