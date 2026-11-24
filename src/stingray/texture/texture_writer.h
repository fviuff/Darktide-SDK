#pragma once
#include "stingray/oodle.h"
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace dtglb::stingray::texture {

enum class TextureFiltering { ColorLinear, ColorSrgb, Normal };

struct TextureProfile {
    std::string id;
    std::uint32_t dxgi_format = 0;
    std::uint32_t block_bytes = 0;
    bool srgb = false;
    std::uint32_t body_flags = 0;
    std::uint32_t footer_word = 0;
    TextureFiltering filtering = TextureFiltering::ColorLinear;};

const TextureProfile& substance_basic_bc_profile();
// Base color plus alpha used by the masked/transparent families.
const TextureProfile& substance_basic_bca_profile();
const TextureProfile& substance_basic_nm_profile();
const TextureProfile& substance_basic_orm_profile();
// Linear BC1 emission used by the PBR-emissive family.
const TextureProfile& pbr_emissive_em_profile();

struct ImageRGBA {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> pixels;
};

struct CookedTexture {
    std::string resource_name;
    std::string stream_name;
    std::filesystem::path texture_path;
    std::filesystem::path stream_path;
    std::string profile;
    std::uint32_t source_width = 0;
    std::uint32_t source_height = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t mip_count = 0;
    std::uint32_t streamed_mip_count = 0;
    std::uint32_t chunk_count = 0;
    std::uint32_t resident_dds_bytes = 0;
    bool dimensions_normalized = false;
};

bool decode_image_rgba(const std::vector<std::uint8_t>& bytes, const std::string& mime_type, ImageRGBA& out, std::string& error);
bool normalize_image_for_family(const ImageRGBA& source, const TextureProfile& profile, ImageRGBA& out, bool& changed, std::string& error);
bool normalize_image_for_family(const ImageRGBA& source, ImageRGBA& out, bool& changed, std::string& error);

std::string make_texture_stream_name(const std::string& resource_name);



bool write_native_texture_rgba(
    const ImageRGBA& source,
    const TextureProfile& profile,
    const std::string& resource_name,
    const std::filesystem::path& texture_path,
    const std::filesystem::path& stream_path,
    CookedTexture& out,
    std::string& error);

// The game's texture bodies come as kind 1 (Oodle-packed DDS) or kind 0 (plain DDS, no chunk table);
// returns kind 1 either way, which is what the Custom Assets patcher takes.
bool texture_body_as_kind1(const std::vector<std::uint8_t>& body, std::vector<std::uint8_t>& out, std::string& error);

// Encode an image as a texture body laid out exactly like one of the game's textures (its kind 0 or kind 1 body):
// same size, mip count, pixel format and footer, so it can stand in for it in a material. Block-compressed mips above
// 32 KiB go to *stream (streamed when on screen, like the compiler's own textures); empty when nothing streams.
bool encode_texture_like(const ImageRGBA& source, const std::vector<std::uint8_t>& game_body,
                         std::vector<std::uint8_t>& body, std::vector<std::uint8_t>& stream, std::string& error);

bool inspect_texture_blob(const std::vector<std::uint8_t>& blob, std::string& report, std::string& error);

// The largest mip a game texture body holds itself (the streamed mips live in a separate file), as 8-bit RGBA
// holding the stored values (sRGB formats stay sRGB encoded; *srgb says which).
bool texture_body_image(const std::vector<std::uint8_t>& body, ImageRGBA& out, std::string& error, bool* srgb = nullptr);
// one mip's stored blocks/pixels (DXGI format) as RGBA8, sRGB bytes kept
bool mip_image(std::uint32_t dxgi_format, std::uint32_t width, std::uint32_t height, const std::vector<std::uint8_t>& data,
               ImageRGBA& out, std::string& error);

bool write_png(const ImageRGBA& image, const std::filesystem::path& path, std::string& error);

} // namespace dtglb::stingray::texture
