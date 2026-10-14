#pragma once

#include "compiler/resource_key.h"
#include "compiler/target_profile.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace dtglb::compiler {

class CompilationContext {
public:
    CompilationContext() = default;

    ResourceKey generated_key(std::string type, std::string local_name) const;

    const std::string& file_base() const noexcept { return file_base_; }
    const TargetProfile& target_profile() const noexcept { return *target_profile_; }

private:
    friend bool make_compilation_context(const std::string&, CompilationContext&, std::string&,
                                         const std::optional<std::string>&);
    explicit CompilationContext(std::string file_base)
        : file_base_(std::move(file_base)), target_profile_(&darktide_target_profile()) {}

    std::string file_base_;
    std::optional<std::string> resource_directory_;
    const TargetProfile* target_profile_ = &darktide_target_profile();
};

bool make_compilation_context(const std::string& input_stem,
                              CompilationContext& out,
                              std::string& error,
                              const std::optional<std::string>& asset_path = std::nullopt);

// Copies of game resources (shader materials, effect materials, textures, units) are named after the game resource
// and a digest of their final bytes, the same in every asset and every mod: assets that need the same copy carry
// identical files under one name, and the game loads it once however many packages list it. local_name is the kind
// and game hash (e.g. shader_<hash>); bytes are the copy as written, with any field holding its own name zeroed.
std::string game_copy_stem(const std::string& local_name, const std::vector<std::uint8_t>& bytes);
ResourceKey game_copy_key(std::string type, const std::string& stem);

} // namespace dtglb::compiler
