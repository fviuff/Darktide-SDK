"""Blender authoring and compilation workflow for Darktide assets."""

import bpy
from bpy.props import PointerProperty
from . import fit_skeleton
from . import retarget
from . import particle_effect
from . import particle_preview
from . import state_machine_preview
from . import flow_editor
from .settings import DarktideOwnShaderParameter, DarktideOwnShaderTexture, DarktideAimSettings, DarktideAnimationState, DarktideAnimationTransition, DarktideAnimationVariable, DarktideCollectionSettings, DarktideColliderSettings, DarktideDangleSettings, DarktideLightSettings, DarktideLooseTexture, DarktideMaterialSettings, DarktideSceneSettings, DarktideShaderTexture, DarktideShaderValue
from .operators import DARKTIDE_OT_new_shader_code, DARKTIDE_OT_own_shader_row, DARKTIDE_OT_add_animation_state, DARKTIDE_OT_add_animation_transition, DARKTIDE_OT_add_animation_variable, DARKTIDE_OT_add_loose_texture, DARKTIDE_OT_build, DARKTIDE_OT_clear_game_events, DARKTIDE_OT_create_asset_collection, DARKTIDE_OT_export_intermediate, DARKTIDE_OT_import_game_collision, DARKTIDE_OT_import_game_events, DARKTIDE_OT_import_game_unit, DARKTIDE_OT_import_reference_skeleton, DARKTIDE_OT_inspect, DARKTIDE_OT_load_shader_settings, DARKTIDE_OT_make_collision_proxy, DARKTIDE_OT_make_gibs, DARKTIDE_OT_new_flow, DARKTIDE_OT_remove_animation_state, DARKTIDE_OT_remove_animation_transition, DARKTIDE_OT_remove_animation_variable, DARKTIDE_OT_remove_gibs, DARKTIDE_OT_remove_loose_texture, DARKTIDE_OT_state_bones_from_selection, DARKTIDE_OT_update_asset_collection
from .panels import DARKTIDE_PT_bone_aim, DARKTIDE_PT_bone_dangle, DarktidePreferences, SIDEBAR_PANELS


bl_info = {
    "name": "Darktide Assets",
    "author": "fviuff",
    "version": (0, 7, 0),
    "blender": (4, 2, 0),
    "location": "View3D > Sidebar > Darktide",
    "category": "Import-Export",
}


CLASSES = (DarktidePreferences, *fit_skeleton.CLASSES, *particle_effect.CLASSES, DarktideAnimationState, DarktideAnimationVariable, DarktideAnimationTransition,
           DarktideSceneSettings, DarktideLooseTexture, DarktideCollectionSettings, DarktideShaderValue, DarktideShaderTexture, DarktideOwnShaderParameter, DarktideOwnShaderTexture, DarktideMaterialSettings, DarktideDangleSettings, DarktideAimSettings, DarktideColliderSettings, DarktideLightSettings,
           DARKTIDE_OT_add_loose_texture, DARKTIDE_OT_remove_loose_texture,
           DARKTIDE_OT_create_asset_collection, DARKTIDE_OT_update_asset_collection,
           DARKTIDE_OT_add_animation_state, DARKTIDE_OT_remove_animation_state, DARKTIDE_OT_state_bones_from_selection, DARKTIDE_OT_load_shader_settings,
           DARKTIDE_OT_add_animation_variable, DARKTIDE_OT_remove_animation_variable,
           DARKTIDE_OT_add_animation_transition, DARKTIDE_OT_remove_animation_transition,
           DARKTIDE_OT_make_collision_proxy, DARKTIDE_OT_export_intermediate, DARKTIDE_OT_new_flow,
           DARKTIDE_OT_inspect, DARKTIDE_OT_build, DARKTIDE_OT_import_reference_skeleton, DARKTIDE_OT_import_game_unit, DARKTIDE_OT_import_game_collision,
           DARKTIDE_OT_make_gibs, DARKTIDE_OT_remove_gibs, DARKTIDE_OT_new_shader_code, DARKTIDE_OT_own_shader_row,
           DARKTIDE_OT_import_game_events, DARKTIDE_OT_clear_game_events,
           *fit_skeleton.UI_CLASSES, *retarget.CLASSES, *SIDEBAR_PANELS, DARKTIDE_PT_bone_dangle, DARKTIDE_PT_bone_aim)


def register():
    flow_editor.register()
    particle_preview.register()
    state_machine_preview.register()
    for cls in CLASSES:
        bpy.utils.register_class(cls)
    bpy.types.Scene.dt_asset = PointerProperty(type=DarktideSceneSettings)
    bpy.types.Object.dt_collider = PointerProperty(type=DarktideColliderSettings)
    bpy.types.Collection.dt_asset_identity = PointerProperty(type=DarktideCollectionSettings)
    bpy.types.Material.dt_material = PointerProperty(type=DarktideMaterialSettings)
    bpy.types.Bone.dt_dangle = PointerProperty(type=DarktideDangleSettings)
    bpy.types.Bone.dt_aim = PointerProperty(type=DarktideAimSettings)
    bpy.types.Object.dt_particles = PointerProperty(type=particle_effect.DarktideParticleSettings)
    bpy.types.Light.dt_light = PointerProperty(type=DarktideLightSettings)


def unregister():
    del bpy.types.Bone.dt_dangle
    del bpy.types.Bone.dt_aim
    del bpy.types.Object.dt_particles
    del bpy.types.Light.dt_light
    del bpy.types.Object.dt_collider
    del bpy.types.Collection.dt_asset_identity
    del bpy.types.Material.dt_material
    del bpy.types.Scene.dt_asset
    for cls in reversed(CLASSES):
        bpy.utils.unregister_class(cls)
    state_machine_preview.unregister()
    particle_preview.unregister()
    flow_editor.unregister()


if __name__ == "__main__":
    register()
