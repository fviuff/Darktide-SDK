#pragma once
#include "compiler/compilation_context.h"
#include "compiler/resource_graph.h"
#include "scene/scene.h"

namespace dtglb::compiler {
// The asset's loose textures (AssetDefinition::loose_textures): images no material of the asset uses, shipped for
// scripts to put into material slots (Unit.set_texture_for_materials). Each is encoded as its kind says (colour,
// colour with alpha, normal map, linear data) or exactly like one of the game's textures, the one it stands in for.
// Adds them to `textures` (they are graph roots).
bool build_loose_textures(const Scene& scene, const CompilationContext& context, ResourceGraph& graph,
                          std::vector<ResourceKey>& textures, std::string& error);
}
