"""The sidebar, bone and preference panels."""

import bpy
from bpy.props import StringProperty
from . import fit_skeleton
from . import retarget
from . import state_graph
from . import particle_effect
from . import state_machine_preview
from .settings import _asset_options, _default_game_folder, _shader_preset, prefs
from .export import _asset_stem, _collision_review, _v2_objects
from .operators import DARKTIDE_OT_new_shader_code, DARKTIDE_OT_own_shader_row, DARKTIDE_OT_add_animation_state, DARKTIDE_OT_add_animation_transition, DARKTIDE_OT_add_animation_variable, DARKTIDE_OT_build, DARKTIDE_OT_clear_game_events, DARKTIDE_OT_create_asset_collection, DARKTIDE_OT_export_intermediate, DARKTIDE_OT_import_game_collision, DARKTIDE_OT_import_game_events, DARKTIDE_OT_import_game_unit, DARKTIDE_OT_import_reference_skeleton, DARKTIDE_OT_inspect, DARKTIDE_OT_load_shader_settings, DARKTIDE_OT_make_collision_proxy, DARKTIDE_OT_make_gibs, DARKTIDE_OT_new_flow, DARKTIDE_OT_remove_animation_state, DARKTIDE_OT_remove_animation_transition, DARKTIDE_OT_remove_animation_variable, DARKTIDE_OT_remove_gibs, DARKTIDE_OT_state_bones_from_selection, DARKTIDE_OT_update_asset_collection


class DarktidePreferences(bpy.types.AddonPreferences):
    """Paths that belong to this computer, not to a .blend: set once for every project."""
    bl_idname = __package__

    compiler: StringProperty(name="Compiler", subtype="FILE_PATH", description="DarktideGLBCompiler.exe")
    game_folder: StringProperty(name="Darktide folder", subtype="DIR_PATH", default=_default_game_folder(),
                                description="The game's install folder (the one with binaries and bundle in it)")
    game_extract_folder: StringProperty(name="Game files extract", subtype="DIR_PATH",
                                        description="Folder of game files extracted with limn: skeletons, game effects, "
                                                    "the game unit panel and game textures read from it")

    def draw(self, context):
        draw_setup(self.layout, context)


def _draw_light(layout, obj):
    lamp = obj.data
    settings = lamp.dt_light
    box = layout.box()
    box.label(text="Darktide lamp", icon="LIGHT")
    if lamp.type == "SUN":
        box.label(text="Sun lamps are skipped")
        return
    if lamp.type == "AREA" and lamp.shape == "ELLIPSE":
        box.label(text="Ellipses become round disks (Size)")
    box.label(text="Color, power, temperature, range and shadows: the lamp's own settings")
    box.prop(lamp, "volume_factor", text="Fog")
    box.prop(settings, "particle_intensity")
    if lamp.type in ("POINT", "SPOT"):
        box.prop(settings, "falloff_start")
    if lamp.use_shadow:
        box.prop(settings, "shadow_resolution")
        box.prop(settings, "dynamic")
    box.prop(settings, "lens_flare")
    if lamp.type == "SPOT" or (lamp.type == "AREA" and lamp.shape in ("DISK", "ELLIPSE")):
        box.prop(settings, "reflector")
    box.prop(settings, "ies")
    if settings.ies == "custom":
        box.prop(settings, "ies_custom")


class DARKTIDE_PT_bone_aim(bpy.types.Panel):
    bl_label = "Darktide Aim"
    bl_idname = "DARKTIDE_PT_bone_aim"
    bl_space_type = "PROPERTIES"
    bl_region_type = "WINDOW"
    bl_context = "bone"

    @classmethod
    def poll(cls, context):
        return context.bone is not None

    def draw_header(self, context):
        self.layout.prop(context.bone.dt_aim, "enabled", text="")

    def draw(self, context):
        aim = context.bone.dt_aim
        column = self.layout.column()
        column.active = aim.enabled
        column.prop(aim, "target")
        column.prop(aim, "turn")
        column.label(text="Put this bone where the pointing ends (eye, muzzle)")


class DARKTIDE_PT_bone_dangle(bpy.types.Panel):
    bl_label = "Darktide Dangle"
    bl_idname = "DARKTIDE_PT_bone_dangle"
    bl_space_type = "PROPERTIES"
    bl_region_type = "WINDOW"
    bl_context = "bone"

    @classmethod
    def poll(cls, context):
        return context.bone is not None

    def draw_header(self, context):
        self.layout.prop(context.bone.dt_dangle, "enabled", text="")

    def draw(self, context):
        dangle = context.bone.dt_dangle
        column = self.layout.column()
        column.active = dangle.enabled
        column.prop(dangle, "mode", expand=True)
        names = (("mass", "gravity", "jiggle_stiffness", "jiggle_damping", "max_stretch") if dangle.mode == "jiggle"
                 else ("length", "mass", "gravity", "damping", "stiffness", "max_angle"))
        for name in names:
            column.prop(dangle, name)
        column.label(text="Skin the swinging mesh to this bone")


def draw_setup(layout, context):
    """The paths of this computer (add-on preferences) and the project's output folder."""
    settings = prefs()
    layout.prop(settings, "compiler")
    layout.prop(settings, "game_folder")
    layout.prop(settings, "game_extract_folder")
    if context.scene is not None:
        layout.prop(context.scene.dt_asset, "output_path")
        layout.prop(context.scene.dt_asset, "compile_timeout")


class _SidebarPanel:
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "Darktide"


class DARKTIDE_PT_setup(_SidebarPanel, bpy.types.Panel):
    bl_label = "Setup"
    bl_idname = "DARKTIDE_PT_setup"
    bl_order = 0

    def draw(self, context):
        settings = prefs()
        layout = self.layout
        draw_setup(layout, context)
        if not settings.compiler.strip():
            layout.label(text="Set the compiler to DarktideGLBCompiler.exe", icon="INFO")
        if not settings.game_folder.strip():
            layout.label(text="Set the Darktide folder (textures and game shaders need it)", icon="INFO")


class DARKTIDE_PT_asset_panel(_SidebarPanel, bpy.types.Panel):
    bl_label = "Asset"
    bl_idname = "DARKTIDE_PT_asset_panel"
    bl_order = 1

    def draw(self, context):
        layout = self.layout
        scene = context.scene
        settings = scene.dt_asset
        collection = settings.asset_collection
        options = _asset_options(scene)
        layout.operator(DARKTIDE_OT_create_asset_collection.bl_idname)
        if collection and collection.dt_asset_identity.owned_by_addon:
            layout.operator(DARKTIDE_OT_update_asset_collection.bl_idname)
        layout.prop(settings, "asset_collection")
        if collection:
            layout.prop(collection.dt_asset_identity, "asset_path")
        else:
            layout.prop(settings, "asset_path", text="Initial resource path")
            layout.operator(DARKTIDE_OT_create_asset_collection.bl_idname, text="New Empty Asset")
            layout.label(text="Create an asset collection before export or compile")
        layout.prop(options, "asset_filename")
        layout.prop(options, "scale")
        layout.prop(options, "weapon_materials")
        if collection:
            layout.prop(collection.dt_asset_identity, "enemy_outline")
            layout.prop(collection.dt_asset_identity, "material_layers")
        layout.prop(options, "output_kind")
        if options.output_kind != "model":
            row = layout.row(align=True)
            row.prop(options, "select_clip")
            if options.select_clip:
                row.prop(options, "clip_index")
        layout.operator(DARKTIDE_OT_inspect.bl_idname)
        if options.inspection_summary:
            inspection = layout.box()
            inspection.label(text="Compiler source view (last inspection)")
            for line in options.inspection_summary.splitlines():
                inspection.label(text=line)
        layout.operator(DARKTIDE_OT_export_intermediate.bl_idname)
        layout.operator(DARKTIDE_OT_build.bl_idname)


class DARKTIDE_PT_animation(_SidebarPanel, bpy.types.Panel):
    bl_label = "Animation"
    bl_idname = "DARKTIDE_PT_animation"
    bl_order = 2
    bl_options = {"DEFAULT_CLOSED"}

    def draw(self, context):
        layout = self.layout
        options = _asset_options(context.scene)
        if options.output_kind == "model":
            layout.label(text="Output kind Model leaves the animations out")
            return
        simple_row = layout.row(align=True)
        simple_row.enabled = options.output_kind == "all"
        simple_row.prop(options, "embed_simple_animation")
        if options.embed_simple_animation:
            simple_row.prop(options, "simple_animation_clip", text="Clip")
        travel_row = layout.row(align=True)
        travel_row.prop(options, "in_place")
        travel_row.prop(options, "root_motion")
        loop_row = layout.row()
        loop_row.enabled = options.output_kind == "all"
        loop_row.prop(options, "auto_loop_single_clip")
        loop_row = layout.row()
        loop_row.enabled = options.output_kind == "all"
        loop_row.prop(options, "create_looping_state_machine")
        graph_row = layout.row()
        graph_row.enabled = options.output_kind == "all"
        graph_row.prop(options, "create_state_graph")
        if options.output_kind != "all":
            layout.label(text="State machines need Output kind All", icon="ERROR")
        elif options.create_looping_state_machine and options.create_state_graph:
            layout.label(text="Choose one animation controller", icon="ERROR")
        elif options.create_looping_state_machine:
            row = layout.row(align=True)
            row.prop(options, "loop_clip_index", text="Clip")
            row.prop(options, "state_machine_playback", text="")
        elif options.auto_loop_single_clip and not options.create_state_graph:
            layout.label(text="A looping controller is added only when the exported GLB has one clip")
        elif options.create_state_graph:
            _draw_state_graph(layout, options)
            state_machine_preview.draw(layout, options)
        layout.prop(options, "animation_fit_advanced", text="Animation fit settings")
        if options.animation_fit_advanced:
            fit = layout.box()
            fit.prop(options, "animation_translation_tolerance")
            fit.prop(options, "animation_scale_tolerance")
            fit.prop(options, "animation_rotation_tolerance_radians")


def _draw_state_graph(layout, options):
    layout.label(text="State 0 is the initial state (higher layers start in their first state)")
    states = options.state_graph_states
    for index, state in enumerate(states):
        state_box = layout.box()
        row = state_box.row(align=True)
        row.label(text="State " + str(index))
        row.prop(state, "state_name", text="")
        remove = row.operator(DARKTIDE_OT_remove_animation_state.bl_idname, text="", icon="X")
        remove.index = index
        row = state_box.row(align=True)
        row.prop(state, "layer")
        row.prop(state, "empty")
        if not state.empty:
            row.prop(state, "additive")
            row = state_box.row(align=True)
            row.prop(state, "bones")
            pick = row.operator(DARKTIDE_OT_state_bones_from_selection.bl_idname, text="", icon="BONE_DATA")
            pick.index = index
            row = state_box.row(align=True)
            row.prop(state, "blend")
            row.prop(state, "random")
        if state.empty:
            pass
        elif state.random:
            row = state_box.row(align=True)
            row.prop(state, "random_pick", text="")
            row.prop(state, "playback", text="")
            state_box.prop(state, "blend_clips")
        elif state.blend:
            row.prop(state, "blend_2d")
            row = state_box.row(align=True)
            row.prop(state, "blend_variable_pick", text="X" if state.blend_2d else "")
            if state.blend_2d:
                row.prop(state, "blend_variable2_pick", text="Y")
            row.prop(state, "playback", text="")
            state_box.prop(state, "blend_clips")
        else:
            row.prop(state, "clip_pick", text="")
            row.prop(state, "playback", text="")
        if not state.empty:
            row = state_box.row(align=True)
            row.prop(state, "speed_from_variable", text="Speed from variable")
            row.prop(state, "speed_variable_pick" if state.speed_from_variable else "speed", text="")
            state_box.prop(state, "events_at")
            row = state_box.row(align=True)
            row.prop(state, "exit_event")
            if state.exit_event.strip():
                row.prop(state, "exit_blend")
    layout.operator(DARKTIDE_OT_add_animation_state.bl_idname, icon="ADD")
    layout.label(text="Float variables", icon="DRIVER")
    for index, variable in enumerate(options.state_graph_variables):
        variable_box = layout.box()
        row = variable_box.row(align=True)
        row.label(text="Variable " + str(index))
        row.prop(variable, "variable_name", text="")
        remove = row.operator(DARKTIDE_OT_remove_animation_variable.bl_idname, text="", icon="X")
        remove.index = index
        variable_box.prop(variable, "initial_value")
        row = variable_box.row(align=True)
        row.prop(variable, "minimum")
        row.prop(variable, "maximum")
    layout.operator(DARKTIDE_OT_add_animation_variable.bl_idname, icon="ADD")
    layout.label(text="Transitions", icon="DRIVER_TRANSFORM")
    for index, transition in enumerate(options.state_graph_transitions):
        transition_box = layout.box()
        row = transition_box.row(align=True)
        if transition.from_any:
            row.label(text="From any state")
        else:
            row.prop(transition, "from_pick")
        row.prop(transition, "from_any", text="", icon="WORLD")
        transition_box.prop(transition, "to_pick")
        transition_box.prop(transition, "event_name")
        transition_box.prop(transition, "condition")
        if transition.condition == "range":
            transition_box.prop(transition, "variable_pick")
            row = transition_box.row(align=True)
            row.prop(transition, "lower")
            row.prop(transition, "upper")
            row = transition_box.row(align=True)
            row.prop(transition, "lower_exclusive")
            row.prop(transition, "upper_exclusive")
        row = transition_box.row(align=True)
        row.prop(transition, "blend_seconds")
        remove = row.operator(DARKTIDE_OT_remove_animation_transition.bl_idname, text="", icon="X")
        remove.index = index
    layout.operator(DARKTIDE_OT_add_animation_transition.bl_idname, icon="ADD")
    layout.label(text="Event names take * (stagger_* is every stagger event).")
    game = layout.box()
    game.label(text="Game events", icon="IMPORT")
    game.prop(bpy.context.scene.dt_asset, "game_unit", text="")
    game.operator(DARKTIDE_OT_import_game_events.bl_idname)
    events = state_graph.game_events(options)
    if events:
        row = game.row(align=True)
        row.label(text=str(len(events)) + " from " + options.state_graph_game_events_source.rsplit("/", 1)[-1])
        row.operator(DARKTIDE_OT_clear_game_events.bl_idname, text="", icon="X")


class DARKTIDE_PT_flow(_SidebarPanel, bpy.types.Panel):
    bl_label = "Flow"
    bl_idname = "DARKTIDE_PT_flow"
    bl_order = 3
    bl_options = {"DEFAULT_CLOSED"}

    def draw(self, context):
        options = _asset_options(context.scene)
        row = self.layout.row(align=True)
        row.prop(options, "flow_tree")
        row.operator(DARKTIDE_OT_new_flow.bl_idname, text="", icon="ADD")
        self.layout.label(text="Edit it in a node editor set to Darktide Flow")


class DARKTIDE_PT_skeleton(_SidebarPanel, bpy.types.Panel):
    bl_label = "Skeleton"
    bl_idname = "DARKTIDE_PT_skeleton"
    bl_order = 4
    bl_options = {"DEFAULT_CLOSED"}

    def draw(self, context):
        layout = self.layout
        collection = context.scene.dt_asset.asset_collection
        options = _asset_options(context.scene)
        if not prefs().game_extract_folder.strip():
            layout.label(text="Set 'Game files extract' in Setup first", icon="INFO")
        if collection is None:
            layout.operator(fit_skeleton.DARKTIDE_OT_new_starter_asset.bl_idname, icon="MESH_CAPSULE")
            layout.label(text="Or make an asset first for the reference skeleton and fit tools")
            return
        identity = collection.dt_asset_identity
        layout.prop(identity, "skeleton_preset")
        if identity.skeleton_preset_status:
            layout.label(text=identity.skeleton_preset_status[:90], icon="ERROR")
        layout.prop(identity, "reference_unit")
        layout.prop(identity, "reference_bones")
        layout.prop(identity, "skeleton_resource")
        layout.operator(DARKTIDE_OT_import_reference_skeleton.bl_idname)
        layout.prop(options, "only_weighted_bones")
        layout.label(text="Imported armature is added to this asset collection")
        fit_skeleton.draw_fit_panel(layout, context, collection)
        retarget.draw(layout, collection)


class DARKTIDE_PT_game_unit(_SidebarPanel, bpy.types.Panel):
    bl_label = "Game Unit"
    bl_idname = "DARKTIDE_PT_game_unit"
    bl_order = 5
    bl_options = {"DEFAULT_CLOSED"}

    @classmethod
    def poll(cls, context):
        return context.scene.dt_asset.asset_collection is not None

    def draw(self, context):
        layout = self.layout
        if not prefs().game_extract_folder.strip():
            layout.label(text="Set 'Game files extract' in Setup first", icon="INFO")
        layout.prop(context.scene.dt_asset, "game_unit", text="")
        layout.operator(DARKTIDE_OT_import_game_unit.bl_idname)
        layout.operator(DARKTIDE_OT_import_game_collision.bl_idname)
        if context.scene.dt_asset.asset_collection is not None:
            gib_box = layout.box()
            gib_box.prop(_asset_options(context.scene), "gib_breed")
            row = gib_box.row(align=True)
            row.operator(DARKTIDE_OT_make_gibs.bl_idname)
            row.operator(DARKTIDE_OT_remove_gibs.bl_idname, text="", icon="X")
        options = _asset_options(context.scene)
        layout.prop(options, "unit_package")
        box = layout.box()
        box.prop(options, "make_breed")
        if options.make_breed:
            box.prop(options, "breed_name")
            box.prop(options, "breed_base")
            box.prop(options, "breed_body")
            box.prop(options, "breed_health")
            row = box.row(align=True)
            row.prop(options, "breed_walk_speed")
            row.prop(options, "breed_run_speed")


class DARKTIDE_PT_object(_SidebarPanel, bpy.types.Panel):
    bl_label = "Active Object"
    bl_idname = "DARKTIDE_PT_object"
    bl_order = 6

    @classmethod
    def poll(cls, context):
        return context.object is not None

    def draw(self, context):
        layout = self.layout
        obj = context.object
        layout.label(text=obj.name, icon="OBJECT_DATA")
        layout.prop(obj.dt_collider, "visibility_group")
        row = layout.row(align=True)
        row.prop(obj.dt_collider, "render_visible")
        row.prop(obj.dt_collider, "render_shadow")
        layout.prop(obj.dt_collider, "lod_group")
        if obj.dt_collider.lod_group.strip():
            row = layout.row(align=True)
            row.prop(obj.dt_collider, "lod_level")
            row.prop(obj.dt_collider, "lod_down_to")
        if obj.type == "EMPTY":
            layout.prop(obj.dt_collider, "part_name")
            particle_effect.draw(layout, obj)
        if obj.type == "LIGHT":
            _draw_light(layout, obj)


class DARKTIDE_PT_physics(_SidebarPanel, bpy.types.Panel):
    bl_label = "Collision and Physics"
    bl_idname = "DARKTIDE_PT_physics"
    bl_order = 7
    bl_options = {"DEFAULT_CLOSED"}

    def draw(self, context):
        layout = self.layout
        scene = context.scene
        collection = scene.dt_asset.asset_collection
        options = _asset_options(scene)
        asset_objects = list(collection.all_objects) if collection else []
        version2 = _v2_objects(asset_objects) if asset_objects else False
        if not version2:
            body = layout.box()
            body.label(text="Whole asset")
            body.prop(options, "visible_meshes_collide", text="Use visible meshes as collision")
            if options.visible_meshes_collide or any(o.dt_collider.role != "render" for o in asset_objects):
                body.prop(options, "actor")
                if options.actor == "dynamic":
                    body.prop(options, "mass")
                body.prop(options, "material")
            body.label(text="Static uses the authored mesh surface")
        ragdoll_row = layout.row()
        ragdoll_row.enabled = options.output_kind == "all"
        ragdoll_row.prop(options, "ragdoll_event")
        mover = layout.box()
        mover.prop(options, "use_mover")
        if options.use_mover:
            row = mover.row(align=True)
            row.prop(options, "mover_height")
            row.prop(options, "mover_radius")
            mover.prop(options, "mover_slope")
            mover.prop(options, "mover_filter")
        notes = _collision_review(scene, asset_objects) if asset_objects else []
        if notes:
            review = layout.box()
            review.label(text="Collision review")
            for message in notes[:3]:
                review.label(text=message)
        obj = context.object
        if obj is None:
            return
        collider = layout.box()
        collider.label(text="Active object: " + obj.name)
        if obj.type == "MESH":
            collider.prop(obj.dt_collider, "is_volume")
            if obj.dt_collider.is_volume:
                collider.prop(obj.dt_collider, "volume_name")
                return
            collider.prop(obj.dt_collider, "unit_actor")
            if obj.dt_collider.unit_actor != "none":
                collider.prop(obj.dt_collider, "actor_shape")
                collider.prop(obj.dt_collider, "actor_name")
                if obj.dt_collider.unit_actor == "custom":
                    collider.prop(obj.dt_collider, "actor_template")
                    collider.prop(obj.dt_collider, "actor_shape_template")
                if options.make_breed:
                    collider.prop(obj.dt_collider, "hit_zone_group")
                collider.prop(obj.dt_collider, "actor_spawn")
                return
        collider.prop(obj.dt_collider, "is_body")
        if obj.dt_collider.is_body:
            collider.prop(obj.dt_collider, "actor")
            if obj.dt_collider.actor == "dynamic":
                collider.prop(obj.dt_collider, "mass")
            collider.prop(obj.dt_collider, "material")
        if obj.type == "MESH":
            collider.prop(obj.dt_collider, "role")
            if options.visible_meshes_collide and obj.dt_collider.role == "render":
                collider.prop(obj.dt_collider, "use_asset_collision")
            if obj.dt_collider.role != "render":
                collider.prop(obj.dt_collider, "shape")
                if version2:
                    collider.prop(obj.dt_collider, "body")
            if collection and collection.all_objects.get(obj.name) == obj:
                collider.operator(DARKTIDE_OT_make_collision_proxy.bl_idname)
        joint = obj.dt_collider
        if version2 or obj.type == "EMPTY":
            box = layout.box()
            box.label(text="Joint")
            box.prop(joint, "joint_kind")
            if joint.joint_kind != "none":
                box.prop(joint, "body_a")
                box.prop(joint, "body_b")
                if joint.joint_kind in {"hinge", "ragdoll"}:
                    box.prop(joint, "hinge_limits")
                    if joint.hinge_limits:
                        box.prop(joint, "twist_min")
                        box.prop(joint, "twist_max")
                if joint.joint_kind == "ragdoll":
                    box.prop(joint, "swing_y")
                    box.prop(joint, "swing_z")
                if joint.joint_kind == "slider":
                    box.prop(joint, "slide_limits")
                    if joint.slide_limits:
                        box.prop(joint, "slide_min")
                        box.prop(joint, "slide_max")
                if joint.joint_kind != "fixed":
                    row = box.row(align=True)
                    row.prop(joint, "spring_stiffness")
                    row.prop(joint, "spring_damping")
                row = box.row(align=True)
                row.prop(joint, "break_force")
                row.prop(joint, "break_torque")


class DARKTIDE_PT_material(_SidebarPanel, bpy.types.Panel):
    bl_label = "Active Material"
    bl_idname = "DARKTIDE_PT_material"
    bl_order = 8

    @classmethod
    def poll(cls, context):
        return context.object is not None and context.object.active_material is not None

    def draw(self, context):
        layout = self.layout
        material = context.object.active_material
        settings = material.dt_material
        layout.label(text=material.name, icon="MATERIAL")
        layout.prop(settings, "mode")
        if settings.mode not in ("external", "template"):
            layout.prop(settings, "double_sided")
        if settings.mode == "external":
            layout.prop(settings, "resource")
        elif settings.mode == "game_shader":
            if not prefs().game_folder.strip():
                layout.label(text="Set the Darktide folder in Setup first", icon="INFO")
            row = layout.row()
            row.active = not settings.game_material.strip()
            row.prop(settings, "game_shader")
            layout.prop(settings, "game_material")
            preset = _shader_preset(settings)
            if settings.game_material.strip() and not preset:
                layout.label(text="Not in the game files extract (set it in Setup)", icon="ERROR")
            if preset and (len(settings.shader_value_rows) != len(preset["variables"]) or
                           len(settings.shader_texture_rows) != len(preset["textures"])):
                layout.operator(DARKTIDE_OT_load_shader_settings.bl_idname, icon="FILE_REFRESH")
            elif preset:
                box = layout.box()
                box.label(text="Settings (tick to change, unticked keeps the game's)")
                for row in settings.shader_value_rows:
                    line = box.row(align=True)
                    line.prop(row, "changed", text="")
                    line.label(text=row.name)
                    values = line.row(align=True)
                    values.active = row.changed
                    if row.is_color:
                        values.prop(row, "color", text="")
                    else:
                        for index in range(row.floats):
                            values.prop(row, "value", index=index, text="")
                if settings.shader_texture_rows:
                    box = layout.box()
                    box.label(text="Textures (images from this material's own nodes)")
                    for row in settings.shader_texture_rows:
                        line = box.row(align=True)
                        line.label(text=row.name)
                        line.prop(row, "source", text="")
                if settings.shader_values or settings.shader_textures:
                    box = layout.box()
                    box.label(text="Other settings, typed by name")
                    box.prop(settings, "shader_values")
                    box.prop(settings, "shader_textures")
        elif settings.mode == "shader":
            row = layout.row(align=True)
            row.prop(settings, "shader_code")
            row.operator(DARKTIDE_OT_new_shader_code.bl_idname, text="", icon="FILE_NEW")
            box = layout.box()
            box.label(text="Parameters (read by name in the code)")
            for index, row in enumerate(settings.own_parameters):
                line = box.row(align=True)
                line.prop(row, "name", text="")
                line.prop(row, "floats", text="")
                if row.is_color:
                    line.prop(row, "color", text="")
                else:
                    values = line.row(align=True)
                    for k in range(row.floats):
                        values.prop(row, "value", index=k, text="")
                line.prop(row, "is_color", text="", icon="COLOR")
                remove = line.operator(DARKTIDE_OT_own_shader_row.bl_idname, text="", icon="X")
                remove.kind, remove.index = "parameter", index
            add = box.operator(DARKTIDE_OT_own_shader_row.bl_idname, text="Add Parameter", icon="ADD")
            add.kind, add.index = "parameter", -1
            box = layout.box()
            box.label(text="Textures (sample_<name>(uv) in the code)")
            for index, row in enumerate(settings.own_textures):
                line = box.row(align=True)
                line.prop(row, "name", text="")
                line.prop(row, "source", text="")
                line.prop(row, "runtime", text="", icon="SCRIPT")
                remove = line.operator(DARKTIDE_OT_own_shader_row.bl_idname, text="", icon="X")
                remove.kind, remove.index = "texture", index
            add = box.operator(DARKTIDE_OT_own_shader_row.bl_idname, text="Add Texture", icon="ADD")
            add.kind, add.index = "texture", -1
            row = layout.row(align=True)
            row.prop(settings, "own_clip")
            cutoff = row.row(align=True)
            cutoff.active = settings.own_clip
            cutoff.prop(settings, "own_cutoff")
            layout.prop(settings, "own_decal")
            layout.prop(settings, "surface")
        elif settings.mode == "donor":
            layout.prop(settings, "donor_stream")
            for toggle, value in (("override_nm_r_blend", "nm_r_blend"),
                                  ("override_shared_blend", "shared_blend"),
                                  ("override_bc_blend", "bc_blend")):
                row = layout.row(align=True)
                row.prop(settings, toggle, text="")
                row.prop(settings, value)
        else:
            layout.prop(settings, "surface")


class DARKTIDE_PT_loose_textures(_SidebarPanel, bpy.types.Panel):
    bl_label = "Loose Textures"
    bl_idname = "DARKTIDE_PT_loose_textures"
    bl_order = 9
    bl_options = {"DEFAULT_CLOSED"}

    def draw(self, context):
        layout = self.layout
        collection = context.scene.dt_asset.asset_collection
        if collection is None:
            layout.label(text="Make an asset collection first")
            return
        identity = collection.dt_asset_identity
        folder = identity.asset_path.strip().rsplit("/", 1)[0] if "/" in identity.asset_path else ""
        stem = _asset_stem(identity.asset_filename)
        for index, item in enumerate(identity.loose_textures):
            box = layout.box()
            row = box.row(align=True)
            row.prop(item, "name")
            row.operator("darktide.remove_loose_texture", text="", icon="X").index = index
            box.template_ID(item, "image", open="image.open")
            box.prop(item, "kind")
            if item.kind == "like":
                box.prop(item, "like")
            if folder and stem:
                box.label(text=folder + "/" + stem + "_" + item.name.strip())
        layout.operator("darktide.add_loose_texture", icon="ADD")


SIDEBAR_PANELS = (DARKTIDE_PT_setup, DARKTIDE_PT_asset_panel, DARKTIDE_PT_animation, DARKTIDE_PT_flow,
                  DARKTIDE_PT_skeleton, DARKTIDE_PT_game_unit, DARKTIDE_PT_object, DARKTIDE_PT_physics,
                  DARKTIDE_PT_material, DARKTIDE_PT_loose_textures)
