#include "stingray/texture/texture_reader.h"

#include "stingray/oodle.h"

#include <algorithm>
#include <cstring>

namespace dtglb::stingray::texture {
namespace {

constexpr std::size_t kChunk = 65536;
constexpr std::size_t kDdsHeader = 148;  // "DDS " + 124-byte header + 20-byte DX10 header

std::uint32_t u32_at(const std::vector<std::uint8_t>& data, std::size_t at) {
    std::uint32_t value = 0;
    if (at + 4 <= data.size()) std::memcpy(&value, data.data() + at, 4);
    return value;
}

// the DXGI format of a DDS (DX10 header, or the legacy FourCC / bit masks)
std::uint32_t dds_dxgi(const std::vector<std::uint8_t>& dds) {
    if (dds.size() < 128 || std::memcmp(dds.data(), "DDS ", 4) != 0) return 0;
    if (std::memcmp(dds.data() + 84, "DX10", 4) == 0) return u32_at(dds, 128);
    switch (u32_at(dds, 84)) {
        case 111: return 54;  // R16_FLOAT
        case 112: return 34;  // R16G16_FLOAT
        case 113: return 10;  // R16G16B16A16_FLOAT
        case 114: return 41;  // R32_FLOAT
        case 115: return 16;  // R32G32_FLOAT
        case 116: return 2;   // R32G32B32A32_FLOAT
        case 0x31545844: return 71;  // DXT1 -> BC1
        case 0x33545844: return 74;  // DXT3 -> BC2
        case 0x35545844: return 77;  // DXT5 -> BC3
        case 0x31495441: return 80;  // ATI1 -> BC4
        case 0x32495441: return 83;  // ATI2 -> BC5
        case 0: {
            const std::uint32_t bits = u32_at(dds, 88), red = u32_at(dds, 92), alpha = u32_at(dds, 104);
            if (bits == 32) return red == 0xff ? 28 : 87;  // R8G8B8A8 / B8G8R8A8
            if (bits == 8) return alpha ? 65 : 61;         // A8 / R8
            if (bits == 16) return red == 0xffff ? 56 : 49;  // R16 (luminance: terrain height maps) / R8G8
            return 0;
        }
        default: return 0;
    }
}

std::size_t mip_bytes(std::uint32_t width, std::uint32_t height, std::uint32_t block, bool compressed) {
    if (compressed) return static_cast<std::size_t>(std::max(1u, (width + 3) / 4)) * std::max(1u, (height + 3) / 4) * block;
    return static_cast<std::size_t>(width) * height * block;
}

// faces of a DDS: DX10 misc flag 0x4 = cubemap (6 faces per array element), else the array size; legacy caps2 0x200
void dds_faces(const std::vector<std::uint8_t>& dds, GameTexture& out) {
    if (dds.size() >= kDdsHeader && std::memcmp(dds.data() + 84, "DX10", 4) == 0) {
        out.cube = (u32_at(dds, 136) & 0x4u) != 0;
        out.faces = std::max(1u, u32_at(dds, 140)) * (out.cube ? 6u : 1u);
    } else {
        out.cube = (u32_at(dds, 112) & 0x200u) != 0;
        out.faces = out.cube ? 6u : 1u;
    }
}

// the mips of a DDS as they follow its header, face after face
bool dds_mips(const std::vector<std::uint8_t>& dds, GameTexture& out, std::string& error) {
    out.dxgi_format = dds_dxgi(dds);
    out.width = u32_at(dds, 16);
    out.height = u32_at(dds, 12);
    dds_faces(dds, out);
    bool compressed = false;
    const auto block = dxgi_block_bytes(out.dxgi_format, compressed);
    if (!block || !out.width || !out.height) {
        error = "texture uses a pixel format the reader doesn't know (DXGI " + std::to_string(out.dxgi_format) + ")";
        return false;
    }
    const std::uint32_t count = std::max(1u, u32_at(dds, 28));
    std::size_t at = std::memcmp(dds.data() + 84, "DX10", 4) == 0 ? kDdsHeader : 128;
    for (std::uint32_t f = 0; f < out.faces; ++f) {
        std::vector<std::vector<std::uint8_t>> mips;
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto size = mip_bytes(std::max(1u, out.width >> i), std::max(1u, out.height >> i), block, compressed);
            if (at + size > dds.size()) { error = "texture mip is truncated"; return false; }
            mips.emplace_back(dds.begin() + static_cast<std::ptrdiff_t>(at), dds.begin() + static_cast<std::ptrdiff_t>(at + size));
            at += size;
        }
        if (out.faces > 1) out.face_mips.push_back(mips);
        if (f == 0) out.mips = std::move(mips);
    }
    return true;
}

}  // namespace

std::uint32_t dxgi_block_bytes(std::uint32_t format, bool& compressed) {
    compressed = true;
    switch (format) {
        case 70: case 71: case 72: case 79: case 80: case 81: return 8;                       // BC1, BC4
        case 73: case 74: case 75: case 76: case 77: case 78: case 82: case 83: case 84:       // BC2, BC3, BC5
        case 94: case 95: case 96: case 97: case 98: case 99: return 16;                        // BC6H, BC7
        default: break;
    }
    compressed = false;
    switch (format) {
        case 2: return 16;                                  // R32G32B32A32_FLOAT
        case 10: case 11: case 16: return 8;                // R16G16B16A16_FLOAT/_UNORM, R32G32_FLOAT
        case 24: case 26: case 28: case 29: case 34: case 41: case 87: case 91: return 4;
        case 49: case 54: case 56: return 2;                // R8G8_UNORM, R16_FLOAT, R16_UNORM
        case 61: case 65: return 1;                         // R8_UNORM, A8_UNORM
        default: return 0;
    }
}

bool GameTexture::srgb() const { return dxgi_is_srgb(dxgi_format) || (flags & 0x100u); }

bool dxgi_is_srgb(std::uint32_t format) {
    return format == 29 || format == 72 || format == 75 || format == 78 || format == 91 || format == 99;
}

bool read_game_texture(const std::vector<std::uint8_t>& file, const ReadLoose& read_loose, bool full_resolution,
                       GameTexture& out, std::string& error) {
    out = {};
    // the bundle entry: u64 type, u64 name, u32 variants (1), 4 zero bytes, {u32 kind, u8 loose, u32 body size, u8 1,
    // u32 tail size}, body, tail (the stream name). loose 1: the body is the name of a loose file holding the real one
    if (file.size() < 38 || u32_at(file, 16) != 1) { error = "not a single-variant texture resource"; return false; }
    const bool loose_body = file[28] == 1;
    const std::size_t body_size = u32_at(file, 29), tail_size = u32_at(file, 34);
    if (38 + body_size + tail_size > file.size()) { error = "texture resource is truncated"; return false; }
    std::vector<std::uint8_t> body(file.begin() + 38, file.begin() + 38 + static_cast<std::ptrdiff_t>(body_size));
    out.stream_name.assign(reinterpret_cast<const char*>(file.data() + 38 + body_size), tail_size);
    out.stream_name.resize(strnlen(out.stream_name.c_str(), out.stream_name.size()));
    if (loose_body) {
        const std::string loose(reinterpret_cast<const char*>(body.data()), strnlen(reinterpret_cast<const char*>(body.data()), body.size()));
        if (!read_loose(loose, body, error)) return false;
    }
    if (body.size() < 12 + 156) { error = "texture body is truncated"; return false; }

    if (u32_at(body, 0) == 0) {
        // kind 0: a plain DDS with every mip
        const std::vector<std::uint8_t> dds(body.begin() + 12, body.end() - 156);
        if (!dds_mips(dds, out, error)) return false;
        out.flags = u32_at(body, body.size() - 156 + 4);
        out.stream_name.clear();
        return true;
    }
    if (u32_at(body, 0) != 1) { error = "texture body is neither kind 0 nor kind 1"; return false; }

    const std::size_t packed = u32_at(body, 4);
    std::vector<std::uint8_t> dds(u32_at(body, 8));
    if (12 + packed + 20 + 16 * 8 + 8 > body.size() || dds.size() < kDdsHeader) { error = "texture body is truncated"; return false; }
    if (!oodle_decompress(body.data() + 12, packed, dds, error)) return false;
    std::size_t p = 12 + packed;
    if (u32_at(body, p) != 67) { error = "texture mip block is missing"; return false; }
    if (!u32_at(body, p + 12)) {
        // an empty mip block (no size, no table): the DDS header says it all and every mip is resident
        if (!dds_mips(dds, out, error)) return false;
        out.stream_name.clear();
        return true;
    }
    out.flags = u32_at(body, p + 4);
    out.streamed_mips = u32_at(body, p + 8);
    out.width = u32_at(body, p + 12);
    out.height = u32_at(body, p + 16);
    out.dxgi_format = dds_dxgi(dds);
    dds_faces(dds, out);
    bool compressed = false;
    const auto block = dxgi_block_bytes(out.dxgi_format, compressed);
    if (!block || !out.width || !out.height || out.streamed_mips > 16) {
        error = "texture uses a pixel format the reader doesn't know (DXGI " + std::to_string(out.dxgi_format) + ")";
        return false;
    }
    struct Mip { std::uint32_t offset, size; };
    std::vector<Mip> table;
    for (std::size_t i = 0; i < 16; ++i) {
        const Mip mip{u32_at(body, p + 20 + i * 8), u32_at(body, p + 24 + i * 8)};
        if (!mip.size) break;
        table.push_back(mip);
    }
    if (table.size() <= out.streamed_mips) { error = "texture has no resident mips"; return false; }
    p += 20 + 128;
    // a texture that streams nothing has no chunk table: one word and the footer follow the mip table
    const bool chunk_table = body.size() >= p + 12 + 4;
    const std::uint32_t chunks = chunk_table ? u32_at(body, p + 4) : 0;
    p += 12;
    std::vector<std::uint32_t> ends(chunks);
    for (std::uint32_t i = 0; i < chunks; ++i) ends[i] = u32_at(body, p + static_cast<std::size_t>(i) * 4);

    // the streamed mips: Oodle-packed 64 KiB tiles in mip order, each tile 64 block rows of (64 KiB / 64) bytes
    const bool full = full_resolution && out.streamed_mips;
    std::vector<std::uint8_t> stream;
    if (full) {
        if (out.stream_name.empty()) { error = "texture streams mips but names no stream file"; return false; }
        if (!read_loose(out.stream_name, stream, error)) return false;
        if (!compressed) { error = "streamed texture is not block compressed"; return false; }
        std::size_t chunk = 0;
        std::vector<std::uint8_t> tile(kChunk);
        for (std::uint32_t m = 0; m < out.streamed_mips; ++m) {
            const std::uint32_t width = std::max(1u, out.width >> m), height = std::max(1u, out.height >> m);
            const std::size_t blocks_x = (width + 3) / 4, blocks_y = (height + 3) / 4, row = blocks_x * block;
            const std::size_t tile_row = kChunk / 64, tile_blocks_x = tile_row / block;
            if (table[m].size != row * blocks_y || blocks_x % tile_blocks_x || blocks_y % 64) {
                error = "streamed mip doesn't divide into 64 KiB tiles";
                return false;
            }
            std::vector<std::uint8_t> surface(table[m].size);
            for (std::size_t by = 0; by < blocks_y; by += 64)
                for (std::size_t bx = 0; bx < blocks_x; bx += tile_blocks_x, ++chunk) {
                    if (chunk >= chunks) { error = "texture stream has fewer chunks than its mips need"; return false; }
                    const std::size_t from = chunk ? ends[chunk - 1] : 0;
                    if (ends[chunk] > stream.size() || ends[chunk] <= from) { error = "texture stream is shorter than its chunk table"; return false; }
                    tile.assign(kChunk, 0);
                    if (!oodle_decompress(stream.data() + from, ends[chunk] - from, tile, error)) return false;
                    for (std::size_t r = 0; r < 64; ++r)
                        std::memcpy(surface.data() + (by + r) * row + bx * block, tile.data() + r * tile_row, tile_row);
                }
            out.mips.push_back(std::move(surface));
        }
        if (chunk != chunks || (chunks && ends.back() != stream.size())) {
            error = "texture stream holds more than its mips use";
            return false;
        }
    }
    // the resident mips, face after face (the table gives the first face's; each face takes their total)
    if (out.faces > 1 && out.streamed_mips) { error = "cubemap or array texture streams mips"; return false; }
    std::size_t face_bytes = 0;
    for (std::size_t m = out.streamed_mips; m < table.size(); ++m) face_bytes = std::max<std::size_t>(face_bytes, table[m].offset + table[m].size);
    for (std::uint32_t f = 0; f < out.faces; ++f) {
        std::vector<std::vector<std::uint8_t>> mips;
        for (std::size_t m = out.streamed_mips; m < table.size(); ++m) {
            const std::size_t at = kDdsHeader + f * face_bytes + table[m].offset;
            if (at + table[m].size > dds.size()) { error = "texture mip is truncated"; return false; }
            mips.emplace_back(dds.begin() + static_cast<std::ptrdiff_t>(at), dds.begin() + static_cast<std::ptrdiff_t>(at + table[m].size));
        }
        if (out.faces > 1) out.face_mips.push_back(mips);
        if (f == 0) for (auto& mip : mips) out.mips.push_back(std::move(mip));
    }
    if (!full) {
        // only the resident mips: the image starts at the first of them
        out.width = std::max(1u, out.width >> out.streamed_mips);
        out.height = std::max(1u, out.height >> out.streamed_mips);
        out.streamed_mips = 0;
    }
    return true;
}

}  // namespace dtglb::stingray::texture
