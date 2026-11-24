"""Gibs for an enemy body: the pieces that fly off and the stumps left behind, cut from the body by its weights.

The game gibs a minion (scripts/managers/minion/minion_gibbing.lua) by shrinking one of its bones to 0.01, linking
a stump unit to another bone and throwing a gib unit spawned at a third; which bones is per breed (gib_cuts.json,
read from the game's gibbing templates). A body linked onto the minion by node name shrinks with it. For each cut
this makes two parts of the asset (units of their own in the same build):

- the piece: everything weighted to the shrunk bones, rigid, its root at the spawn bone with a dynamic body the
  game pushes, the cut closed with the game's gore flesh material, and the retail gib's flow (a blood splat where
  it lands, the dismember impact sound)
- the stump: the same cut closed from the body's side, at the stump bone

The game's own spray effects, sounds, wounds, burning and dissolving run on them like on its own gibs.
"""
import json
import math
import os

import bmesh
import bpy
from mathutils import Vector

TAG = "darktide_gib"
# the gore flesh every retail cap and gib cut uses (poxwalker, traitor guard and cultist caps alike)
GORE_MATERIAL = "#ID[f9c911c8a0c204b2]"
IMPACT_SOUND = "wwise/events/weapon/play_combat_dismember_head_generic_impact"
_DATA = None


def data():
    global _DATA
    if _DATA is None:
        with open(os.path.join(os.path.dirname(__file__), "gib_cuts.json"), encoding="utf-8") as handle:
            _DATA = json.load(handle)
    return _DATA


def breed_items(_self=None, _context=None):
    return [(name, name, "Gibs like the game's " + name) for name in sorted(data()["breeds"])]


def cuts_for(breed):
    template = data()["breeds"].get(breed)
    if template is None:
        raise ValueError("No gibbing for breed '%s'" % breed)
    return data()["cuts"][template]


def blood_type(breed):
    # BloodSettings.blood_ball.blood_type_decal has default and poxwalker
    return "poxwalker" if "poxwalker" in data()["breeds"].get(breed, "") else "default"


def short_name(scale):
    return "_".join(name[2:] if name.startswith("j_") else name for name in scale)


def cut_names(stem, cut):
    """Unit (part) names and node names of one cut. Runtime Lua uses the same names."""
    short = short_name(cut["scale"])
    piece, stump = stem + "_gib_" + short, stem + "_stump_" + short
    return {"piece": piece, "stump": stump, "actor": "rp_" + piece, "sound": "g_" + piece, "stump_root": "rp_" + stump}


def _subtree(rig, names):
    bones = rig.data.bones
    found, pending = set(), [bones[name] for name in names if name in bones]
    while pending:
        bone = pending.pop()
        if bone.name not in found:
            found.add(bone.name)
            pending.extend(bone.children)
    return found


def _bone_world(rig, name):
    return rig.matrix_world @ rig.data.bones[name].matrix_local


def _skinned_meshes(objects, rig):
    return [obj for obj in objects if obj.type == "MESH" and not obj.get(TAG) and any(
        modifier.type == "ARMATURE" and modifier.object == rig for modifier in obj.modifiers)]


def _gore_material():
    material = bpy.data.materials.get("darktide_gore")
    if material is None:
        material = bpy.data.materials.new("darktide_gore")
        material.use_nodes = True
        bsdf = material.node_tree.nodes.get("Principled BSDF")
        if bsdf:
            bsdf.inputs["Base Color"].default_value = (0.25, 0.02, 0.02, 1.0)
            bsdf.inputs["Roughness"].default_value = 0.35
    material.dt_material.mode = "external"
    material.dt_material.resource = GORE_MATERIAL
    return material


def remove_gibs(collection):
    for obj in [obj for obj in collection.all_objects if obj.get(TAG)]:
        mesh = obj.data if obj.type == "MESH" else None
        bpy.data.objects.remove(obj)
        if mesh is not None and mesh.users == 0:
            bpy.data.meshes.remove(mesh)


def _close(bm, joint, reach, material_index, bulge):
    """Close the cut: every open loop that runs along the cut (edges the two sides shared, tagged) or lies near the
    joint gets a fan, its middle pushed out by bulge (a bit of flesh, not a flat lid)."""
    made = []
    loops = []
    pending = {edge for edge in bm.edges if edge.is_boundary}
    while pending:
        start = pending.pop()
        loop = [start.verts[0], start.verts[1]]
        while True:
            nxt = next((e for e in loop[-1].link_edges if e in pending), None)
            if nxt is None:
                break
            pending.discard(nxt)
            other = nxt.other_vert(loop[-1])
            if other is loop[0]:
                break
            loop.append(other)
        near = any(edge.tag for vert in loop for edge in vert.link_edges if edge.is_boundary) or             min((vert.co - joint).length for vert in loop) < reach
        if len(loop) >= 3 and near:
            loops.append(loop)
    for loop in loops:
        middle = sum((v.co for v in loop), Vector()) / len(loop)
        normal = Vector()
        for a, b in zip(loop, loop[1:] + loop[:1]):
            normal += (a.co - middle).cross(b.co - middle)
        if normal.length < 1e-9:
            continue
        normal.normalize()
        # face outward, away from the material the loop belongs to
        side = next((f for e in loop[0].link_edges if e.is_boundary for f in e.link_faces), None)
        if side is not None and normal.dot(side.calc_center_median() - middle) > 0:
            normal = -normal
            loop.reverse()
        radius = max((v.co - middle).length for v in loop)
        centre = bm.verts.new(middle + normal * radius * bulge)
        uv = bm.loops.layers.uv.active
        tangent = (loop[0].co - middle).normalized()
        bitangent = normal.cross(tangent)
        for a, b in zip(loop, loop[1:] + loop[:1]):
            try:
                face = bm.faces.new((a, b, centre))
            except ValueError:
                continue
            face.material_index = material_index
            if uv is not None:
                for corner in face.loops:
                    offset = corner.vert.co - middle
                    corner[uv].uv = (0.5 + offset.dot(tangent) / (2 * radius), 0.5 + offset.dot(bitangent) / (2 * radius))
            made.append(face)
    return made


def _cut(obj, inside):
    """World-space bmesh of obj and the faces mostly weighted to the bones in inside."""
    bm = bmesh.new()
    bm.from_mesh(obj.data)
    bm.faces.layers.int.new("darktide_piece")   # layers first: adding one later invalidates face references
    bm.transform(obj.matrix_world)
    deform = bm.verts.layers.deform.active
    names = {group.index: group.name for group in obj.vertex_groups}
    share = {}
    for vert in bm.verts:
        weights = vert[deform].items() if deform else []
        total = sum(w for g, w in weights if g in names)
        cut = sum(w for g, w in weights if names.get(g) in inside)
        share[vert] = cut / total if total > 0 else 0.0
    piece = {face for face in bm.faces if sum(share[v] for v in face.verts) / len(face.verts) > 0.5}
    return bm, piece


def _keep(bm, faces):
    bmesh.ops.delete(bm, geom=[face for face in bm.faces if face not in faces], context="FACES")
    loose = [vert for vert in bm.verts if not vert.link_faces]
    if loose:
        bmesh.ops.delete(bm, geom=loose, context="VERTS")


def _object(name, bm, materials, collection, parent):
    mesh = bpy.data.meshes.new(name)
    deform = bm.verts.layers.deform.active
    if deform is not None:
        bm.verts.layers.deform.remove(deform)
    bm.to_mesh(mesh)
    for material in materials:
        mesh.materials.append(material)
    obj = bpy.data.objects.new(name, mesh)
    collection.objects.link(obj)
    obj.parent = parent
    obj[TAG] = True
    return obj


def _empty(name, collection, matrix, parent=None):
    obj = bpy.data.objects.new(name, None)
    obj.empty_display_size = 0.05
    collection.objects.link(obj)
    obj.parent = parent
    if parent is None:
        obj.matrix_world = matrix
    obj[TAG] = True
    return obj


# The game's gib template with these pieces and stumps: a function (template, cuts, folder) -> copy. cuts maps a
# cut (its shrunk bones, comma separated) to {piece, actor, sound, stump}; cuts the body has none for lose their
# piece and stump (the bone still shrinks), and the game's material overrides for its own pieces are dropped.
RUNTIME_LUA = """local function own_gib(gib_entry, cuts, folder)
	local scale = gib_entry.scale_node
	local cut = cuts[type(scale) == "table" and table.concat(scale, ",") or scale or ""]
	gib_entry.material_overrides = nil
	if not cut then
		gib_entry.gib_settings = nil
		gib_entry.stump_settings = nil
		return
	end
	local gib = gib_entry.gib_settings
	if gib then
		gib.gib_unit = folder .. cut.piece
		gib.gib_flesh_unit = nil
		gib.gib_actor = cut.actor
		if gib.vfx then
			gib.vfx.node_name = cut.actor
		end
		if gib.sfx and gib.sfx.node_name then
			gib.sfx.node_name = cut.sound
		end
	end
	if gib_entry.stump_settings then
		if cut.stump then
			gib_entry.stump_settings.stump_unit = folder .. cut.stump
		else
			gib_entry.stump_settings = nil
		end
	end
end

return function(template, cuts, folder)
	local copy = table.clone(template)
	for _, by_type in pairs(copy) do
		if type(by_type) == "table" then
			for _, gib_entry in pairs(by_type) do
				if type(gib_entry) == "table" then
					for _, one in ipairs(gib_entry.conditional or (gib_entry[1] and gib_entry) or { gib_entry }) do
						own_gib(one, cuts, folder)
					end
				end
			end
		end
	end
	return copy
end
"""


def cuts_lua(made, indent=""):
    """The cuts table for RUNTIME_LUA."""
    def text(value):
        return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'
    lines = ["{"]
    for key, names in sorted(made.items()):
        lines.append(indent + "\t[%s] = { %s }," % (text(key), ", ".join(
            "%s = %s" % (field, text(names[field])) for field in ("piece", "actor", "sound", "stump") if field in names)))
    lines.append(indent + "}")
    return "\n".join(lines)


def piece_flow(names, breed):
    """The retail gib's flow: on touching something a blood splat (BloodManager.blood_ball_collision) and,
    once the piece is out, the dismember impact sound at its sound node."""
    return {
        "nodes": [
            {"id": "hit", "type": "actor_contact", "actor": names["actor"]},
            {"id": "splat", "type": "lua", "function": "blood_ball_collision",
             "params": {"colliding_actor": "actor", "collision_position": "vector3", "blood_type": "string",
                        "remove_blood_ball": "bool", "collision_normal": "vector3"},
             "inputs": {"blood_type": blood_type(breed), "remove_blood_ball": False}},
            {"id": "ready", "type": "gate", "inputs": {"start_open": False}},
            {"id": "wait", "type": "delay", "inputs": {"time": 0.01}},
            {"id": "sound", "type": "lua", "class": "WwiseFlowCallbacks", "function": "wwise_trigger_event",
             "params": {"event_resource": "string", "unit": "unit", "unit_node": "string"},
             "returns": {"playing_id": "float", "source_id": "float"},
             "inputs": {"event_resource": IMPACT_SOUND, "unit_node": names["sound"]}},
        ],
        "links": [["hit.start", "splat.in"], ["hit.start", "ready.pass"], ["ready.out", "sound.in"],
                  ["unit_spawned.out", "wait.in"], ["wait.out", "ready.open"]],
        "connections": [["hit.actor", "splat.colliding_actor"], ["hit.position", "splat.collision_position"],
                        ["hit.normal", "splat.collision_normal"]],
    }


def make_gibs(collection, rig, breed, stem, density=1000.0):
    """Cut the asset's body into the breed's gibs. Returns {cut key: names} for the cuts made."""
    remove_gibs(collection)
    meshes = _skinned_meshes(collection.all_objects, rig)
    if not meshes:
        raise ValueError("Gibs need meshes skinned to the darktide armature in the asset")
    gore = _gore_material()
    made = {}
    for cut in cuts_for(breed):
        spawn = cut["spawn"] or cut["scale"][0]
        if any(name not in rig.data.bones for name in cut["scale"] + [cut["stump"], spawn]):
            continue
        inside = _subtree(rig, cut["scale"])
        names = cut_names(stem, cut)
        spawn_world = _bone_world(rig, spawn)
        stump_world = _bone_world(rig, cut["stump"])
        cut_bone = rig.data.bones[cut["scale"][0]]
        joint = rig.matrix_world @ cut_bone.head_local
        reach = max(0.05, min(0.25, 0.5 * cut_bone.length))
        root = stump_root = None
        volume = 0.0
        for obj in meshes:
            bm, piece = _cut(obj, inside)
            if not piece:
                bm.free()
                continue
            for edge in bm.edges:
                edge.tag = len(edge.link_faces) == 2 and (edge.link_faces[0] in piece) != (edge.link_faces[1] in piece)
            side = bm.faces.layers.int.get("darktide_piece")
            for face in piece:
                face[side] = 1
            stump_bm = bm.copy()
            _keep(bm, piece)
            materials = list(obj.data.materials) + [gore]
            _close(bm, joint, reach, len(materials) - 1, 0.25)
            volume += abs(bm.calc_volume())
            stump_side = stump_bm.faces.layers.int.get("darktide_piece")
            _keep(stump_bm, {face for face in stump_bm.faces if not face[stump_side]})
            lid = set(_close(stump_bm, joint, reach, 0, 0.25))
            bm.transform(spawn_world.inverted())
            if root is None:
                root = _empty(names["actor"], collection, spawn_world)
                _empty(names["sound"], collection, spawn_world, root)
            piece_obj = _object(names["piece"] + "_" + obj.name, bm, materials, collection, root)
            piece_obj.dt_collider.role = "both"
            piece_obj.dt_collider.shape = "convex"
            piece_obj.dt_collider.body = root
            bm.free()
            if not lid:
                stump_bm.free()
                continue
            _keep(stump_bm, lid)
            for face in stump_bm.faces:
                face.material_index = 0
            stump_bm.transform(stump_world.inverted())
            if stump_root is None:
                stump_root = _empty(names["stump_root"], collection, stump_world)
            _object(names["stump"] + "_" + obj.name, stump_bm, [gore], collection, stump_root)
            stump_bm.free()
        if root is None:
            continue
        root.dt_collider.part_name = names["piece"]
        root.dt_collider.is_body = True
        root.dt_collider.actor = "dynamic"
        root.dt_collider.mass = max(0.2, min(30.0, volume * density))
        root["darktide_part_flow"] = json.dumps(piece_flow(names, breed))
        if stump_root is not None:
            stump_root.dt_collider.part_name = names["stump"]
        else:
            del names["stump"], names["stump_root"]
        made[",".join(cut["scale"])] = names
    collection["darktide_gibs"] = json.dumps({"breed": breed, "cuts": made})
    return made
