import argparse
from collections import Counter
import hashlib
import json
import math
from pathlib import Path
import struct
from bsp import Map, vector


REPO_ROOT = Path(__file__).resolve().parents[2]


SOLID = -2
UNSAFE = {SOLID, -4, -5, -6}
SOLID_BRUSHES = {
    "func_wall", "func_wall_toggle", "func_breakable", "func_pushable", "func_door",
    "func_door_rotating", "func_train", "func_tracktrain", "func_plat", "func_platrot",
    "func_pendulum", "func_rotating", "func_conveyor",
}
MOVERS = {"func_door", "func_door_rotating", "func_train", "func_tracktrain", "func_plat", "func_platrot"}
TSV_COLUMNS = [
    "edgeId", "sourceMap", "destinationMap", "sourceEntityIndex", "model", "targetname",
    "landmark", "spawnflags", "changeTarget", "changeDelay",
    "sourceLandmarkX", "sourceLandmarkY", "sourceLandmarkZ",
    "destinationLandmarkX", "destinationLandmarkY", "destinationLandmarkZ",
    "triggerMinX", "triggerMinY", "triggerMinZ", "triggerMaxX", "triggerMaxY", "triggerMaxZ",
    "initiatorX", "initiatorY", "initiatorZ", "initiatorDuck",
    "initiatorPitch", "initiatorYaw", "initiatorRoll",
    "teammateX", "teammateY", "teammateZ", "teammateDuck",
    "teammatePitch", "teammateYaw", "teammateRoll", "activation",
]


def add(a, b):
    return [a[i] + b[i] for i in range(3)]


def sub(a, b):
    return [a[i] - b[i] for i in range(3)]


def distance(a, b):
    return math.sqrt(sum((a[i] - b[i]) ** 2 for i in range(3)))


def center(bounds):
    return [(bounds[0][i] + bounds[1][i]) / 2 for i in range(3)]


def intersects(point, duck, bounds):
    half = (16, 16, 18 if duck else 36)
    return all(point[i] + half[i] > bounds[0][i] and point[i] - half[i] < bounds[1][i]
               for i in range(3))


def separate(a, aduck, b, bduck):
    return abs(a[0] - b[0]) >= 33 or abs(a[1] - b[1]) >= 33 or \
        abs(a[2] - b[2]) >= (18 if aduck else 36) + (18 if bduck else 36) + 1


def rounded(point):
    return [round(float(value), 4) for value in point] if point is not None else None


class Geometry(Map):
    def __init__(self, name, path):
        super().__init__(name, path)
        offset, size = self.lumps[1]
        self.planes = [struct.unpack_from("<4f", self.data, offset + i) for i in range(0, size, 20)]
        offset, size = self.lumps[9]
        self.clips = [struct.unpack_from("<ihh", self.data, offset + i) for i in range(0, size, 8)]
        self.heads = struct.unpack_from("<4i", self.data, self.lumps[14][0] + 36)
        offset, size = self.lumps[5]
        self.nodes = [struct.unpack_from("<ihh", self.data, offset + i) for i in range(0, size, 24)]
        offset, size = self.lumps[10]
        self.leaf_contents = [struct.unpack_from("<i", self.data, offset + i)[0] for i in range(0, size, 28)]
        self.brushes = [(entity, self.model_bounds(entity)) for entity in self.entities
                        if entity.get("classname") in SOLID_BRUSHES]
        self.hazards = [(entity, self.model_bounds(entity)) for entity in self.entities
                        if entity.get("classname") == "trigger_hurt" and float(entity.get("dmg", "0")) > 0]

    def contents(self, point, hull):
        node = self.heads[hull]
        while node >= 0:
            plane_index, front, back = self.clips[node]
            nx, ny, nz, offset = self.planes[plane_index]
            node = front if point[0] * nx + point[1] * ny + point[2] * nz >= offset else back
        return node

    def point_contents(self, point):
        # Расширенный корпус снаружи карты иногда пуст, хотя сам центр находится в solid leaf
        node = self.heads[0]
        while node >= 0:
            plane_index, front, back = self.nodes[node]
            nx, ny, nz, offset = self.planes[plane_index]
            node = front if point[0] * nx + point[1] * ny + point[2] * nz >= offset else back
        return self.leaf_contents[-1 - node]

    def clear(self, point, hull):
        return self.contents(point, hull) not in UNSAFE and self.point_contents(point) not in UNSAFE

    def segment(self, start, end, hull):
        # Это только геометрия world BSP, состояние сущностей проверяет сервер
        def visit(node, p0, p1, t0, t1, normal):
            if node < 0:
                return (t0, normal) if node == SOLID else None
            plane_index, front, back = self.clips[node]
            plane = self.planes[plane_index]
            d0 = sum(p0[i] * plane[i] for i in range(3)) - plane[3]
            d1 = sum(p1[i] * plane[i] for i in range(3)) - plane[3]
            if d0 >= 0 and d1 >= 0:
                return visit(front, p0, p1, t0, t1, normal)
            if d0 < 0 and d1 < 0:
                return visit(back, p0, p1, t0, t1, normal)
            fraction = d0 / (d0 - d1)
            middle = [p0[i] + (p1[i] - p0[i]) * fraction for i in range(3)]
            tm = t0 + (t1 - t0) * fraction
            near, far = (front, back) if d0 >= 0 else (back, front)
            hit = visit(near, p0, middle, t0, tm, normal)
            if hit:
                return hit
            entering = list(plane[:3]) if d0 >= 0 else [-value for value in plane[:3]]
            return visit(far, middle, p1, tm, t1, entering)

        return visit(self.heads[hull], start, end, 0, 1, [0, 0, 0])

    def floor(self, point, duck, depth=72):
        hull = 3 if duck else 1
        if not self.clear(point, hull):
            return None
        hit = self.segment(point, add(point, [0, 0, -depth]), hull)
        if not hit or hit[1][2] < 0.7:
            return None
        result = add(point, [0, 0, -depth * hit[0] + 1])
        if not self.clear(result, hull):
            return None
        return result

    def overlaps(self, point, duck):
        result = []
        for entity, bounds in self.brushes:
            if bounds and intersects(point, duck, bounds):
                result.append({"sourceEntityIndex": entity["index"], "classname": entity["classname"],
                               "model": entity.get("model", ""), "targetname": entity.get("targetname", ""),
                               "globalname": entity.get("globalname", ""),
                               "movingOrControlled": entity["classname"] in MOVERS})
        return result

    def brush_contents(self, entity, point, hull):
        model = entity.get("model", "")
        if not model.startswith("*"):
            return None
        offset = self.lumps[14][0] + int(model[1:]) * 64
        node = struct.unpack_from("<4i", self.data, offset + 36)[hull]
        origin = vector(entity.get("origin"))
        local = sub(point, origin)
        while node >= 0:
            plane_index, front, back = self.clips[node]
            plane = self.planes[plane_index]
            node = front if sum(local[i] * plane[i] for i in range(3)) >= plane[3] else back
        return node

    def unsafe_trigger(self, point, duck):
        # START_OFF и пустые части составного brush не наносят урон при исходном состоянии
        return any(not int(entity.get("spawnflags", "0")) & 2 and bounds and
                   intersects(point, duck, bounds) and
                   self.brush_contents(entity, point, 3 if duck else 1) == SOLID
                   for entity, bounds in self.hazards)


def landmarks(bsp, name):
    return [entity for entity in bsp.targets.get(name, []) if entity.get("classname") == "info_landmark"]


def destination_landmark(bsp, name, source_origin):
    native = landmarks(bsp, name)
    if native:
        return vector(native[0].get("origin")), "native_exact"
    if bsp.name in {"c1a0b", "c1a0e", "c1a0c"} and name in {"c1a0btoc", "c1a0etoc"}:
        alias = landmarks(bsp, "c1a0etoc" if name == "c1a0btoc" else "c1a0btoc")
        if alias:
            return vector(alias[0].get("origin")), "coop_story_alias"
    if source_origin is not None:
        for entity in bsp.entities:
            if entity.get("classname") == "info_landmark" and distance(vector(entity.get("origin")), source_origin) < 1:
                return vector(entity.get("origin")), "coop_same_origin_fallback"
    return None, "missing"


def transition_volumes(bsp, name):
    return [bsp.describe(entity) for entity in bsp.targets.get(name, [])
            if entity.get("classname") == "trigger_transition"]


def in_transition(point, duck, volumes):
    return not volumes or any(volume.get("bounds") and intersects(point, duck, volume["bounds"])
                              for volume in volumes)


def axis_values(lo, hi):
    values = [(lo + hi) / 2, lo + (hi - lo) * 0.25, lo + (hi - lo) * 0.75,
              lo + 1, hi - 1, lo - 15, hi + 15]
    return list(dict.fromkeys(round(value, 4) for value in values))


def placement(bsp, point, duck, angles):
    return {"origin": rounded(point), "duck": bool(duck), "angles": rounded(angles),
            "worldHull": "duck" if duck else "standing",
            "pointWorldContents": bsp.point_contents(point),
            "standingWorldContents": bsp.contents(point, 1), "duckWorldContents": bsp.contents(point, 3),
            "worldFloorOrigin": rounded(bsp.floor(point, duck)),
            "brushAabbOverlaps": bsp.overlaps(point, duck),
            "touchesNativeHurtVolume": bsp.unsafe_trigger(point, duck)}


def dense_values(lo, hi, step=8):
    return list(dict.fromkeys(axis_values(lo, hi) +
                             [lo + index * step for index in range(int((hi - lo) // step) + 1)]))


def find_initiator(source, destination, bounds, volumes, source_lm, offset, activation,
                   dense=False, alternatives=False):
    if bounds is None:
        return [] if alternatives else None
    region = bounds
    region_kind = "original_trigger_aabb"
    if activation == "use" and volumes:
        region = min((volume["bounds"] for volume in volumes if volume.get("bounds")),
                     key=lambda value: distance(center(value), source_lm or center(bounds)), default=bounds)
        region_kind = "native_transition_volume_aabb"
    elif activation == "use" and source_lm is not None:
        # Именованные служебные brushes могут находиться в потолке и запускаться дверью
        region = [add(source_lm, [-48, -48, -36]), add(source_lm, [48, 48, 72])]
        region_kind = "landmark_neighborhood_no_native_transition_volume"
    origin = center(region)
    points = []
    for duck in (False, True):
        half_height = 18 if duck else 36
        zs = [origin[2], region[0][2] + half_height + 1, region[1][2] - half_height - 1]
        if source_lm:
            zs += [source_lm[2], source_lm[2] + 18, source_lm[2] + 36]
        zs += [region[0][2] - half_height + 2, region[1][2] + half_height - 2]
        if dense:
            zs += dense_values(region[0][2] - half_height + 2, region[1][2] + half_height - 2)
        xs = dense_values(region[0][0], region[1][0]) if dense else axis_values(region[0][0], region[1][0])
        ys = dense_values(region[0][1], region[1][1]) if dense else axis_values(region[0][1], region[1][1])
        for x in xs:
            for y in ys:
                for z in dict.fromkeys(zs):
                    point = [x, y, z]
                    hull = 3 if duck else 1
                    if not intersects(point, duck, region) or not source.clear(point, hull):
                        continue
                    floor = source.floor(point, duck)
                    if floor is not None and intersects(floor, duck, region):
                        point = floor
                    if (activation == "use" or dense) and not in_transition(point, duck, volumes):
                        continue
                    if source.unsafe_trigger(point, duck):
                        continue
                    destination_clear = offset is not None and destination.clear(add(point, offset), hull)
                    overlaps = source.overlaps(point, duck)
                    fixed_overlaps = sum(not item["movingOrControlled"] for item in overlaps)
                    # Статический порядок предпочитает опору и свободные корпуса по обе стороны
                    score = (fixed_overlaps, len(overlaps), 0 if floor is not None else 1,
                             0 if destination_clear else 1, int(duck), distance(point, origin))
                    points.append((score, point, duck))
    if not points:
        return [] if alternatives else None
    results = []
    seen = set()
    per_pose = Counter()
    for _, point, duck in sorted(points, key=lambda value: value[0]):
        key = (duck, *rounded(point))
        if key in seen or per_pose[duck] >= 24:
            continue
        seen.add(key)
        per_pose[duck] += 1
        result = placement(source, point, duck, [0, 0, 0])
        result["setupRegion"] = region_kind
        result["touchesOriginalTriggerAabb"] = intersects(point, duck, bounds)
        result["inTransitionVolume"] = in_transition(point, duck, volumes)
        result["originalTriggerCenterInside"] = all(bounds[0][i] <= point[i] <= bounds[1][i] for i in range(3))
        if not alternatives:
            return result
        results.append(result)
    return results


def find_teammate(source, leader, bounds, volumes, expanded=False, require_volume=True):
    if leader is None:
        return None
    base = leader["origin"]
    candidates = []
    offsets = [(x, y, 0) for radius in (36, 48, 72, 96, 144) for x, y in
               ((radius, 0), (-radius, 0), (0, radius), (0, -radius),
                (radius, radius), (radius, -radius), (-radius, radius), (-radius, -radius))]
    if expanded:
        # Для изолированной проверки переноса товарищ может стоять за углом или ниже в шахте
        offsets += [(x, y, z) for x in range(-128, 129, 16) for y in range(-128, 129, 16)
                    for z in (-73, -72, -55, -54, -37, -36, -18, 0, 18, 36, 37, 54, 55, 72, 73)]
    for duck in (leader["duck"], not leader["duck"]):
        start = add(base, [0, 0, 0 if duck == leader["duck"] else (-18 if duck else 18)])
        for x, y, z in offsets:
            point = add(start, [x, y, z])
            hull = 3 if duck else 1
            if not source.clear(point, hull) or (not expanded and source.segment(base, point, hull)):
                continue
            floor = source.floor(point, duck)
            positions = [floor, point] if expanded and floor else [floor or point]
            for candidate in positions:
                if not separate(base, leader["duck"], candidate, duck) or source.unsafe_trigger(candidate, duck):
                    continue
                if expanded and require_volume and not in_transition(candidate, duck, volumes):
                    continue
                overlaps = source.overlaps(candidate, duck)
                direct = source.clear(start, hull) and source.segment(start, candidate, hull) is None
                score = (sum(not item["movingOrControlled"] for item in overlaps), len(overlaps),
                         0 if floor else 1, 0 if in_transition(candidate, duck, volumes) else 1,
                         (0 if direct else 1) if expanded else 0,
                         1 if intersects(candidate, duck, bounds) else 0, distance(candidate, base), int(duck))
                candidates.append((score, candidate, duck, direct))
    if not candidates:
        return None
    _, point, duck, direct = min(candidates, key=lambda value: value[0])
    result = placement(source, point, duck, leader["angles"])
    result["touchesOriginalTriggerAabb"] = intersects(point, duck, bounds)
    result["inTransitionVolume"] = in_transition(point, duck, volumes)
    result["straightWorldHullPathFromInitiator"] = direct
    return result


def improve_pair(source, destination, bounds, volumes, source_lm, offset, activation, leader, teammate):
    if leader and leader["inTransitionVolume"]:
        if teammate and teammate["inTransitionVolume"]:
            return leader, teammate, "original_sparse_pair"
        teammate = find_teammate(source, leader, bounds, volumes, expanded=True)
        if teammate:
            return leader, teammate, "expanded_3d_teammate"
    options = find_initiator(source, destination, bounds, volumes, source_lm, offset, activation,
                             dense=True, alternatives=True)
    # Сначала пробуем обе позы, чтобы один стоящий игрок не занял всю маленькую камеру
    options.sort(key=lambda item: (bool(item["brushAabbOverlaps"]), item["worldFloorOrigin"] is None))
    if options:
        leader = options[0]
    for candidate in options:
        teammate = find_teammate(source, candidate, bounds, volumes, expanded=True)
        if teammate and not candidate["brushAabbOverlaps"] and not teammate["brushAabbOverlaps"]:
            return candidate, teammate, "dense_joint_pair"
    if leader and leader["inTransitionVolume"] and volumes:
        # В однопользовательской камере товарищ может ждать снаружи и переноситься к лидеру
        teammate = find_teammate(source, leader, bounds, volumes, expanded=True, require_volume=False)
        if teammate and not leader["brushAabbOverlaps"] and not teammate["brushAabbOverlaps"]:
            return leader, teammate, "nearby_outside_native_volume"
    return leader, None, "dense_search_pair_unavailable"


def reverse_edges(destination, source_name, landmark):
    return [destination.describe(entity) for entity in destination.entities
            if entity.get("classname") == "trigger_changelevel" and entity.get("map") == source_name
            and entity.get("landmark") == landmark]


def native_callers(bsp, target):
    if not target:
        return []
    result = []
    for entity in bsp.entities:
        outputs = [key for key in ("target", "netname") if entity.get(key) == target]
        # Платформы и камеры вызывают выход path_corner.message при достижении узла
        if entity.get("classname") in {"path_corner", "path_track", "info_bigmomma"} and entity.get("message") == target:
            outputs.append("message")
        if entity.get("classname") == "multi_manager":
            outputs += [key for key in entity if key.split("#", 1)[0] == target]
        if outputs:
            result.append({"sourceEntityIndex": entity["index"], "classname": entity.get("classname"),
                           "targetname": entity.get("targetname", ""), "outputs": outputs})
    return result


def inbound_change_callers(maps, destination_name, target):
    if not target:
        return []
    return [{"sourceMap": bsp.name, "sourceEntityIndex": entity["index"],
             "classname": "trigger_changelevel", "outputs": ["changetarget"],
             "delay": float(entity.get("changedelay", "0"))}
            for bsp in maps.values() for entity in bsp.entities
            if entity.get("classname") == "trigger_changelevel" and entity.get("map") == destination_name
            and entity.get("changetarget") == target]


def checked_scene_pair(source, destination, bounds, volumes, landmark, leader, teammate):
    key = (source.name, destination.name)
    correction = None
    if key == ("c2a3d", "c2a3e"):
        # После teleport845 и задержки gotodrag12 игрок стоит на исходном полу805
        leader = placement(source, [-2348, -664, 805], False, [0, 270, 0])
        teammate = placement(source, [-2312, -664, 805], False, [0, 270, 0])
        leader["nativeTeleportPlayerOrigin"] = [-2348, -664, 845]
        leader["nativeChangelevelDelaySeconds"] = 12
        correction = "authored_ambush_after_native_wait_on_source_floor"
    elif key == ("c4a1b", "c4a1c"):
        # Для исходного портала нужна точка внутри входа, а не пол ниже его за краем
        leader = placement(source, [-760, 1508, 196], True, [0, 0, 0])
        teammate = find_teammate(source, leader, bounds, volumes, expanded=True, require_volume=False)
        correction = "authored_portal_entry_crouching_hull"
    elif key == ("c4a1c", "c4a1b") and teammate:
        # Конвейер под товарищем находится выше world пола на 32 единицы
        teammate = placement(source, [-922, -1610, -379], False, teammate["angles"])
        correction = "teammate_above_original_conveyor_surface"
    elif key == ("c3a2", "c3a2e"):
        # AABB поршня шире его solid hull, обе точки проходят в исходном проёме
        leader = placement(source, [1969, 313, 337], False, [0, 0, 0])
        teammate = placement(source, [1921, 313, 337], False, [0, 0, 0])
        piston = source.targets["c3a2_piston"][0]
        if any(source.brush_contents(piston, player["origin"], 1) == SOLID for player in (leader, teammate)):
            raise ValueError("Piston setup is inside the original brush hull")
        correction = "original_piston_brush_hull_opening"
    elif key == ("c1a2", "c1a2d"):
        # Runtime обнаружил сдвинутый ящик у старой точки, crouch seed проходит над ним
        teammate = placement(source, [1327, -1485.5, -485], True, [0, 0, 0])
        correction = "teammate_above_runtime_pushable_collision"
    elif key in {("c2a4b", "c2a4c"), ("c2a4c", "c2a4b")}:
        # На точной границе AABB движок задевает конвейер, корпус поднимается над его верхом
        point = [-424, -2672, -429] if source.name == "c2a4b" else [-1728, -584, -429]
        teammate = placement(source, point, True, [0, 0, 0])
        correction = "teammate_above_runtime_conveyor_collision"
    elif key == ("c3a2b", "c3a2c") and landmark == "c3a2_bc2":
        # Дверь dontopen касается прежнего корпуса на границе y560
        teammate = placement(source, [-416, 508, 161], True, [0, 0, 0])
        correction = "teammate_away_from_runtime_door_collision"
    if correction:
        for role, player in (("initiator", leader), ("teammate", teammate)):
            if player is None:
                continue
            if not source.clear(player["origin"], 3 if player["duck"] else 1):
                raise ValueError(f"Scene setup is not world clear: {key}/{role}")
            player["touchesOriginalTriggerAabb"] = intersects(player["origin"], player["duck"], bounds)
            player["inTransitionVolume"] = in_transition(player["origin"], player["duck"], volumes)
            if role == "initiator":
                player["setupRegion"] = correction
                player["originalTriggerCenterInside"] = all(bounds[0][i] <= player["origin"][i] <= bounds[1][i] for i in range(3))
            else:
                player["straightWorldHullPathFromInitiator"] = source.segment(leader["origin"], player["origin"], 3 if player["duck"] else 1) is None
    return leader, teammate, correction


def build(catalog_path, maps_path):
    chapters = json.loads(catalog_path.read_text(encoding="utf-8-sig"))["chapters"]
    chapter_by_map = {name: index for index, chapter in enumerate(chapters) for name in chapter["maps"]}
    maps = {name: Geometry(name, maps_path / (name + ".bsp")) for name in chapter_by_map}
    edges = []
    for source_name, source in maps.items():
        for entity in source.entities:
            if entity.get("classname") != "trigger_changelevel":
                continue
            destination = maps[entity["map"]]
            landmark = entity.get("landmark", "")
            source_markers = landmarks(source, landmark)
            source_origin = vector(source_markers[0].get("origin")) if source_markers else None
            destination_origin, resolution = destination_landmark(destination, landmark, source_origin)
            offset = sub(destination_origin, source_origin) if destination_origin is not None and source_origin is not None else None
            bounds = source.model_bounds(entity)
            flags = int(entity.get("spawnflags", "0"))
            activation = "use" if flags & 2 else "touch"
            volumes = transition_volumes(source, landmark)
            leader = find_initiator(source, destination, bounds, volumes, source_origin, offset, activation)
            callers = native_callers(source, entity.get("targetname", ""))
            inbound_callers = inbound_change_callers(maps, source_name, entity.get("targetname", ""))
            scripted_portal = (source_name, destination.name) in {("c4a1a", "c4a1b"), ("c4a1f", "c4a3")}
            authored_use = activation == "touch" and callers and (not leader or
                bounds[1][2] - bounds[0][2] <= 16 or scripted_portal)
            if authored_use:
                activation = "use"
                leader = find_initiator(source, destination, bounds, volumes, source_origin, offset, activation)
            teammate = find_teammate(source, leader, bounds, volumes)
            setup_search = "original_sparse_pair"
            if activation == "touch" or entity.get("targetname"):
                leader, teammate, setup_search = improve_pair(source, destination, bounds, volumes,
                                                               source_origin, offset, activation, leader, teammate)
            leader, teammate, scene_correction = checked_scene_pair(source, destination, bounds, volumes, landmark, leader, teammate)
            if scene_correction:
                setup_search = scene_correction
            edge = {
                "edgeId": len(edges), "sourceMap": source_name, "destinationMap": destination.name,
                "sourceEntityIndex": entity["index"], "model": entity.get("model", ""),
                "targetname": entity.get("targetname", ""), "landmark": landmark,
                "spawnflags": flags, "activation": activation, "nativeCallers": callers,
                "inboundChangeTargetCallers": inbound_callers,
                "runtimeCallbackAvailable": activation == "touch" or bool(entity.get("targetname")),
                "changeTarget": entity.get("changetarget", ""), "changeDelay": float(entity.get("changedelay", "0")),
                "sourceLandmarkOrigin": source_origin, "destinationLandmarkOrigin": destination_origin,
                "destinationLandmarkResolution": resolution, "landmarkOffset": offset,
                "triggerBounds": bounds, "sourceTransitionVolumes": volumes,
                "reverseTransitions": reverse_edges(destination, source_name, landmark),
                "setup": {"initiator": leader, "teammate": teammate}, "flags": [],
                "setupSearch": setup_search,
                "branch": "same_chapter" if chapter_by_map[source_name] == chapter_by_map[destination.name]
                else ("chapter_forward" if chapter_by_map[source_name] < chapter_by_map[destination.name] else "chapter_return"),
            }
            if activation == "use":
                edge["flags"].append("authored_script_use_for_service_brush" if authored_use else "native_use_only_no_touch_callback")
                if not edge["targetname"]:
                    edge["flags"].append("native_no_use_callback_without_targetname")
            if source_origin is None:
                edge["flags"].append("missing_source_landmark")
            if resolution == "missing":
                edge["flags"].append("missing_destination_landmark_falls_back_to_map_spawn")
            elif resolution != "native_exact":
                edge["flags"].append(resolution)
            if not leader:
                edge["flags"].append("no_world_clear_initiator_candidate")
            if not teammate:
                edge["flags"].append("no_world_clear_teammate_candidate")
            edge["expectedArrival"] = {}
            for role, player in (("initiator", leader), ("teammate", teammate)):
                if not player:
                    continue
                if player["brushAabbOverlaps"]:
                    edge["flags"].append(role + "_brush_aabb_overlap_runtime_required")
                if player["worldFloorOrigin"] is None:
                    edge["flags"].append(role + "_no_world_floor_within72_runtime_required")
                if role == "teammate" and volumes and not player["inTransitionVolume"]:
                    edge["flags"].append("teammate_outside_native_volume_collects_at_leader")
                if offset is None:
                    continue
                personal = role == "initiator" or bool(volumes) and player["inTransitionVolume"]
                incoming = player if personal else leader
                point = add(incoming["origin"], offset)
                arrival = placement(destination, point, incoming["duck"], incoming["angles"])
                arrival["personalOrigin"] = personal
                edge["expectedArrival"][role] = arrival
                if not destination.clear(point, 3 if incoming["duck"] else 1):
                    edge["flags"].append(role + "_raw_arrival_world_blocked_runtime_correction_required")
            if source_name.startswith("c0a0") or destination.name.startswith("c0a0"):
                edge["flags"].append("intro_train_state_and_moving_spawn_required")
            if (source_name, destination.name) in {("c2a3d", "c2a3e"), ("c3a2d", "c4a1"),
                                                  ("c4a2b", "c4a1a"), ("c4a3", "c5a1")}:
                edge["flags"].append("scripted_campaign_boundary_full_scene_not_exercised_by_direct_use")
            edges.append(edge)
    counts = Counter(flag for edge in edges for flag in edge["flags"])
    return {
        "schemaVersion": 1,
        "scope": {
            "purpose": "Two-player original trigger_changelevel transport checks, not manual destination map changes",
            "mapsPath": str(maps_path.resolve()), "mapCount": len(maps),
            "sourceEntityIndex": "Zero-based original BSP entity index, never a runtime edict index",
            "geometry": "Original world BSP standing hull1, crouching hull3 and hull0 leaf PointContents; original brush AABB only",
            "safety": "Candidate setup requires runtime floor/hull/hurt-volume and actual trigger/transition-volume validation",
            "limitations": ["Dynamic doors, movers, persistent actors, monsters, other players and script prerequisites are not simulated",
                            "AABB brush overlap is conservative and is not a full dynamic brush trace",
                            "Use-only brushes require their real assigned Use callback; touching them is not a valid substitute",
                            "A successful isolated transport check does not prove the preceding puzzle, boss, scene or full campaign walkthrough",
                            "A safe source teammate location behind a corner does not prove a direct straight walking path from the initiator",
                            "Two engine fake clients are not two Steam computers or a reconnect/sign-on protocol test"],
            "runtimeContract": "Touch the original resolved trigger or invoke its native Use callback when USEONLY; observe real deferred CHANGE_LEVEL and both arrivals",
        },
        "summary": {"transitionCount": len(edges), "touchCount": sum(edge["activation"] == "touch" for edge in edges),
                    "useCount": sum(edge["activation"] == "use" for edge in edges),
                    "useOnlyCount": sum(bool(edge["spawnflags"] & 2) for edge in edges),
                    "twoCandidatePairs": sum(bool(edge["setup"]["initiator"] and edge["setup"]["teammate"]) for edge in edges),
                    "twoCallbackCandidatePairs": sum(bool(edge["runtimeCallbackAvailable"] and edge["setup"]["initiator"]
                                                          and edge["setup"]["teammate"]) for edge in edges),
                    "unavailableCallbackCount": sum(not edge["runtimeCallbackAvailable"] for edge in edges),
                    "flags": dict(sorted(counts.items()))},
        "codeAudit": {
            "arrival": "CoopCampaignBeginTransition retains the initiator position and only teammate positions inside explicitly authored trigger_transition volumes; other teammates use the leader anchor and zero velocity. ServerActivate adds destination-minus-source landmark offset, FindSpawn resolves safe nearby arrival",
            "missingLandmark": "Story adds only the c1a0btoc/c1a0etoc aliases; Campaign also accepts an existing landmark within1unit of the source origin; otherwise it uses ordinary map spawn",
            "safeSearch": "FindSafeCoopPosition checks original position, bounded floor lifts1/2/4/8, bounded global-door edge correction, then hull-connected floor-checked offsets48/96/144 and alternate crouch height",
            "safeSearchLimit": "The original-position acceptance checks hull and harmful contents but does not require a supporting floor",
            "farTeammateRisk": "Runtime baseline edge150 confirmed a clear but floorless far arrival with the former no-volume fallback. Production now gathers non-initiators when no explicit volume exists; far-player recheck must confirm this fix",
            "reverseGuard": "Reverse volumes require matching source map and exact landmark, except the c1a0c b/e landmark aliases at the same physical point;1.5second cooldown and exit-from-volume gating suppress bounceback",
            "cameraBoundary": "Story can intentionally freeze players after successful restoration; record early post-Restore snapshots before camera control and do not classify intentional FL_FROZEN as failed transport",
        },
        "mapEvidence": [{"map": name, "bytes": len(bsp.data), "sha256": hashlib.sha256(bsp.data).hexdigest()}
                        for name, bsp in maps.items()],
        "endBranches": [{"map": name, "entities": [bsp.describe(entity) for entity in bsp.entities
                         if entity.get("classname") in {"game_end", "trigger_endsection"} or
                         (name == "c5a1" and entity.get("targetname") == "end3")],
                         "limitation": "Terminal script/game-end behavior is separate from trigger_changelevel transport"}
                        for name, bsp in maps.items() if name in {"c4a3", "c5a1"}],
        "edges": edges,
    }


def write_tsv(report, path):
    def coords(point):
        return point if point is not None else ["nan"] * 3

    def player_values(player):
        return coords(player["origin"] if player else None) + [int(player["duck"]) if player else 0] + \
            (player["angles"] if player else [0, 0, 0])

    rows = ["#" + "\t".join(TSV_COLUMNS)]
    for edge in report["edges"]:
        values = [edge[key] for key in TSV_COLUMNS[:10]]
        values += coords(edge["sourceLandmarkOrigin"]) + coords(edge["destinationLandmarkOrigin"])
        bounds = edge["triggerBounds"] or [["nan"] * 3, ["nan"] * 3]
        values += bounds[0] + bounds[1]
        setup = edge["setup"] if edge["runtimeCallbackAvailable"] else {"initiator": None, "teammate": None}
        values += player_values(setup["initiator"]) + player_values(setup["teammate"])
        values += [edge["activation"]]
        if len(values) != len(TSV_COLUMNS):
            raise ValueError("Unexpected TSV column count")
        rows.append("\t".join(str(value) if value != "" else "-" for value in values))
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(rows) + "\n", encoding="ascii")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--maps", type=Path, default=Path(r"F:\Games\Half-Life\valve\maps"))
    parser.add_argument("--campaign", type=Path, default=REPO_ROOT / "game/hlcoop/campaign.json")
    parser.add_argument("--output", type=Path, default=REPO_ROOT / "artifacts/transition-check/catalog.json")
    parser.add_argument("--tsv", type=Path, default=REPO_ROOT / "artifacts/transition-check/transition-edges.tsv")
    args = parser.parse_args()
    report = build(args.campaign, args.maps)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    write_tsv(report, args.tsv)
    print(json.dumps(report["summary"], ensure_ascii=False))


if __name__ == "__main__":
    main()
