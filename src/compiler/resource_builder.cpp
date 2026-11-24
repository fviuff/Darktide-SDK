#include "compiler/resource_builder.h"
#include "compiler/material_builder.h"
#include "compiler/loose_textures.h"
#include "compiler/particles_builder.h"
#include "compiler/source_queries.h"
#include "stingray/unit/unit_v115.h"
#include "stingray/bones/bones_writer.h"
#include "stingray/murmur_hash.h"
#include "stingray/resource_name.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <cmath>
#include <map>
#include <set>

namespace dtglb::compiler {
namespace {
bool remap_joint_channels(std::vector<VertexChannel>& channels,
                          const std::vector<std::uint32_t>& source_to_reference,
                          std::string& error) {
    for (auto& channel : channels) {
        if (channel.semantic != VertexChannel::Semantic::BlendIndices) continue;
        for (float& value : channel.values) {
            if (!std::isfinite(value) || value < 0.0f ||
                value >= static_cast<float>(source_to_reference.size()) || std::floor(value) != value) {
                error = "reference BONES remap found a vertex joint index outside the source skin palette";
                return false;
            }
            value = static_cast<float>(source_to_reference[static_cast<std::size_t>(value)]);
        }
    }
    return true;
}

bool apply_reference_bones(const Scene& source, std::size_t skin_index,
                           const app::CompileOptions& options, Scene& output,
                           ResourceKey& reference_key, bool& subset, std::string& error) {
    subset = false;
    if (!options.reference_bones_file || !options.skeleton_resource) {
        error = "reference BONES requires both a file and explicit skeleton resource identity";
        return false;
    }
    reference_key = {"bones", *options.skeleton_resource};
    if (!reference_key.valid()) { error = "invalid --skeleton-resource identity"; return false; }
    stingray::bones::BonesResource reference;
    if (!stingray::bones::load_cooked_bones(*options.reference_bones_file,
                                            reference_key.name, reference, error)) return false;
    if (skin_index >= source.skins.size()) { error = "reference BONES selected skin index is out of range"; return false; }
    output = source;
    auto& skin = output.skins[skin_index];
    const auto& source_skin = source.skins[skin_index];
    const auto source_names = canonical_bone_names(source_skin);
    if (source_skin.joint_names.size() != source_skin.joints.size() || source_names != source_skin.joint_names) {
        error = "reference BONES requires complete, unique source joint names that are already valid native bone identities";
        for (std::size_t i = 0; i < source_names.size(); ++i)
            if (i >= source_skin.joint_names.size() || source_names[i] != source_skin.joint_names[i]) {
                error += " (\"" + (i < source_skin.joint_names.size() ? source_skin.joint_names[i] : std::string()) +
                         "\" would have to be \"" + source_names[i] + "\")";
                break;
            }
        return false;
    }
    std::vector<std::uint32_t> source_to_reference;
    std::vector<std::string> target_names = reference.names;
    std::size_t matched = 0;
    for (const auto& name : source_names)
        if (std::find(reference.names.begin(), reference.names.end(), name) != reference.names.end()) ++matched;
    if (!source_names.empty() && matched == 0) {
        error = "no source skin joint matches reference BONES (" + std::to_string(reference.names.size()) +
            " names, e.g. " + reference.names.front() + "); fit the rig to the skeleton or pick another one";
        return false;
    }
    if (!source_names.empty() && (matched < reference.names.size() || matched < source_names.size())) {
        // same as how the game does gloves and such, they all ship their own tiny bones file
        // Retail-gear style: an owned BONES with the matched joints in reference order,
        // followed by the rig's own extra joints (cloth, jiggle, holders) in source order.
        // Matched joints follow the parent's same-named nodes when linked; extras ride along.
        subset = true;
        std::vector<std::uint32_t> matched_reference;
        for (const auto& name : source_names) {
            const auto found = std::find(reference.names.begin(), reference.names.end(), name);
            if (found != reference.names.end())
                matched_reference.push_back(static_cast<std::uint32_t>(found - reference.names.begin()));
        }
        std::vector<std::uint32_t> sorted = matched_reference;
        std::sort(sorted.begin(), sorted.end());
        target_names.clear();
        for (const auto index : sorted) target_names.push_back(reference.names[index]);
        std::uint32_t next_extra = static_cast<std::uint32_t>(sorted.size());
        for (const auto& name : source_names) {
            const auto found = std::find(reference.names.begin(), reference.names.end(), name);
            if (found == reference.names.end()) {
                target_names.push_back(name);
                source_to_reference.push_back(next_extra++);
            } else {
                const auto index = static_cast<std::uint32_t>(found - reference.names.begin());
                source_to_reference.push_back(static_cast<std::uint32_t>(
                    std::lower_bound(sorted.begin(), sorted.end(), index) - sorted.begin()));
            }
        }
    } else if (!stingray::bones::map_bone_names(source_names, reference.names, source_to_reference, error)) {
        return false;
    }

    const auto reorder = [&](const auto& values, auto& mapped) {
        if (values.empty()) { mapped.clear(); return true; }
        if (values.size() != source_to_reference.size()) return false;
        mapped.resize(values.size());
        for (std::size_t old = 0; old < values.size(); ++old) mapped[source_to_reference[old]] = values[old];
        return true;
    };
    std::vector<int> joints;
    std::vector<Matrix4> inverse_gltf, inverse_stingray;
    if (!reorder(skin.joints, joints) ||
        !reorder(skin.inverse_bind_matrices_gltf, inverse_gltf) ||
        !reorder(skin.inverse_bind_matrices_stingray, inverse_stingray)) {
        error = "reference BONES remap requires inverse-bind arrays to match the complete source skin palette";
        return false;
    }
    skin.joints = std::move(joints);
    skin.joint_names = target_names;
    if (canonical_bone_names(skin) != target_names) {
        error = "reference BONES names are not representable as exact UNIT joint identities";
        return false;
    }
    skin.inverse_bind_matrices_gltf = std::move(inverse_gltf);
    skin.inverse_bind_matrices_stingray = std::move(inverse_stingray);
    for (auto& primitive : output.primitives) {
        if (primitive.source_node < 0 || static_cast<std::size_t>(primitive.source_node) >= output.nodes.size() ||
            output.nodes[static_cast<std::size_t>(primitive.source_node)].skin != static_cast<int>(skin_index)) continue;
        if (!remap_joint_channels(primitive.source_channels, source_to_reference, error) ||
            !remap_joint_channels(primitive.channels, source_to_reference, error)) return false;
    }
    return true;
}

Scene with_placeholder_geometry(const Scene& source) {
    Scene lowered = source;
    Primitive primitive;
    primitive.name = "__empty_scene_placeholder__";
    primitive.material_name = "__no_material__";
    primitive.source_node = -1;
    primitive.mode = 4;
    primitive.indices = {0, 1, 2};
    primitive.channels = {
        {VertexChannel::Semantic::Position, 0, 3, {0,0,0, 0,0,0, 0,0,0}},
        {VertexChannel::Semantic::Normal, 0, 3, {0,0,1, 0,0,1, 0,0,1}},
        {VertexChannel::Semantic::Tangent, 0, 4, {1,0,0,1, 1,0,0,1, 1,0,0,1}},
        {VertexChannel::Semantic::Texcoord, 0, 2, {0,0, 0,0, 0,0}},
    };
    primitive.source_channels = primitive.channels;
    lowered.primitives.push_back(std::move(primitive));
    return lowered;
}

// A skeleton that only carries animation: the UNIT writer builds BONES, nodes and the simple animation from the
// skinned meshes, so the rig gets one zero-area triangle on its root bone (never drawn) to carry its skin.
Scene with_skeleton_carrier(const Scene& source, std::size_t skin_index) {
    Scene lowered = source;
    const auto& skin = source.skins[skin_index];
    int owner = -1;
    for (std::size_t i = 0; i < source.nodes.size() && owner < 0; ++i)
        if (source.nodes[i].skin == static_cast<int>(skin_index)) owner = static_cast<int>(i);
    const auto& root = source.nodes[static_cast<std::size_t>(skin.joints.front())].world_stingray;
    const float x = root[12], y = root[13], z = root[14];
    Primitive primitive;
    primitive.name = "__skeleton_carrier__";
    primitive.material_name = "__no_material__";
    primitive.source_node = owner;
    primitive.mode = 4;
    primitive.indices = {0, 1, 2};
    primitive.channels = {
        {VertexChannel::Semantic::Position, 0, 3, {x,y,z, x,y,z, x,y,z}},
        {VertexChannel::Semantic::Normal, 0, 3, {0,0,1, 0,0,1, 0,0,1}},
        {VertexChannel::Semantic::Tangent, 0, 4, {1,0,0,1, 1,0,0,1, 1,0,0,1}},
        {VertexChannel::Semantic::Texcoord, 0, 2, {0,0, 0,0, 0,0}},
        {VertexChannel::Semantic::BlendIndices, 0, 4, {0,0,0,0, 0,0,0,0, 0,0,0,0}},
        {VertexChannel::Semantic::BlendWeights, 0, 4, {1,0,0,0, 1,0,0,0, 1,0,0,0}},
    };
    primitive.source_channels = primitive.channels;
    lowered.primitives.push_back(std::move(primitive));
    return lowered;
}

std::set<int> subtree_nodes(const Scene& scene, const std::vector<int>& roots) {
    std::set<int> nodes;
    std::vector<int> pending(roots);
    while (!pending.empty()) {
        const int index = pending.back(); pending.pop_back();
        if (index < 0 || static_cast<std::size_t>(index) >= scene.nodes.size() || !nodes.insert(index).second) continue;
        const auto& children = scene.nodes[static_cast<std::size_t>(index)].children;
        pending.insert(pending.end(), children.begin(), children.end());
    }
    return nodes;
}

// The scene as one unit of the build sees it: the nodes under its roots and what hangs on them.
// primitive_map gives each kept primitive's index in the whole scene.
Scene unit_subscene(const Scene& scene, const std::vector<int>& roots, bool main,
                    std::vector<std::size_t>& primitive_map) {
    const auto nodes = subtree_nodes(scene, roots);
    const auto inside = [&](int node) { return nodes.count(node) != 0; };
    Scene out = scene;
    out.scene_roots = roots;
    out.primitives.clear();
    primitive_map.clear();
    for (std::size_t i = 0; i < scene.primitives.size(); ++i)
        if (inside(scene.primitives[i].source_node)) { out.primitives.push_back(scene.primitives[i]); primitive_map.push_back(i); }
    out.collider_primitives.clear();
    for (const auto& primitive : scene.collider_primitives)
        if (inside(primitive.source_node)) out.collider_primitives.push_back(primitive);
    auto& d = out.asset_definition;
    const auto keep = [&](auto& list) {
        list.erase(std::remove_if(list.begin(), list.end(), [&](const auto& entry) { return !inside(entry.source_node); }),
                   list.end());
    };
    keep(d.colliders); keep(d.node_bodies); keep(d.joints); keep(d.node_actors); keep(d.volumes);
    keep(d.visibility_groups); keep(d.dangles); keep(d.aims); keep(d.lod_levels); keep(d.render_settings);
    keep(d.light_settings);
    if (!main) { d.body.reset(); d.movers.clear(); }
    d.flow.clear();
    d.script_data.clear();
    for (const auto& [node, text] : scene.asset_definition.unit_flows) if (inside(node)) d.flow = text;
    for (const auto& [node, text] : scene.asset_definition.unit_script_data) if (inside(node)) d.script_data = text;
    d.parts.clear();
    return out;
}

void set_feature_status(std::vector<std::string>& statuses, const std::string& feature,
                        const std::string& value) {
    for (std::size_t i = 0; i + 1 < statuses.size(); i += 2) {
        if (statuses[i] == feature) {
            statuses[i + 1] = value;
            return;
        }
    }
}
} // namespace

bool build_glb_resources(const Scene& source, const app::CompileOptions& options,
                         const CompilationContext& context, const CompilationPlan& plan, ResourceGraph& graph,
                         std::vector<std::string>& feature_status, std::string& error) {
    graph = {};
    if (!plan.gaps.empty()) { error = "cannot build resources from an unsupported compilation plan"; return false; }
    const auto& base = context.file_base();
    const auto no_skin = static_cast<std::size_t>(-1);
    auto selected = plan.skin_index.value_or(no_skin);
    if (selected != no_skin && !source.asset_definition.parts.empty()) {
        // the plan's skin can be a part's own armature; the main unit then has none (the part carries its BONES)
        std::vector<int> part_roots;
        for (const auto& part : source.asset_definition.parts) part_roots.push_back(part.source_node);
        const auto in_parts = subtree_nodes(source, part_roots);
        const auto& joints = source.skins[selected].joints;
        if (!joints.empty() && std::all_of(joints.begin(), joints.end(), [&](int joint) { return in_parts.count(joint) != 0; }))
            selected = no_skin;
    }
    Scene reference_scene;
    bool reference_subset = false;
    const Scene* emitted_source = &source;
    const auto generated_bones = context.generated_key("bones", base + "_bones");
    ResourceKey bones = generated_bones;
    if (options.reference_bones_file || options.skeleton_resource) {
        if (selected == no_skin) { error = "reference BONES requires a compilation plan with one selected skin"; return false; }
        if (!apply_reference_bones(source, selected, options, reference_scene, bones, reference_subset, error)) return false;
        emitted_source = &reference_scene;
        if (reference_subset) bones = generated_bones;
        else graph.external.push_back({bones, true});
    }
    if (selected != no_skin) {
        if (!options.reference_bones_file || reference_subset) graph.owned.push_back({bones, base + "_bones.bones",
            stingray::bones::BonesResource{bones.name, canonical_bone_names(emitted_source->skins[selected]), {}}, {}});
        set_feature_status(feature_status, "bones", reference_subset ? "emitted_reference_subset" :
            options.reference_bones_file ? "external_reference" : "emitted");
    }
    // Parts are extra units of this build (asset_definition.h PartDefinition). Their materials are built once
    // with the main unit's, over the whole scene, so every unit shares them and their textures.
    const auto& parts = emitted_source->asset_definition.parts;
    std::vector<std::string> material_names;
    material_names.reserve(source.materials.size());
    for (const auto& material : source.materials) material_names.push_back(material.name);
    stingray::unit::MaterialSlotLowering all_slots;
    std::vector<std::pair<std::string, std::string>> all_bindings;
    if (plan.emit_unit && !parts.empty()) {
        std::vector<int> indices;
        for (const auto& primitive : emitted_source->primitives) indices.push_back(primitive.material);
        all_slots = stingray::unit::lower_material_slots(material_names, indices);
        if (plan.emit_materials && options.material_override.empty() &&
            !build_core_materials(source, context, graph, all_bindings, error, &all_slots)) return false;
    }
    // One UNIT of the build: the main one or a part (primitive_map: its primitives' indices in the whole scene).
    const auto emit_one_unit = [&](const Scene& unit_source, const std::string& stem, bool main,
                                   const std::vector<std::size_t>& primitive_map) -> bool {
        const bool skeleton_unit = main && !plan.placeholder_unit && unit_source.primitives.empty() && selected != no_skin;
        const bool placeholder_unit = main && (plan.placeholder_unit || (!parts.empty() && unit_source.primitives.empty() && !skeleton_unit));
        const Scene placeholder = placeholder_unit ? with_placeholder_geometry(unit_source) :
            skeleton_unit ? with_skeleton_carrier(unit_source, selected) : Scene{};
        const Scene& unit_scene = placeholder_unit || skeleton_unit ? placeholder : unit_source;
        if (!main && unit_scene.primitives.empty()) { error = "part '" + stem + "' has no meshes"; return false; }
        const auto unit_key = context.generated_key("unit", stem);
        stingray::unit::WriteOptions unit_options;
        unit_options.resource_name = unit_key.name;
        if (main && (plan.state_machine_animation_index || !plan.state_machine_states.empty() || plan.rest_state))
            unit_options.animation_state_machine_resource = unit_key.name;
        unit_options.material_override = options.material_override;
        unit_options.material_layers = options.material_layers;
        unit_options.authored_physics = options.physics;
        unit_options.ragdoll_handoff = main && !plan.ragdoll_event.empty();
        if (main && options.physics && options.fitted_physics)
            unit_options.fitted_physics = options.fitted_physics;
        std::vector<int> material_indices;
        material_indices.reserve(unit_scene.primitives.size());
        for (const auto& primitive : unit_scene.primitives) material_indices.push_back(primitive.material);
        const auto material_slots = parts.empty() ? stingray::unit::lower_material_slots(material_names, material_indices)
                                                  : stingray::unit::MaterialSlotLowering{};
        if (parts.empty()) {
            unit_options.primitive_material_slots = material_slots.primitive_slots;
        } else {
            // added placeholder/carrier triangles have no material
            for (std::size_t i = 0; i < unit_scene.primitives.size(); ++i)
                unit_options.primitive_material_slots.push_back(
                    i < primitive_map.size() ? all_slots.primitive_slots[primitive_map[i]] : std::string("__no_material__"));
        }
        // The selected skin is also used for animation-only scenes.  UNIT's
        // skeleton field, however, is cardinality-coupled to the skins that
        // its own primitives reference; an animation-only synthetic skin must
        // not make a static/placeholder UNIT claim a BONES resource.
        const bool unit_uses_selected_skin = main && selected != no_skin && std::any_of(
            unit_scene.primitives.begin(), unit_scene.primitives.end(), [&](const Primitive& primitive) {
                return primitive.source_node >= 0 &&
                    static_cast<std::size_t>(primitive.source_node) < unit_scene.nodes.size() &&
                    unit_scene.nodes[static_cast<std::size_t>(primitive.source_node)].skin ==
                        static_cast<int>(selected);
            });
        std::set<ResourceKey> dependencies;
        if (unit_uses_selected_skin) unit_options.skeleton_resource = bones.name;
        if (!main) {
            // a skinned part carries its own BONES; its skin's joints have to be inside the part
            std::set<int> skins;
            for (const auto& primitive : unit_scene.primitives)
                if (primitive.source_node >= 0 && unit_scene.nodes[static_cast<std::size_t>(primitive.source_node)].skin >= 0)
                    skins.insert(unit_scene.nodes[static_cast<std::size_t>(primitive.source_node)].skin);
            if (skins.size() > 1) { error = "part '" + stem + "' uses more than one armature"; return false; }
            if (!skins.empty()) {
                const auto& skin = unit_scene.skins[static_cast<std::size_t>(*skins.begin())];
                const auto inside = subtree_nodes(unit_scene, unit_scene.scene_roots);
                for (const int joint : skin.joints)
                    if (!inside.count(joint)) {
                        error = "part '" + stem + "' is skinned to bone '" + unit_scene.nodes[static_cast<std::size_t>(joint)].name +
                            "', which isn't inside the part (put the armature under the part)";
                        return false;
                    }
                const auto part_bones = context.generated_key("bones", stem + "_bones");
                graph.owned.push_back({part_bones, stem + "_bones.bones",
                    stingray::bones::BonesResource{part_bones.name, canonical_bone_names(skin), {}}, {}});
                unit_options.skeleton_resource = part_bones.name;
                dependencies.insert(part_bones);
            }
        }
        if (plan.simple_animation_index && unit_uses_selected_skin && !placeholder_unit) {
            stingray::animation::BuiltAnimation simple;
            if (!stingray::animation::build_skeletal_animation(*emitted_source, emitted_source->skins[selected],
                    emitted_source->animations[*plan.simple_animation_index], unit_key.name, simple, error,
                    &options.animation_fit)) return false;
            constexpr std::size_t kEnvelopeSize = 0x26;
            if (simple.cooked.size() <= kEnvelopeSize) { error = "simple animation produced no ANIMATION body"; return false; }
            unit_options.simple_animation.assign(simple.cooked.begin() + kEnvelopeSize, simple.cooked.end());
            unit_options.simple_animation_skin = selected;
            set_feature_status(feature_status, "simple_animation", "emitted");
        }
        // The command-line override intentionally replaces every per-slot
        // material choice, including authored external and generated intents.
        if (plan.emit_materials && options.material_override.empty()) {
            if (parts.empty()) {
                if (!build_core_materials(source, context, graph, unit_options.material_bindings, error, &material_slots)) return false;
            } else {
                for (const auto& binding : all_bindings)
                    if (std::find(unit_options.primitive_material_slots.begin(), unit_options.primitive_material_slots.end(),
                                  binding.first) != unit_options.primitive_material_slots.end())
                        unit_options.material_bindings.push_back(binding);
            }
            if (!unit_options.material_bindings.empty()) {
                const auto used = used_material_indices(source);
                const bool has_external = std::any_of(used.begin(), used.end(), [&](std::size_t index) {
                    return source.materials[index].intent == MaterialInfo::Intent::External;
                });
                set_feature_status(feature_status, "materials", has_external ? "emitted_per_slot_materials" : "emitted_core_pbr");
            }
        }
        if (unit_uses_selected_skin) dependencies.insert(bones);
        if (main && (plan.state_machine_animation_index || !plan.state_machine_states.empty() || plan.rest_state))
            dependencies.insert({"state_machine", unit_key.name});
        for (const auto& binding : unit_options.material_bindings) dependencies.insert({"material", binding.second});
        if (!options.material_override.empty()) {
            ResourceKey external{"material", options.material_override};
            graph.external.push_back({external, true}); dependencies.insert(external);
        }
        // lamps with one of the game's IES profiles ship a copy of it, named by content like the other game copies
        for (const auto& light : unit_scene.asset_definition.light_settings) {
            if (light.ies.empty()) continue;
            std::uint64_t hash = 0;
            if (light.ies[0] == '#') {
                try { hash = std::stoull(light.ies.substr(1), nullptr, 16); } catch (const std::exception&) {}
                if (light.ies.size() != 17 || !hash) { error = "IES profile '" + light.ies + "' must be # and 16 hex digits"; return false; }
            } else {
                hash = stingray::resource_name_hash(light.ies);
            }
            char hex[17];
            std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(hash));
            if (light.ies_extract.empty()) { error = "IES profiles need the game files extract (" + light.ies + ")"; return false; }
            const auto file = std::filesystem::path(light.ies_extract) / (std::string(hex) + ".ies");
            std::ifstream in(file, std::ios::binary);
            const std::vector<std::uint8_t> blob((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            std::uint32_t body_size = 0, stream_size = 0;
            if (blob.size() >= 38) { std::memcpy(&body_size, blob.data() + 29, 4); std::memcpy(&stream_size, blob.data() + 34, 4); }
            if (blob.size() < 38 || 38ull + body_size + stream_size != blob.size() || stream_size != 0) {
                error = "cannot read the game's IES profile " + light.ies + " from " + file.string() +
                    " (extract with limn's 'ies' filter too)";
                return false;
            }
            std::vector<std::uint8_t> body(blob.begin() + 38, blob.end());
            const auto ies_stem = game_copy_stem("ies_" + std::string(hex), body);
            const auto key = game_copy_key("ies", ies_stem);
            if (std::none_of(graph.owned.begin(), graph.owned.end(), [&](const auto& node) { return node.key == key; }))
                graph.owned.push_back({key, ies_stem + ".ies", IesResource{std::move(body)}, {}});
            dependencies.insert(key);
            unit_options.light_ies[light.source_node] = stingray::resource_name_hash(key.name);
        }
        stingray::unit::UnitResource unit;
        if (!stingray::unit::build_unit_v115(unit_scene, unit, error, unit_options)) {
            if (!main) error = "part '" + stem + "': " + error;
            return false;
        }
        graph.owned.push_back({unit_key, stem + ".unit", std::move(unit), {dependencies.begin(), dependencies.end()}});
        graph.roots.push_back(unit_key);
        set_feature_status(feature_status, "unit", "emitted");
        return true;
    };
    if (plan.emit_unit) {
        if (parts.empty()) {
            if (!emit_one_unit(*emitted_source, base, true, {})) return false;
        } else {
            std::vector<int> main_roots;
            for (const int root : emitted_source->scene_roots)
                if (std::none_of(parts.begin(), parts.end(), [&](const PartDefinition& part) { return part.source_node == root; }))
                    main_roots.push_back(root);
            std::vector<std::size_t> map;
            if (!emit_one_unit(unit_subscene(*emitted_source, main_roots, true, map), base, true, map)) return false;
            for (const auto& part : parts) {
                if (part.name == base) { error = "part '" + part.name + "' has the asset's own name"; return false; }
                if (!emit_one_unit(unit_subscene(*emitted_source, {part.source_node}, false, map), part.name, false, map))
                    return false;
            }
        }
    }
    if (plan.emit_animations && selected != no_skin) {
        for (const auto i : plan.animation_indices) {
            if (i >= source.animations.size()) { error = "compilation plan animation index is out of range"; return false; }
            const auto stem = base + "_animation_" + std::to_string(i);
            const auto key = context.generated_key("animation", stem);
            stingray::animation::BuiltAnimation animation;
            if (!stingray::animation::build_skeletal_animation(*emitted_source, emitted_source->skins[selected], emitted_source->animations[i], key.name, animation, error, &options.animation_fit)) return false;
            stingray::animation::SampledAnimationError fidelity;
            std::string fidelity_error;
            if (!stingray::animation::measure_animation_error(animation.cooked, emitted_source->skins[selected], emitted_source->animations[i], fidelity, fidelity_error)) {
                error="post-serialization animation evaluation failed: "+fidelity_error;return false;
            }
            bool best_effort = false;
            for (const auto& fit : animation.fits) best_effort = best_effort || fit.best_effort;
            if(!best_effort && (fidelity.translation>options.animation_fit.translation_tolerance || fidelity.scale>options.animation_fit.scale_tolerance ||
               fidelity.rotation_radians>options.animation_fit.rotation_tolerance_radians)) {
                error="post-serialization animation samples exceed configured fit tolerance";return false;
            }
            animation.fidelity = std::move(fidelity);
            graph.owned.push_back({key, stem + ".animation", std::move(animation), {bones}});
            graph.roots.push_back(key);
            set_feature_status(feature_status, "animation", "emitted");
        }
    }
    // Dangling bones: one pendulum per bone, addressed by its slot in the BONES list.
    std::vector<stingray::BoneConstraint> pendulums;
    const auto bone_count = selected == no_skin ? 0u :
        static_cast<std::uint32_t>(canonical_bone_names(emitted_source->skins[selected]).size());
    if (!source.asset_definition.dangles.empty() && selected != no_skin) {
        const auto& source_skin = source.skins[selected];
        const auto source_names = canonical_bone_names(source_skin);
        const auto emitted_names = canonical_bone_names(emitted_source->skins[selected]);
        for (const auto& dangle : source.asset_definition.dangles) {
            const auto joint = std::find(source_skin.joints.begin(), source_skin.joints.end(), dangle.source_node);
            const auto bone_name = joint == source_skin.joints.end() ? std::string() :
                source_names[static_cast<std::size_t>(joint - source_skin.joints.begin())];
            const auto slot = std::find(emitted_names.begin(), emitted_names.end(), bone_name);
            if (bone_name.empty() || slot == emitted_names.end()) {
                error = "dangling bone '" + source.nodes[static_cast<std::size_t>(dangle.source_node)].name +
                    "' is not a bone of the emitted skeleton";
                return false;
            }
            stingray::BoneConstraint pendulum;
            pendulum.bone_slot = static_cast<std::uint32_t>(slot - emitted_names.begin());
            pendulum.length = dangle.length;
            pendulum.mass = dangle.mass;
            pendulum.gravity = dangle.gravity;
            pendulum.damping = dangle.damping;
            pendulum.rest_stiffness = dangle.stiffness;
            pendulum.max_angle_degrees = dangle.max_angle_degrees;
            if (dangle.jiggle) {
                pendulum.kind = stingray::BoneConstraint::Kind::Spring;
                pendulum.stiffness = dangle.stiffness;
                pendulum.max_stretch = dangle.max_stretch;
            }
            pendulums.push_back(pendulum);
        }
        set_feature_status(feature_status, "dangling_bones", "emitted");
    }
    // Aiming bones: the aim joint, and the bones that turn toward the target (by name), as BONES slots.
    if (!source.asset_definition.aims.empty() && selected != no_skin) {
        const auto& source_skin = source.skins[selected];
        const auto source_names = canonical_bone_names(source_skin);
        const auto emitted_names = canonical_bone_names(emitted_source->skins[selected]);
        const auto slot_of = [&](int node, std::uint32_t& slot) {
            const auto joint = std::find(source_skin.joints.begin(), source_skin.joints.end(), node);
            if (joint == source_skin.joints.end()) return false;
            const auto& name = source_names[static_cast<std::size_t>(joint - source_skin.joints.begin())];
            const auto found = std::find(emitted_names.begin(), emitted_names.end(), name);
            slot = static_cast<std::uint32_t>(found - emitted_names.begin());
            return found != emitted_names.end();
        };
        for (const auto& aim : source.asset_definition.aims) {
            stingray::BoneConstraint constraint;
            constraint.kind = stingray::BoneConstraint::Kind::Aim;
            constraint.target = aim.target;
            const auto& aim_name = source.nodes[static_cast<std::size_t>(aim.source_node)].name;
            if (!slot_of(aim.source_node, constraint.bone_slot)) {
                error = "aiming bone '" + aim_name + "' is not a bone of the emitted skeleton";
                return false;
            }
            for (const auto& [bone, weight] : aim.turn) {
                int node = -1;
                for (const int joint : source_skin.joints)
                    if (source.nodes[static_cast<std::size_t>(joint)].name == bone) node = joint;
                std::uint32_t slot = 0;
                if (!slot_of(node, slot)) {
                    error = "aiming bone '" + aim_name + "' turns '" + bone + "', which is not a bone of the skeleton";
                    return false;
                }
                constraint.turn.push_back({slot, weight});
            }
            pendulums.push_back(std::move(constraint));
        }
        set_feature_status(feature_status, "aiming_bones", "emitted");
    }
    // Ragdoll handoff: the dynamic node bodies (named after their nodes, see
    // authored_scene.cpp) form the actor set the ragdoll state creates.
    std::vector<std::uint32_t> ragdoll_actors;
    if (!plan.ragdoll_event.empty()) {
        for (const auto& body : source.asset_definition.node_bodies) {
            if (body.actor != "dynamic") continue;
            const auto& name = source.nodes[static_cast<std::size_t>(body.source_node)].name;
            ragdoll_actors.push_back(stingray::id32_from_id64(name.empty() ? body.id : name));
        }
        set_feature_status(feature_status, "ragdoll", "emitted_event_" + plan.ragdoll_event);
    }
    std::optional<ResourceKey> rest_animation;
    if (plan.rest_state) {
        // A constant key on the root joint; every other channel stays at rest.
        const auto& skin = emitted_source->skins[selected];
        if (skin.joints.empty()) { error = "dangling bones need a skin with joints"; return false; }
        const auto& root = emitted_source->nodes[static_cast<std::size_t>(skin.joints.front())];
        AnimationInfo rest;
        rest.name = "rest";
        AnimationTrack track;
        track.target_node = skin.joints.front();
        track.path = AnimationPath::Translation;
        track.times = {0.0f, 1.0f};
        track.value_components = 3;
        for (int key = 0; key < 2; ++key)
            for (int axis = 0; axis < 3; ++axis) track.values.push_back(root.local_stingray[12 + axis]);
        rest.tracks.push_back(std::move(track));
        const auto animation_key = context.generated_key("animation", base + "_rest");
        stingray::animation::BuiltAnimation animation;
        if (!stingray::animation::build_skeletal_animation(*emitted_source, skin, rest, animation_key.name,
                animation, error, &options.animation_fit)) return false;
        graph.owned.push_back({animation_key, base + "_rest.animation", std::move(animation), {bones}});
        rest_animation = animation_key;
        if (plan.ragdoll_event.empty()) {
            const auto machine_key = context.generated_key("state_machine", base);
            auto machine = stingray::write_minimal_single_clip_state_machine(machine_key.name, animation_key.name, true, pendulums, bone_count);
            graph.owned.push_back({machine_key, base + ".state_machine", std::move(machine), {animation_key}});
        }
    }
    if (plan.state_machine_animation_index) {
        const auto animation_index = *plan.state_machine_animation_index;
        if (selected == no_skin || animation_index >= source.animations.size()) {
            error = "state machine plan has no valid selected animation skin";
            return false;
        }
        const auto animation_key = context.generated_key(
            "animation", base + "_animation_" + std::to_string(animation_index));
        const auto machine_key = context.generated_key("state_machine", base);
        auto machine = stingray::write_minimal_single_clip_state_machine(
            machine_key.name, animation_key.name, plan.state_machine_looping, pendulums, bone_count);
        graph.owned.push_back({machine_key, base + ".state_machine", std::move(machine), {animation_key}});
    }
    if (!plan.state_machine_states.empty() || (rest_animation && !plan.ragdoll_event.empty())) {
        if (selected == no_skin) {
            error = "direct-event state machine plan has no valid selected animation skin";
            return false;
        }
        std::vector<stingray::DirectEventState> states;
        std::set<ResourceKey> dependencies;
        states.reserve(plan.state_machine_states.size() + 2);
        if (rest_animation) {
            states.push_back({"rest", rest_animation->name, true});
            dependencies.insert(*rest_animation);
        }
        // A mask bone covers itself and every bone below it; later entries win.
        const auto& emitted_skin = emitted_source->skins[selected];
        const auto emitted_bone_names = canonical_bone_names(emitted_skin);
        const auto resolve_mask = [&](const PlannedStateMachineState& planned, stingray::DirectEventState& state) {
            std::map<std::uint32_t, float> weights;
            for (const auto& [bone, weight] : planned.mask) {
                const auto named = std::find(emitted_bone_names.begin(), emitted_bone_names.end(), bone);
                if (named == emitted_bone_names.end()) {
                    error = "state '" + planned.name + "' masks bone '" + bone + "', which is not in the skeleton";
                    return false;
                }
                const int top = emitted_skin.joints[static_cast<std::size_t>(named - emitted_bone_names.begin())];
                for (std::size_t slot = 0; slot < emitted_skin.joints.size(); ++slot)
                    for (int node = emitted_skin.joints[slot]; node >= 0; node = emitted_source->nodes[static_cast<std::size_t>(node)].parent)
                        if (node == top) { weights[static_cast<std::uint32_t>(slot)] = weight; break; }
            }
            state.bone_weights.assign(weights.begin(), weights.end());
            state.layer = planned.layer;
            state.empty = planned.empty;
            state.additive = planned.additive;
            state.events_at = planned.events_at;
            state.exit_event = planned.exit_event;
            state.exit_blend = planned.exit_blend;
            return true;
        };
        for (const auto& planned : plan.state_machine_states) {
            const auto key_of = [&](std::size_t index) {
                return context.generated_key("animation", base + "_animation_" + std::to_string(index));
            };
            if (!planned.blend.empty()) {
                stingray::DirectEventState state;
                state.name = planned.name;
                state.looping = planned.looping;
                state.blend_variable = planned.blend_variable;
                state.blend_variable2 = planned.blend_variable2;
                state.speed = planned.speed;
                state.speed_variable = planned.speed_variable;
                state.random = planned.random;
                state.randomization = planned.randomization;
                for (const auto& [index, value] : planned.blend) {
                    if (index >= source.animations.size()) { error = "blend state references an invalid animation"; return false; }
                    const auto animation_key = key_of(index);
                    const auto clip = state.blend.size();
                    state.blend.push_back({animation_key.name, value,
                                           clip < planned.blend_value2.size() ? planned.blend_value2[clip] : 0.0f});
                    dependencies.insert(animation_key);
                }
                if (!resolve_mask(planned, state)) return false;
                states.push_back(std::move(state));
                continue;
            }
            if (planned.empty) {
                stingray::DirectEventState state;
                state.name = planned.name;
                state.looping = planned.looping;
                if (!resolve_mask(planned, state)) return false;
                states.push_back(std::move(state));
                continue;
            }
            if (planned.animation_index >= source.animations.size()) {
                error = "direct-event state machine plan references an invalid animation";
                return false;
            }
            const auto animation_key = key_of(planned.animation_index);
            stingray::DirectEventState state;
            state.name = planned.name;
            state.animation_resource_name = animation_key.name;
            state.looping = planned.looping;
            state.speed = planned.speed;
            state.speed_variable = planned.speed_variable;
            if (!resolve_mask(planned, state)) return false;
            states.push_back(std::move(state));
            dependencies.insert(animation_key);
        }
        auto transitions = plan.state_machine_transitions;
        if (!plan.ragdoll_event.empty()) {
            const auto ragdoll_state = states.size();
            for (std::size_t from = 0; from < ragdoll_state; ++from)
                if (states[from].layer == 0) transitions.push_back({from, ragdoll_state, plan.ragdoll_event, 0.0f});
            stingray::DirectEventState ragdoll;
            ragdoll.name = "ragdoll";
            ragdoll.ragdoll = true;
            states.push_back(ragdoll);
        }
        const auto machine_key = context.generated_key("state_machine", base);
        auto machine = stingray::write_direct_event_state_machine(
            machine_key.name, states, transitions,
            plan.state_machine_variables, plan.state_machine_selectors, pendulums, bone_count, ragdoll_actors,
            plan.state_machine_declared_events);
        graph.owned.push_back({machine_key, base + ".state_machine", std::move(machine),
            {dependencies.begin(), dependencies.end()}});
    }
    std::vector<ResourceKey> effects;
    if (!build_particle_effects(source, context, graph, effects, error)) return false;
    graph.roots.insert(graph.roots.end(), effects.begin(), effects.end());
    std::vector<ResourceKey> loose;
    if (!build_loose_textures(source, context, graph, loose, error)) return false;
    graph.roots.insert(graph.roots.end(), loose.begin(), loose.end());
    return true;
}
}
