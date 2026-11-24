#pragma once
// Reads a game texture (or one we wrote) back to its stored pixel blocks, every mip at full resolution.
//
// Body kind 1: u32 1, u32 packed size, u32 DDS size, the Oodle-packed DDS (148-byte DX10 header + the resident mips),
// then u32 67, u32 flags, u32 streamed mip count, u32 width, u32 height, 16 mips {offset, size} (streamed mips count
// from the start of the stream data, resident ones from the end of the DDS header), 128 bytes, u32 8 + 4 chunks,
// u32 chunks, u16 0, u16 chunks, the chunks' cumulative packed ends, u32 footer. The streamed mips live in the
// texture's .stream file as Oodle-packed 64 KiB chunks, mip by mip, each chunk a tile of 64 block rows by 64 KiB / 64
// rows (128 blocks of 8 bytes or 64 of 16). Kind 0: three zero words and a plain DDS holding every mip.
// Cubemaps and arrays hold their faces one after another, each with its full mip chain (the mip table describes one face).
// The blocks are returned as stored (BCn stays compressed; sRGB stays sRGB).
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace dtglb::stingray::texture {

struct GameTexture {
    std::uint32_t dxgi_format = 0;  // DXGI_FORMAT value
    std::uint32_t width = 0, height = 0;
    std::vector<std::vector<std::uint8_t>> mips;  // largest first; only the resident ones without the stream
    // cubemaps (6 faces +X -X +Y -Y +Z -Z, per array element) and arrays: every face's mips (face_mips[0] = mips),
    // empty for a plain texture
    std::uint32_t faces = 1;
    bool cube = false;
    std::vector<std::vector<std::vector<std::uint8_t>>> face_mips;
    std::string stream_name;                      // data/xx/<hash>.stream, empty when everything is resident
    std::uint32_t streamed_mips = 0;              // how many of the largest mips are in the stream
    std::uint32_t flags = 0;                      // the mip block's flags: 0x100 = sampled as sRGB
    // the engine samples a texture as sRGB when its flags say so (most colour maps store a plain UNORM format)
    bool srgb() const;
};

// reads a file under the game's bundle folder (data/xx/...)
using ReadLoose = std::function<bool(const std::string& relative, std::vector<std::uint8_t>& data, std::string& error)>;

// file: the texture resource as the bundle holds it. Its body may name a loose file that holds the real body; the streamed
// mips come from the .stream file the resource names. full_resolution false reads only the resident mips.
bool read_game_texture(const std::vector<std::uint8_t>& file, const ReadLoose& read_loose, bool full_resolution,
                       GameTexture& out, std::string& error);

// bytes per 4x4 block (BCn) or per pixel; 0 = unknown format
std::uint32_t dxgi_block_bytes(std::uint32_t format, bool& compressed);
bool dxgi_is_srgb(std::uint32_t format);

}  // namespace dtglb::stingray::texture
