#include "stingray/oodle.h"

#include <fstream>
#include <limits>
#include <stdexcept>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace dtglb::stingray::texture {
namespace {

#ifdef _WIN32
HMODULE load_oodle_module(const std::filesystem::path& path, DWORD* failure = nullptr) {
    DWORD old_mode = 0;
    const bool changed = SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX,
                                            &old_mode) != 0;
    HMODULE module = LoadLibraryW(path.c_str());
    const DWORD load_error = module ? ERROR_SUCCESS : GetLastError();
    if (changed) SetThreadErrorMode(old_mode, nullptr);
    if (failure) *failure = load_error;
    return module;
}

std::filesystem::path oodle_config_file() {
    const auto env_path = [](const wchar_t* name) {
        const DWORD needed = GetEnvironmentVariableW(name, nullptr, 0);
        if (!needed) return std::filesystem::path{};
        std::vector<wchar_t> value(needed);
        const DWORD copied = GetEnvironmentVariableW(name, value.data(), needed);
        return copied && copied < needed ? std::filesystem::path(value.data()) : std::filesystem::path{};
    };
    auto root = env_path(L"DARKTIDE_OODLE_CONFIG_DIR");
    if (root.empty()) root = env_path(L"LOCALAPPDATA");
    if (root.empty()) return {};
    if (GetEnvironmentVariableW(L"DARKTIDE_OODLE_CONFIG_DIR", nullptr, 0)) return root / L"oodle_path.txt";
    return root / L"DarktideGLBCompiler" / L"oodle_path.txt";
}

std::filesystem::path oodle_dll_from_input(const std::filesystem::path& input) {
    std::error_code ec;
    if (std::filesystem::is_directory(input, ec)) return input / L"binaries" / L"oo2core_9_win64.dll";
    return input;
}

bool valid_oodle_dll(const std::filesystem::path& path, std::string& error) {
    DWORD load_error = ERROR_SUCCESS;
    HMODULE module = load_oodle_module(path, &load_error);
    if (!module) {
        error = "cannot load Oodle DLL at " + path.string() + " (Windows error " + std::to_string(load_error) + ")";
        return false;
    }
    const bool exports = GetProcAddress(module, "OodleLZ_GetCompressedBufferSizeNeeded") &&
        GetProcAddress(module, "OodleLZ_Compress") && GetProcAddress(module, "OodleLZ_Decompress");
    FreeLibrary(module);
    if (!exports) error = "Oodle DLL is missing required exports: OodleLZ_GetCompressedBufferSizeNeeded, OodleLZ_Compress, OodleLZ_Decompress";
    return exports;
}

class DarktideOodle {
public:
    using BoundFn = std::size_t (WINAPI*)(int, long long);
    using CompressFn = long long (WINAPI*)(int, const void*, long long, void*, int, const void*, const void*, const void*, void*, long long);
    using DecompressFn = long long (WINAPI*)(const void*, long long, void*, long long, int, int, int, void*, long long, void*, void*, void*, long long, int);

    ~DarktideOodle() { if (module_) FreeLibrary(module_); }
    DarktideOodle(const DarktideOodle&) = delete;
    DarktideOodle& operator=(const DarktideOodle&) = delete;
    DarktideOodle() = default;

    bool load(std::string& error) {
        std::vector<std::filesystem::path> candidates;
        std::string candidate_error;
        auto env_path = [](const wchar_t* name) -> std::filesystem::path {
            const DWORD needed = GetEnvironmentVariableW(name, nullptr, 0);
            if (!needed) return {};
            std::vector<wchar_t> value(needed);
            const DWORD copied = GetEnvironmentVariableW(name, value.data(), needed);
            if (!copied || copied >= needed) return {};
            return std::filesystem::path(value.data());
        };
        const auto explicit_dll = env_path(L"DARKTIDE_OODLE_DLL");
        const auto game_root = env_path(L"DARKTIDE_GAME_ROOT");
        const bool explicit_override = !explicit_dll.empty() || !game_root.empty();
        if (!explicit_dll.empty()) {
            candidates.push_back(explicit_dll);
        } else if (!game_root.empty()) {
            candidates.push_back(game_root / L"binaries" / L"oo2core_9_win64.dll");
        } else {
            std::vector<wchar_t> executable_path(32768);
            const DWORD executable_length = GetModuleFileNameW(nullptr, executable_path.data(),
                static_cast<DWORD>(executable_path.size()));
            if (executable_length && executable_length < executable_path.size()) {
                candidates.push_back(std::filesystem::path(executable_path.data(), executable_path.data() + executable_length)
                    .parent_path() / L"oo2core_9_win64.dll");
            }
            const auto config = oodle_config_file();
            if (!config.empty()) {
                std::ifstream saved(config, std::ios::binary);
                std::string utf8_path;
                if (saved && std::getline(saved, utf8_path) && !utf8_path.empty()) {
                    try {
                        if (utf8_path.find('\0') != std::string::npos) throw std::runtime_error("path contains NUL");
                        candidates.push_back(std::filesystem::u8path(utf8_path));
                    } catch (const std::exception&) {
                        candidate_error = "Saved Oodle path is invalid UTF-8; rerun --configure-oodle with the game's binaries folder or DLL";
                    }
                }
            }
            // cant ship oodle ourselves so just go look where steam usually puts the game
            const auto program_files = env_path(L"ProgramFiles(x86)");
            if (!program_files.empty()) candidates.push_back(program_files / L"Steam" / L"steamapps" / L"common" / L"Warhammer 40,000 DARKTIDE" / L"binaries" / L"oo2core_9_win64.dll");
        }

        for (const auto& raw_candidate : candidates) {
            const auto candidate = oodle_dll_from_input(raw_candidate);
            DWORD load_error = ERROR_SUCCESS;
            module_ = load_oodle_module(candidate, &load_error);
            if (!module_) {
                if (explicit_override) candidate_error = "Explicit Oodle location could not be loaded at " + candidate.string() +
                    " (Windows error " + std::to_string(load_error) + ")";
                continue;
            }
            bound_ = reinterpret_cast<BoundFn>(GetProcAddress(module_, "OodleLZ_GetCompressedBufferSizeNeeded"));
            compress_ = reinterpret_cast<CompressFn>(GetProcAddress(module_, "OodleLZ_Compress"));
            decompress_ = reinterpret_cast<DecompressFn>(GetProcAddress(module_, "OodleLZ_Decompress"));
            if (bound_ && compress_ && decompress_) return true;
            candidate_error = "Oodle DLL is missing required exports at " + candidate.string();
            FreeLibrary(module_); module_ = nullptr;
        }
        error = "Darktide Oodle DLL was not found or could not be loaded. Run --configure-oodle <game directory or oo2core_9_win64.dll>; "
            "or set DARKTIDE_OODLE_DLL / DARKTIDE_GAME_ROOT. Expected oo2core_9_win64.dll under the game's binaries directory.";
        if (explicit_override) error = candidate_error + ". Correct DARKTIDE_OODLE_DLL or DARKTIDE_GAME_ROOT and retry.";
        else if (!candidate_error.empty()) error = candidate_error + ". Reconfigure with the installed game's oo2core_9_win64.dll.";
        return false;
    }

    bool compress_roundtrip(const std::vector<std::uint8_t>& input, std::vector<std::uint8_t>& packed, const char* label, std::string& error) const {
        if (input.empty() || input.size() > static_cast<std::size_t>(std::numeric_limits<long long>::max())) { error = std::string("invalid Oodle input for ") + label; return false; }
        constexpr int kKraken = 8;
        constexpr int kNormal = 4;
        const auto bound = bound_(kKraken, static_cast<long long>(input.size()));
        if (!bound) { error = std::string("Oodle returned an invalid compression bound for ") + label; return false; }
        std::vector<std::uint8_t> output(bound);
        const auto written = compress_(kKraken, input.data(), static_cast<long long>(input.size()), output.data(), kNormal, nullptr, nullptr, nullptr, nullptr, 0);
        if (written <= 0 || static_cast<std::size_t>(written) > output.size()) { error = std::string("Oodle compression failed for ") + label; return false; }
        output.resize(static_cast<std::size_t>(written));
        std::vector<std::uint8_t> restored(input.size());
        const auto decoded = decompress_(output.data(), written, restored.data(), static_cast<long long>(restored.size()), 1, 1, 0, nullptr, 0, nullptr, nullptr, nullptr, 0, 3);
        if (decoded != static_cast<long long>(input.size()) || restored != input) { error = std::string("Oodle round-trip validation failed for ") + label; return false; }
        packed = std::move(output);
        return true;
    }

    bool decompress(const std::uint8_t* input, std::size_t size, std::vector<std::uint8_t>& output, std::string& error) const {
        const auto decoded = decompress_(input, static_cast<long long>(size), output.data(), static_cast<long long>(output.size()),
                                         1, 0, 0, nullptr, 0, nullptr, nullptr, nullptr, 0, 3);
        if (decoded != static_cast<long long>(output.size())) { error = "Oodle could not unpack the texture"; return false; }
        return true;
    }

    // up to output.size() bytes; output is cut to what was unpacked
    bool decompress_up_to(const std::uint8_t* input, std::size_t size, std::vector<std::uint8_t>& output) const {
        const auto decoded = decompress_(input, static_cast<long long>(size), output.data(), static_cast<long long>(output.size()),
                                         1, 0, 0, nullptr, 0, nullptr, nullptr, nullptr, 0, 3);
        if (decoded <= 0 || decoded > static_cast<long long>(output.size())) return false;
        output.resize(static_cast<std::size_t>(decoded));
        return true;
    }

private:
    HMODULE module_ = nullptr;
    BoundFn bound_ = nullptr;
    CompressFn compress_ = nullptr;
    DecompressFn decompress_ = nullptr;
};

// the game's DLL, loaded once for the process (loading it is slow, bundles need it for every chunk)
const DarktideOodle* loaded_oodle(std::string& error) {
    static std::string load_error;
    static const DarktideOodle* oodle = [] {
        auto* instance = new DarktideOodle();
        if (instance->load(load_error)) return const_cast<const DarktideOodle*>(instance);
        delete instance;
        return static_cast<const DarktideOodle*>(nullptr);
    }();
    if (!oodle) error = load_error;
    return oodle;
}
#endif

} // namespace

bool oodle_compress(const std::vector<std::uint8_t>& input, std::vector<std::uint8_t>& packed, std::string& error,
                    const char* label) {
#ifdef _WIN32
    const DarktideOodle* oodle = loaded_oodle(error);
    return oodle && oodle->compress_roundtrip(input, packed, label, error);
#else
    (void)input; (void)packed; (void)label;
    error = "Oodle compression requires Darktide's Windows Oodle DLL (unsupported on this platform)";
    return false;
#endif
}

bool oodle_decompress(const std::vector<std::uint8_t>& packed, std::vector<std::uint8_t>& output, std::string& error) {
#ifdef _WIN32
    const DarktideOodle* oodle = loaded_oodle(error);
    if (!oodle) return false;
    if (!oodle->decompress(packed.data(), packed.size(), output, error)) { error = "Oodle could not unpack the data"; return false; }
    return true;
#else
    (void)packed; (void)output;
    error = "Oodle decompression requires Darktide's Windows Oodle DLL (unsupported on this platform)";
    return false;
#endif
}

bool oodle_decompress(const std::uint8_t* packed, std::size_t size, std::vector<std::uint8_t>& output, std::string& error) {
#ifdef _WIN32
    const DarktideOodle* oodle = loaded_oodle(error);
    if (!oodle) return false;
    if (!oodle->decompress(packed, size, output, error)) { error = "Oodle could not unpack the data"; return false; }
    return true;
#else
    (void)packed; (void)size; (void)output;
    error = "Oodle decompression requires Darktide's Windows Oodle DLL (unsupported on this platform)";
    return false;
#endif
}

bool oodle_decompress_up_to(const std::uint8_t* packed, std::size_t size, std::vector<std::uint8_t>& output, std::string& error) {
#ifdef _WIN32
    const DarktideOodle* oodle = loaded_oodle(error);
    if (!oodle) return false;
    if (!oodle->decompress_up_to(packed, size, output)) { error = "Oodle could not unpack the data"; return false; }
    return true;
#else
    (void)packed; (void)size; (void)output;
    error = "Oodle decompression requires Darktide's Windows Oodle DLL (unsupported on this platform)";
    return false;
#endif
}

bool configure_oodle(const std::filesystem::path& dll_or_game_directory, std::string& error) {
#ifdef _WIN32
    const auto dll = oodle_dll_from_input(dll_or_game_directory);
    if (!valid_oodle_dll(dll, error)) return false;
    const auto config = oodle_config_file();
    if (config.empty()) {
        error = "LOCALAPPDATA is unavailable; set DARKTIDE_OODLE_CONFIG_DIR to choose a configuration directory";
        return false;
    }
    std::error_code ec;
    const auto absolute = std::filesystem::absolute(dll, ec).lexically_normal();
    if (ec) { error = "could not resolve Oodle DLL path: " + ec.message(); return false; }
    std::filesystem::create_directories(config.parent_path(), ec);
    if (ec) { error = "could not create Oodle configuration directory: " + ec.message(); return false; }
    const auto encoded = absolute.u8string();
    const auto temporary = config.parent_path() / (L"oodle_path.txt.tmp." + std::to_wstring(GetCurrentProcessId()));
    std::ofstream saved(temporary, std::ios::binary | std::ios::trunc);
    if (!saved || !saved.write(reinterpret_cast<const char*>(encoded.data()), static_cast<std::streamsize>(encoded.size())) || !saved.put('\n')) {
        saved.close();
        std::filesystem::remove(temporary, ec);
        error = "could not write temporary Oodle configuration file: " + config.parent_path().string();
        return false;
    }
    saved.flush();
    saved.close();
    if (!saved) {
        std::filesystem::remove(temporary, ec);
        error = "could not finish writing temporary Oodle configuration file: " + config.parent_path().string();
        return false;
    }
    if (!MoveFileExW(temporary.c_str(), config.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD move_error = GetLastError();
        std::filesystem::remove(temporary, ec);
        error = "could not replace Oodle configuration file (Windows error " + std::to_string(move_error) + ")";
        return false;
    }
    return true;
#else
    (void)dll_or_game_directory;
    error = "Oodle configuration is available only on Windows";
    return false;
#endif
}

std::filesystem::path game_root() {
#ifdef _WIN32
    // same places the oodle dll is looked up, the dll lives in <game>/binaries
    const auto env_path = [](const wchar_t* name) -> std::filesystem::path {
        const DWORD needed = GetEnvironmentVariableW(name, nullptr, 0);
        if (!needed) return {};
        std::vector<wchar_t> value(needed);
        const DWORD copied = GetEnvironmentVariableW(name, value.data(), needed);
        return copied && copied < needed ? std::filesystem::path(value.data()) : std::filesystem::path{};
    };
    std::error_code ec;
    if (auto root = env_path(L"DARKTIDE_GAME_ROOT"); !root.empty()) return root;
    if (const auto config = oodle_config_file(); !config.empty()) {
        std::ifstream saved(config, std::ios::binary);
        std::string utf8_path;
        if (saved && std::getline(saved, utf8_path) && !utf8_path.empty() && utf8_path.find('\0') == std::string::npos) {
            const auto dll = std::filesystem::u8path(utf8_path);
            const auto root = dll.parent_path().parent_path();
            if (std::filesystem::is_directory(root / L"bundle", ec)) return root;
        }
    }
    if (const auto program_files = env_path(L"ProgramFiles(x86)"); !program_files.empty())
        return program_files / L"Steam" / L"steamapps" / L"common" / L"Warhammer 40,000 DARKTIDE";
#endif
    return {};
}

} // namespace dtglb::stingray::texture
