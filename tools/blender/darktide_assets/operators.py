"""The add-on's operators (buttons)."""

import os
import re
import json
import subprocess
import struct
import bpy
import bmesh
from bpy.props import IntProperty, StringProperty
from .reference_skeleton import ReferenceSkeletonError, _murmur64
from . import game_unit
from . import game_state_machine
from . import state_graph
from . import breed
from . import flow_editor
from . import gibs
from .settings import OWN_SHADER_TEMPLATE, _COLLECTION_OPTIONS, _asset_options, _copy_state_graph, _shader_rows, prefs
from .export import _asset_stem, _collision_review, _compiler_path, _glb_animations, _inspection_summary, _material_layers, _nearest_body, _reference_armature, _run_compiler, _suggest_stem, _validate_state_graph, _windows_path, _wine_tools, export_asset


class DARKTIDE_OT_inspect(bpy.types.Operator):
    bl_idname = "darktide.inspect_asset"
    bl_label = "Inspect Asset"
    bl_description = "Export the collection and list exactly which meshes, skins, and numbered clips the compiler sees"

    def execute(self, context):
        settings = context.scene.dt_asset
        options = _asset_options(context.scene)
        try:
            compiler = _compiler_path()
            wine_tools = _wine_tools(compiler)
            stem = _asset_stem(options.asset_filename)
            output_dir = bpy.path.abspath(settings.output_path or "//")
            os.makedirs(output_dir, exist_ok=True)
            glb_path = os.path.join(output_dir, stem + ".glb")
            export_asset(context, glb_path,
                         donor_path_converter=(lambda path: _windows_path(path, wine_tools)) if wine_tools else None)
            collision_notes = _collision_review(context.scene, list(settings.asset_collection.all_objects))
            command_glb = _windows_path(glb_path, wine_tools)
            result = _run_compiler(compiler, [command_glb, "--inspect"], wine_tools,
                                    capture_output=True, text=True,
                                    timeout=settings.compile_timeout or None, check=False)
            inspection_path = os.path.join(output_dir, stem + ".inspect.txt")
            with open(inspection_path, "w", encoding="utf-8") as report:
                report.write(result.stdout or "")
                report.write(result.stderr or "")
                if collision_notes:
                    report.write("\nCollision review:\n" + "\n".join(collision_notes) + "\n")
            if result.returncode:
                options.inspection_summary = ""
                self.report({"ERROR"}, "Inspection failed; see " + inspection_path)
                return {"CANCELLED"}
            options.inspection_summary = _inspection_summary(result.stdout)
            if collision_notes:
                options.inspection_summary += "\nCollision review: " + collision_notes[0]
            self.report({"INFO"}, "Source inspected; see " + inspection_path)
            return {"FINISHED"}
        except (OSError, ValueError, subprocess.TimeoutExpired) as exc:
            options.inspection_summary = ""
            self.report({"ERROR"}, "Inspection failed: " + str(exc)[:180])
            return {"CANCELLED"}


class DARKTIDE_OT_build(bpy.types.Operator):
    bl_idname = "darktide.build_asset"
    bl_label = "Compile Asset"

    def execute(self, context):
        settings = context.scene.dt_asset
        collection = settings.asset_collection
        if collection is None:
            self.report({"ERROR"}, "Create an asset collection from the selection first")
            return {"CANCELLED"}
        asset_path = collection.dt_asset_identity.asset_path.strip()
        options = _asset_options(context.scene)
        if options.create_looping_state_machine and options.create_state_graph:
            self.report({"ERROR"}, "Choose either the single-clip state machine or event-driven state graph")
            return {"CANCELLED"}
        if (options.create_looping_state_machine or options.create_state_graph) and options.output_kind != "all":
            self.report({"ERROR"}, "Create state machine requires Output kind All")
            return {"CANCELLED"}
        if options.create_state_graph:
            if options.select_clip:
                self.report({"ERROR"}, "Disable Select clip when using an event-driven state graph")
                return {"CANCELLED"}
            try:
                _validate_state_graph(options)
            except ValueError as exc:
                self.report({"ERROR"}, str(exc))
                return {"CANCELLED"}
        if bool(collection.dt_asset_identity.reference_bones.strip()) != bool(
                collection.dt_asset_identity.skeleton_resource.strip()):
            self.report({"ERROR"}, "Reference BONES and skeleton resource must both be set")
            return {"CANCELLED"}
        reference_bones = collection.dt_asset_identity.reference_bones.strip()
        if reference_bones and not os.path.isfile(bpy.path.abspath(reference_bones)):
            self.report({"ERROR"}, "Reference BONES file not found: " + bpy.path.abspath(reference_bones))
            return {"CANCELLED"}
        breed_lua = None
        if options.make_breed:
            try:
                if not asset_path:
                    raise ValueError("A breed needs the asset's resource path (its body)")
                breed_lua = breed.lua(options, asset_path, [obj for obj in collection.all_objects
                                                            if obj.type == "MESH" and obj.dt_collider.unit_actor != "none" and not obj.dt_collider.is_volume])
            except ValueError as exc:
                self.report({"ERROR"}, str(exc))
                return {"CANCELLED"}
        try:
            compiler = _compiler_path()
            wine_tools = _wine_tools(compiler)
            stem = _asset_stem(options.asset_filename)
            output_dir = bpy.path.abspath(settings.output_path or "//")
            os.makedirs(output_dir, exist_ok=True)
        except (OSError, ValueError) as exc:
            self.report({"ERROR"}, str(exc))
            return {"CANCELLED"}
        glb_path = os.path.join(output_dir, stem + ".glb")
        try:
            export_asset(context, glb_path,
                         donor_path_converter=(lambda path: _windows_path(path, wine_tools)) if wine_tools else None)
        except Exception as exc:
            self.report({"ERROR"}, "GLB export failed: " + str(exc))
            return {"CANCELLED"}
        try:
            glb_animations, glb_skins = _glb_animations(glb_path)
        except (OSError, ValueError, json.JSONDecodeError) as exc:
            self.report({"ERROR"}, "Could not read exported GLB animations: " + str(exc)[:150])
            return {"CANCELLED"}
        collision_notes = _collision_review(context.scene, list(collection.all_objects))
        compiled_dir = os.path.join(output_dir, stem)
        log_path = os.path.join(compiled_dir, "build.log")
        try:
            os.makedirs(compiled_dir, exist_ok=True)
            command = [_windows_path(glb_path, wine_tools), "-o",
                       _windows_path(compiled_dir, wine_tools),
                       "--scale", str(options.scale), "--output-kind", options.output_kind,
                       "--animation-translation-tolerance", str(options.animation_translation_tolerance),
                       "--animation-scale-tolerance", str(options.animation_scale_tolerance),
                       "--animation-rotation-tolerance-radians", str(options.animation_rotation_tolerance_radians)]
            if options.select_clip and options.output_kind != "model":
                command.extend(["--clip", str(options.clip_index)])
            # a state machine drives a skeleton; animated objects without one keep the clip as a simple animation
            use_single_clip_machine = options.create_looping_state_machine or (
                options.auto_loop_single_clip and not options.create_state_graph
                and options.output_kind == "all" and len(glb_animations) == 1 and glb_skins > 0
            )
            if use_single_clip_machine:
                clip_index = options.loop_clip_index if options.create_looping_state_machine else 0
                playback = options.state_machine_playback if options.create_looping_state_machine else "loop"
                command.extend(["--once-clip" if playback == "once" else "--loop-clip",
                                str(clip_index)])
            uses_state_machine = use_single_clip_machine or options.create_state_graph
            if not options.embed_simple_animation:
                command.append("--no-simple-animation")
            elif not uses_state_machine and options.output_kind == "all" and options.simple_animation_clip:
                command.extend(["--simple-clip", str(options.simple_animation_clip)])
            if options.ragdoll_event.strip() and options.output_kind == "all":
                command.extend(["--ragdoll-event", options.ragdoll_event.strip()])
            if (options.unit_package or options.make_breed) and asset_path:
                command.extend(["--package-name", asset_path])
            if options.root_motion and options.output_kind != "model":
                command.append("--root-motion")
            elif options.in_place and options.output_kind != "model":
                command.append("--in-place")
            if options.create_state_graph:
                for state in options.state_graph_states:
                    clips = state_graph.clips_text(options, state.blend_clips)
                    if state.empty:
                        command.extend(["--sm-empty", state.state_name.strip(), str(state.layer)])
                    elif state.random:
                        command.extend(["--sm-random", state.state_name.strip(), state.random_pick, state.playback, clips])
                    elif state.blend:
                        variables = str(state.blend_variable) + ("," + str(state.blend_variable2) if state.blend_2d else "")
                        command.extend(["--sm-blend", state.state_name.strip(), variables, state.playback, clips])
                    else:
                        command.extend(["--sm-state", state.state_name.strip(), str(state.clip_index), state.playback])
                    if state.speed_from_variable:
                        command.extend(["--sm-speed", state.state_name.strip(), "var:" + str(state.speed_variable)])
                    elif state.speed != 1.0:
                        command.extend(["--sm-speed", state.state_name.strip(), repr(float(state.speed))])
                    if state.layer and not state.empty:
                        command.extend(["--sm-layer", state.state_name.strip(), str(state.layer)])
                    bones = ",".join(part.strip().replace(" ", "") for part in state.bones.split(",") if part.strip())
                    if bones:
                        command.extend(["--sm-mask", state.state_name.strip(), bones])
                    if state.additive and not state.empty:
                        command.extend(["--sm-additive", state.state_name.strip()])
                    timed = ",".join(part.strip().replace(" ", "") for part in state.events_at.split(",") if part.strip())
                    if timed and not state.empty:
                        command.extend(["--sm-events", state.state_name.strip(), timed])
                    if state.exit_event.strip() and not state.empty:
                        command.extend(["--sm-exit", state.state_name.strip(), state.exit_event.strip(),
                                        repr(float(state.exit_blend))])
                for variable in options.state_graph_variables:
                    command.extend(["--sm-variable", variable.variable_name.strip(),
                                    str(variable.initial_value), str(variable.minimum), str(variable.maximum)])
                game_events = state_graph.game_events(options)
                if game_events:
                    command.extend(["--sm-declare-events", ",".join(game_events)])
                for transition in state_graph.expand(options):
                    if transition.condition == "range":
                        command.extend(["--sm-range-transition", str(transition.from_state),
                                        str(transition.to_state), transition.event_name.strip(),
                                        str(transition.blend_seconds), str(transition.variable_index),
                                        str(transition.lower), str(transition.upper),
                                        "exclusive" if transition.lower_exclusive else "inclusive",
                                        "exclusive" if transition.upper_exclusive else "inclusive"])
                    else:
                        command.extend(["--sm-transition", str(transition.from_state), str(transition.to_state),
                                        transition.event_name.strip(), str(transition.blend_seconds)])
            if asset_path:
                command.extend(["--asset-path", asset_path])
            if reference_bones:
                command.extend(["--reference-bones",
                                _windows_path(bpy.path.abspath(reference_bones), wine_tools),
                                "--skeleton-resource",
                                collection.dt_asset_identity.skeleton_resource.strip()])
            for layer, material in _material_layers(collection.dt_asset_identity):
                command.extend(["--material-layer", layer, material])
            if sum(len(part) + 3 for part in command) > 8000:
                # more than a Windows command line holds (big state machines): the arguments go in a file
                arguments_path = os.path.splitext(glb_path)[0] + ".arguments.txt"
                with open(arguments_path, "w", encoding="utf-8", newline="\n") as handle:
                    handle.write("\n".join(command) + "\n")
                command = ["@" + _windows_path(arguments_path, wine_tools)]
            result = _run_compiler(compiler, command, wine_tools,
                                   capture_output=True, text=True,
                                    timeout=settings.compile_timeout or None, check=False)
            with open(log_path, "w", encoding="utf-8") as log:
                if collision_notes:
                    log.write("Collision review:\n" + "\n".join(collision_notes) + "\n\n")
                log.write(result.stdout or "")
                log.write(result.stderr or "")
            if result.returncode:
                lines = (result.stderr + "\n" + result.stdout).splitlines()
                first_error = next((line.strip()[2:].strip() for line in lines
                                    if line.lstrip().startswith("- ")), None)
                if not first_error:
                    first_error = next((line.strip() for line in lines if line.strip()),
                                       "Compiler exited with code " + str(result.returncode))
                self.report({"ERROR"}, first_error[:180] + "; see " + log_path)
                return {"CANCELLED"}
            manifest_path = os.path.join(compiled_dir, "compile_manifest.json")
            if os.path.isfile(manifest_path):
                with open(manifest_path, "r", encoding="utf-8") as manifest_file:
                    gaps = json.load(manifest_file).get("compiler_gaps", [])
                if gaps:
                    self.report({"WARNING"}, str(gaps[0])[:180] + "; see " + log_path)
            breed_note = ""
            if breed_lua is not None:
                breed_path = os.path.join(output_dir, options.breed_name.strip() + "_breed.lua")
                with open(breed_path, "w", encoding="utf-8", newline="\n") as handle:
                    handle.write(breed_lua)
                breed_note = "; breed in " + breed_path
            memory_note = ""
            # the compiler's line "memory: <x> MB loaded with the package..., <y> MB of texture detail streamed in..."
            memory = re.search(r"memory: ([0-9.]+) MB loaded.*?([0-9.]+) MB of texture detail", result.stdout or "")
            if memory:
                memory_note = "; loads %s MB, %s MB of texture detail streams in when on screen" % (memory.group(1), memory.group(2))
            if collision_notes:
                self.report({"WARNING"}, collision_notes[0][:180] + "; see " + log_path + breed_note)
            else:
                self.report({"INFO"}, "Asset built" + memory_note + "; see " + log_path + breed_note)
            return {"FINISHED"}
        except (OSError, ValueError, subprocess.TimeoutExpired) as exc:
            with open(log_path, "w", encoding="utf-8") as log:
                if isinstance(exc, subprocess.TimeoutExpired):
                    for captured in (exc.stdout, exc.stderr):
                        if captured:
                            log.write(captured.decode("utf-8", errors="replace")
                                      if isinstance(captured, bytes) else captured)
                log.write("\n" + str(exc) + "\n")
            self.report({"ERROR"}, "Compiler failed: " + str(exc)[:150] + "; see " + log_path)
            return {"CANCELLED"}


class DARKTIDE_OT_import_game_unit(bpy.types.Operator):
    bl_idname = "darktide.import_game_unit"
    bl_label = "Import Game Unit Nodes"
    bl_description = ("Add the attach points, effect points and animated nodes of a game unit as named empties, "
                      "placed where the game has them")
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        settings = context.scene.dt_asset
        folder = bpy.path.abspath(prefs().game_extract_folder.strip())
        resource = settings.game_unit.strip().lower()
        if not folder or not os.path.isdir(folder):
            self.report({"ERROR"}, "Set 'Game files extract' in the Setup panel to your limn extract first")
            return {"CANCELLED"}
        if not resource:
            self.report({"ERROR"}, "Type the game unit's resource path")
            return {"CANCELLED"}
        collection = settings.asset_collection
        if collection is None:
            collection = _new_asset_collection(context, set())
        try:
            count, unknown = game_unit.import_nodes(context, collection, folder, resource)
        except (OSError, ReferenceSkeletonError, ValueError) as exc:
            self.report({"ERROR"}, str(exc)[:200])
            return {"CANCELLED"}
        note = (", " + str(unknown) + " only known by hash (#...), keep those names as they are") if unknown else ""
        self.report({"INFO"}, "Imported " + str(count) + " nodes into " + collection.name + note)
        return {"FINISHED"}


class DARKTIDE_OT_import_game_collision(bpy.types.Operator):
    bl_idname = "darktide.import_game_collision"
    bl_label = "Import Game Collision (EXPERIMENTAL)"
    bl_description = ("Copy a game unit's hit zones, ragdoll and mover: shape meshes on your rig's bones (by bone "
                      "name), ragdoll joints with the game's limits, and the mover into this asset's collision settings")
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        settings = context.scene.dt_asset
        folder = bpy.path.abspath(prefs().game_extract_folder.strip())
        resource = settings.game_unit.strip().lower()
        if not folder or not os.path.isdir(folder):
            self.report({"ERROR"}, "Set 'Game files extract' in the Setup panel to your limn extract first")
            return {"CANCELLED"}
        if not resource:
            self.report({"ERROR"}, "Type the game unit's resource path")
            return {"CANCELLED"}
        collection = settings.asset_collection
        if collection is None:
            collection = _new_asset_collection(context, set())
        try:
            ragdoll = None
            unit_path = os.path.join(folder, "%016x.unit" % _murmur64(resource))
            if os.path.isfile(unit_path):
                # the ragdoll lives in the unit's PhysX collection, which the compiler reads
                compiler = _compiler_path()
                wine_tools = _wine_tools(compiler)
                result = _run_compiler(compiler, ["--physics-collection", _windows_path(unit_path, wine_tools)], wine_tools,
                                       capture_output=True, text=True, check=False)
                if result.returncode == 0:
                    ragdoll = json.loads(result.stdout)

                def decode(meshes):
                    # cooked convex / triangle shapes, through the compiler's PhysX
                    path = os.path.join(bpy.app.tempdir or os.path.dirname(unit_path), "darktide_cooked_shapes.bin")
                    with open(path, "wb") as handle:
                        for kind, cooked in meshes:
                            handle.write(struct.pack("<II", kind, len(cooked)) + cooked)
                    decoded = _run_compiler(compiler, ["--physics-meshes", _windows_path(path, wine_tools)], wine_tools,
                                            capture_output=True, text=True, check=False)
                    if decoded.returncode:
                        raise ValueError("The compiler could not read the game's collision meshes: " +
                                         (decoded.stderr or "").strip()[:150])
                    return json.loads(decoded.stdout)
            else:
                decode = None
            count, skipped, mover, bodies, nodes = game_unit.import_collision(context, collection, folder, resource,
                                                                       _asset_options(context.scene), ragdoll, decode)
        except (OSError, ReferenceSkeletonError, ValueError, json.JSONDecodeError) as exc:
            self.report({"ERROR"}, str(exc)[:200])
            return {"CANCELLED"}
        text = "Imported " + str(count) + " hit zone and actor shapes"
        if bodies:
            text += ", a ragdoll of " + str(bodies) + " bodies (ragdoll event: " + _asset_options(context.scene).ragdoll_event + ")"
        if mover:
            text += " and the mover"
        if nodes:
            text += ", " + str(nodes) + " of its nodes your rig didn't have"
        if skipped:
            text += ", left out " + str(skipped) + " mesh-shaped ones"
        self.report({"INFO"}, text)
        return {"FINISHED"}


def _gib_rig(collection):
    return next((obj for obj in collection.all_objects if obj.type == "ARMATURE" and "j_hips" in obj.data.bones), None)


class DARKTIDE_OT_make_gibs(bpy.types.Operator):
    bl_idname = "darktide.make_gibs"
    bl_label = "Make Gibs"
    bl_description = ("Cut your body into the pieces the game knocks off this breed (by your weights), with stumps "
                      "and the game's gore on the cuts. They compile as their own units next to the asset")
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        collection = context.scene.dt_asset.asset_collection
        if collection is None:
            self.report({"ERROR"}, "Create an asset collection first")
            return {"CANCELLED"}
        rig = _gib_rig(collection)
        if rig is None:
            self.report({"ERROR"}, "The asset needs a darktide armature (rig it to an enemy skeleton first)")
            return {"CANCELLED"}
        options = _asset_options(context.scene)
        try:
            made = gibs.make_gibs(collection, rig, options.gib_breed,
                                  _asset_stem(collection.dt_asset_identity.asset_filename))
        except ValueError as exc:
            self.report({"ERROR"}, str(exc))
            return {"CANCELLED"}
        self.report({"INFO"}, "%d gibs: %s" % (len(made), ", ".join(gibs.short_name(key.split(",")) for key in made)))
        return {"FINISHED"}


class DARKTIDE_OT_remove_gibs(bpy.types.Operator):
    bl_idname = "darktide.remove_gibs"
    bl_label = "Remove Gibs"
    bl_description = "Delete the gib pieces and stumps Make Gibs made"
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        collection = context.scene.dt_asset.asset_collection
        if collection is not None:
            gibs.remove_gibs(collection)
            collection.pop("darktide_gibs", None)
        return {"FINISHED"}


class DARKTIDE_OT_import_game_events(bpy.types.Operator):
    bl_idname = "darktide.import_game_events"
    bl_label = "Import Game Events (EXPERIMENTAL)"
    bl_description = ("List every event and variable of a game unit's state machine (type the unit, e.g. an enemy's "
                      "base unit, or the state machine itself), so your state machine takes everything the game's "
                      "scripts for that character send")
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        folder = bpy.path.abspath(prefs().game_extract_folder.strip())
        resource = context.scene.dt_asset.game_unit.strip().lower()
        if not folder or not os.path.isdir(folder):
            self.report({"ERROR"}, "Set 'Game files extract' in the Setup panel to your limn extract first")
            return {"CANCELLED"}
        if not resource:
            self.report({"ERROR"}, "Type the game unit's resource path")
            return {"CANCELLED"}
        try:
            machine, events, variables = game_state_machine.load(folder, resource)
        except (OSError, ReferenceSkeletonError, ValueError) as exc:
            self.report({"ERROR"}, str(exc)[:200])
            return {"CANCELLED"}
        options = _asset_options(context.scene)
        options.create_state_graph = True
        options.state_graph_game_events = ",".join(events)
        options.state_graph_game_events_source = machine
        have = {variable.variable_name.strip() for variable in options.state_graph_variables}
        added = 0
        for name, default, low, high in variables:
            if name in have:
                continue
            variable = options.state_graph_variables.add()
            variable.variable_name, variable.initial_value, variable.minimum, variable.maximum = name, default, low, high
            added += 1
        unnamed = sum(1 for event in events if event.startswith("#"))
        self.report({"INFO"}, str(len(events)) + " events (" + str(unnamed) + " only known by hash) and " +
                    str(added) + " new variables from " + machine)
        return {"FINISHED"}


class DARKTIDE_OT_clear_game_events(bpy.types.Operator):
    bl_idname = "darktide.clear_game_events"
    bl_label = "Clear Game Events"
    bl_description = "Stop listing the imported game events"
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        options = _asset_options(context.scene)
        options.state_graph_game_events = ""
        options.state_graph_game_events_source = ""
        return {"FINISHED"}


class DARKTIDE_OT_import_reference_skeleton(bpy.types.Operator):
    bl_idname = "darktide.import_reference_skeleton"
    bl_label = "Import Rest-Pose Reference Skeleton"
    bl_description = "Create an armature from the selected native UNIT scene graph and BONES table"

    def execute(self, context):
        collection = context.scene.dt_asset.asset_collection
        if collection is None:
            collection = _new_asset_collection(context, set())
        identity = collection.dt_asset_identity
        unit_path = bpy.path.abspath(identity.reference_unit.strip())
        bones_path = bpy.path.abspath(identity.reference_bones.strip())
        if not unit_path or not bones_path or not identity.skeleton_resource.strip():
            self.report({"ERROR"}, "Choose a UNIT, matching BONES, and skeleton resource identity")
            return {"CANCELLED"}
        if not os.path.isfile(unit_path) or not os.path.isfile(bones_path):
            self.report({"ERROR"}, "Reference UNIT or BONES file was not found")
            return {"CANCELLED"}
        try:
            obj = _reference_armature(context, collection, unit_path, bones_path,
                                      identity.skeleton_resource.strip())
        except (OSError, ReferenceSkeletonError, RuntimeError, ValueError) as exc:
            self.report({"ERROR"}, "Reference skeleton import failed: " + str(exc)[:180])
            return {"CANCELLED"}
        self.report({"INFO"}, "Imported " + str(len(obj.data.bones)) + " BONES joints into " + collection.name)
        return {"FINISHED"}


def _selection_closure(context):
    selected = set(context.selected_objects)
    if not selected:
        raise ValueError("Select at least one object to define the asset")
    objects = set()
    pending = list(selected)
    while pending:
        obj = pending.pop()
        if obj in objects:
            continue
        objects.add(obj)
        pending.extend(obj.children)

    # Parent, armature and physics links are dependencies, but traversing their children
    # would pull unrelated siblings into an asset selected by one child mesh.
    pending = list(objects)
    while pending:
        obj = pending.pop()
        settings = obj.dt_collider
        dependencies = [obj.parent, settings.body, settings.body_a, settings.body_b]
        dependencies += [modifier.object for modifier in obj.modifiers if modifier.type == "ARMATURE"]
        for target in dependencies:
            if target is not None and target not in objects:
                objects.add(target)
                pending.append(target)
    return objects


def _replace_members(context, collection, objects):
    for child in tuple(collection.children):
        collection.children.unlink(child)
    for obj in tuple(collection.objects):
        collection.objects.unlink(obj)
    for obj in context.scene.objects:
        if obj in objects:
            collection.objects.link(obj)


def _convex_hull_mesh(mesh, name):
    """Make the simulated collision volume visible as editable Blender faces."""
    bm = bmesh.new()
    try:
        vertices = [bm.verts.new(vertex.co) for vertex in mesh.vertices]
        if len(vertices) < 4:
            raise ValueError("dynamic collision needs a volume with at least four vertices")
        result = bmesh.ops.convex_hull(bm, input=vertices, use_existing_faces=False)
        faces = [part for part in result["geom"] if isinstance(part, bmesh.types.BMFace)]
        if len(faces) < 4:
            raise ValueError("dynamic collision needs a non-planar convex volume")
        points = []
        indices = {}
        polygons = []
        for face in faces:
            polygon = []
            for vertex in face.verts:
                if vertex not in indices:
                    indices[vertex] = len(points)
                    points.append(tuple(vertex.co))
                polygon.append(indices[vertex])
            polygons.append(polygon)
        hull = bpy.data.meshes.new(name)
        hull.from_pydata(points, [], polygons)
        hull.update()
        return hull
    finally:
        bm.free()


class DARKTIDE_OT_create_asset_collection(bpy.types.Operator):
    bl_idname = "darktide.create_asset_collection"
    bl_label = "New Asset from Selection"
    bl_description = "Create a new asset collection from selected objects and their assembly"
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        try:
            objects = _selection_closure(context) if context.selected_objects else set()
        except ValueError as exc:
            self.report({"ERROR"}, str(exc))
            return {"CANCELLED"}
        collection = _new_asset_collection(context, objects)
        self.report({"INFO"}, "Created " + collection.name + " with " + str(len(objects)) + " objects")
        return {"FINISHED"}


def _new_asset_collection(context, objects):
    settings = context.scene.dt_asset
    first_asset = settings.asset_collection is None
    name = context.active_object.name if context.active_object else "Darktide Asset"
    collection = bpy.data.collections.new(name + " Asset")
    context.scene.collection.children.link(collection)
    options = collection.dt_asset_identity
    options.owned_by_addon = True
    options.asset_path = settings.asset_path if first_asset else ""
    for field in _COLLECTION_OPTIONS:
        setattr(options, field, getattr(settings, field))
    if first_asset:
        _copy_state_graph(settings, options)
    else:
        options.asset_filename = _suggest_stem(name)
        options.visible_meshes_collide = False
        options.actor = "static"
        options.output_kind = "all"
        options.select_clip = False
        options.create_looping_state_machine = False
        options.create_state_graph = False
    options.settings_version = 2
    _replace_members(context, collection, objects)
    settings.asset_collection = collection
    return collection


class DARKTIDE_OT_update_asset_collection(bpy.types.Operator):
    bl_idname = "darktide.update_asset_collection"
    bl_label = "Replace Asset Members from Selection"
    bl_description = "Replace only this addon's current asset collection membership"
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        collection = context.scene.dt_asset.asset_collection
        if collection is None or not collection.dt_asset_identity.owned_by_addon:
            self.report({"ERROR"}, "Select an asset collection created by this addon")
            return {"CANCELLED"}
        try:
            objects = _selection_closure(context)
        except ValueError as exc:
            self.report({"ERROR"}, str(exc))
            return {"CANCELLED"}
        _replace_members(context, collection, objects)
        self.report({"INFO"}, "Updated " + collection.name + " with " + str(len(objects)) + " objects")
        return {"FINISHED"}


class DARKTIDE_OT_add_animation_state(bpy.types.Operator):
    bl_idname = "darktide.add_animation_state"
    bl_label = "Add State"

    def execute(self, context):
        states = _asset_options(context.scene).state_graph_states
        state = states.add()
        state.state_name = "State " + str(len(states) - 1)
        return {"FINISHED"}


class DARKTIDE_OT_state_bones_from_selection(bpy.types.Operator):
    bl_idname = "darktide.state_bones_from_selection"
    bl_label = "From Selected Bones"
    bl_description = ("Fill Bones with the bones selected on the asset's rig (pose mode). A bone brings everything "
                      "below it, so only the topmost selected ones are listed; weights you typed stay")
    index: IntProperty()

    def execute(self, context):
        options = _asset_options(context.scene)
        collection = context.scene.dt_asset.asset_collection
        rigs = [obj for obj in (collection.all_objects if collection else []) if obj.type == "ARMATURE"]
        selected = [pose_bone.bone for rig in rigs for pose_bone in rig.pose.bones if pose_bone.select]
        if not selected:
            self.report({"ERROR"}, "Select the bones on the rig first (pose mode, click or box select)")
            return {"CANCELLED"}
        picked = set(selected)
        top = [bone for bone in selected if not any(parent in picked for parent in bone.parent_recursive)]
        state = options.state_graph_states[self.index]
        weights = {}
        for entry in state.bones.split(","):
            name, _, weight = entry.strip().partition(":")
            if name and weight:
                weights[name] = weight
        state.bones = ", ".join(bone.name + (":" + weights[bone.name] if bone.name in weights else "") for bone in top)
        return {"FINISHED"}


class DARKTIDE_OT_load_shader_settings(bpy.types.Operator):
    bl_idname = "darktide.load_shader_settings"
    bl_label = "Show Shader Settings"
    bl_description = "List this game shader's settings and texture slots to edit (values typed in older files carry over)"

    def execute(self, context):
        _shader_rows(context.object.active_material.dt_material)
        return {"FINISHED"}


class DARKTIDE_OT_remove_animation_state(bpy.types.Operator):
    bl_idname = "darktide.remove_animation_state"
    bl_label = "Remove State"
    index: IntProperty()

    def execute(self, context):
        options = _asset_options(context.scene)
        states = options.state_graph_states
        if 0 <= self.index < len(states):
            transitions = options.state_graph_transitions
            for transition_index in reversed(range(len(transitions))):
                transition = transitions[transition_index]
                if transition.from_state == self.index or transition.to_state == self.index:
                    transitions.remove(transition_index)
                    continue
                if transition.from_state > self.index:
                    transition.from_state -= 1
                if transition.to_state > self.index:
                    transition.to_state -= 1
            states.remove(self.index)
        return {"FINISHED"}


class DARKTIDE_OT_add_animation_variable(bpy.types.Operator):
    bl_idname = "darktide.add_animation_variable"
    bl_label = "Add Variable"

    def execute(self, context):
        variables = _asset_options(context.scene).state_graph_variables
        variable = variables.add()
        variable.variable_name = "Variable " + str(len(variables) - 1)
        return {"FINISHED"}


class DARKTIDE_OT_remove_animation_variable(bpy.types.Operator):
    bl_idname = "darktide.remove_animation_variable"
    bl_label = "Remove Variable"
    index: IntProperty()

    def execute(self, context):
        options = _asset_options(context.scene)
        variables = options.state_graph_variables
        if 0 <= self.index < len(variables):
            transitions = options.state_graph_transitions
            for transition_index in reversed(range(len(transitions))):
                transition = transitions[transition_index]
                if transition.condition != "range":
                    continue
                if transition.variable_index == self.index:
                    transitions.remove(transition_index)
                elif transition.variable_index > self.index:
                    transition.variable_index -= 1
            variables.remove(self.index)
        return {"FINISHED"}


class DARKTIDE_OT_add_animation_transition(bpy.types.Operator):
    bl_idname = "darktide.add_animation_transition"
    bl_label = "Add Transition"

    def execute(self, context):
        transitions = _asset_options(context.scene).state_graph_transitions
        transition = transitions.add()
        transition.from_state = 0
        transition.to_state = 1 if len(_asset_options(context.scene).state_graph_states) > 1 else 0
        return {"FINISHED"}


class DARKTIDE_OT_remove_animation_transition(bpy.types.Operator):
    bl_idname = "darktide.remove_animation_transition"
    bl_label = "Remove Transition"
    index: IntProperty()

    def execute(self, context):
        transitions = _asset_options(context.scene).state_graph_transitions
        if 0 <= self.index < len(transitions):
            transitions.remove(self.index)
        return {"FINISHED"}


class DARKTIDE_OT_make_collision_proxy(bpy.types.Operator):
    bl_idname = "darktide.make_collision_proxy"
    bl_label = "Create Editable Collision Mesh"
    bl_description = "Create collision-only meshes from evaluated Blender geometry; simulated bodies get a visible convex hull"
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        collection = context.scene.dt_asset.asset_collection
        if collection is None:
            self.report({"ERROR"}, "Choose an asset collection first")
            return {"CANCELLED"}
        sources = [obj for obj in context.selected_objects
                   if obj.type == "MESH" and collection.all_objects.get(obj.name) == obj and
                   obj.dt_collider.role != "collision"]
        if not sources:
            self.report({"ERROR"}, "Select visible mesh objects in the active asset")
            return {"CANCELLED"}
        depsgraph = context.evaluated_depsgraph_get()
        prepared = []
        pending_mesh = None
        try:
            for source in sources:
                parent_body = source if source.dt_collider.is_body else (source.dt_collider.body or _nearest_body(source))
                dynamic = (parent_body.dt_collider.actor if parent_body else _asset_options(context.scene).actor) == "dynamic"
                evaluated = source.evaluated_get(depsgraph)
                mesh = bpy.data.meshes.new_from_object(
                    evaluated, preserve_all_data_layers=False, depsgraph=depsgraph)
                pending_mesh = mesh
                if mesh is None or not mesh.polygons:
                    raise ValueError(source.name + " has no evaluated mesh faces for collision")
                if dynamic:
                    hull = _convex_hull_mesh(mesh, source.name + "_collision")
                    bpy.data.meshes.remove(mesh)
                    mesh = hull
                    pending_mesh = mesh
                mesh.materials.clear()
                prepared.append((source, mesh, parent_body, dynamic))
                pending_mesh = None
        except (RuntimeError, ValueError) as exc:
            if pending_mesh is not None and pending_mesh.users == 0:
                bpy.data.meshes.remove(pending_mesh)
            for _, mesh, _, _ in prepared:
                bpy.data.meshes.remove(mesh)
            self.report({"ERROR"}, "Collision mesh creation failed: " + str(exc)[:160])
            return {"CANCELLED"}
        created = []
        for source, mesh, parent_body, dynamic in prepared:
            proxy = source.copy()
            proxy.data = mesh
            proxy.name = source.name + "_collision"
            proxy.animation_data_clear()
            for modifier in tuple(proxy.modifiers):
                proxy.modifiers.remove(modifier)
            for constraint in tuple(proxy.constraints):
                proxy.constraints.remove(constraint)
            collection.objects.link(proxy)
            world = source.evaluated_get(depsgraph).matrix_world.copy()
            proxy.parent = source if source.dt_collider.is_body else source.parent
            proxy.matrix_world = world
            proxy.dt_collider.stable_id = ""
            proxy.dt_collider.is_body = False
            proxy.dt_collider.joint_kind = "none"
            proxy.dt_collider.role = "collision"
            proxy.dt_collider.body = parent_body
            proxy.dt_collider.shape = "convex" if dynamic else "geometry"
            proxy.display_type = "WIRE"
            proxy.show_in_front = True
            proxy.hide_render = True
            source.dt_collider.role = "render"
            source.dt_collider.use_asset_collision = False
            created.append(proxy)
        for obj in context.selected_objects:
            obj.select_set(False)
        for proxy in created:
            proxy.select_set(True)
        context.view_layer.objects.active = created[0]
        self.report({"INFO"}, "Created " + str(len(created)) + " editable collision meshes")
        return {"FINISHED"}


class DARKTIDE_OT_export_intermediate(bpy.types.Operator):
    bl_idname = "darktide.export_intermediate"
    bl_label = "Export Intermediate GLB"

    def execute(self, context):
        settings = context.scene.dt_asset
        if not settings.asset_collection:
            self.report({"ERROR"}, "Create an asset collection from the selection first")
            return {"CANCELLED"}
        try:
            stem = _asset_stem(_asset_options(context.scene).asset_filename)
            output_dir = bpy.path.abspath(settings.output_path or "//")
            os.makedirs(output_dir, exist_ok=True)
            export_asset(context, os.path.join(output_dir, stem + ".glb"))
        except Exception as exc:
            self.report({"ERROR"}, "GLB export failed: " + str(exc))
            return {"CANCELLED"}
        self.report({"INFO"}, "Intermediate GLB exported")
        return {"FINISHED"}


class DARKTIDE_OT_add_loose_texture(bpy.types.Operator):
    bl_idname = "darktide.add_loose_texture"
    bl_label = "Add Loose Texture"
    bl_description = "Ship an extra texture with the asset for your scripts to put into material slots"

    def execute(self, context):
        collection = context.scene.dt_asset.asset_collection
        if collection is None:
            self.report({"ERROR"}, "Make an asset collection first")
            return {"CANCELLED"}
        items = collection.dt_asset_identity.loose_textures
        item = items.add()
        taken = {other.name for other in items}
        index = len(items)
        while "texture_%d" % index in taken:
            index += 1
        item.name = "texture_%d" % index
        return {"FINISHED"}


class DARKTIDE_OT_remove_loose_texture(bpy.types.Operator):
    bl_idname = "darktide.remove_loose_texture"
    bl_label = "Remove Loose Texture"
    index: IntProperty()

    def execute(self, context):
        collection = context.scene.dt_asset.asset_collection
        if collection is not None and 0 <= self.index < len(collection.dt_asset_identity.loose_textures):
            collection.dt_asset_identity.loose_textures.remove(self.index)
        return {"FINISHED"}


class DARKTIDE_OT_new_flow(bpy.types.Operator):
    """Make a new flow for this asset; edit it in the node editor (Darktide Flow)"""
    bl_idname = "darktide.new_flow"
    bl_label = "New Flow"
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        options = _asset_options(context.scene)
        tree = bpy.data.node_groups.new("Unit flow", flow_editor.TREE)
        spawned = tree.nodes.new("DarktideFlowNode_unit_spawned")
        spawned.location = (-300, 0)
        options.flow_tree = tree
        self.report({"INFO"}, "Open a node editor and switch it to Darktide Flow to edit " + tree.name)
        return {"FINISHED"}


class DARKTIDE_OT_new_shader_code(bpy.types.Operator):
    """Make a text with a starting surface function for this material's own shader (edit it in a Text Editor)"""
    bl_idname = "darktide.new_shader_code"
    bl_label = "New Shader Code"
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        material = context.object.active_material
        settings = material.dt_material
        text = bpy.data.texts.new(material.name + " shader")
        text.from_string(OWN_SHADER_TEMPLATE)
        settings.shader_code = text
        if not settings.own_parameters:
            row = settings.own_parameters.add()
            row.name, row.floats, row.is_color = "tint", 3, True
        if not settings.own_textures:
            row = settings.own_textures.add()
            row.name, row.source = "color", "base_color"
        self.report({"INFO"}, "Open a Text Editor and pick " + text.name + " to edit the shader")
        return {"FINISHED"}


class DARKTIDE_OT_own_shader_row(bpy.types.Operator):
    """Add or remove a parameter or texture of this material's own shader"""
    bl_idname = "darktide.own_shader_row"
    bl_label = "Shader Row"
    bl_options = {"REGISTER", "UNDO"}
    kind: StringProperty(options={"HIDDEN"})
    index: IntProperty(default=-1, options={"HIDDEN"})

    def execute(self, context):
        settings = context.object.active_material.dt_material
        rows = settings.own_parameters if self.kind == "parameter" else settings.own_textures
        if self.index >= 0:
            rows.remove(self.index)
        else:
            row = rows.add()
            row.name = ("value" if self.kind == "parameter" else "texture") + str(len(rows))
        return {"FINISHED"}
