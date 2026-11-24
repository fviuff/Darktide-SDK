"""The add-on's stored settings: scene, collection, material, bone, object and light properties."""

import os
import re
import json
import math
import struct
import bpy
from bpy.props import BoolProperty, CollectionProperty, EnumProperty, FloatProperty, FloatVectorProperty, IntProperty, PointerProperty, StringProperty
from .reference_skeleton import _murmur64
from . import fit_skeleton
from .skeleton_presets import SKELETON_PRESET_LIST, SKELETON_PRESETS
from . import game_unit
from . import game_material
from . import state_graph
from . import breed
from . import flow_editor
from . import gibs


SCHEMA_KEY = "darktide_asset"


MATERIAL_SCHEMA_KEY = "darktide_material"


ACTOR_ITEMS = [
    ("static", "Fixed collider", "Stays fixed and provides collision geometry"),
    ("dynamic", "Simulated body", "PhysX simulates this rigid body when loaded into a physics scene"),
    ("keyframed", "Moved externally", "Kinematic body; a runtime controller must supply motion"),
]


def _enum(items, default):
    return EnumProperty(items=[(v, v.title(), "") for v in items], default=default)


_BREED_ITEMS = []


def _breed_items():
    if not _BREED_ITEMS:
        _BREED_ITEMS.extend((name, name, entry["base_unit"]) for name, entry in sorted(breed.breeds().items()))
    return _BREED_ITEMS


_ZONE_ITEMS = []


def _zone_items():
    if not _ZONE_ITEMS:
        _ZONE_ITEMS.append(("auto", "From the game breed", "The zone the game breed gives this actor's name"))
        _ZONE_ITEMS.extend((zone, zone.replace("_", " ").capitalize(), "") for zone in breed.zone_names())
    return _ZONE_ITEMS


# Dropdowns that show states, clips and variables by name while the graph keeps their numbers. Blender needs the
# item lists kept alive while a dropdown is open.
_PICK_ITEMS = {}


def _graph_options(context):
    scene = context.scene if context else bpy.context.scene
    return _asset_options(scene) if scene else None


def _pick_items(kind, context):
    options = _graph_options(context)
    names = []
    if options is not None:
        if kind == "state":
            names = [state.state_name for state in options.state_graph_states]
        elif kind == "variable":
            names = [variable.variable_name for variable in options.state_graph_variables]
        else:
            clips = state_graph.clip_names(options)
            names = [clips.get(number, "clip " + str(number)) for number in range(max(clips) + 1)] if clips else []
    items = [(str(number), "%d  %s" % (number, name), "", number) for number, name in enumerate(names)]
    if not items:
        items = [("0", "0  (Inspect Asset to see clip names)" if kind == "clip" else "0", "", 0)]
    _PICK_ITEMS[kind] = items
    return items


def _picker(kind, field, name, description=""):
    return EnumProperty(name=name, description=description, items=lambda self, context: _pick_items(kind, context),
                        get=lambda self: getattr(self, field), set=lambda self, value: setattr(self, field, value))


class DarktideAnimationState(bpy.types.PropertyGroup):
    state_name: StringProperty(name="State name", default="State")
    clip_index: IntProperty(name="Clip index", default=0, min=0)
    clip_pick: _picker("clip", "clip_index", "Clip", "The action this state plays")
    playback: EnumProperty(name="Playback", items=[
        ("loop", "Loop", "Repeat this clip"),
        ("once", "Play once", "Play this clip without looping"),
    ], default="loop")
    blend: BoolProperty(name="Blend", default=False,
                        description="Blend several clips by a float variable (e.g. stand, walk and run by speed)")
    blend_variable: IntProperty(name="Variable", default=0, min=0,
                                description="Index of the float variable that picks the mix")
    blend_clips: StringProperty(name="Clips", default="0:0, 1:1",
                                description="clip:value pairs, the clip by its name or number. A clip plays fully "
                                            "when the variable equals its value and fades into the clips next to it, "
                                            "e.g. walk:1, run:2. With 2D on, clip:x:y, e.g. idle:0:0, right:1:0, "
                                            "left:-1:0, forward:0:1")
    blend_2d: BoolProperty(name="2D", default=False,
                           description="Blend by two variables at once, e.g. strafing: x sideways, y forward")
    blend_variable2: IntProperty(name="Variable Y", default=1, min=0,
                                 description="Index of the float variable for the second direction")
    blend_variable_pick: _picker("variable", "blend_variable", "Variable", "The float variable that picks the mix")
    blend_variable2_pick: _picker("variable", "blend_variable2", "Variable Y",
                                  "The float variable for the second direction")
    random: BoolProperty(name="Random", default=False,
                         description="Play one of several clips picked at random (idle variations and the like); "
                                     "Clips lists them as clip:weight, e.g. 0:3, 1:1 plays clip 0 three times as often")
    random_pick: EnumProperty(name="Pick", items=[
        ("every_loop", "Every loop", "Pick a clip again each time one finishes"),
        ("no_repeat", "Every loop, no repeat", "Pick again each time, never the same clip twice in a row"),
        ("on_entry", "On entry", "Pick once when the state starts and keep it"),
    ], default="every_loop")
    speed: FloatProperty(name="Speed", default=1.0, description="Playback speed, 1 = as animated")
    speed_from_variable: BoolProperty(name="Speed from variable", default=False,
                                      description="Play at the speed a float variable holds, which your mod sets")
    speed_variable: IntProperty(name="Speed variable", default=0, min=0,
                                description="Index of the float variable that sets the playback speed")
    speed_variable_pick: _picker("variable", "speed_variable", "Speed variable",
                                 "The float variable that sets the playback speed")
    layer: IntProperty(name="Layer", default=0, min=0,
                       description="0 is the base layer. Higher layers play on top of it, each starting in its first "
                                   "state; transitions stay inside a layer and an event reaches every layer")
    empty: BoolProperty(name="Empty", default=False,
                        description="Play nothing on this layer, so the layers below show through")
    additive: BoolProperty(name="Additive", default=False,
                           description="Add this clip on top of the layers below instead of replacing them")
    events_at: StringProperty(name="Events at", default="",
                              description="Send these state machine events when the clip reaches a time in seconds, "
                                          "as seconds:event, e.g. 0.4:hit, 1.2:fire. Transitions on other states "
                                          "and layers react to them like to Unit.animation_event")
    exit_event: StringProperty(name="Exit event", default="",
                               description="Sent once just before the clip ends, e.g. done, with a transition on "
                                           "done back to idle for a clip that plays once")
    exit_blend: FloatProperty(name="Seconds left", default=0.2, min=0.0,
                              description="How long before the end the exit event goes out (use the transition's "
                                          "blend time so the next clip is fully in when this one ends)")
    bones: StringProperty(name="Bones", default="",
                          description="Only these bones play this state, each with everything below it, e.g. "
                                      "j_spine1 for the upper body. bone:weight plays it partly, e.g. j_neck:0.5, later "
                                      "entries win. Empty = the whole body")


class DarktideAnimationVariable(bpy.types.PropertyGroup):
    variable_name: StringProperty(name="Variable name", default="Variable")
    initial_value: FloatProperty(name="Initial value", default=0.0)
    minimum: FloatProperty(name="Minimum", default=0.0)
    maximum: FloatProperty(name="Maximum", default=1.0)
    preview_value: FloatProperty(name="Preview value", default=0.0,
                                 description="The variable's value while Play State Machine runs")


class DarktideAnimationTransition(bpy.types.PropertyGroup):
    from_state: IntProperty(name="From state index", default=0, min=0)
    from_any: BoolProperty(name="From any state", default=False,
                           description="Take this transition from every state of the target's layer (a transition "
                                       "of a state's own for the same event wins)")
    to_state: IntProperty(name="To state index", default=0, min=0)
    from_pick: _picker("state", "from_state", "From")
    to_pick: _picker("state", "to_state", "To")
    event_name: StringProperty(name="Event name")
    blend_seconds: FloatProperty(name="Blend seconds", default=0.0, min=0.0)
    condition: EnumProperty(name="Condition", items=[
        ("always", "Always on event", "Take this transition when the named event arrives"),
        ("range", "Variable range", "Choose this transition when a float variable falls inside its range"),
    ], default="always")
    variable_index: IntProperty(name="Variable index", default=0, min=0)
    variable_pick: _picker("variable", "variable_index", "Variable")
    lower: FloatProperty(name="Lower bound", default=0.0)
    upper: FloatProperty(name="Upper bound", default=1.0)
    lower_exclusive: BoolProperty(name="Exclude lower bound", default=False)
    upper_exclusive: BoolProperty(name="Exclude upper bound", default=False)


def apply_skeleton_preset(identity, folder, key):
    """Fill the reference UNIT/BONES/resource fields from a limn-extracted folder (<16hex>.<ext> files)."""
    resource = SKELETON_PRESETS[key]
    folder = bpy.path.abspath(folder.strip()) if folder else ""
    if not folder or not os.path.isdir(folder):
        raise ValueError("Set 'Game files extract' in the Setup panel to your limn extract")
    stem = "%016x" % _murmur64(resource)
    unit = os.path.join(folder, stem + ".unit")
    bones = os.path.join(folder, stem + ".bones")
    missing = [path for path in (unit, bones) if not os.path.isfile(path)]
    if missing:
        raise ValueError("Preset " + resource + " needs " + ", ".join(missing) + " (not found in the extract folder)")
    identity.reference_unit = unit
    identity.reference_bones = bones
    identity.skeleton_resource = resource


def _skeleton_preset_changed(self, context):
    self.skeleton_preset_status = ""
    if self.skeleton_preset == "custom":
        return
    try:
        apply_skeleton_preset(self, prefs().game_extract_folder, self.skeleton_preset)
        fit_skeleton.refresh_reference_names(self)
    except ValueError as exc:
        self.skeleton_preset_status = str(exc)


def _default_game_folder():
    for root in (r"C:\Program Files (x86)\Steam\steamapps\common\Warhammer 40,000 DARKTIDE",
                 os.path.expanduser("~/.local/share/Steam/steamapps/common/Warhammer 40,000 DARKTIDE")):
        if os.path.isdir(os.path.join(root, "bundle")):
            return root
    return ""


def prefs():
    return bpy.context.preferences.addons[__package__].preferences


class DarktideSceneSettings(bpy.types.PropertyGroup):
    game_unit: StringProperty(name="Game unit",
                              description="Resource path of a game unit whose nodes you need (attach, effect and animated "
                                          "nodes): a piece of gear, a weapon part, a prop, a character")
    only_weighted_bones: BoolProperty(
        name="Only weighted bones (gear)",
        description="Export only bones carrying vertex weights plus their ancestors (a subset skeleton, like the game's own gear)",
        default=False,
    )
    actor: EnumProperty(name="Body behavior", items=ACTOR_ITEMS, default="static")
    mass: FloatProperty(name="Mass", default=2.0, min=0.0)
    material: _enum(("default", "iron", "rubber"), "default")
    output_path: StringProperty(name="Output path", subtype="DIR_PATH")
    asset_filename: StringProperty(name="Asset filename", default="asset")
    asset_path: StringProperty(name="Resource path", description="Stable extensionless identity, e.g. content/mods/my_mod/trolley; blank uses the GLB filename")
    scale: FloatProperty(name="Scale", default=1.0, min=0.000001)
    asset_collection: PointerProperty(name="Asset collection", type=bpy.types.Collection)
    visible_meshes_collide: BoolProperty(
        name="Visible meshes collide",
        description="Use visible mesh geometry as render-and-collision shapes in this UNIT; does not create a ground plane or drive animation",
        default=False,
    )
    output_kind: EnumProperty(name="Output kind", default="all", items=[
        ("all", "All", "Compile the model and animations"),
        ("model", "Model", "Compile model resources"),
        ("animations", "Animations", "Compile animation resources"),
    ])
    select_clip: BoolProperty(name="Select clip", default=False)
    clip_index: IntProperty(name="Zero-based clip index", default=0, min=0)
    embed_simple_animation: BoolProperty(
        name="Simple animation",
        description="Without a state machine, embed one clip in the UNIT so scripts can play it with Unit.play_simple_animation",
        default=True,
    )
    simple_animation_clip: IntProperty(name="Simple animation clip", default=0, min=0)
    in_place: BoolProperty(
        name="In place",
        description="Remove the forward travel walk/run clips carry in their root bone so they loop on the spot. "
                    "build.log lists each clip's speed for moving the unit from your mod",
        default=False,
    )
    flow_tree: PointerProperty(
        name="Flow", type=bpy.types.NodeTree, poll=lambda self, tree: tree.bl_idname == flow_editor.TREE,
        description="The unit's flow (Darktide Flow node tree in the node editor): what happens when it spawns or "
                    "when a script calls Unit.flow_event(unit, name)",
    )
    weapon_materials: BoolProperty(
        name="Weapon or gear materials",
        description="Build the generated materials on the game's weapon shaders, so level decals like snow and dirt "
                    "don't land on them (the game's weapons and gear work that way)",
        default=False,
    )
    ragdoll_event: StringProperty(
        name="Ragdoll event",
        description="Animation event that turns the rig into a ragdoll (Unit.animation_event(unit, name)). "
                    "Dynamic bodies on the rig's bones stay switched off until then",
    )
    auto_loop_single_clip: BoolProperty(
        name="Auto-loop single-clip assets",
        description="Create a looping state machine when the exported GLB contains exactly one animation clip",
        default=True,
    )
    create_looping_state_machine: BoolProperty(
        name="Manual single-clip state machine",
        description="Create a one-state controller for the selected clip; automatic state retirement is not provided",
        default=False,
    )
    loop_clip_index: IntProperty(name="State clip index", default=0, min=0)
    state_machine_playback: EnumProperty(name="Playback", items=[
        ("loop", "Loop", "Repeat the selected clip"),
        ("once", "Play once", "Play the selected clip without looping"),
    ], default="loop")
    create_state_graph: BoolProperty(
        name="Create event-driven state graph",
        description="Create a multi-state controller whose transitions are triggered by named runtime events",
        default=False,
    )
    state_graph_states: CollectionProperty(type=DarktideAnimationState)
    state_graph_variables: CollectionProperty(type=DarktideAnimationVariable)
    state_graph_transitions: CollectionProperty(type=DarktideAnimationTransition)
    state_graph_game_events: StringProperty(name="Game events", default="")
    state_graph_game_events_source: StringProperty(name="Game events from", default="")
    animation_translation_tolerance: FloatProperty(name="Translation tolerance", default=1e-4, min=1e-12, precision=8)
    animation_scale_tolerance: FloatProperty(name="Scale tolerance", default=1e-4, min=1e-12, precision=8)
    animation_rotation_tolerance_radians: FloatProperty(name="Rotation tolerance (radians)", default=1e-3, min=1e-12, precision=8)
    animation_fit_advanced: BoolProperty(name="Show animation fit controls", default=False)
    compile_timeout: IntProperty(name="Compile timeout (seconds)", description="0 waits without a time limit", default=600, min=0)
    inspection_summary: StringProperty(name="Last source inspection", default="")


class DarktideLooseTexture(bpy.types.PropertyGroup):
    name: StringProperty(name="Name", default="texture",
                         description="Lowercase letters, digits and _; the texture becomes <asset>_<name>")
    image: PointerProperty(name="Image", type=bpy.types.Image)
    kind: EnumProperty(name="Encode as", default="color", items=[
        ("color", "Color", "A colour picture (albedo, a colour gradient, a decal): compressed like the game's "
                           "colour textures, in the sRGB colour space"),
        ("color_alpha", "Color with alpha", "A colour picture whose alpha channel matters (cut-outs, opacity)"),
        ("normal", "Normal map", "A tangent space normal map (red and green kept, blue rebuilt by the shader)"),
        ("data", "Masks / data", "Values, not colours: roughness, metal, masks, AO; stored linear"),
        ("like", "Same as a game texture", "Exactly like one of the game's textures, the one it stands in for: same "
                                           "size, format and colour space. Needs the game files extract"),
    ])
    like: StringProperty(name="Game texture",
                         description="The game texture it stands in for, by resource path or # and its 16 digit hash")


class DarktideCollectionSettings(bpy.types.PropertyGroup):
    loose_textures: CollectionProperty(type=DarktideLooseTexture)
    asset_path: StringProperty(name="Resource path", description="Stable extensionless identity for this asset")
    reference_unit: StringProperty(name="Reference UNIT", subtype="FILE_PATH")
    reference_bones: StringProperty(name="Reference BONES", subtype="FILE_PATH")
    skeleton_resource: StringProperty(name="Skeleton resource", description="Extensionless native skeleton resource identity")
    skeleton_preset: EnumProperty(name="Preset", default="custom", update=_skeleton_preset_changed, items=[
        ("custom", "Custom", "Enter the reference paths manually")] +
        [(key, label, resource) for key, label, resource in SKELETON_PRESET_LIST])
    skeleton_preset_status: StringProperty(name="Preset status", default="")
    bone_map: CollectionProperty(type=fit_skeleton.DarktideBoneMapRow)
    bone_map_index: IntProperty(default=0)
    reference_bone_names: CollectionProperty(type=fit_skeleton.DarktideReferenceBoneName)
    fit_report: StringProperty(name="Last fit report", default="")
    animation_source: PointerProperty(
        name="Animation from", type=bpy.types.Object, poll=lambda _self, obj: obj.type == "ARMATURE",
        description="Armature whose current animation Carry Over Animation bakes onto the Darktide armature "
                    "(empty = the rig the model was fitted from)",
    )
    fit_keep_torso: BoolProperty(
        name="Keep torso shape",
        description="The spine bones only move to their Darktide joints, without being stretched or tilted "
                    "(stops chests creasing or sagging on rigs whose spine differs a lot)",
        default=False,
    )
    fit_keep_proportions: BoolProperty(
        name="Keep proportions",
        description="Scale the model as a whole to the best average of all its bone lengths and scale each limb "
                    "evenly instead of stretching it along the bone (keeps the model's own build)",
        default=False,
    )
    only_weighted_bones: BoolProperty(
        name="Only weighted bones (gear)",
        description="Export only bones carrying vertex weights plus their ancestors (a subset skeleton, like the game's own gear)",
        default=False,
    )
    enemy_outline: BoolProperty(
        name="Enemy outline layers",
        description="Give the unit the game's enemy outline layers, so Unit.set_material_layer(unit, "
                    "\"minion_outline\" / \"minion_outline_reversed_depth\", true) outlines it like a tagged enemy "
                    "(the second one through walls). The outline materials are the game's; they are loaded while any "
                    "enemy is",
        default=False,
    )
    material_layers: StringProperty(
        name="Material layers",
        description="More material layers for Unit.set_material_layer: name=material, separated by ;. The material "
                    "is a resource path or #ID[16 hex digits]",
    )
    owned_by_addon: BoolProperty(name="Darktide asset collection", default=False, options={"HIDDEN"})
    settings_version: IntProperty(default=0, options={"HIDDEN"})
    actor: EnumProperty(name="Body behavior", items=ACTOR_ITEMS, default="static")
    mass: FloatProperty(name="Mass", default=2.0, min=0.0)
    material: _enum(("default", "iron", "rubber"), "default")
    visible_meshes_collide: BoolProperty(
        name="Visible meshes collide",
        description="Use this asset's visible mesh geometry as a collider",
        default=False,
    )
    asset_filename: StringProperty(name="Asset filename", default="asset")
    scale: FloatProperty(name="Scale", default=1.0, min=0.000001)
    output_kind: EnumProperty(name="Output kind", default="all", items=[
        ("all", "All", "Compile the model and animations"),
        ("model", "Model", "Compile model resources"),
        ("animations", "Animations", "Compile animation resources"),
    ])
    select_clip: BoolProperty(name="Select clip", default=False)
    clip_index: IntProperty(name="Zero-based clip index", default=0, min=0)
    embed_simple_animation: BoolProperty(
        name="Simple animation",
        description="Without a state machine, embed one clip in the UNIT so scripts can play it with Unit.play_simple_animation",
        default=True,
    )
    simple_animation_clip: IntProperty(name="Simple animation clip", default=0, min=0)
    in_place: BoolProperty(
        name="In place",
        description="Remove the forward travel walk/run clips carry in their root bone so they loop on the spot. "
                    "build.log lists each clip's speed for moving the unit from your mod",
        default=False,
    )
    root_motion: BoolProperty(
        name="Root motion",
        description="Move the forward travel of walk/run clips from the hips onto root_point, where the game reads "
                    "it as the character's movement (its own characters and enemies work that way). "
                    "build.log lists each clip's speed",
        default=False,
    )
    flow_tree: PointerProperty(
        name="Flow", type=bpy.types.NodeTree, poll=lambda self, tree: tree.bl_idname == flow_editor.TREE,
        description="The unit's flow (Darktide Flow node tree in the node editor): what happens when it spawns or "
                    "when a script calls Unit.flow_event(unit, name)",
    )
    weapon_materials: BoolProperty(
        name="Weapon or gear materials",
        description="Build the generated materials on the game's weapon shaders, so level decals like snow and dirt "
                    "don't land on them (the game's weapons and gear work that way)",
        default=False,
    )
    ragdoll_event: StringProperty(
        name="Ragdoll event",
        description="Animation event that turns the rig into a ragdoll (Unit.animation_event(unit, name)). "
                    "Dynamic bodies on the rig's bones stay switched off until then",
    )
    auto_loop_single_clip: BoolProperty(
        name="Auto-loop single-clip assets",
        description="Create a looping state machine when the exported GLB contains exactly one animation clip",
        default=True,
    )
    create_looping_state_machine: BoolProperty(
        name="Manual single-clip state machine",
        description="Create a one-state controller for the selected clip; automatic state retirement is not provided",
        default=False,
    )
    loop_clip_index: IntProperty(name="State clip index", default=0, min=0)
    state_machine_playback: EnumProperty(name="Playback", items=[
        ("loop", "Loop", "Repeat the selected clip"),
        ("once", "Play once", "Play the selected clip without looping"),
    ], default="loop")
    create_state_graph: BoolProperty(
        name="Create event-driven state graph",
        description="Create a multi-state controller whose transitions are triggered by named runtime events",
        default=False,
    )
    state_graph_states: CollectionProperty(type=DarktideAnimationState)
    state_graph_variables: CollectionProperty(type=DarktideAnimationVariable)
    state_graph_transitions: CollectionProperty(type=DarktideAnimationTransition)
    state_graph_game_events: StringProperty(
        name="Game events", default="",
        description="Every event a game character's scripts send (Import Game Events); the state machine lists them "
                    "all, since the game stops on an event it doesn't know. Events no transition takes do nothing")
    state_graph_game_events_source: StringProperty(name="Game events from", default="")
    animation_translation_tolerance: FloatProperty(name="Translation tolerance", description="Maximum measured translation error for fitted animation", default=1e-4, min=1e-12, precision=8)
    animation_scale_tolerance: FloatProperty(name="Scale tolerance", description="Maximum measured scale error for fitted animation", default=1e-4, min=1e-12, precision=8)
    animation_rotation_tolerance_radians: FloatProperty(name="Rotation tolerance (radians)", description="Maximum measured angular error for fitted animation", default=1e-3, min=1e-12, precision=8)
    animation_fit_advanced: BoolProperty(name="Show animation fit controls", default=False)
    inspection_summary: StringProperty(name="Last source inspection", default="")
    use_mover: BoolProperty(
        name="Mover",
        description="Give the unit a mover: the upright capsule that walks a character through the level and keeps it "
                    "out of walls (the game's characters and enemies have one named mover)",
        default=False,
    )
    mover_height: FloatProperty(name="Height", default=1.8, min=0.01, subtype="DISTANCE")
    mover_radius: FloatProperty(name="Radius", default=0.4, min=0.01, subtype="DISTANCE")
    mover_slope: FloatProperty(name="Max slope", default=0.6981317, min=0.0, max=1.5707963, subtype="ANGLE",
                               description="Steepest ground it walks up")
    mover_filter: StringProperty(name="Collides with", default="filter_minion_mover",
                                 description="The game's collision filter for it: filter_minion_mover for enemies, "
                                             "filter_player_mover for player characters")
    unit_package: BoolProperty(
        name="Package named like the unit",
        description="Load the asset as a package with the asset's own resource path, like the game's units. An enemy "
                    "body needs it: the game loads a breed's base unit as a package of that name when a mission loads "
                    "(Make a breed always does it)",
        default=False,
    )
    gib_breed: EnumProperty(name="Gibs like", items=gibs.breed_items,
                            description="The game breed whose gibbing your body gets: which bones come off and "
                                        "where the stumps go")
    make_breed: BoolProperty(
        name="Make a breed (EXPERIMENTAL)",
        description="Write a Lua file next to the compiled asset that adds an enemy of its own to the game: it behaves "
                    "like the game breed you pick, with this asset as its body",
        default=False,
    )
    breed_name: StringProperty(name="Name", default="my_enemy",
                               description="The new breed's name, lowercase (what your mod spawns it by)")
    breed_base: EnumProperty(name="Behaves like", items=lambda self, context: _breed_items(),
                             description="The game breed whose scripts drive it: behaviour, attacks, sounds, "
                                         "staggers")
    breed_body: EnumProperty(
        name="Body",
        items=(
            ("own", "Own body", "Your model is the whole body: the game breed's weapons are hung on it, its other body "
                                "pieces are left off, and so are its wounds, gibbing and dissolving (they work on those "
                                "pieces)"),
            ("game", "Game body pieces too", "The game breed's body pieces, wounds and gibbing on top of your model, "
                                             "like the game's own enemy"),
        ),
        default="own",
    )
    breed_health: StringProperty(name="Health", default="",
                                 description="One number (health on the easiest difficulty, the others scaled like "
                                             "the game breed's) or five, one per difficulty. Empty keeps the game "
                                             "breed's")
    breed_walk_speed: FloatProperty(name="Walk speed", default=0.0, min=0.0,
                                    description="Metres per second, 0 keeps the game breed's")
    breed_run_speed: FloatProperty(name="Run speed", default=0.0, min=0.0,
                                   description="Metres per second, 0 keeps the game breed's")


_GAME_SHADERS = None


def _game_shaders():
    """The game's own shaders offered in the Game shader list (game_shaders.json)."""
    global _GAME_SHADERS
    if _GAME_SHADERS is None:
        try:
            with open(os.path.join(os.path.dirname(__file__), "game_shaders.json"), encoding="utf-8") as handle:
                _GAME_SHADERS = {preset["id"]: preset for preset in json.load(handle)["presets"]}
        except (OSError, ValueError, KeyError):
            _GAME_SHADERS = {}
    return _GAME_SHADERS


def _shader_preset(settings):
    """the picked game shader, or the game material typed by resource path (None when it can't be read)"""
    if settings.game_material.strip():
        try:
            return game_material.preset(settings.game_material, bpy.path.abspath(prefs().game_extract_folder),
                                        bpy.path.abspath(prefs().game_folder))
        except (OSError, ValueError, struct.error):
            return None
    return _game_shaders().get(settings.game_shader)


def _game_shader_items(self, context):
    return [(key, preset["label"], preset["shader"]) for key, preset in _game_shaders().items()] or [("", "None", "")]


def _parse_assignments(text, what):
    """'name=1,0.5,0; other=2' -> {name: [floats]} (what='numbers') or {name: word} (what='words')."""
    result = {}
    for part in text.replace("\n", ";").split(";"):
        if not part.strip():
            continue
        if "=" not in part:
            raise ValueError("Expected name=value in '" + part.strip() + "'")
        name, value = (item.strip() for item in part.split("=", 1))
        if what == "numbers":
            numbers = [float(item) for item in value.replace(" ", ",").split(",") if item.strip()]
            if not 1 <= len(numbers) <= 4 or not all(math.isfinite(number) for number in numbers):
                raise ValueError("'" + name + "' needs 1 to 4 finite numbers")
            result[name] = numbers[0] if len(numbers) == 1 else numbers
        else:
            if value not in ("base_color", "normal", "orm", "emissive"):
                raise ValueError("Texture channel '" + name + "' must map to base_color, normal, orm or emissive")
            result[name] = value
    return result


_TEXTURE_SOURCES = (
    ("game", "Game's texture", "Keep the texture the game's material has in this slot"),
    ("base_color", "Base Color", "The image plugged into Base Color on this material"),
    ("normal", "Normal", "The image behind the Normal Map node on this material"),
    ("orm", "ORM", "The occlusion / roughness / metallic image of this material"),
    ("emissive", "Emission", "The image plugged into Emission on this material"),
)


class DarktideShaderValue(bpy.types.PropertyGroup):
    """One setting of a game shader, like a row of the Stingray property editor."""
    floats: IntProperty(default=1, min=1, max=4)
    is_color: BoolProperty(default=False)
    changed: BoolProperty(name="Change", default=False,
                          description="Use your own value for this setting (unticked keeps the game's)")
    value: FloatVectorProperty(name="Value", size=4)
    color: FloatVectorProperty(name="Color", size=4, subtype="COLOR", min=0.0, soft_max=1.0)


class DarktideShaderTexture(bpy.types.PropertyGroup):
    source: EnumProperty(name="Image", items=_TEXTURE_SOURCES, default="game",
                         description="Which image goes into this texture slot")


class DarktideOwnShaderParameter(bpy.types.PropertyGroup):
    """A value an own shader reads by name (a material variable of the same name)."""
    floats: IntProperty(name="Numbers", default=1, min=1, max=4, description="How many numbers: 1, or 2-4 for a vector")
    is_color: BoolProperty(name="Colour", default=False, description="Edit it as a colour (3 or 4 numbers)")
    value: FloatVectorProperty(name="Value", size=4)
    color: FloatVectorProperty(name="Colour", size=4, subtype="COLOR", min=0.0, soft_max=1.0, default=(1, 1, 1, 1))


class DarktideOwnShaderTexture(bpy.types.PropertyGroup):
    """A texture an own shader samples as sample_<name>(uv)."""
    source: EnumProperty(name="Image", items=_TEXTURE_SOURCES[1:], default="base_color",
                         description="Which of this material's images it is")


OWN_SHADER_TEMPLATE = """// Your own shader. Fill s from the inputs; leave out what you don't change.
// Inputs: i.uv, i.world_position, i.normal, i.tangent, i.bitangent, i.view_direction, i.time, i.front_face
// Outputs: s.base_color, s.opacity, s.normal, s.roughness, s.metallic, s.ao, s.emissive
// Textures are sample_<name>(uv), parameters are read by name. A normal map:
//     s.normal = tangent_to_world(i, unpack_normal(sample_normal_map(i.uv)));
void surface(SurfaceInput i, inout Surface s)
{
    float4 color = sample_color(i.uv);
    s.base_color = color.rgb * tint.rgb;
    s.roughness = 0.5;
    s.metallic = 0.0;
}
"""


def own_shader_extras(settings, material_name):
    """The own shader's code, parameters and textures as the compiler takes them."""
    text = settings.shader_code
    if text is None or not text.as_string().strip():
        raise ValueError("Material " + material_name + ": pick the text with its shader code (New Shader Code makes one)")
    names = set()
    parameters, textures = {}, {}
    for row in settings.own_parameters:
        name = row.name.strip()
        if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name) or name in names:
            raise ValueError("Material " + material_name + ": parameter names need letters, digits and _ and must differ ('" + name + "')")
        names.add(name)
        values = list(row.color if row.is_color else row.value)[:row.floats]
        parameters[name] = values
    for row in settings.own_textures:
        name = row.name.strip()
        if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name) or name in names:
            raise ValueError("Material " + material_name + ": texture names need letters, digits and _ and must differ ('" + name + "')")
        names.add(name)
        textures[name] = row.source
    extras = {"code": text.as_string(), "parameters": parameters, "textures": textures}
    if settings.own_clip:
        extras["alpha_clip"] = settings.own_cutoff
    if settings.own_decal:
        extras["decal"] = True
    return extras


def _shader_rows(settings):
    """Fill the settings and texture rows from the picked game shader; typed values from older files carry over."""
    preset = _shader_preset(settings)
    if not preset:
        return
    kept = {row.name: (row.changed, list(row.value), list(row.color)) for row in settings.shader_value_rows}
    try:
        typed = _parse_assignments(settings.shader_values, "numbers")
    except ValueError:
        typed = {}
    settings.shader_value_rows.clear()
    for variable in preset["variables"]:
        row = settings.shader_value_rows.add()
        row.name, row.floats = variable["name"], variable["floats"]
        row.is_color = "color" in variable["name"] and variable["floats"] >= 3
        default = (list(variable["default"]) + [0.0] * 4)[:4]
        row.value = default
        row.color = default[:3] + [default[3] if variable["floats"] == 4 else 1.0]
        if variable["name"] in typed:
            numbers = typed.pop(variable["name"])
            numbers = numbers if isinstance(numbers, list) else [numbers]
            numbers = (numbers + default[len(numbers):])[:4]
            row.changed, row.value, row.color = True, numbers, numbers
        elif variable["name"] in kept:
            row.changed, row.value, row.color = kept[variable["name"]]
    kept_textures = {row.name: row.source for row in settings.shader_texture_rows}
    try:
        typed_textures = _parse_assignments(settings.shader_textures, "words")
    except ValueError:
        typed_textures = {}
    settings.shader_texture_rows.clear()
    for slot in preset["textures"]:
        row = settings.shader_texture_rows.add()
        row.name = slot
        row.source = typed_textures.pop(slot, None) or kept_textures.get(slot, "game")
    # typed names this shader's list doesn't have stay typed (Other settings)
    settings.shader_values = "; ".join("%s=%s" % (name, ",".join("%g" % v for v in (value if isinstance(value, list) else [value])))
                                       for name, value in typed.items())
    settings.shader_textures = "; ".join("%s=%s" % item for item in typed_textures.items())


def _shader_assignments(settings):
    """(variables, textures) for the compiler from the rows, plus typed values of an older file whose rows were never
    built."""
    variables = _parse_assignments(settings.shader_values, "numbers")
    textures = _parse_assignments(settings.shader_textures, "words")
    for row in settings.shader_value_rows:
        if row.changed:
            numbers = list(row.color if row.is_color else row.value)[:row.floats]
            variables[row.name] = numbers[0] if row.floats == 1 else numbers
    for row in settings.shader_texture_rows:
        if row.source != "game":
            textures[row.name] = row.source
    return variables, textures


class DarktideMaterialSettings(bpy.types.PropertyGroup):
    mode: EnumProperty(name="Material intent", default="generated", items=[
        ("generated", "Generated from glTF PBR", "Build a Darktide material from this material's glTF PBR values"),
        ("external", "Use external material", "Bind an existing native Darktide material resource"),
        ("donor", "World blend material", "Reuse one of the game's world surface blend materials from its .stream file, with your blend values"),
        ("game_shader", "Game shader", "Use one of the game's own shaders (glass, water, hologram, glow, fur...) with your values"),
        ("shader", "Own shader", "Your own shader code: a surface function in HLSL that sets colour, normal, roughness, "
                                 "metal, glow and opacity; lit, shadowed and streamed like the game's own"),
    ])
    shader_code: PointerProperty(type=bpy.types.Text, name="Code",
                                 description="The text (Text Editor) with the shader's surface function")
    own_parameters: CollectionProperty(type=DarktideOwnShaderParameter)
    own_textures: CollectionProperty(type=DarktideOwnShaderTexture)
    own_clip: BoolProperty(name="Clip by opacity", default=False,
                           description="Cut away the parts where the code's s.opacity is below the cutoff (in its shadow too)")
    own_cutoff: FloatProperty(name="Cutoff", default=0.5, min=0.0, max=1.0)
    own_decal: BoolProperty(name="Decal", default=False,
                            description="Paint the code's colour onto whatever is inside the mesh's box, projected along "
                                        "its Y axis, like the game's blood and scorch marks (use a box mesh)")
    game_shader: EnumProperty(name="Shader", items=_game_shader_items,
                              update=lambda self, context: _shader_rows(self),
                              description="Game shader to copy; its settings and texture slots are listed below")
    game_material: StringProperty(name="Game material", update=lambda self, context: _shader_rows(self),
                                  description="Copy any of the game's materials instead, by resource path "
                                              "(content/...) or #hash; needs the game files extract")
    shader_value_rows: CollectionProperty(type=DarktideShaderValue)
    shader_texture_rows: CollectionProperty(type=DarktideShaderTexture)
    shader_values: StringProperty(name="Values", description="Variables to change, e.g. color=1,0.2,0.1; opacity=0.8")
    shader_textures: StringProperty(name="Textures",
                                    description="Texture channels to fill from this material's glTF images, "
                                                "e.g. bca=base_color; nm=normal (others: orm, emissive)")
    resource: StringProperty(name="Material resource", description="Native material resource identity, e.g. content/mods/my_mod/materials/paint")
    donor_stream: StringProperty(name="Donor stream", description="Native v61 .streamdata or .stream material file", subtype="FILE_PATH")
    override_nm_r_blend: BoolProperty(name="Override nm_r_blend", default=False)
    nm_r_blend: FloatProperty(name="nm_r_blend", default=0.0)
    override_shared_blend: BoolProperty(name="Override shared_blend", default=False)
    shared_blend: FloatProperty(name="shared_blend", default=0.0)
    override_bc_blend: BoolProperty(name="Override bc_blend", default=False)
    bc_blend: FloatProperty(name="bc_blend", default=0.0)
    double_sided: BoolProperty(
        name="Double-sided", default=False,
        description="Show the back faces too (leaves, cloth, cards, flat signs). They are added as extra triangles, "
                    "so only tick it where it's needed",
    )
    surface: EnumProperty(name="Surface response", default="default", items=[
        ("default", "Default", "Default material surface context"),
        ("metal_solid", "Solid metal", "Solid metal surface context"),
        ("metal_sheet", "Sheet metal", "Sheet metal surface context"),
        ("cloth", "Cloth", "Cloth surface context"),
        ("concrete", "Concrete", "Concrete surface context"),
        ("brick", "Brick", "Brick surface context"),
        ("bone", "Bone", "Bone surface context"),
        ("plastic", "Plastic", "Plastic surface context"),
    ])


class DarktideDangleSettings(bpy.types.PropertyGroup):
    enabled: BoolProperty(name="Dangle", default=False,
                          description="Let this bone swing freely under gravity in game (pendulum). "
                                      "The unit gets a state machine; mods start it with Unit.enable_animation_state_machine")
    length: FloatProperty(name="Length", default=0.0, min=0.0, subtype="DISTANCE",
                          description="Pendulum length; 0 uses the distance to the child bone, or this bone's length")
    mass: FloatProperty(name="Mass", default=1.0, min=0.001)
    gravity: FloatProperty(name="Gravity", default=9.82)
    damping: FloatProperty(name="Damping", default=3.0, min=0.0, description="Higher settles faster")
    stiffness: FloatProperty(name="Stiffness", default=0.0, min=0.0,
                             description="Pull back toward the bone's rest direction; 0 hangs freely")
    max_angle: FloatProperty(name="Max angle", default=0.0, min=0.0, max=180.0,
                             description="Swing limit in degrees from the rest direction; 0 is unlimited")
    mode: EnumProperty(name="Mode", default="swing", items=[
        ("swing", "Swing", "Pendulum: the bone swings around its head (straps, cables, tassels)"),
        ("jiggle", "Jiggle", "Spring: the bone's position bounces behind its animated position (pouches, bellies)"),
    ])
    jiggle_stiffness: FloatProperty(name="Stiffness", default=1000.0, min=0.0, description="Spring strength")
    jiggle_damping: FloatProperty(name="Damping", default=400.0, min=0.0, description="Higher settles faster")
    max_stretch: FloatProperty(name="Max stretch", default=0.1, min=0.001, subtype="DISTANCE",
                               description="How far the bone may lag behind its animated position")


class DarktideAimSettings(bpy.types.PropertyGroup):
    enabled: BoolProperty(name="Aim", default=False,
                          description="Turn bones so this bone points at a target your mod moves "
                                      "(Unit.animation_set_constraint_target), like a turret or a head following you")
    target: StringProperty(name="Target", default="aim_target",
                           description="Target name; your mod finds it with Unit.animation_find_constraint_target")
    turn: StringProperty(name="Turn", default="",
                         description="Bones that turn toward the target, as bone:weight, e.g. j_spine2:0.2, j_neck:1 "
                                     "for an aim on j_head. Empty turns this bone's parent fully")


class DarktideColliderSettings(bpy.types.PropertyGroup):
    stable_id: StringProperty(name="Stable ID")
    is_body: BoolProperty(name="Body", default=False)
    actor: EnumProperty(name="Body behavior", items=ACTOR_ITEMS, default="static")
    mass: FloatProperty(name="Mass", default=1.0, min=0.0)
    material: _enum(("default", "iron", "rubber"), "default")
    body: PointerProperty(name="Body", type=bpy.types.Object)
    joint_kind: EnumProperty(name="Joint", default="none", items=[
        ("none", "None", ""), ("fixed", "Fixed", ""), ("hinge", "Hinge", "Turns around the empty's x axis"),
        ("ragdoll", "Ragdoll", "Swings and twists within limits, like a body joint"),
        ("slider", "Slider", "Slides along the empty's x axis, like a drawer"),
        ("ball", "Ball", "Turns freely in every direction"),
    ])
    body_a: PointerProperty(name="Body A", type=bpy.types.Object)
    body_b: PointerProperty(name="Body B", type=bpy.types.Object)
    hinge_limits: BoolProperty(name="Limit twist", default=False)
    twist_min: FloatProperty(name="Twist minimum", default=-3.14159265, subtype="ANGLE")
    twist_max: FloatProperty(name="Twist maximum", default=3.14159265, subtype="ANGLE")
    swing_y: FloatProperty(name="Swing Y", default=0.78539816, subtype="ANGLE", min=0.0)
    swing_z: FloatProperty(name="Swing Z", default=0.78539816, subtype="ANGLE", min=0.0)
    slide_limits: BoolProperty(name="Limit travel", default=False)
    slide_min: FloatProperty(name="Travel minimum", default=-0.5, subtype="DISTANCE")
    slide_max: FloatProperty(name="Travel maximum", default=0.5, subtype="DISTANCE")
    spring_stiffness: FloatProperty(name="Spring", default=0.0, min=0.0,
                                    description="Pulls the joint back to how it sits in blender (0 = no spring), "
                                                "e.g. a door that swings shut by itself")
    spring_damping: FloatProperty(name="Spring damping", default=0.0, min=0.0,
                                  description="Slows the spring down so it settles instead of bouncing")
    break_force: FloatProperty(name="Break force", default=0.0, min=0.0,
                               description="The joint snaps above this force in newtons (0 = never)")
    break_torque: FloatProperty(name="Break torque", default=0.0, min=0.0,
                                description="The joint snaps above this twisting force (0 = never)")
    shape: EnumProperty(name="Collision geometry", default="geometry", items=[
        ("geometry", "Mesh surface", "Use this mesh's triangle surface for static/keyframed bodies"),
        ("convex", "Convex mesh", "Use a convex hull of this authored mesh for a simulated body"),
        ("capsule", "Capsule", "A capsule fitted to this mesh along its longest axis (bodies of their own)"),
        ("sphere", "Sphere", "A sphere fitted to this mesh (bodies of their own)"),
        ("box", "Box", "A box fitted to this mesh (bodies of their own)"),
    ])
    role: _enum(("render", "collision", "both"), "render")
    unit_actor: EnumProperty(name="Own actor", default="none", items=[
        ("none", "None", "A normal mesh"),
        ("hit_zone", "Hit zone (EXPERIMENTAL)", "An enemy hit zone: shots and swings land on it, it follows the bone it's parented "
                                 "to while the character animates. Breeds list their hit zones by these names"),
        ("trigger", "Trigger volume", "Not solid: a Trigger Volume node in the unit's flow fires when a player moves "
                                      "into or out of it"),
        ("custom", "Other", "An actor of its own with the physics templates you type in"),
    ], description="Turn this mesh into a collision actor of its own, named after the object (not drawn)")
    actor_name: StringProperty(name="Actor name", description="Name scripts find it by (empty = the object's name)")
    is_volume: BoolProperty(name="Volume", default=False,
                            description="Turn this mesh into an area scripts can test positions against "
                                        "(Unit.is_point_inside_volume), not drawn and not solid. Its bottom face is the "
                                        "floor outline, its top the height")
    volume_name: StringProperty(name="Volume name",
                                description="Name scripts find it by (empty = the object's name). The game's own "
                                            "scripts look for c_volume")
    actor_shape: EnumProperty(name="Shape", default="capsule", items=[
        ("capsule", "Capsule", "Fitted along the mesh's longest axis"),
        ("sphere", "Sphere", ""),
        ("box", "Box", ""),
        ("convex", "Convex", "A convex hull of the mesh"),
        ("geometry", "Mesh surface", "The triangles themselves (not for simulated bodies)"),
    ])
    actor_template: StringProperty(name="Actor template", default="keyframed",
                                   description="physics_properties actor template: static, dynamic, keyframed, "
                                               "or #1234abcd for one only known by its hash")
    actor_shape_template: StringProperty(name="Shape template", default="default",
                                         description="physics_properties shape template (what it collides with), "
                                                     "or #1234abcd")
    actor_spawn: BoolProperty(name="Created at spawn", default=True,
                              description="Off leaves it to a script (Unit.create_actor)")
    hit_zone_group: EnumProperty(name="Hit zone group (EXPERIMENTAL)", items=lambda self, context: _zone_items(),
                                 description="Which part of the body a hit here counts as (head takes headshots) "
                                             "in a breed you make")
    use_asset_collision: BoolProperty(
        name="Use as asset collision",
        description="When the asset uses visible mesh collision, include this mesh as a collider",
        default=True,
    )
    part_name: StringProperty(
        name="Own unit",
        description="Compiles this empty and everything parented under it as a unit of its own, next to the asset in "
                    "the same folder and package (content/mods/.../<this name>), using the same materials and textures. "
                    "The empty is that unit's origin. Lowercase letters, digits and _",
    )
    visibility_group: StringProperty(
        name="Visibility group",
        description="Name of a group this object's meshes (and all meshes below it) belong to; "
                    "toggle in game with Unit.set_visibility(unit, name, visible)",
    )
    render_visible: BoolProperty(
        name="Visible", default=True,
        description="Draw this object's meshes (and all meshes below it). Off with Casts shadow on makes a "
                    "shadow-only stand-in, like the game's simplified shadow meshes",
    )
    render_shadow: BoolProperty(
        name="Casts shadow", default=True,
        description="This object's meshes (and all meshes below it) cast shadows",
    )
    lod_group: StringProperty(
        name="LOD group",
        description="Makes this object (its meshes and all meshes below it) one detail level of a LOD object "
                    "with this name. The game's units use \"lod\"; weapons and gear need it named that way",
    )
    lod_level: IntProperty(
        name="Level", min=0, default=0,
        description="0 = full detail, 1 = the next simpler version, and so on",
    )
    lod_down_to: FloatProperty(
        name="Visible down to", min=0.0, default=0.0, precision=3,
        description="Screen height share (1 = the object fills the screen height) below which the next level "
                    "takes over. 0 on the last level keeps it visible at any distance",
    )


_COLLECTION_OPTIONS = ("actor", "mass", "material", "visible_meshes_collide",
                       "asset_filename", "scale", "output_kind", "select_clip", "clip_index",
                       "embed_simple_animation", "simple_animation_clip", "auto_loop_single_clip",
                       "create_looping_state_machine", "loop_clip_index", "state_machine_playback",
                       "create_state_graph", "ragdoll_event", "in_place", "weapon_materials", "flow_tree",
                       "animation_translation_tolerance", "animation_scale_tolerance",
                       "animation_rotation_tolerance_radians", "animation_fit_advanced",
                       "only_weighted_bones")


def _copy_state_graph(source, target):
    rows = (
        ("state_graph_states", ("state_name", "clip_index", "playback", "blend", "blend_variable", "blend_clips",
                                "speed", "speed_from_variable", "speed_variable", "random", "random_pick",
                                "layer", "empty", "additive", "bones", "blend_2d", "blend_variable2",
                                "events_at", "exit_event", "exit_blend")),
        ("state_graph_variables", ("variable_name", "initial_value", "minimum", "maximum")),
        ("state_graph_transitions", ("from_state", "to_state", "event_name", "blend_seconds",
                                     "condition", "variable_index", "lower", "upper",
                                     "lower_exclusive", "upper_exclusive")),
    )
    for collection_name, fields in rows:
        source_rows = getattr(source, collection_name)
        target_rows = getattr(target, collection_name)
        if target_rows:
            continue
        for source_row in source_rows:
            target_row = target_rows.add()
            for field in fields:
                setattr(target_row, field, getattr(source_row, field))


def _asset_options(scene):
    """Keep authoring choices with the collection when a .blend holds many assets."""
    scene_settings = scene.dt_asset
    collection = scene_settings.asset_collection
    if collection is None:
        return scene_settings
    options = collection.dt_asset_identity
    if options.settings_version == 0:
        for name in _COLLECTION_OPTIONS:
            setattr(options, name, getattr(scene_settings, name))
        _copy_state_graph(scene_settings, options)
        options.settings_version = 2
    elif options.settings_version < 2:
        options.auto_loop_single_clip = scene_settings.auto_loop_single_clip
        options.settings_version = 2
    return options


IES_PROFILES = ["bollard", "comet", "defined", "defineddiffuse", "defineddiffusespot", "definedspot",
                "narrow/flashlight_custom_01", "narrow/flashlight_custom_02", "narrow/flashlight_custom_03",
                "narrow/narrow_01", "narrow/narrow_02", "narrow/narrow_03", "narrow/narrow_04", "narrow/narrow_05",
                "narrow/narrow_06", "narrow/narrow_07", "overhead", "pear", "round", "toppost", "xarrow", "xarrowdiffuse",
                "xarrowsoft"]


class DarktideLightSettings(bpy.types.PropertyGroup):
    particle_intensity: FloatProperty(name="Particles", default=1.0, min=0.0, soft_max=2.0,
                                      description="How strongly it lights smoke and other lit particles")
    falloff_start: FloatProperty(name="Full brightness to", default=0.0, min=0.0, subtype="DISTANCE",
                                 description="Distance it stays at full brightness before fading out to its range "
                                             "(point and spot lamps)")
    shadow_resolution: EnumProperty(name="Shadow detail", default="128", items=[
        (str(size), str(size), "Largest shadow map size in pixels") for size in (32, 64, 128, 256, 512, 1024)])
    dynamic: BoolProperty(name="Moving shadows", default=False,
                          description="Updates its shadows every frame: for a lamp that moves or lights things that "
                                      "move (a lamp on a weapon or a swinging lantern)")
    lens_flare: BoolProperty(name="Lens flare", default=False)
    reflector: BoolProperty(name="Spot reflector", default=False,
                            description="Bounces some light back around the lamp like a reflector dish (spot and disk lamps)")
    ies: EnumProperty(name="Light shape", default="none",
                      items=[("none", "None", "Even light in every direction the lamp shines")] +
                            [(name, name.split("/")[-1].replace("_", " "), "The game's light profile " + name)
                             for name in IES_PROFILES] +
                            [("custom", "Other", "Another of the game's light profiles by its resource path or #hash")],
                      description="An IES light profile from the game: the pattern the light makes (rings, a hot "
                                  "spot, a narrow beam). Needs the game files extract")
    ies_custom: StringProperty(name="Profile", description="content/environment/ies_profiles/... or # and 16 hex digits")
