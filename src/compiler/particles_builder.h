#pragma once
#include "compiler/compilation_context.h"
#include "compiler/resource_graph.h"
#include "scene/scene.h"

namespace dtglb::compiler {
// Each of the scene's particle effects becomes a renamed copy of the game's effect. Its materials, their shader
// provider/parent and their textures are shipped with the asset too (edited where the author asked), so the
// effect works wherever the asset is loaded. Adds the effects to `effects` (they are graph roots).
bool build_particle_effects(const Scene& scene, const CompilationContext& context, ResourceGraph& graph,
                            std::vector<ResourceKey>& effects, std::string& error);
// The body (and stream) of one of the game's cooked resources in a limn extract (<extract>/<16 hex>.<type>); a stub
// naming a loose file under <game>/bundle is replaced by that file.
bool read_game_resource(const std::string& extract, std::uint64_t hash, const char* type,
                        std::vector<std::uint8_t>& body, std::string& stream, std::string& error);
}
