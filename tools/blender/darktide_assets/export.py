"""Exporting an asset collection to the compiler's GLB (with the Darktide extras) and running the compiler."""

import os
import shutil
import sys
import json
import subprocess
import uuid
import copy
import re
import math
import struct
import fnmatch
import tempfile
import bpy
from mathutils import Matrix, Vector
from .reference_skeleton import load_reference
from . import game_unit
from . import state_graph
from . import particle_effect
from . import flow_editor
from .settings import MATERIAL_SCHEMA_KEY, SCHEMA_KEY, _asset_options, _shader_assignments, _shader_preset, own_shader_extras, prefs


def _new_id():
    return str(uuid.uuid4())


def _reference_armature(context, collection, unit_path, bones_path, resource_name):
    nodes, scene_node_count = load_reference(unit_path, bones_path, resource_name)
    armature = bpy.data.armatures.new("Darktide Reference Skeleton")
    obj = bpy.data.objects.new("Darktide Reference Skeleton", armature)
    collection.objects.link(obj)
    previous_active = context.view_layer.objects.active
    previous_selected = list(context.selected_objects)
    previous_mode = previous_active.mode if previous_active else "OBJECT"
    try:
        if previous_mode != "OBJECT":
            bpy.ops.object.mode_set(mode="OBJECT")
        bpy.ops.object.select_all(action="DESELECT")
        obj.select_set(True)
        context.view_layer.objects.active = obj
        bpy.ops.object.mode_set(mode="EDIT")
        edit_bones = armature.edit_bones
        by_name = {}
        world_matrices = {}
        for node in nodes:
            values = node["world"]
            world = Matrix(tuple(tuple(values[row + column * 4] for column in range(4))
                                 for row in range(4)))
            location = Vector((values[12], values[13], values[14]))
            by_name[node["name"]] = edit_bones.new(node["name"])
            world_matrices[node["name"]] = (world, location)
        children = {node["name"]: [] for node in nodes}
        for node in nodes:
            if node["parent"]:
                children[node["parent"]].append(node["name"])
        for node in nodes:
            name = node["name"]
            bone = by_name[name]
            world, head = world_matrices[name]
            child_positions = [world_matrices[child][1] for child in children[name]]
            length = min(((point - head).length for point in child_positions if (point - head).length > 1e-5),
                         default=0.03)
            basis = world.to_3x3().normalized()
            bone.head = head
            bone.tail = head + (basis @ Vector((0.0, 0.0, length)))
            bone.align_roll(basis @ Vector((0.0, -1.0, 0.0)))
            if node["parent"]:
                bone.parent = by_name[node["parent"]]
                bone.use_connect = False
        bpy.ops.object.mode_set(mode="OBJECT")
    except Exception:
        if obj.mode == "EDIT":
            bpy.ops.object.mode_set(mode="OBJECT")
        bpy.data.objects.remove(obj, do_unlink=True)
        bpy.data.armatures.remove(armature)
        raise
    finally:
        bpy.ops.object.select_all(action="DESELECT")
        for selected in previous_selected:
            if selected.name in context.view_layer.objects:
                selected.select_set(True)
        context.view_layer.objects.active = previous_active
        if previous_active and previous_mode != "OBJECT":
            bpy.ops.object.mode_set(mode=previous_mode)
    obj["darktide_reference_unit"] = bpy.path.abspath(unit_path)
    obj["darktide_reference_bones"] = bpy.path.abspath(bones_path)
    obj["darktide_scene_node_count"] = scene_node_count
    return obj


def _glb_animations(path):
    """Return (animation names, number of skins) from the JSON chunk of the exported GLB."""
    with open(path, "rb") as glb:
        header = glb.read(12)
        if len(header) != 12:
            raise ValueError("Exported GLB has an incomplete header")
        magic, version, total_length = struct.unpack("<4sII", header)
        if magic != b"glTF" or version != 2 or total_length < 20:
            raise ValueError("Exported file is not a valid GLB 2.0")
        while glb.tell() + 8 <= total_length:
            chunk_length, chunk_type = struct.unpack("<II", glb.read(8))
            chunk = glb.read(chunk_length)
            if len(chunk) != chunk_length:
                raise ValueError("Exported GLB has an incomplete chunk")
            if chunk_type == 0x4E4F534A:
                document = json.loads(chunk.decode("utf-8"))
                return ([animation.get("name", "") for animation in document.get("animations", [])],
                        len(document.get("skins", [])))
        raise ValueError("Exported GLB does not contain a JSON chunk")


def _validate_state_graph(options):
    states = options.state_graph_states
    if not states:
        raise ValueError("Add at least one state to the event-driven state graph")
    names = [state.state_name.strip() for state in states]
    if any(not name for name in names):
        raise ValueError("Every state needs a name")
    if len(set(names)) != len(names):
        raise ValueError("State names must be unique")
    layers = sorted({state.layer for state in states})
    if layers != list(range(len(layers))):
        raise ValueError("Layers have to count up from 0 without gaps (layer " + str(len(layers)) + " is missing)")
    for transition in options.state_graph_transitions:
        if (transition.from_state < len(states) and transition.to_state < len(states) and
                states[transition.from_state].layer != states[transition.to_state].layer):
            raise ValueError("Transition from state " + str(transition.from_state) + " to state " +
                             str(transition.to_state) + " crosses layers; transitions stay inside a layer")
    clip_count = re.search(r"Clips \((\d+)\)", options.inspection_summary)
    if clip_count:
        count = int(clip_count.group(1))
        for index, state in enumerate(states):
            if state.clip_index >= count:
                raise ValueError("State " + str(index) + " clip index must be below " + str(count))
    variables = options.state_graph_variables
    variable_names = [variable.variable_name.strip() for variable in variables]
    if any(not name for name in variable_names) or len(set(variable_names)) != len(variable_names):
        raise ValueError("State graph variable names must be unique and nonempty")
    for index, variable in enumerate(variables):
        if (not all(math.isfinite(value) for value in
                    (variable.initial_value, variable.minimum, variable.maximum)) or
                variable.minimum > variable.maximum or
                not variable.minimum <= variable.initial_value <= variable.maximum):
            raise ValueError("Variable " + str(index) + " needs finite ordered bounds containing its initial value")
    event_bindings = {}
    for transition in options.state_graph_transitions:
        if "*" in transition.event_name and not any(fnmatch.fnmatchcase(event, transition.event_name.strip())
                                                    for event in state_graph.known_events(options)):
            raise ValueError("No event matches " + transition.event_name.strip())
    for index, transition in enumerate(state_graph.expand(options)):
        if transition.from_state >= len(states) or transition.to_state >= len(states):
            raise ValueError("Transition " + str(index) + " refers to a missing state")
        if not transition.event_name.strip():
            raise ValueError("Every transition needs an event name")
        if not math.isfinite(transition.blend_seconds) or transition.blend_seconds < 0:
            raise ValueError("Transition blend seconds must be a finite nonnegative value")
        variable_index = transition.variable_index if transition.condition == "range" else None
        key = (transition.from_state, transition.event_name.strip())
        if key in event_bindings and (event_bindings[key] != variable_index or variable_index is None):
            raise ValueError("State/event " + str(key) + " must use one variable selector or one direct transition")
        event_bindings[key] = variable_index
        if variable_index is not None:
            if variable_index >= len(variables):
                raise ValueError("Transition " + str(index) + " refers to a missing variable")
            if (not math.isfinite(transition.lower) or not math.isfinite(transition.upper) or
                    transition.lower > transition.upper or
                    (transition.lower == transition.upper and
                     (transition.lower_exclusive or transition.upper_exclusive))):
                raise ValueError("Transition " + str(index) + " needs a nonempty finite range")


def _collider_role(obj, include_visible=False):
    if obj.dt_collider.unit_actor != "none" or obj.dt_collider.is_volume:
        return "render"     # an actor's or volume's mesh is its shape, the compiler takes it out
    role = obj.dt_collider.role
    if role == "render" and include_visible and obj.dt_collider.use_asset_collision:
        return "both"
    return role


def _collider_objects(objects, include_visible=False):
    return [o for o in objects if o.type == "MESH" and
            _collider_role(o, include_visible) != "render"]


def _authored_objects(objects, include_visible=False):
    return [o for o in objects if o.dt_collider.is_body or o.dt_collider.joint_kind != "none" or
            o.dt_collider.visibility_group.strip() or o.dt_collider.lod_group.strip() or
            not o.dt_collider.render_visible or not o.dt_collider.render_shadow or
            (o.type == "EMPTY" and o.dt_particles.enabled) or o.type == "LIGHT" or
            (o.type == "MESH" and _collider_role(o, include_visible) != "render") or
            (o.type == "MESH" and (o.dt_collider.unit_actor != "none" or o.dt_collider.is_volume))]


def _actor_extras(obj):
    settings = obj.dt_collider
    hit_zone = settings.unit_actor == "hit_zone"
    # a trigger players set off, like the retail train backdrop's (b11d4026ca356550): a static actor on the trigger
    # shape template that collides with group 1, the player's capsule actor (the one named "trigger" collides with
    # nothing; physics_properties 80bffd3179f4a023)
    trigger = settings.unit_actor == "trigger"
    result = {
        "name": settings.actor_name.strip() or obj.name,
        "template": game_unit.HIT_ZONE_TEMPLATE if hit_zone else "static" if trigger else settings.actor_template.strip(),
        "shape_template": game_unit.HIT_ZONE_SHAPE_TEMPLATE if hit_zone else "#5fa361eb" if trigger else
        settings.actor_shape_template.strip(),
        "shape": settings.actor_shape,
        "spawn": settings.actor_spawn,
    }
    if not result["template"] or not result["shape_template"]:
        raise ValueError(obj.name + ": an actor needs an actor template and a shape template")
    return result


def _mover_extras(options):
    return {"height": options.mover_height, "radius": options.mover_radius, "slope_limit": options.mover_slope,
            "collision_filter": options.mover_filter.strip() or "filter_minion_mover"}


def _resolve_ids(objects, include_visible=False):
    """Ensure persistent IDs exist and make duplicates unique at export time."""
    seen = set()
    for obj in _authored_objects(objects, include_visible):
        stable_id = obj.dt_collider.stable_id.strip()
        if not stable_id or stable_id in seen:
            stable_id = _new_id()
        obj.dt_collider.stable_id = stable_id
        seen.add(stable_id)


def _scene_extras(scene):
    settings = _asset_options(scene)
    return {
        "version": 1,
        "body": {
            "id": "main",
            "actor": settings.actor,
            "mass": settings.mass,
            "material": settings.material,
        },
    }


def _nearest_body(obj):
    current = obj
    seen = set()
    while current and current not in seen:
        seen.add(current)
        if current.dt_collider.is_body:
            return current
        current = current.parent
    return None


def _v2_objects(objects):
    return any(o.dt_collider.is_body or o.dt_collider.joint_kind != "none" for o in objects)


def _validate_v2(objects, include_visible=False):
    objects = set(objects)
    bodies = [o for o in objects if o.dt_collider.is_body]
    for body in bodies:
        if body.dt_collider.actor == "dynamic" and body.dt_collider.mass <= 0:
            raise ValueError("Dynamic body mass must be positive: " + body.name)
        if not any(o.type == "MESH" and _collider_role(o, include_visible) != "render" and
                   (o.dt_collider.body or _nearest_body(o)) == body for o in objects):
            raise ValueError("Every version 2 body requires a collider")
    for obj in objects:
        settings = obj.dt_collider
        if obj.type == "MESH" and _collider_role(obj, include_visible) != "render":
            target = settings.body or _nearest_body(obj)
            if target not in objects or not target.dt_collider.is_body:
                raise ValueError("Collider must target a body in the active scene")
        if settings.joint_kind != "none":
            if settings.body_a not in objects or settings.body_b not in objects or not settings.body_a.dt_collider.is_body or not settings.body_b.dt_collider.is_body:
                raise ValueError("Joint endpoints must be bodies in the active scene")
            if settings.body_a == settings.body_b:
                raise ValueError("Joint endpoints must be distinct")
            if settings.body_a.dt_collider.actor != "dynamic" and settings.body_b.dt_collider.actor != "dynamic":
                raise ValueError("A joint needs at least one dynamic body")
            if settings.hinge_limits and settings.twist_min > settings.twist_max:
                raise ValueError("Twist minimum must not exceed maximum")
            if settings.joint_kind == "slider" and settings.slide_limits and settings.slide_min > settings.slide_max:
                raise ValueError("Travel minimum must not exceed maximum")


def _v2_object_extras(obj, include_visible=False):
    settings = obj.dt_collider
    result = {"version": 2, "id": settings.stable_id}
    if settings.is_body:
        result["body"] = {"actor": settings.actor, "mass": settings.mass, "material": settings.material}
    if obj.type == "MESH" and _collider_role(obj, include_visible) != "render":
        target = settings.body or _nearest_body(obj)
        result["collider"] = {"body": target.dt_collider.stable_id,
                              "shape": settings.shape, "role": _collider_role(obj, include_visible)}
    if settings.joint_kind != "none":
        joint = {"body_a": settings.body_a.dt_collider.stable_id, "body_b": settings.body_b.dt_collider.stable_id, "kind": settings.joint_kind}
        if settings.joint_kind == "hinge" and settings.hinge_limits:
            joint.update({"twist_min": settings.twist_min, "twist_max": settings.twist_max})
        if settings.joint_kind == "ragdoll":
            joint.update({"swing_y": settings.swing_y, "swing_z": settings.swing_z})
            if settings.hinge_limits:
                joint.update({"twist_min": settings.twist_min, "twist_max": settings.twist_max})
        if settings.joint_kind == "slider" and settings.slide_limits:
            joint.update({"slide_min": settings.slide_min, "slide_max": settings.slide_max})
        if settings.joint_kind != "fixed" and (settings.spring_stiffness > 0.0 or settings.spring_damping > 0.0):
            joint.update({"spring_stiffness": settings.spring_stiffness, "spring_damping": settings.spring_damping})
        if settings.break_force > 0.0:
            joint["break_force"] = settings.break_force
        if settings.break_torque > 0.0:
            joint["break_torque"] = settings.break_torque
        result["joint"] = joint
    if settings.visibility_group.strip():
        result["visibility_group"] = settings.visibility_group.strip()
    if settings.lod_group.strip():
        result["lod"] = _lod_extras(settings)
    if not settings.render_visible or not settings.render_shadow:
        result["render"] = _render_extras(settings)
    if obj.type == "LIGHT":
        result["light"] = _light_extras(obj)
    return result


def _light_extras(obj):
    lamp = obj.data
    settings = lamp.dt_light
    # the colour without the temperature (the game applies that itself), which the glTF exporter multiplies in
    result = {"shadow": bool(lamp.use_shadow), "temperature": lamp.temperature if lamp.use_temperature else 6500.0,
              "red": lamp.color[0], "green": lamp.color[1], "blue": lamp.color[2],
              "volumetric": lamp.volume_factor, "particle": settings.particle_intensity,
              "falloff_start": settings.falloff_start, "shadow_resolution": int(settings.shadow_resolution),
              "lens_flare": settings.lens_flare, "reflector": settings.reflector, "dynamic": settings.dynamic}
    if settings.ies != "none":
        profile = settings.ies_custom.strip() if settings.ies == "custom" else \
            "content/environment/ies_profiles/" + settings.ies
        if not profile:
            raise ValueError(obj.name + ": type the light profile's resource path or #hash")
        extract = os.path.abspath(bpy.path.abspath(prefs().game_extract_folder.strip()))
        if not prefs().game_extract_folder.strip() or not os.path.isdir(extract):
            raise ValueError(obj.name + ": light shapes need 'Game files extract' in the Setup panel set to your limn extract")
        result["ies"] = profile
        result["extract"] = extract
    if lamp.type == "AREA":
        # glTF has no area lamps, so the compiler makes the light from these
        rectangle = lamp.shape in ("SQUARE", "RECTANGLE")
        result["area"] = {
            "shape": "rectangle" if rectangle else "disk",
            "width": lamp.size,
            "height": lamp.size_y if lamp.shape in ("RECTANGLE", "ELLIPSE") else lamp.size,
            "watts": lamp.energy,
            "range": lamp.cutoff_distance if lamp.use_custom_distance else 0.0,
            "spread": lamp.spread,
        }
    return result


def _render_extras(settings):
    return {"visible": settings.render_visible, "shadow": settings.render_shadow}


def _lod_extras(settings):
    return {"group": settings.lod_group.strip(), "level": settings.lod_level, "down_to": settings.lod_down_to}


def _object_extras(obj, include_visible=False):
    settings = obj.dt_collider
    role = "both" if include_visible and settings.role == "render" else settings.role
    return {
        "version": 1,
        "id": settings.stable_id,
        "collider": {"body": "main", "shape": settings.shape, "role": role},
    }


def _set_if_supported(kwargs, operator, name, value):
    if name in operator.get_rna_type().properties:
        kwargs[name] = value


def _set_required(kwargs, operator, name, value):
    if name not in operator.get_rna_type().properties:
        raise RuntimeError("Installed glTF exporter lacks required option: " + name)
    kwargs[name] = value


def _copy_idproperty(value):
    if hasattr(value, "to_dict"):
        return copy.deepcopy(value.to_dict())
    return copy.deepcopy(value)


ENEMY_OUTLINE_LAYERS = (("minion_outline", "#ID[365ea051cdf53ac8]"),
                        ("minion_outline_reversed_depth", "#ID[e6481945702287a1]"))


def _material_layers(identity):
    layers = list(ENEMY_OUTLINE_LAYERS) if identity.enemy_outline else []
    for part in identity.material_layers.split(";"):
        if not part.strip():
            continue
        name, _, material = part.partition("=")
        if not name.strip() or not material.strip():
            raise ValueError("Material layers: write name=material, e.g. my_glow=content/mods/my_mod/glow")
        layers.append((name.strip(), material.strip()))
    return layers


def _asset_stem(value):
    stem = (value or "").strip() or "asset"
    reserved = {"CON", "PRN", "AUX", "NUL"} | {
        prefix + str(index) for prefix in ("COM", "LPT") for index in range(1, 10)}
    if (os.path.isabs(stem) or stem in {".", ".."} or
            any(char in '<>:"/\\|?*' or ord(char) < 32 for char in stem) or
            stem.endswith(".") or stem.split(".", 1)[0].upper() in reserved):
        raise ValueError("Asset filename must be a simple filename stem")
    return stem


def _suggest_stem(name):
    return re.sub(r"[^A-Za-z0-9_-]+", "_", name).strip("_") or "asset"


def _compiler_path():
    if not prefs().compiler.strip():
        raise ValueError("Set the compiler in the Setup panel first")
    compiler = bpy.path.abspath(prefs().compiler)
    if not os.path.isfile(compiler):
        raise ValueError("Compiler executable not found: " + compiler)
    return compiler


def _wine_tools(compiler):
    """Return Wine and winepath executables for a Linux-hosted Windows compiler."""
    if not sys.platform.startswith("linux") or not os.path.basename(compiler).lower().endswith(".exe"):
        return None
    wine = shutil.which("wine") or shutil.which("wine64")
    if not wine:
        raise ValueError("This .exe compiler needs Wine on Linux; install wine or wine64 and retry")
    winepath = shutil.which("winepath") or shutil.which("winepath-stable")
    if not winepath:
        raise ValueError("This .exe compiler needs winepath on Linux; install winepath or winepath-stable and retry")
    return wine, winepath


def _windows_path(path, wine_tools):
    if wine_tools is None:
        return os.fspath(path)
    result = subprocess.run([wine_tools[1], "-w", os.fspath(path)],
                            capture_output=True, text=True, timeout=120, check=False)
    converted = (result.stdout or "").strip()
    if result.returncode or not converted:
        detail = (result.stderr or result.stdout or "winepath returned no path").strip()
        raise ValueError("Could not convert path for Wine: " + os.fspath(path) + "; " + detail[:140])
    return converted


def _run_compiler(compiler, arguments, wine_tools, **kwargs):
    command = ([wine_tools[0], compiler] if wine_tools else [compiler]) + arguments
    # the compiler reads the game's files (Oodle, shaders, stub textures) from the Darktide folder of the setup
    game = prefs().game_folder.strip()
    if game:
        kwargs["env"] = dict(kwargs.get("env") or os.environ,
                             DARKTIDE_GAME_ROOT=_windows_path(os.path.abspath(bpy.path.abspath(game)), wine_tools))
    return subprocess.run(command, **kwargs)


def _inspection_summary(output):
    """Keep the compiler's clip indices visible beside the Blender clip control."""
    lines = output.splitlines()
    summary = []
    in_clips = False
    for line in lines:
        if line.startswith(("Active meshes:", "Skins (", "Clips (", "Authored physics:")):
            summary.append(line)
            in_clips = line.startswith("Clips (")
        elif in_clips and line.startswith("  ["):
            summary.append(line.strip())
        elif in_clips:
            in_clips = False
    return "\n".join(summary)


def _validate_material_resource(value, material_name):
    name = value
    if not name or name.startswith(("/", "\\")) or "\\" in name or ":" in name:
        raise ValueError("External material resource must be a valid native resource path: " + material_name)
    if any(ord(char) < 32 for char in name):
        raise ValueError("External material resource must be a valid native resource path: " + material_name)
    if any(part == ".." for part in name.split("/")):
        raise ValueError("External material resource must be a valid native resource path: " + material_name)
    return name


def _collection_materials(objects):
    materials = set()
    for obj in objects:
        if obj.type == "MESH" and obj.data and obj.dt_collider.role != "collision":
            materials.update(material for material in obj.data.materials if material)
    return sorted(materials, key=lambda material: (material.name, material.as_pointer()))


def _has_constant_emission_output(material):
    if not material.use_nodes or material.node_tree is None:
        return False
    outputs = [node for node in material.node_tree.nodes
               if node.type == "OUTPUT_MATERIAL" and node.is_active_output]
    if len(outputs) != 1:
        return False
    surface = outputs[0].inputs.get("Surface")
    if surface is None or len(surface.links) != 1:
        return False
    emission = surface.links[0].from_node
    if emission.type != "EMISSION" or surface.links[0].from_socket.name != "Emission":
        return False
    for input_name in ("Color", "Strength"):
        socket = emission.inputs.get(input_name)
        if socket is None or socket.is_linked:
            return False
    return True


_BONE_PATH = re.compile(r'pose\.bones\["((?:[^"\\]|\\.)*)"\]')


def _hide_foreign_actions(objects):
    """The glTF exporter adds every bone action in the file to a lone armature, also actions made for another rig
    (an mmd tools model's dance, the rig an animation was carried over from). Those would come out as motionless
    clips, so actions that move none of the exported bones go on the exporter's own skip list for this export.
    Returns an undo function, or None when nothing needed hiding."""
    bones = {bone.name for obj in objects if obj.type == "ARMATURE" for bone in obj.data.bones}
    users = {}
    for obj in bpy.data.objects:
        data = obj.animation_data
        if data is None:
            continue
        used = [data.action] + [strip.action for track in data.nla_tracks for strip in track.strips]
        for action in used:
            if action is not None:
                users.setdefault(action, set()).add(obj)
    exported = set(objects)
    foreign = []
    for action in bpy.data.actions:
        if action in users and not users[action] & exported:
            foreign.append(action)          # someone else's animation, e.g. the rig it was carried over from
            continue
        moved = set()
        for layer in action.layers:
            for strip in layer.strips:
                for bag in strip.channelbags:
                    for curve in bag.fcurves:
                        match = _BONE_PATH.match(curve.data_path)
                        if match:
                            moved.add(match.group(1))
        if moved and not moved & bones:
            foreign.append(action)
    if not foreign or not bpy.data.scenes:
        return None
    scene = bpy.data.scenes[0]                    # the exporter reads the list from the first scene
    registered = not hasattr(bpy.types.Scene, "gltf_action_filter")
    if registered:
        import io_scene_gltf2
        bpy.types.Scene.gltf_action_filter = bpy.props.CollectionProperty(type=io_scene_gltf2.GLTF2_filter_action)
    added, changed = [], []
    for action in foreign:
        item = next((i for i in scene.gltf_action_filter if i.action == action), None)
        if item is None:
            item = scene.gltf_action_filter.add()
            item.action = action
            added.append(action)
        elif item.keep:
            changed.append(item)
        item.keep = False

    def undo():
        for item in changed:
            item.keep = True
        for index in reversed(range(len(scene.gltf_action_filter))):
            if scene.gltf_action_filter[index].action in added:
                scene.gltf_action_filter.remove(index)
        if registered:
            del bpy.types.Scene.gltf_action_filter
    return undo


def _mmd_stand_in(material):
    """mmd tools materials draw through its own node group, which the glTF exporter can't read. For the export the
    material gets a Principled BSDF output with the same base texture and colour (cut-out textures alpha clipped,
    as the game draws them). Returns an undo function, or None when it isn't an mmd tools material."""
    import numpy as np
    tree = material.node_tree if material.use_nodes else None
    output = next((node for node in tree.nodes if node.type == "OUTPUT_MATERIAL" and node.is_active_output),
                  None) if tree else None
    surface = output.inputs.get("Surface") if output else None
    shader = surface.links[0].from_node if surface and surface.is_linked else None
    if shader is None or shader.type != "GROUP" or not all(
            shader.inputs.get(name) for name in ("Base Tex", "Diffuse Color", "Alpha")):
        return None
    added = [tree.nodes.new("ShaderNodeBsdfPrincipled"), tree.nodes.new("ShaderNodeOutputMaterial")]
    bsdf, stand_in = added
    tree.links.new(bsdf.outputs["BSDF"], stand_in.inputs["Surface"])
    bsdf.inputs["Roughness"].default_value = 0.8
    texture = shader.inputs["Base Tex"].links[0].from_node if shader.inputs["Base Tex"].is_linked else None
    if texture is not None and texture.type == "TEX_IMAGE" and texture.image is not None:
        tree.links.new(texture.outputs["Color"], bsdf.inputs["Base Color"])
        image = texture.image
        pixels = np.empty(len(image.pixels), np.float32)
        image.pixels.foreach_get(pixels)
        if image.channels == 4 and pixels.size and (pixels[3::4] < 0.5).any():
            clip = tree.nodes.new("ShaderNodeMath")
            clip.operation = "ROUND"
            added.append(clip)
            tree.links.new(texture.outputs["Alpha"], clip.inputs[0])
            tree.links.new(clip.outputs[0], bsdf.inputs["Alpha"])
    else:
        r, g, b, _ = shader.inputs["Diffuse Color"].default_value
        bsdf.inputs["Base Color"].default_value = (r, g, b, 1.0)
        if shader.inputs["Alpha"].default_value < 0.999:
            bsdf.inputs["Alpha"].default_value = shader.inputs["Alpha"].default_value
    stand_in.is_active_output = True

    def undo():
        for node in added:
            tree.nodes.remove(node)
        output.is_active_output = True
    return undo


def _apply_modifiers_for_export(objects):
    """Keep Blender's evaluated geometry, except where it would erase morphs."""
    morphs = [obj for obj in objects if obj.type == "MESH" and obj.data and
              obj.data.shape_keys and len(obj.data.shape_keys.key_blocks) > 1]
    if not morphs:
        return True
    modified = [obj.name for obj in objects if obj.type == "MESH" and
                any(mod.show_viewport and mod.type != "ARMATURE" for mod in obj.modifiers)]
    if modified:
        raise ValueError("Blender's glTF exporter cannot preserve both shape keys and evaluated modifiers in this asset: "
                         + ", ".join(modified) + ". Apply those modifiers to a separate mesh or export the meshes separately.")
    return False


def _collision_review(scene, objects):
    options = _asset_options(scene)
    version2 = _v2_objects(objects)
    messages = []
    for obj in objects:
        if obj.type != "MESH" or _collider_role(obj, options.visible_meshes_collide) == "render":
            continue
        body = (obj.dt_collider.body or _nearest_body(obj)) if version2 else None
        actor = body.dt_collider.actor if body else options.actor
        if actor == "dynamic" and obj.dt_collider.shape == "geometry":
            messages.append(obj.name + ": simulated bodies use a convex hull of this mesh; "
                            "use separate editable collision meshes for concave shapes")
    return messages


def _weighted_bone_sets(objects):
    """Map each armature to the names of bones with nonzero vertex weights or objects hung on them (rigid parts,
    hit zones, ragdoll bodies), plus all ancestors."""
    result = {}
    for obj in objects:
        if obj.parent is not None and obj.parent.type == "ARMATURE" and obj.parent_type == "BONE":
            keep = result.setdefault(obj.parent, set())
            bone = obj.parent.data.bones.get(obj.parent_bone)
            while bone is not None:
                keep.add(bone.name)
                bone = bone.parent
        if obj.type != "MESH":
            continue
        rigs = {mod.object for mod in obj.modifiers if mod.type == "ARMATURE" and mod.object is not None}
        if not rigs:
            continue
        group_names = {group.index: group.name for group in obj.vertex_groups}
        used = {group_names[g.group] for vertex in obj.data.vertices for g in vertex.groups
                if g.weight > 0.0 and g.group in group_names}
        for rig in rigs:
            keep = result.setdefault(rig, set())
            for name in used:
                bone = rig.data.bones.get(name)
                while bone is not None:
                    keep.add(bone.name)
                    bone = bone.parent
    return result


# glb is just the handoff to the compiler, nobody is supposed to open it
def export_asset(context, path, donor_path_converter=None):
    """Export the active asset collection to GLB with temporary schema extras.

    ``context`` must provide a Blender context whose ``scene`` is the asset
    scene. ``path`` is the complete output .glb path. Scene/object extras and
    all other authoring properties are restored even if export fails.
    """
    scene = context.scene
    collection = scene.dt_asset.asset_collection
    if collection is None:
        raise ValueError("Create an asset collection from the selection first")
    objects = list(collection.all_objects)
    if not objects:
        raise ValueError("The asset collection is empty")
    apply_modifiers = _apply_modifiers_for_export(objects)
    view_objects = context.view_layer.objects
    missing = [obj.name for obj in objects
               if view_objects.get(obj.name) != obj or not obj.visible_get(view_layer=context.view_layer)]
    if missing:
        raise ValueError(
            "Asset collection members are excluded from the active view layer: "
            + ", ".join(missing)
            + ". Include the asset collection in the active view layer before exporting."
        )
    version2 = _v2_objects(objects)
    include_visible = _asset_options(scene).visible_meshes_collide
    _resolve_ids(objects, include_visible)
    authored = set(_authored_objects(objects, include_visible))
    materials = _collection_materials(objects)
    for material in materials:
        if material.dt_material.mode == "external":
            _validate_material_resource(material.dt_material.resource, material.name)
        elif material.dt_material.mode == "game_shader":
            preset = _shader_preset(material.dt_material)
            if not preset:
                raise ValueError("Material " + material.name + (" needs a game shader" if not material.dt_material.game_material.strip()
                                 else ": game material " + material.dt_material.game_material.strip()
                                 + " isn't in the game files extract"))
            stream = os.path.join(bpy.path.abspath(prefs().game_folder), "bundle", *preset["stream"].split("/"))
            if not os.path.isfile(stream):
                raise ValueError("Game shader stream not found (set the Darktide folder): " + stream)
            _shader_assignments(material.dt_material)
        elif material.dt_material.mode == "shader":
            own_shader_extras(material.dt_material, material.name)
        elif material.dt_material.mode == "donor":
            donor_path = os.path.abspath(bpy.path.abspath(material.dt_material.donor_stream)) if material.dt_material.donor_stream else ""
            if not donor_path or not os.path.isfile(donor_path):
                raise ValueError("Material " + material.name + " needs an existing native donor stream")
            if os.path.splitext(donor_path)[1].lower() not in (".streamdata", ".stream"):
                raise ValueError("Material " + material.name + " donor must be a .streamdata or .stream file")
            for enabled, value, key in (
                (material.dt_material.override_nm_r_blend, material.dt_material.nm_r_blend, "nm_r_blend"),
                (material.dt_material.override_shared_blend, material.dt_material.shared_blend, "shared_blend"),
                (material.dt_material.override_bc_blend, material.dt_material.bc_blend, "bc_blend"),
            ):
                if enabled and not math.isfinite(value):
                    raise ValueError("Material " + material.name + " override " + key + " must be finite")
    if version2:
        _validate_v2(objects, include_visible)
    had_scene_extra = SCHEMA_KEY in scene
    original_scene_extra = _copy_idproperty(scene[SCHEMA_KEY]) if had_scene_extra else None
    changed_objects = []
    changed_materials = []
    backface_culling = []
    stand_ins = []
    hidden_actions = None
    changed_bones = []
    changed_bone_extras = []
    original_selection = [obj for obj in view_objects if obj.select_get()]
    original_active = context.view_layer.objects.active
    try:
        for obj in original_selection:
            obj.select_set(False)
        for obj in objects:
            obj.select_set(True)
        unselected = [obj.name for obj in objects if not obj.select_get()]
        if unselected:
            raise ValueError(
                "Asset collection members cannot be selected for glTF export: "
                + ", ".join(unselected)
                + ". Make them selectable in the active view layer."
            )
        if objects:
            context.view_layer.objects.active = objects[0]
        scene.pop(SCHEMA_KEY, None)
        if not version2 and _collider_objects(objects, include_visible):
            scene[SCHEMA_KEY] = _scene_extras(scene)
        colliders = set(_collider_objects(objects, include_visible))
        for obj in objects:
            had_original = SCHEMA_KEY in obj
            original = _copy_idproperty(obj[SCHEMA_KEY]) if had_original else None
            changed_objects.append((obj, original, had_original))
            obj.pop(SCHEMA_KEY, None)
            if version2 and obj in authored:
                obj[SCHEMA_KEY] = _v2_object_extras(obj, include_visible)
            elif obj in colliders:
                obj[SCHEMA_KEY] = _object_extras(obj, include_visible)
            elif obj in authored:  # visibility group only
                obj[SCHEMA_KEY] = {"version": 1, "id": obj.dt_collider.stable_id}
            group = obj.dt_collider.visibility_group.strip()
            if group and not version2:
                obj[SCHEMA_KEY]["visibility_group"] = group
            if obj.dt_collider.lod_group.strip() and not version2:
                obj[SCHEMA_KEY]["lod"] = _lod_extras(obj.dt_collider)
            if (not obj.dt_collider.render_visible or not obj.dt_collider.render_shadow) and not version2:
                obj[SCHEMA_KEY]["render"] = _render_extras(obj.dt_collider)
            if obj.type == "LIGHT" and not version2:
                obj[SCHEMA_KEY]["light"] = _light_extras(obj)
            if obj.type == "MESH" and obj.dt_collider.is_volume:
                obj[SCHEMA_KEY]["volume"] = {"name": obj.dt_collider.volume_name.strip() or obj.name}
            elif obj.type == "MESH" and obj.dt_collider.unit_actor != "none":
                obj[SCHEMA_KEY]["actor"] = _actor_extras(obj)
            if obj.type == "EMPTY" and obj.dt_collider.part_name.strip():
                if SCHEMA_KEY not in obj:
                    if not obj.dt_collider.stable_id.strip():
                        obj.dt_collider.stable_id = _new_id()
                    obj[SCHEMA_KEY] = {"version": 1, "id": obj.dt_collider.stable_id}
                obj[SCHEMA_KEY]["part"] = obj.dt_collider.part_name.strip()
                if obj.get("darktide_part_flow"):
                    obj[SCHEMA_KEY]["flow"] = obj["darktide_part_flow"]
            if obj.type == "EMPTY" and obj.dt_particles.enabled:
                extract = os.path.abspath(bpy.path.abspath(prefs().game_extract_folder.strip()))
                if not prefs().game_extract_folder.strip() or not os.path.isdir(extract):
                    raise ValueError("Particle effects need 'Game files extract' in the Setup panel set to your limn extract")
                obj[SCHEMA_KEY]["particles"] = particle_effect.node_extras(
                    obj, extract, donor_path_converter or (lambda value: value))
        data = _collection_data(scene.dt_asset.asset_collection)
        flow_tree = _asset_options(scene).flow_tree
        use_mover = _asset_options(scene).use_mover
        asset_collection = scene.dt_asset.asset_collection
        loose = _loose_texture_extras(asset_collection, _asset_stem(asset_collection.dt_asset_identity.asset_filename)) \
            if asset_collection else []
        if loose and not objects:
            raise ValueError("Loose textures need at least one object in the asset collection (an empty is enough)")
        if (flow_tree is not None or data or use_mover or loose) and objects:
            # the asset's own unit carries them, not one of its parts
            def in_part(obj):
                while obj is not None:
                    if obj.type == "EMPTY" and obj.dt_collider.part_name.strip():
                        return True
                    obj = obj.parent
                return False
            holder = next((obj for obj in objects if not in_part(obj)), objects[0])
            if SCHEMA_KEY not in holder:
                if not holder.dt_collider.stable_id.strip():
                    holder.dt_collider.stable_id = _new_id()
                holder[SCHEMA_KEY] = {"version": 1, "id": holder.dt_collider.stable_id}
            if flow_tree is not None:
                holder[SCHEMA_KEY]["flow"] = json.dumps(flow_editor.description(flow_tree))
            if data:
                holder[SCHEMA_KEY]["data"] = json.dumps(data)
            if use_mover:
                holder[SCHEMA_KEY]["mover"] = _mover_extras(_asset_options(scene))
            if loose:
                holder[SCHEMA_KEY]["textures"] = json.dumps(loose)
        for obj in objects:
            if obj.type != "ARMATURE":
                continue
            for bone in obj.data.bones:
                if not bone.dt_dangle.enabled and not bone.dt_aim.enabled:
                    continue
                had_original = SCHEMA_KEY in bone
                original = _copy_idproperty(bone[SCHEMA_KEY]) if had_original else None
                changed_bone_extras.append((bone, original, had_original))
                extras = {"version": 1, "id": "dangle:" + obj.name + ":" + bone.name}
                dangle = bone.dt_dangle
                if dangle.enabled and dangle.mode == "jiggle":
                    extras["dangle"] = {"mode": "jiggle", "mass": dangle.mass, "gravity": dangle.gravity,
                                        "stiffness": dangle.jiggle_stiffness, "damping": dangle.jiggle_damping,
                                        "max_stretch": dangle.max_stretch}
                elif dangle.enabled:
                    extras["dangle"] = {"mass": dangle.mass, "gravity": dangle.gravity, "damping": dangle.damping,
                                        "stiffness": dangle.stiffness, "max_angle": dangle.max_angle,
                                        "length": dangle.length if dangle.length > 0.0 else bone.length}
                if bone.dt_aim.enabled:
                    extras["aim"] = {"target": bone.dt_aim.target.strip(), "turn": _aim_turn(bone)}
                bone[SCHEMA_KEY] = extras
        for material in materials:
            had_original = MATERIAL_SCHEMA_KEY in material
            original = _copy_idproperty(material[MATERIAL_SCHEMA_KEY]) if had_original else None
            changed_materials.append((material, original, had_original))
            material.pop(MATERIAL_SCHEMA_KEY, None)
            # the glTF exporter writes doubleSided from Backface Culling, which Blender leaves off by default
            backface_culling.append((material, material.use_backface_culling))
            material.use_backface_culling = not material.dt_material.double_sided
            if material.dt_material.mode == "external":
                material[MATERIAL_SCHEMA_KEY] = {
                    "version": 1,
                    "mode": "external",
                    "resource": _validate_material_resource(material.dt_material.resource, material.name),
                }
            elif material.dt_material.mode == "game_shader":
                preset = _shader_preset(material.dt_material)
                material[MATERIAL_SCHEMA_KEY] = {
                    "version": 1,
                    "mode": "game_shader",
                    "stream": (donor_path_converter or (lambda value: value))(os.path.join(
                        os.path.abspath(bpy.path.abspath(prefs().game_folder)), "bundle", *preset["stream"].split("/"))),
                    "variables": _shader_assignments(material.dt_material)[0],
                    "textures": _shader_assignments(material.dt_material)[1],
                    # the materials holding the compiled shaders; the compiler ships copies of them
                    "shader_streams": preset.get("shader_streams", {}),
                }
            elif material.dt_material.mode == "shader":
                intent = {"version": 1, "mode": "shader", **own_shader_extras(material.dt_material, material.name)}
                if material.dt_material.surface != "default":
                    intent["surface"] = material.dt_material.surface
                material[MATERIAL_SCHEMA_KEY] = intent
            elif material.dt_material.mode == "donor":
                settings = material.dt_material
                overrides = {}
                for enabled, value, key in (
                    (settings.override_nm_r_blend, settings.nm_r_blend, "nm_r_blend"),
                    (settings.override_shared_blend, settings.shared_blend, "shared_blend"),
                    (settings.override_bc_blend, settings.bc_blend, "bc_blend"),
                ):
                    if enabled:
                        overrides[key] = value
                material[MATERIAL_SCHEMA_KEY] = {
                    "version": 1,
                    "mode": "donor",
                    "family": "world_surface_blend_v1",
                    "stream": (donor_path_converter(os.path.abspath(bpy.path.abspath(settings.donor_stream)))
                               if donor_path_converter else
                               os.path.abspath(bpy.path.abspath(settings.donor_stream))),
                    "variable_overrides": overrides,
                }
            elif _has_constant_emission_output(material):
                intent = {"version": 1, "mode": "emissive"}
                if material.dt_material.surface != "default":
                    intent["surface"] = material.dt_material.surface
                material[MATERIAL_SCHEMA_KEY] = intent
            else:
                undo = _mmd_stand_in(material)
                if undo:
                    stand_ins.append(undo)
                intent = {"version": 1, "mode": "generated"}
                if material.dt_material.surface != "default":
                    intent["surface"] = material.dt_material.surface
                if _asset_options(scene).weapon_materials:
                    intent["weapon"] = True
                material[MATERIAL_SCHEMA_KEY] = intent
        kwargs = {"filepath": os.fspath(path), "export_format": "GLB"}
        operator = bpy.ops.export_scene.gltf
        if _asset_options(scene).only_weighted_bones:
            # Temporarily mark unweighted bones non-deform; the exporter then drops them.
            for rig, keep in _weighted_bone_sets(objects).items():
                for bone in rig.data.bones:
                    if bone.use_deform and bone.name not in keep:
                        changed_bones.append(bone)
                        bone.use_deform = False
            _set_required(kwargs, operator, "export_def_bones", True)
        _set_required(kwargs, operator, "use_active_scene", True)
        _set_required(kwargs, operator, "use_selection", True)
        _set_required(kwargs, operator, "export_extras", True)
        _set_required(kwargs, operator, "export_apply", apply_modifiers)
        _set_if_supported(kwargs, operator, "export_lights", True)  # point/spot lamps become unit lights
        _set_if_supported(kwargs, operator, "export_tangents", True)
        _set_if_supported(kwargs, operator, "export_all_influences", True)
        _set_if_supported(kwargs, operator, "will_save_settings", False)
        hidden_actions = _hide_foreign_actions(objects)
        result = operator(**kwargs)
        if result != {"FINISHED"}:
            raise RuntimeError("glTF export did not finish: " + repr(result))
        return result
    finally:
        if not had_scene_extra:
            scene.pop(SCHEMA_KEY, None)
        else:
            scene[SCHEMA_KEY] = original_scene_extra
        for obj, original, had_original in changed_objects:
            if not had_original:
                obj.pop(SCHEMA_KEY, None)
            else:
                obj[SCHEMA_KEY] = original
        for bone in changed_bones:
            bone.use_deform = True
        for bone, original, had_original in changed_bone_extras:
            if not had_original:
                bone.pop(SCHEMA_KEY, None)
            else:
                bone[SCHEMA_KEY] = original
        for material, culling in backface_culling:
            material.use_backface_culling = culling
        for undo in stand_ins:
            undo()
        if hidden_actions:
            hidden_actions()
        for material, original, had_original in changed_materials:
            if not had_original:
                material.pop(MATERIAL_SCHEMA_KEY, None)
            else:
                material[MATERIAL_SCHEMA_KEY] = original
        for obj in view_objects:
            if obj.select_get():
                obj.select_set(False)
        for obj in original_selection:
            if obj.name in view_objects:
                obj.select_set(True)
        context.view_layer.objects.active = original_active


def _aim_turn(bone):
    """The bones an aiming bone turns, as {name: weight}; empty means its parent at full weight."""
    turn = {}
    for part in bone.dt_aim.turn.split(","):
        name, _, weight = part.strip().partition(":")
        if not name.strip():
            continue
        if name.strip() not in bone.id_data.bones:
            raise ValueError("Aim on bone '" + bone.name + "' turns '" + name.strip() + "', which is not a bone of this armature")
        try:
            value = float(weight) if weight.strip() else 1.0
        except ValueError:
            raise ValueError("Aim on bone '" + bone.name + "': '" + part.strip() + "' is not bone or bone:weight") from None
        if not 0.0 <= value <= 1.0:
            raise ValueError("Aim on bone '" + bone.name + "': weights go from 0 to 1")
        if name.strip() == bone.name:
            raise ValueError("Aim on bone '" + bone.name + "' can't turn itself, it's the end that points")
        turn[name.strip()] = value
    if not turn:
        if bone.parent is None:
            raise ValueError("Aim on bone '" + bone.name + "' needs bones to turn (it has no parent)")
        turn[bone.parent.name] = 1.0
    if not bone.dt_aim.target.strip():
        raise ValueError("Aim on bone '" + bone.name + "' needs a target name")
    return turn


def _loose_texture_extras(collection, stem):
    """The collection's loose textures for the compiler: each image as a file (saved as a PNG copy when it isn't a
    plain file on disk)."""
    result = []
    names = set()
    for item in collection.dt_asset_identity.loose_textures:
        name = item.name.strip()
        if not name or any(not (c.islower() or c.isdigit() or c == "_") for c in name) or name in names:
            raise ValueError("Loose texture names must be unique lowercase letters, digits and _: " + repr(name))
        names.add(name)
        image = item.image
        if image is None:
            raise ValueError("Loose texture " + name + " has no image")
        path = os.path.abspath(bpy.path.abspath(image.filepath)) if image.filepath else ""
        if not path or not os.path.isfile(path) or image.packed_file or image.is_dirty or image.source != "FILE":
            folder = os.path.join(tempfile.gettempdir(), "darktide_assets_textures", stem or "asset")
            os.makedirs(folder, exist_ok=True)
            path = os.path.join(folder, name + ".png")
            # the pixels into a plain image (a copy of a generated or packed image doesn't keep painted pixels)
            width, height = image.size
            if not width or not height:
                raise ValueError("Loose texture " + name + ": the image " + image.name + " has no pixels")
            pixels = [0.0] * (width * height * 4)
            image.pixels.foreach_get(pixels)
            saved = bpy.data.images.new("__darktide_loose_texture", width, height, alpha=True)
            try:
                saved.colorspace_settings.name = image.colorspace_settings.name
                saved.pixels.foreach_set(pixels)
                saved.filepath_raw = path
                saved.file_format = "PNG"
                saved.save()
            finally:
                bpy.data.images.remove(saved)
        record = {"name": name, "image": path, "kind": item.kind}
        if item.kind == "like":
            extract = os.path.abspath(bpy.path.abspath(prefs().game_extract_folder.strip()))
            if not prefs().game_extract_folder.strip() or not os.path.isdir(extract):
                raise ValueError("Loose texture " + name + ": 'Same as a game texture' needs 'Game files extract' in Setup")
            record["like"] = item.like.strip()
            record["extract"] = extract
        result.append(record)
    return result


def _collection_data(collection):
    """The asset collection's custom properties as the unit's script data (Unit.get_data): text, numbers,
    true/false, groups (nested tables) and lists."""
    if collection is None:
        return {}

    def plain(value):
        if hasattr(value, "to_dict"):
            return {key: plain(item) for key, item in value.to_dict().items()}
        if hasattr(value, "to_list"):
            return [plain(item) for item in value.to_list()]
        if isinstance(value, dict):
            return {key: plain(item) for key, item in value.items()}
        if isinstance(value, (list, tuple)):
            return [plain(item) for item in value]
        if isinstance(value, (bool, str)):
            return value
        if isinstance(value, (int, float)):
            return float(value)
        raise ValueError("Unit data can hold text, numbers, true/false, groups and lists, not " + type(value).__name__)

    registered = set(collection.bl_rna.properties.keys())
    return {key: plain(value) for key, value in collection.items()
            if key not in registered and not key.startswith("_") and key != SCHEMA_KEY}
