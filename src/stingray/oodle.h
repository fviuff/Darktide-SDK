#pragma once
// The game's Oodle DLL (binaries/oo2core_9_win64.dll; Oodle can't be shipped) and the Darktide install folder.
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace dtglb::stingray::texture {

bool configure_oodle(const std::filesystem::path& dll_or_game_directory, std::string& error);

// Oodle Kraken compression with the game's DLL (checked by unpacking it again).
bool oodle_compress(const std::vector<std::uint8_t>& input, std::vector<std::uint8_t>& packed, std::string& error,
                    const char* label = "shader");
// Oodle decompression with the game's DLL; output is sized to the unpacked size beforehand.
bool oodle_decompress(const std::vector<std::uint8_t>& packed, std::vector<std::uint8_t>& output, std::string& error);
bool oodle_decompress(const std::uint8_t* packed, std::size_t size, std::vector<std::uint8_t>& output, std::string& error);
// at most output.size() bytes (bundle chunks), output cut to what came out
bool oodle_decompress_up_to(const std::uint8_t* packed, std::size_t size, std::vector<std::uint8_t>& output, std::string& error);

// The Darktide install (DARKTIDE_GAME_ROOT, else from the configured Oodle DLL, else the default Steam folder).
// Empty when unknown. Retail shader materials are read from <root>/bundle at compile time.
std::filesystem::path game_root();

} // namespace dtglb::stingray::texture
