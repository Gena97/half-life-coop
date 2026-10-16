import argparse
from collections import Counter
import csv
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
import math
from pathlib import Path
import re
import sys

sys.dont_write_bytecode = True
REPO_ROOT = Path(__file__).resolve().parents[2]
_spec = importlib.util.spec_from_file_location("hlcoop_map_report", REPO_ROOT / "scripts/map-check/report.py")
_helpers = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_helpers)
STATES = ("passed", "transition_failed", "setup_not_reached", "protocol_limitation", "not_checked")
SYNTHETIC_IDENTITY = "synthetic_lan_auth_adapter"
SUPPRESSED_VIEW = "suppressed_for_owned_fake_probes"
SUPPRESSED_MUSIC = "suppressed_cd_play_stop_for_owned_fake_probes"
SUPPRESSED_CROSSHAIR = "suppressed_for_owned_fake_probes"
POSITION_MODES = ("landmark_relative", "map_spawn_fallback")
SOURCE_SETUP_MODES = ("point_setup", "native_train_ride", "native_ambush", "point_staged_native_xen_gate_portal")
STATE_VALIDATION_MODES = ("exact_transfer", "native_destination_weaponstrip")
# Имена локальных BSP моделей исходного поезда из проверенного каталога оригинальной кампании
INTRO_TRAIN_MODELS = {"c0a0": "*12", "c0a0a": "*24", "c0a0b": "*15",
                      "c0a0c": "*74", "c0a0d": "*10", "c0a0e": "*1"}
INTRO_TRAIN_EXITS = set(zip(list(INTRO_TRAIN_MODELS)[:-1], list(INTRO_TRAIN_MODELS)[1:]))
PRIVATE_KEYS = {"auth", "authid", "auth_id", "steamid", "steam_id", "userid", "user_id", "password", "rconpassword"}


def public_fields(value):
    if isinstance(value, dict):
        return {key: ("<unexpected auth>" if key in ("raw_auth", "normalized_auth", "source_raw_auth") and
                      item not in ("", "BOT", "STEAM_ID_LAN") else public_fields(item))
                for key, item in value.items() if key.lower() not in PRIVATE_KEYS}
    if isinstance(value, list):
        return [public_fields(item) for item in value]
    return value


def identity_evidence(samples, mode):
    baselines = {}
    issues = set()
    raw_values, normalized_values = set(), set()
    raw_lan_seen = set()
    name_recovered = flags_recovered = normalized_samples = changed_samples = transient_mismatches = 0
    for sample in samples:
        for player in sample.get("players", []):
            probe = player.get("probe")
            if probe not in (0, 1):
                issues.add("unexpected_probe_index")
                continue
            slot, userid = player.get("slot"), player.get("userid")
            raw_userid = player.get("raw_userid")
            if probe not in baselines and sample.get("phase") == "source_spawn" and \
                    isinstance(slot, int) and slot > 0 and isinstance(userid, int) and userid > 0:
                baselines[probe] = (slot, userid, player.get("name"), player.get("auth"))
            if probe in baselines:
                source_slot, source_userid, source_name, source_auth = baselines[probe]
                if slot != source_slot or userid != source_userid or raw_userid != source_userid:
                    issues.add(f"slot_or_userid_changed:{probe}")
                if "raw_netname" in player and player["raw_netname"] not in ("", source_name):
                    issues.add(f"unexpected_raw_name:{probe}")
            raw_auth = player.get("raw_auth")
            normalized_auth = player.get("normalized_auth")
            if isinstance(raw_auth, str):
                raw_values.add(raw_auth if raw_auth in ("", "BOT", "STEAM_ID_LAN") else "<unexpected auth>")
            if isinstance(normalized_auth, str):
                normalized_values.add(normalized_auth if normalized_auth in ("", "BOT", "STEAM_ID_LAN") else "<unexpected auth>")
            if mode == SYNTHETIC_IDENTITY:
                if raw_auth not in ("BOT", "STEAM_ID_LAN"):
                    issues.add(f"unexpected_raw_auth:{probe}")
                if normalized_auth != "STEAM_ID_LAN":
                    issues.add(f"unexpected_normalized_auth:{probe}")
                if player.get("source_raw_auth") not in ("BOT", "STEAM_ID_LAN"):
                    issues.add(f"unexpected_source_raw_auth:{probe}")
                if player.get("slot_userid_matches") is not True:
                    issues.add(f"engine_userid_mismatch:{probe}")
                if raw_auth == "STEAM_ID_LAN":
                    raw_lan_seen.add(probe)
                elif raw_auth == "BOT" and probe in raw_lan_seen:
                    issues.add(f"raw_auth_reversed_to_bot:{probe}")
            elif probe in baselines and raw_auth is not None and raw_auth != baselines[probe][3]:
                issues.add(f"raw_auth_changed_without_adapter:{probe}")
            if player.get("first_ready_observed") and not player.get("identity_matches"):
                issues.add(f"identity_lost_after_arrival:{probe}")
            elif not player.get("identity_matches"):
                transient_mismatches += 1
            name_recovered += bool(player.get("name_recovered"))
            flags_recovered += bool(player.get("flags_recovered"))
            normalized_samples += bool(player.get("auth_normalized"))
            changed_samples += bool(player.get("raw_auth_changed"))
    for probe in (0, 1):
        if probe not in baselines:
            issues.add(f"source_identity_not_observed:{probe}")
    return {"mode": mode, "slotsAndUseridsPreserved": bool(len(baselines) == 2 and
                not any("slot_or_userid_changed" in issue or "engine_userid_mismatch" in issue for issue in issues)),
            "rawAuthValues": sorted(raw_values), "normalizedAuthValues": sorted(normalized_values),
            "unexpectedIdentityIssues": sorted(issues), "authNormalizedSamples": normalized_samples,
            "rawAuthChangedSamples": changed_samples, "nameRecoveryObserved": bool(name_recovered),
            "flagsRecoveryObserved": bool(flags_recovered), "transientIdentityMismatchSamples": transient_mismatches}


def state_evidence(edge, records, result, declared_modes):
    mode = result.get("state_validation_mode", "exact_transfer")
    issues, probes = [], []
    if mode not in declared_modes:
        issues.append("undeclared_state_validation_mode")
    arrivals = {}
    for probe, prefix in ((0, "alpha"), (1, "beta")):
        first = next(((index, record, player) for index, record in enumerate(records)
                      if record.get("type") == "sample" and record.get("phase") in
                      (prefix + "_first_ready", prefix + "_post_restore")
                      for player in record.get("players", [])
                      if player.get("probe") == probe and player.get("first_ready_observed")), None)
        if first is None:
            issues.append(f"missing_state_arrival:{probe}")
        else:
            arrivals[probe] = first
    evidence = {"mode": mode, "verified": False, "issues": issues, "verifiedProbes": probes,
                "rawStateEqualPreserved": True, "sourceSnapshotRewritten": False}
    if mode == "exact_transfer":
        for probe, (_, _, player) in arrivals.items():
            if player.get("state_equal") is True and player.get("state_validation_mode", "exact_transfer") == mode:
                probes.append(probe)
            else:
                issues.append(f"exact_state_mismatch:{probe}")
        evidence["verified"] = not issues and len(probes) == 2
        return evidence
    if mode != "native_destination_weaponstrip":
        issues.append("unsupported_state_validation_mode")
        return evidence
    if edge["edgeId"] != 219 or (edge["sourceMap"], edge["destinationMap"], edge["targetname"], edge["landmark"]) != \
            ("c4a3", "c5a1", "change_to_c5a1", "c4a3toc5a1"):
        issues.append("native_strip_outside_audited_transition")

    def number(value):
        return type(value) in (int, float) and math.isfinite(value)

    def near(value, expected, tolerance=0.1):
        return isinstance(value, list) and len(value) == 3 and all(number(value[i]) and
            abs(value[i] - expected[i]) <= tolerance for i in range(3))

    proof = {}
    for kind in ("native_destination_strip_producer", "native_destination_strip_use", "native_destination_weaponstrip"):
        found = [(index, record) for index, record in enumerate(records) if record.get("type") == kind]
        if len(found) != 1:
            issues.append("native_strip_proof_event_count:" + kind)
        proof[kind] = found[0] if found else (-1, {})
    producer_index, producer = proof["native_destination_strip_producer"]
    use_index, use = proof["native_destination_strip_use"]
    effect_index, effect = proof["native_destination_weaponstrip"]
    for _, event in proof.values():
        if event.get("observed_map") != "c5a1" or any(event.get(field) is not True for field in
                ("observer_only", "before_first_restore", "source_snapshot_unchanged")):
            issues.append("native_strip_observer_scope_invalid")
    if producer.get("producer_bsp_entity") != 588 or producer.get("producer_classname") != "trigger_auto" or \
            producer.get("producer_target") != "strip_weapons" or producer.get("producer_use_type") != 0 or \
            producer.get("native_think_observed") is not True or not near(producer.get("producer_origin"), [-2992, 3024, 352]) or \
            not number(producer.get("producer_delay_seconds")) or abs(producer["producer_delay_seconds"] - 0.3) > 0.001 or \
            type(producer.get("producer_edict")) is not int or producer["producer_edict"] < 1:
        issues.append("native_strip_producer_invalid")
    for event in (use, effect):
        elapsed = event.get("game_time", 0) - producer.get("game_time", 0)
        if event.get("event_bsp_entity") != 589 or event.get("event_classname") != "player_weaponstrip" or \
                event.get("event_targetname") != "strip_weapons" or not near(event.get("event_origin"), [-2960, 3024, 352]) or \
                not number(event.get("event_damage")) or abs(event["event_damage"]) > 0.001 or \
                type(event.get("event_edict")) is not int or event["event_edict"] < 1 or \
                event.get("caller_classname") != "DelayedUse" or event.get("caller_target") != "strip_weapons" or \
                type(event.get("caller_edict")) is not int or event["caller_edict"] < 1 or event.get("use_type") != 0 or \
                event.get("producer_edict") != producer.get("producer_edict") or \
                event.get("producer_game_time") != producer.get("game_time") or not number(elapsed) or not 0.299 <= elapsed <= 5 or \
                not number(event.get("observed_delay_seconds")) or abs(event["observed_delay_seconds"] - elapsed) > 0.001:
            issues.append("native_strip_delayed_use_invalid")
    if use.get("value") != 0 or use.get("event_edict") != effect.get("event_edict") or \
            use.get("caller_edict") != effect.get("caller_edict") or \
            abs(use.get("game_time", 0) - effect.get("game_time", 0)) > 0.01 or \
            effect.get("native_effect_completed") is not True or effect.get("effect") != "clear_weapons_and_ammo":
        issues.append("native_strip_completed_effect_invalid")
    source_index, source = next(((index, record) for index, record in enumerate(records)
                                 if record.get("type") == "sample" and record.get("phase") == "transition_requested"), (-1, {}))
    if not (source_index < producer_index < use_index < effect_index) or \
            any(effect_index >= item[0] for item in arrivals.values()):
        issues.append("native_strip_observation_order_invalid")
    if result.get("native_strip_observed") is not True or result.get("transformed_state_equal") is not True:
        issues.append("native_strip_result_not_verified")
    source_players = {player.get("probe"): player for player in source.get("players", [])}
    comparisons = []
    for probe, (_, _, player) in arrivals.items():
        before, after = source_players.get(probe, {}).get("state") or {}, player.get("state") or {}
        unchanged = all(number(before.get(field)) and number(after.get(field)) and
                        abs(before[field] - after[field]) <= 0.01 for field in ("health", "armor")) and \
            all(type(before.get(field)) is bool and type(after.get(field)) is bool and
                before[field] == after[field] for field in ("suit", "long_jump")) and \
            type(before.get("flash")) is int and type(after.get("flash")) is int and before["flash"] == after["flash"]
        transformed = bool(isinstance(before.get("weapons"), list) and before["weapons"] and
                           isinstance(before.get("ammo"), list) and before["ammo"] and
                           after.get("weapons") == [] and after.get("ammo") == [] and unchanged)
        if not transformed or player.get("state_equal") is not False or player.get("state_validation_mode") != mode or \
                player.get("native_strip_observed") is not True or player.get("transformed_state_equal") is not True:
            issues.append(f"native_strip_transformed_state_mismatch:{probe}")
        else:
            probes.append(probe)
        comparisons.append({"probe": probe, "sourceActualState": before, "firstActualState": after,
                            "personalFieldsPreserved": unchanged, "weaponsAndAmmoStripped": transformed})
    evidence["nativeEvents"] = public_fields([item[1] for item in proof.values()])
    evidence["comparisons"] = comparisons
    evidence["verified"] = not issues and len(probes) == 2
    return evidence


def position_evidence(samples, mode, landmark_verified, state=None):
    def vector(value):
        return isinstance(value, list) and len(value) == 3 and all(
            isinstance(number, (int, float)) and math.isfinite(number) for number in value)

    issues, arrivals = [], []
    for probe, prefix in ((0, "alpha"), (1, "beta")):
        first = next(((sample, player) for sample in samples
                      if sample.get("phase") in (prefix + "_first_ready", prefix + "_post_restore")
                      for player in sample.get("players", [])
                      if player.get("probe") == probe and player.get("first_ready_observed")), None)
        if first is None:
            issues.append(f"missing_first_arrival:{probe}")
            continue
        sample, player = first
        origin, expected = player.get("origin"), player.get("expected_origin")
        reference = player.get("map_spawn_reference")
        valid_state = player.get("state_equal") is True or bool(state and state["verified"] and probe in state["verifiedProbes"])
        if not (player.get("identity_matches") and player.get("alive") and not player.get("pending", True) and
                player.get("clear_hull") and valid_state and player.get("position_ok") and vector(origin)):
            issues.append(f"invalid_first_arrival:{probe}")
        target = reference if mode == "map_spawn_fallback" else expected
        if not vector(target) or not vector(origin) or any(
                abs(origin[axis] - target[axis]) > limit + 0.01 for axis, limit in enumerate((192, 192, 96))):
            issues.append(f"arrival_reference_proximity:{probe}")
        if mode == "map_spawn_fallback":
            reference_class, reference_edict = player.get("map_spawn_reference_class"), player.get("map_spawn_reference_edict")
            valid_reference = (reference_class == "coop_level_custom" and reference_edict == -1) or \
                              (reference_class in ("info_player_coop", "info_player_start", "info_player_deathmatch") and
                               isinstance(reference_edict, int) and reference_edict > 0)
            if expected is not None or player.get("first_point_contents_clear") is not True or not valid_reference:
                issues.append(f"invalid_map_spawn_reference:{probe}")
        arrivals.append({"probe": probe, "phase": sample.get("phase"), "origin": origin,
                         "expectedOrigin": expected, "mapSpawnReference": reference,
                         "mapSpawnReferenceClass": player.get("map_spawn_reference_class"),
                         "mapSpawnReferenceEdict": player.get("map_spawn_reference_edict"),
                         "firstPointContentsClear": player.get("first_point_contents_clear")})
    group_distance = None
    if len(arrivals) == 2 and all(vector(arrival["origin"]) for arrival in arrivals):
        group_distance = math.sqrt(sum((arrivals[0]["origin"][axis] - arrivals[1]["origin"][axis]) ** 2 for axis in range(3)))
    if mode == "map_spawn_fallback":
        if landmark_verified is not False:
            issues.append("fallback_cannot_verify_landmark")
        if group_distance is None or group_distance > 256.01:
            issues.append("fallback_group_distance")
    elif mode != "landmark_relative" or landmark_verified is not True:
        issues.append("landmark_not_verified")
    return {"mode": mode, "verified": not issues, "landmarkVerified": landmark_verified is True,
            "groupFirstArrivalDistance": group_distance, "firstArrivals": arrivals, "issues": issues}


def source_scene_evidence(edge, setup, samples, events, result, declared_modes, far_mode):
    setup = setup or {}
    activation = setup.get("activation")
    mode = activation if activation in SOURCE_SETUP_MODES[1:] else "point_setup"
    issues = []
    expected = "point_setup"
    if not far_mode and "native_train_ride" in declared_modes and (edge["sourceMap"], edge["destinationMap"]) in INTRO_TRAIN_EXITS:
        expected = "native_train_ride"
    elif "native_ambush" in declared_modes and edge["sourceMap"] == "c2a3d" and \
            edge["destinationMap"] == "c2a3e" and edge["targetname"] == "gotodrag" and edge["landmark"] == "c2a3de" and \
            (not far_mode or activation == "native_ambush"):
        expected = "native_ambush"
    elif not far_mode and "point_staged_native_xen_gate_portal" in declared_modes and edge["edgeId"] == 208 and \
            (edge["sourceMap"], edge["destinationMap"], edge["targetname"], edge["landmark"]) == \
            ("c4a1a", "c4a1b", "change", "c4a1b"):
        expected = "point_staged_native_xen_gate_portal"
    if mode not in declared_modes or mode != expected:
        issues.append("source_setup_mode_mismatch")
    evidence = {"mode": mode, "expectedMode": expected, "verified": False, "issues": issues,
                "sourcePointStaging": setup.get("source_point_staging", setup.get("point_staging", setup.get("point_teleport"))),
                "events": public_fields(events), "fullSourceWalkthroughChecked": False}
    if mode == "point_setup":
        evidence["scope"] = "prepared_source_point_original_exit_callback"
        evidence["verified"] = not issues
        return evidence

    def number(value):
        return type(value) in (int, float) and math.isfinite(value)

    def vector(value):
        return isinstance(value, list) and len(value) == 3 and all(number(item) for item in value)

    def floor_hits(train):
        hits = train.get("passenger_floor_hits") if isinstance(train, dict) else None
        return isinstance(hits, list) and len(hits) == 2 and all(hit is True for hit in hits)

    def train_model_valid(train, map_name):
        return isinstance(train, dict) and train.get("model") == INTRO_TRAIN_MODELS.get(map_name) and \
            type(train.get("model_index")) is int and train["model_index"] > 0 and \
            train["model_index"] == train.get("expected_model_index")

    def train_path_valid(train):
        return isinstance(train, dict) and train.get("ppath_present") is True and train.get("ppath_live") is True and \
            type(train.get("ppath_edict")) is int and \
            train["ppath_edict"] > 0 and isinstance(train.get("ppath_targetname"), str) and bool(train["ppath_targetname"])

    callback_samples = [sample for sample in samples if sample.get("phase") == "transition_requested" and
                        sample.get("observed_map") == edge["sourceMap"]]
    if not result.get("transition_requested") or not callback_samples:
        issues.append("native_exit_callback_not_observed")
    if setup.get("godmode") is not False or setup.get("noclip") is not False:
        issues.append("native_scene_physics_override")
    if mode == "native_train_ride":
        evidence["scope"] = "native_intro_train_motion_passengers_and_original_exit"
        if far_mode:
            issues.append("native_train_far_mode_not_supported")
        exits = [event for event in events if event.get("type") == "native_train_exit"]
        if len(exits) != 1:
            issues.append("native_train_exit_event_count")
        native_exit = exits[0] if exits else {}
        if native_exit.get("activation") != mode or type(native_exit.get("source_activator_probe")) is not int or \
                native_exit.get("source_activator_probe") not in (0, 1) or \
                native_exit.get("point_teleport") is not False or native_exit.get("passengers_on_train") is not True or \
                native_exit.get("train_moved") is not True:
            issues.append("native_train_exit_invalid")
        if setup.get("point_teleport") is not False or setup.get("train_reposition") is not False or \
                any(sample.get("phase") == "source_parked" for sample in samples):
            issues.append("native_train_source_repositioned")
        start_train, exit_train = setup.get("train"), native_exit.get("train")
        arrival_pair = next((sample for sample in samples if sample.get("phase") == "arrival_pair_observed" and
                             sample.get("observed_map") == edge["destinationMap"]), {})
        for label, train, map_name in (("setup", start_train, edge["sourceMap"]),
                                       ("exit", exit_train, edge["sourceMap"])):
            if not train_model_valid(train, map_name) or not train_path_valid(train):
                issues.append(f"native_train_model_or_path_invalid:{label}")
            if not floor_hits(train):
                issues.append(f"native_train_passenger_floor_missing:{label}")
        arrival_train = arrival_pair.get("train")
        if not train_model_valid(arrival_train, edge["destinationMap"]):
            issues.append("native_train_model_invalid:arrival")
        if not floor_hits(arrival_train):
            issues.append("native_train_passenger_floor_missing:arrival")
        arrival_time = arrival_pair.get("game_time")
        arrival_index = samples.index(arrival_pair) if arrival_pair else len(samples)
        destination_samples = [sample for sample in samples[arrival_index:] if sample.get("observed_map") == edge["destinationMap"] and
                               number(sample.get("game_time")) and number(arrival_time) and sample["game_time"] >= arrival_time]
        arrival_edict = (arrival_train or {}).get("edict")
        if type(arrival_edict) is not int or arrival_edict < 1:
            issues.append("native_train_arrival_edict_invalid")
        for sample in destination_samples:
            train = sample.get("train") or {}
            if train.get("edict") != arrival_edict or not train_model_valid(train, edge["destinationMap"]):
                issues.append("native_train_arrival_identity_changed")
            if not floor_hits(train):
                issues.append("native_train_arrival_passenger_floor_lost")
        initially_ready = train_path_valid(arrival_train)
        path_ready = next((sample for sample in destination_samples if train_path_valid(sample.get("train")) and
                           sample["game_time"] - arrival_time <= 1.001), None)
        # OverrideReset назначает NearestPath асинхронно, последующее восстановление должно быть наблюдаемо
        if path_ready is None:
            issues.append("native_train_arrival_path_not_ready_within1s")
        end_sample = next((sample for sample in reversed(destination_samples) if sample.get("phase") == "end"), {})
        end_train = end_sample.get("train") or {}
        if not train_path_valid(end_train):
            issues.append("native_train_end_path_not_live")
        arrival_origin, end_origin = (arrival_train or {}).get("origin"), end_train.get("origin")
        arrival_movement = math.dist(arrival_origin, end_origin) if vector(arrival_origin) and vector(end_origin) else None
        terminal_origin = [-1125, -614, -254]
        terminal_stop = bool(edge["destinationMap"] == "c0a0e" and destination_samples and
                             (arrival_train or {}).get("ppath_targetname") == "lower109e" and
                             end_train.get("ppath_targetname") == "lower110")
        if terminal_stop:
            for sample in destination_samples:
                train = sample.get("train") or {}
                origin, velocity, speed = train.get("origin"), train.get("velocity"), train.get("speed")
                if not vector(origin) or any(abs(origin[axis] - terminal_origin[axis]) > 1 for axis in range(3)) or \
                        not vector(velocity) or math.dist(velocity, [0, 0, 0]) > 0.01 or not number(speed) or abs(speed) > 0.01:
                    terminal_stop = False
                    break
        if (arrival_movement is None or arrival_movement < 1) and not terminal_stop:
            issues.append("native_train_arrival_motion_below1")
        start_origin = (start_train or {}).get("origin")
        exit_origin = (exit_train or {}).get("origin")
        movement = math.dist(start_origin, exit_origin) if vector(start_origin) and vector(exit_origin) else None
        if movement is None or movement < 64:
            issues.append("native_train_motion_below64")
        if native_exit.get("setup_train_origin") != start_origin:
            issues.append("native_train_setup_origin_mismatch")
        for probe, prefix in ((0, "alpha"), (1, "beta")):
            first = next((player for sample in samples
                          if sample.get("phase") in (prefix + "_first_ready", prefix + "_post_restore")
                          for player in sample.get("players", [])
                          if player.get("probe") == probe and player.get("first_ready_observed")), {})
            if first.get("first_spawn_on_intro_train") is not True:
                issues.append(f"native_train_first_arrival_floor_missing:{probe}")
        evidence["trainMovementDistance"] = movement
        evidence["nativeCallbackObserved"] = bool(exits and callback_samples)
        evidence["arrivalPathInitiallyReady"] = initially_ready
        evidence["pathReadyAfterSeconds"] = round(path_ready["game_time"] - arrival_time, 4) if path_ready else None
        evidence["arrivalTrainMovementDistance"] = arrival_movement
        evidence["terminalStopVerified"] = terminal_stop
        if terminal_stop:
            evidence["terminalStopExpectedOrigin"] = terminal_origin
        evidence["arrivalTrain"] = public_fields(arrival_train)
        evidence["arrivalPathReadyTrain"] = public_fields(path_ready.get("train")) if path_ready else None
        evidence["arrivalTrainEnd"] = public_fields(end_train)
    elif mode == "native_ambush":
        evidence["scope"] = "point_staged_entry_native_ambush_teleport_strip_delay_and_exit"
        exits = [event for event in events if event.get("type") == "native_ambush_exit"]
        if len(exits) != 1:
            issues.append("native_ambush_exit_event_count")
        native_exit = exits[0] if exits else {}
        elapsed = native_exit.get("scene_elapsed_seconds")
        exit_time, setup_time = native_exit.get("game_time"), setup.get("game_time")
        observed_delay = exit_time - setup_time if number(exit_time) and number(setup_time) else None
        if not number(elapsed) or elapsed < 11.5 or not number(observed_delay) or observed_delay < 11.5 or \
                (number(elapsed) and abs(elapsed - observed_delay) > 0.01) or native_exit.get("native_delay_observed") is not True:
            issues.append("native_ambush_delay_not_observed")
        if setup.get("gotodrag_direct_use") is not False or native_exit.get("gotodrag_direct_use") is not False:
            issues.append("native_ambush_direct_gotodrag_use")
        if setup.get("source_scene") != "trigger_once_ambush_mm" or setup.get("scene_entry_model") != "*100" or \
                type(setup.get("scene_entry_edict")) is not int or setup["scene_entry_edict"] < 1 or \
                setup.get("point_staging") is not True or setup.get("point_teleport") is not True or \
                setup.get("native_delay_seconds") != 12 or native_exit.get("source_activator_probe") != 0:
            issues.append("native_ambush_entry_evidence_invalid")
        evidence["sceneElapsedSeconds"] = elapsed
        evidence["observedDelaySeconds"] = observed_delay
        evidence["nativeCallbackObserved"] = bool(exits and callback_samples)
        evidence["gotodragDirectUse"] = native_exit.get("gotodrag_direct_use")
        if far_mode:
            evidence["scope"] = "point_staged_alpha_native_ambush_with_beta_outside_scene"
            if setup.get("beta_point_staging") is not False:
                issues.append("native_ambush_far_beta_staged")
            source_sample = next((sample for sample in samples if sample.get("phase") == "before_native_ambush"), {})
            source_players = {player.get("probe"): player for player in source_sample.get("players", [])}
            setup_origins = [source_players.get(probe, {}).get("origin") for probe in (0, 1)]
            exit_origins = native_exit.get("source_origins")
            separations = {}
            for label, record, origins in (("setup", setup, setup_origins), ("exit", native_exit, exit_origins)):
                if record.get("far_teammate") is not True or record.get("scene_participants") != 1 or \
                        record.get("beta_outside_entry") is not True or record.get("beta_outside_teleport") is not True or \
                        record.get("beta_point_staging") is not False or not vector(record.get("beta_start_origin")):
                    issues.append(f"native_ambush_far_scope_invalid:{label}")
                separation = math.dist(*origins) if isinstance(origins, list) and len(origins) == 2 and \
                    all(vector(origin) for origin in origins) else None
                reported = record.get("source_separation")
                if separation is None or separation <= 256 or not number(reported) or reported <= 256 or \
                        (number(separation) and number(reported) and abs(separation - reported) > 0.01):
                    issues.append(f"native_ambush_far_separation_invalid:{label}")
                separations[label] = separation
            if setup.get("beta_start_origin") != native_exit.get("beta_start_origin") or \
                    setup.get("beta_start_origin") != setup_origins[1]:
                issues.append("native_ambush_far_beta_start_mismatch")
            if native_exit.get("source_scene_verified") is not True:
                issues.append("native_ambush_far_source_scene_not_verified")
            callback_players = {player.get("probe"): player for sample in callback_samples
                                for player in sample.get("players", [])}
            if not isinstance(exit_origins, list) or len(exit_origins) != 2 or any(
                    callback_players.get(probe, {}).get("origin") != exit_origins[probe] for probe in (0, 1)):
                issues.append("native_ambush_far_exit_origin_mismatch")
            beta_callback = callback_players.get(1, {})
            if beta_callback.get("personal_arrival") is not False or \
                    not number(beta_callback.get("source_distance_from_leader")) or beta_callback["source_distance_from_leader"] <= 256:
                issues.append("native_ambush_far_group_arrival_not_observed")
            evidence["farTeammate"] = True
            evidence["sourceSeparations"] = separations
            evidence["betaStartOrigin"] = setup.get("beta_start_origin")
    elif mode == "point_staged_native_xen_gate_portal":
        evidence["scope"] = "two_source_point_stages_native_xen_gate_and_portal_callbacks"
        evidence["continuousTraversalChecked"] = False
        if far_mode:
            issues.append("native_xen_far_mode_not_supported")
        selected = {}
        for kind in ("native_xen_gate_disabled", "native_xen_portal_stage", "native_xen_portal_exit"):
            found = [event for event in events if event.get("type") == kind]
            if len(found) != 1:
                issues.append("native_xen_event_count:" + kind)
            selected[kind] = found[0] if found else {}
        touches = {}
        for stage in ("gate", "portal"):
            found = [event for event in events if event.get("type") == "native_xen_trigger_touch" and event.get("stage") == stage]
            if len(found) != 1:
                issues.append("native_xen_touch_count:" + stage)
            touch = found[0] if found else {}
            if touch.get("manual_touch") is not False or touch.get("point_teleport_at_callback") is not False or \
                    touch.get("source_activator_probe") not in (0, 1) or touch.get("trigger_edict") != setup.get(stage + "_touch_edict"):
                issues.append("native_xen_touch_invalid:" + stage)
            touches[stage] = touch
        gate, portal, native_exit = (selected[kind] for kind in
                                     ("native_xen_gate_disabled", "native_xen_portal_stage", "native_xen_portal_exit"))
        if setup.get("stage") != "gate" or setup.get("point_teleport") is not True or \
                setup.get("source_point_staging") is not True or setup.get("continuous_traversal_checked") is not False or \
                setup.get("native_delay_seconds") != 4 or any(setup.get(field) is not False for field in
                    ("direct_exit_use", "manual_gate_touch", "manual_portal_touch", "manual_world_toggle", "health_repair_after_staging")) or \
                any(setup.get(field) != value for field, value in
                    (("gate_bsp_entity", 336), ("gate_model", "*98"), ("portal_bsp_entity", 354),
                     ("portal_model", "*101"), ("hurt_bsp_entity", 350), ("hurt_model", "*99"),
                     ("hurt_solid", 1), ("hurt_damage", 10000), ("blocker_solid", 0))):
            issues.append("native_xen_setup_invalid")
        for label, record in (("gate", gate), ("portal", portal), ("exit", native_exit)):
            if record.get("gate_native_touch") is not True or record.get("hurt_off") is not True or record.get("hurt_solid") != 0 or \
                    record.get("hurt_damage") != 10000 or record.get("hurt_edict") != setup.get("hurt_edict") or \
                    record.get("blocker_edict") != setup.get("blocker_edict") or record.get("manual_world_toggle") is not False:
                issues.append("native_xen_gate_effect_invalid:" + label)
        if gate.get("manual_gate_touch") is not False or gate.get("blocker_on") is not False or gate.get("blocker_solid") != 0:
            issues.append("native_xen_gate_stage_invalid")
        if portal.get("point_teleport") is not True or portal.get("source_point_staging") is not True or \
                portal.get("continuous_traversal_checked") is not False or portal.get("clear_hulls") is not True or \
                portal.get("both_in_transition_volume") is not True or any(portal.get(field) is not False for field in
                    ("manual_portal_touch", "direct_exit_use", "health_repair_after_staging")):
            issues.append("native_xen_portal_stage_invalid")
        elapsed = native_exit.get("portal_elapsed_seconds")
        touch_time, exit_time = touches["portal"].get("game_time"), native_exit.get("game_time")
        observed = exit_time - touch_time if number(exit_time) and number(touch_time) else None
        if not number(elapsed) or elapsed < 3.95 or not number(observed) or observed < 3.95 or \
                (number(elapsed) and number(observed) and abs(elapsed - observed) > 0.01) or \
                any(native_exit.get(field) is not True for field in
                    ("source_scene_verified", "native_delay_observed", "portal_native_touch", "blocker_on", "source_states_logged_in_transition_requested")) or \
                native_exit.get("blocker_solid") != 4 or any(native_exit.get(field) is not False for field in
                    ("direct_exit_use", "health_repair_after_staging")):
            issues.append("native_xen_portal_exit_invalid")
        timeline = [setup.get("game_time"), touches["gate"].get("game_time"), gate.get("game_time"),
                    portal.get("game_time"), touch_time, exit_time]
        if not all(number(time) for time in timeline) or timeline != sorted(timeline):
            issues.append("native_xen_scene_order_invalid")
        callback_players = {player.get("probe"): player for sample in callback_samples for player in sample.get("players", [])}
        origins = native_exit.get("source_origins")
        if not isinstance(origins, list) or len(origins) != 2 or any(
                not vector(origins[probe]) or callback_players.get(probe, {}).get("origin") != origins[probe] or
                callback_players.get(probe, {}).get("alive") is not True or callback_players.get(probe, {}).get("pending") is not False or
                callback_players.get(probe, {}).get("clear_hull") is not True for probe in (0, 1)):
            issues.append("native_xen_source_state_evidence_invalid")
        evidence["nativeCallbackObserved"] = bool(native_exit and callback_samples)
        evidence["portalElapsedSeconds"] = elapsed
        evidence["observedPortalDelaySeconds"] = observed
    evidence["verified"] = not issues
    return evidence


def load_catalog(json_path, tsv_path):
    catalog = json.loads(json_path.read_text(encoding="utf-8-sig"))
    edges = catalog["edges"]
    if not edges or [edge.get("edgeId") for edge in edges] != list(range(len(edges))):
        raise ValueError("Каталог должен содержать последовательные edgeId от нуля")
    rows = [line.split("\t") for line in tsv_path.read_text(encoding="utf-8-sig").splitlines()
            if line.strip() and not line.startswith("#")]
    if len(rows) != len(edges):
        raise ValueError("Число переходов JSON и TSV различается")
    for edge, row in zip(edges, rows):
        if len(row) != 37 or row[0] != str(edge["edgeId"]) or \
                row[1:3] != [edge["sourceMap"], edge["destinationMap"]] or row[3] != str(edge["sourceEntityIndex"]):
            raise ValueError(f"JSON и TSV не согласованы для edgeId {edge['edgeId']}")
    return catalog


def load_part(path, metadata_path, run_id, catalog_sha256, edges):
    metadata = json.loads(metadata_path.read_text(encoding="utf-8-sig"))
    if metadata.get("project") != "half-life_coop/transition-check" or metadata.get("format") != 1:
        raise ValueError("Метаданные не принадлежат проверке переходов")
    records, trailing_partial = _helpers.read_records(path)
    selected = _helpers.select_run(records, run_id)
    start = selected[0][0]
    identity_mode = start.get("identity_mode", "engine_fake_clients_unadapted")
    view_mode = start.get("view_message_mode", "engine_setview_unadapted")
    music_mode = start.get("music_message_mode", "engine_cd_commands_unadapted")
    crosshair_mode = start.get("crosshair_message_mode", "engine_crosshair_unadapted")
    position_modes = start.get("position_verification_modes", ["landmark_relative"])
    source_setup_modes = start.get("source_setup_modes", ["point_setup"])
    state_validation_modes = start.get("state_validation_modes", ["exact_transfer"])
    if not isinstance(position_modes, list) or not position_modes or any(mode not in POSITION_MODES for mode in position_modes):
        raise ValueError("Неизвестные режимы проверки позиции в начальной записи")
    if not isinstance(source_setup_modes, list) or not source_setup_modes or \
            any(mode not in SOURCE_SETUP_MODES for mode in source_setup_modes):
        raise ValueError("Неизвестные режимы подготовки исходной сцены в начальной записи")
    if not isinstance(state_validation_modes, list) or not state_validation_modes or \
            any(mode not in STATE_VALIDATION_MODES for mode in state_validation_modes):
        raise ValueError("Неизвестные режимы сравнения состояния в начальной записи")
    if metadata.get("runId") != start.get("run"):
        raise ValueError("runId метаданных не совпадает с выбранным прогоном")
    if start.get("fake_clients") != 2 or bool(start.get("far_teammate")) != bool(metadata.get("farMode", 0)):
        raise ValueError("Число клиентов или дальний режим не совпадают с метаданными")
    if metadata.get("catalogSha256", "").lower() != catalog_sha256:
        raise ValueError("TSV отличается от каталога, использованного сервером")
    first, count = metadata["firstIndex"], metadata["count"]
    if not isinstance(first, int) or not isinstance(count, int) or first < 0 or count < 1 or first + count > len(edges):
        raise ValueError("Некорректный диапазон метаданных")
    for field, value in (("first_index", first), ("total", count),
                         ("dwell_seconds", metadata["dwellSeconds"]), ("far_mode", metadata.get("farMode", 0))):
        if field in start and start[field] != value:
            raise ValueError(f"Начальная запись не совпадает с метаданными: {field}")
    dll_sha = metadata.get("serverDllSha256", "").lower()
    if not re.fullmatch(r"[0-9a-f]{64}", dll_sha):
        raise ValueError("В метаданных отсутствует SHA256 серверной DLL")
    dll_path = path.parent / "dlls/hl.dll"
    if dll_path.is_file() and _helpers.file_metadata(dll_path)["sha256"] != dll_sha:
        raise ValueError("Проверочная DLL заменена после записи метаданных")

    samples = {}
    raw_samples = {}
    setups = {}
    scene_events = {}
    edge_records = {}
    view_events = {}
    all_view_events = []
    music_events = {}
    all_music_events = []
    crosshair_events = {}
    all_crosshair_events = []
    results = {}
    issues = []
    for record, _ in selected:
        edge_id = record.get("edge_id")
        if edge_id is None:
            continue
        if not isinstance(edge_id, int) or edge_id < first or edge_id >= first + count:
            issues.append(f"edge_outside_range:{edge_id}")
            continue
        edge = edges[edge_id]
        position_mode = "map_spawn_fallback" if edge.get("destinationLandmarkOrigin") is None else "landmark_relative"
        if record.get("position_verification_mode", "landmark_relative") != position_mode or position_mode not in position_modes:
            issues.append(f"position_verification_mode_mismatch:{edge_id}")
        for field, expected in (("source_map", edge["sourceMap"]), ("destination_map", edge["destinationMap"])):
            if field in record and record[field] != expected:
                issues.append(f"{field}_mismatch:{edge_id}")
        kind = record.get("type")
        if kind == "edge_queued":
            edge_records[edge_id] = []
        edge_records.setdefault(edge_id, []).append(record)
        if kind == "edge_queued":
            samples[edge_id] = []
            raw_samples[edge_id] = []
            view_events[edge_id] = []
            music_events[edge_id] = []
            crosshair_events[edge_id] = []
            scene_events[edge_id] = []
            setups.pop(edge_id, None)
        elif kind == "setup":
            setups[edge_id] = public_fields(record)
        elif kind == "sample":
            samples.setdefault(edge_id, []).append(public_fields(record))
            raw_samples.setdefault(edge_id, []).append(record)
        elif kind in ("native_train_exit", "native_train_dwell_extension", "native_ambush_exit", "native_xen_trigger_touch", "native_xen_gate_disabled",
                      "native_xen_portal_stage", "native_xen_portal_exit"):
            scene_events.setdefault(edge_id, []).append(public_fields(record))
        elif kind == "set_view_suppressed":
            event = public_fields(record)
            view_events.setdefault(edge_id, []).append(event)
            all_view_events.append(event)
            if view_mode != SUPPRESSED_VIEW or record.get("camera_protocol_checked") is not False or \
                    record.get("probe") not in (0, 1) or record.get("call_count") != len(all_view_events):
                issues.append(f"unexpected_set_view_suppression:{edge_id}")
        elif kind == "music_command_suppressed":
            event = public_fields(record)
            music_events.setdefault(edge_id, []).append(event)
            all_music_events.append(event)
            command, track = record.get("command"), record.get("track")
            valid_command = (command == "stop" and track == -1) or \
                            (command == "play" and isinstance(track, int) and track >= 0)
            if music_mode != SUPPRESSED_MUSIC or record.get("music_protocol_checked") is not False or \
                    record.get("probe") not in (0, 1) or record.get("call_count") != len(all_music_events) or not valid_command:
                issues.append(f"unexpected_music_suppression:{edge_id}")
        elif kind == "crosshair_angle_suppressed":
            event = public_fields(record)
            crosshair_events.setdefault(edge_id, []).append(event)
            all_crosshair_events.append(event)
            probe_count = sum(item.get("probe") == record.get("probe") for item in crosshair_events[edge_id])
            if crosshair_mode != SUPPRESSED_CROSSHAIR or record.get("crosshair_protocol_checked") is not False or \
                    record.get("probe") not in (0, 1) or record.get("call_count") != len(all_crosshair_events) or \
                    record.get("probe_call_count") != probe_count:
                issues.append(f"unexpected_crosshair_suppression:{edge_id}")
        elif kind == "edge_result":
            status = record.get("status")
            if status not in STATES[:-1]:
                issues.append(f"invalid_status:{edge_id}")
            if status == "passed" and record.get("observed_map") != edge["destinationMap"]:
                issues.append(f"observed_map_mismatch:{edge_id}")
            if status == "passed":
                if not record.get("transition_requested"):
                    issues.append(f"missing_original_transition_request:{edge_id}")
                if record.get("stability_seconds", 0) + 0.001 < metadata["dwellSeconds"]:
                    issues.append(f"insufficient_destination_dwell:{edge_id}")
            state = state_evidence(edge, edge_records.get(edge_id, []), record, state_validation_modes)
            if status == "passed" and not state["verified"]:
                issues.append(f"invalid_state_evidence:{edge_id}")
            position = position_evidence(raw_samples.get(edge_id, []), position_mode, record.get("landmark_verified"), state)
            if status == "passed" and not position["verified"]:
                issues.append(f"invalid_position_evidence:{edge_id}")
            identity = identity_evidence(raw_samples.get(edge_id, []), identity_mode)
            if status == "passed" and identity["unexpectedIdentityIssues"]:
                issues.append(f"unexpected_identity_in_passed_edge:{edge_id}")
            scene = source_scene_evidence(edge, setups.get(edge_id), raw_samples.get(edge_id, []),
                                          scene_events.get(edge_id, []), record, source_setup_modes, metadata.get("farMode", 0))
            if status == "passed" and not scene["verified"]:
                issues.append(f"invalid_source_scene_evidence:{edge_id}")
            views = list(view_events.get(edge_id, []))
            suppressed_calls = record.get("set_view_suppressed_calls", 0)
            if view_mode == SUPPRESSED_VIEW and (not isinstance(suppressed_calls, int) or not 0 <= suppressed_calls <= len(views)):
                issues.append(f"set_view_evidence_mismatch:{edge_id}")
            music = list(music_events.get(edge_id, []))
            play_calls, stop_calls = (record.get(f"music_{command}_suppressed_calls", 0) for command in ("play", "stop"))
            if music_mode == SUPPRESSED_MUSIC:
                for command, reported in (("play", play_calls), ("stop", stop_calls)):
                    if not isinstance(reported, int) or not 0 <= reported <= sum(event.get("command") == command for event in music):
                        issues.append(f"music_{command}_evidence_mismatch:{edge_id}")
            crosshairs = list(crosshair_events.get(edge_id, []))
            crosshair_calls = record.get("crosshair_suppressed_calls", 0)
            if crosshair_mode == SUPPRESSED_CROSSHAIR and (type(crosshair_calls) is not int or
                    not 0 <= crosshair_calls <= len(crosshairs) or record.get("crosshair_protocol_checked") is not False):
                issues.append(f"crosshair_evidence_mismatch:{edge_id}")
            results[edge_id] = {"result": public_fields(record), "samples": list(samples.get(edge_id, [])), "identity": identity,
                                "position": position,
                                "state": state,
                                "sourceScene": scene,
                                "setup": setups.get(edge_id),
                                "crosshair": {"mode": crosshair_mode, "crosshairProtocolChecked": False,
                                              "reportedBeforeCleanupCalls": crosshair_calls, "events": crosshairs},
                                "music": {"mode": music_mode, "musicProtocolChecked": False,
                                          "reportedBeforeCleanupPlayCalls": play_calls,
                                          "reportedBeforeCleanupStopCalls": stop_calls, "events": music},
                                "view": {"mode": view_mode, "cameraProtocolChecked": False,
                                         "reportedBeforeCleanupCalls": suppressed_calls, "events": views}}
    for edge_id, item in results.items():
        views = list(view_events.get(edge_id, []))
        item["view"]["events"] = views
        item["view"]["setViewSuppressedCalls"] = len(views)
        music = list(music_events.get(edge_id, []))
        item["music"]["events"] = music
        for command in ("play", "stop"):
            item["music"][f"{command}SuppressedCalls"] = sum(event.get("command") == command for event in music)
        crosshairs = list(crosshair_events.get(edge_id, []))
        item["crosshair"]["events"] = crosshairs
        item["crosshair"]["suppressedCalls"] = len(crosshairs)
    terminal = next((record for record, _ in reversed(selected) if record.get("type") == "summary"), None)
    counts = Counter(item["result"].get("status") for item in results.values())
    if terminal:
        expected = {"total": count, "checked": len(results), "passed": counts["passed"],
                    "failed": counts["transition_failed"], "setup_not_reached": counts["setup_not_reached"],
                    "protocol_limitations": counts["protocol_limitation"]}
        for key, value in expected.items():
            if terminal.get(key) != value:
                issues.append(f"summary_{key}_mismatch")
        if view_mode == SUPPRESSED_VIEW and terminal.get("set_view_suppressed_calls") != len(all_view_events):
            issues.append("summary_set_view_suppressed_calls_mismatch")
        if music_mode == SUPPRESSED_MUSIC:
            for command in ("play", "stop"):
                if terminal.get(f"music_{command}_suppressed_calls") != sum(event.get("command") == command for event in all_music_events):
                    issues.append(f"summary_music_{command}_suppressed_calls_mismatch")
        if crosshair_mode == SUPPRESSED_CROSSHAIR and (terminal.get("crosshair_suppressed_calls") != len(all_crosshair_events) or
                terminal.get("crosshair_protocol_checked") is not False):
            issues.append("summary_crosshair_suppressed_calls_mismatch")
    complete = bool(terminal and terminal.get("status") == "complete" and len(results) == count and not issues)
    if identity_mode not in (SYNTHETIC_IDENTITY, "engine_fake_clients_unadapted") or \
            start.get("network_signon_checked") is not False or start.get("steam_auth_checked") is not False:
        issues.append("unsupported_identity_or_network_scope")
        complete = False
    if view_mode not in (SUPPRESSED_VIEW, "engine_setview_unadapted") or \
            (view_mode == SUPPRESSED_VIEW and start.get("camera_protocol_checked") is not False):
        issues.append("unsupported_camera_scope")
        complete = False
    if music_mode not in (SUPPRESSED_MUSIC, "engine_cd_commands_unadapted") or \
            (music_mode == SUPPRESSED_MUSIC and start.get("music_protocol_checked") is not False):
        issues.append("unsupported_music_scope")
        complete = False
    if crosshair_mode not in (SUPPRESSED_CROSSHAIR, "engine_crosshair_unadapted") or \
            (crosshair_mode == SUPPRESSED_CROSSHAIR and start.get("crosshair_protocol_checked") is not False):
        issues.append("unsupported_crosshair_scope")
        complete = False
    return {"metadata": metadata, "metadataPath": metadata_path, "input": path, "first": first, "count": count,
            "identityMode": identity_mode, "viewMessageMode": view_mode, "musicMessageMode": music_mode,
            "crosshairMessageMode": crosshair_mode,
            "positionModes": sorted(set(position_modes)),
            "sourceSetupModes": sorted(set(source_setup_modes)),
            "stateValidationModes": sorted(set(state_validation_modes)),
            "viewEvents": all_view_events, "musicEvents": all_music_events, "crosshairEvents": all_crosshair_events,
            "start": start, "summary": terminal, "complete": complete, "results": results, "samples": samples,
            "issues": sorted(set(issues)), "trailingPartialLineIgnored": trailing_partial,
            "raw": "".join(raw for _, raw in selected)}


def write_csv(path, rows):
    fields = ("edgeId", "sourceMap", "destinationMap", "sourceEntityIndex", "model", "landmark", "activation",
              "status", "reason", "observedMap", "positionVerificationMode", "landmarkVerified", "warnings",
              "sourceSetupMode", "sourceScene", "stateValidationMode", "state", "identity", "position", "view", "music", "crosshair", "setup", "samples", "staticWarnings", "result")
    with path.open("w", encoding="utf-8-sig", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=fields)
        writer.writeheader()
        for row in rows:
            record = {key: row[key] for key in fields}
            for key in ("warnings", "sourceScene", "state", "identity", "position", "view", "music", "crosshair", "setup", "samples", "staticWarnings", "result"):
                record[key] = json.dumps(record[key], ensure_ascii=False, separators=(",", ":"))
            writer.writerow(record)


def main():
    parser = argparse.ArgumentParser(description="Отчёт по существующим проверкам переходов с двумя fake clients")
    parser.add_argument("inputs", type=Path, nargs="+", help="Один или несколько transition-check.jsonl")
    parser.add_argument("--run-ids", type=int, nargs="+", help="runId каждого файла в том же порядке")
    parser.add_argument("--metadata", type=Path, nargs="+", help="Метаданные каждого файла, иначе рядом с JSONL")
    parser.add_argument("--catalog", type=Path, default=REPO_ROOT / "artifacts/transition-check/catalog.json")
    parser.add_argument("--tsv", type=Path, default=REPO_ROOT / "artifacts/transition-check/transition-edges.tsv")
    parser.add_argument("--output", type=Path, help="JSON отчёт, по умолчанию report.json либо progress.json")
    args = parser.parse_args()
    if len({path.resolve() for path in args.inputs}) != len(args.inputs):
        raise ValueError("Один входной файл нельзя указывать несколько раз")
    run_ids = args.run_ids or [None] * len(args.inputs)
    metadata_paths = args.metadata or [path.parent / "transition-check-meta.json" for path in args.inputs]
    if len(run_ids) != len(args.inputs) or len(metadata_paths) != len(args.inputs):
        raise ValueError("Число runId и метаданных должно совпадать с числом входных файлов")
    catalog = load_catalog(args.catalog, args.tsv)
    edges = catalog["edges"]
    tsv_sha = _helpers.file_metadata(args.tsv)["sha256"]
    parts = [load_part(path, metadata, run_id, tsv_sha, edges)
             for path, metadata, run_id in zip(args.inputs, metadata_paths, run_ids)]
    parts.sort(key=lambda part: part["first"])
    if len({part["metadata"]["serverDllSha256"].lower() for part in parts}) != 1:
        raise ValueError("Нельзя объединить прогоны разных серверных DLL")
    if len({(part["metadata"]["dwellSeconds"], part["metadata"].get("farMode", 0)) for part in parts}) != 1:
        raise ValueError("Ближнее и дальнее размещение или разные интервалы наблюдения требуют отдельных отчётов")
    if len({part["identityMode"] for part in parts}) != 1:
        raise ValueError("Разные режимы идентификации игроков требуют отдельных отчётов")
    if len({part["viewMessageMode"] for part in parts}) != 1:
        raise ValueError("Разные режимы адаптера камеры требуют отдельных отчётов")
    if len({part["musicMessageMode"] for part in parts}) != 1:
        raise ValueError("Разные режимы адаптера музыки требуют отдельных отчётов")
    if len({part["crosshairMessageMode"] for part in parts}) != 1:
        raise ValueError("Разные режимы адаптера прицела требуют отдельных отчётов")
    if len({tuple(part["positionModes"]) for part in parts}) != 1:
        raise ValueError("Разные режимы проверки позиции требуют отдельных отчётов")
    if len({tuple(part["sourceSetupModes"]) for part in parts}) != 1:
        raise ValueError("Разные режимы подготовки исходных сцен требуют отдельных отчётов")
    if len({tuple(part["stateValidationModes"]) for part in parts}) != 1:
        raise ValueError("Разные режимы сравнения состояния требуют отдельных отчётов")

    assigned = set()
    latest = {}
    live_samples = {}
    issues = []
    for part in parts:
        indices = set(range(part["first"], part["first"] + part["count"]))
        if assigned & indices:
            raise ValueError("Диапазоны входных прогонов пересекаются")
        assigned.update(indices)
        latest.update(part["results"])
        live_samples.update(part["samples"])
        issues.extend(part["issues"])
    rows = []
    for edge in edges:
        edge_id = edge["edgeId"]
        runtime = latest.get(edge_id, {})
        result = runtime.get("result", {})
        rows.append({
            "edgeId": edge_id, "sourceMap": edge["sourceMap"], "destinationMap": edge["destinationMap"],
            "sourceEntityIndex": edge["sourceEntityIndex"], "model": edge["model"], "targetname": edge["targetname"],
            "landmark": edge["landmark"], "activation": edge["activation"],
            "status": result.get("status", "not_checked"), "reason": result.get("reason", "not_checked"),
            "observedMap": result.get("observed_map"), "result": result or None,
            "positionVerificationMode": "map_spawn_fallback" if edge.get("destinationLandmarkOrigin") is None else "landmark_relative",
            "landmarkVerified": result.get("landmark_verified") is True,
            "identity": runtime.get("identity"),
            "position": runtime.get("position"),
            "sourceSetupMode": (runtime.get("sourceScene") or {}).get("mode"),
            "sourceScene": runtime.get("sourceScene"),
            "stateValidationMode": (runtime.get("state") or {}).get("mode"),
            "state": runtime.get("state"),
            "view": runtime.get("view"),
            "music": runtime.get("music"),
            "crosshair": runtime.get("crosshair"),
            "setup": runtime.get("setup"),
            "warnings": [key for key in ("manual_fake_signon", "passive_death_warning", "deliberate_story_frozen",
                                           "arrival_floor_warning", "arrival_far_from_leader") if result.get(key)],
            "samples": runtime.get("samples", live_samples.get(edge_id, [])),
            "staticWarnings": edge.get("flags", []), "staticSetup": edge.get("setup"),
            "expectedArrival": edge.get("expectedArrival"),
        })
        if (runtime.get("setup") or {}).get("airborne_setup"):
            rows[-1]["warnings"].append("airborne_setup")
        if any(sample.get("phase") == "source_parked" for sample in rows[-1]["samples"]):
            rows[-1]["warnings"].append("source_parked")
    counts = {state: sum(row["status"] == state for row in rows) for state in STATES}
    inputs_complete = all(part["complete"] for part in parts)
    complete = bool(inputs_complete and assigned == set(range(len(edges))) and len(latest) == len(edges) and not issues)
    all_passed = complete and counts["passed"] == len(edges)
    first_metadata = parts[0]["metadata"]
    report = {
        "schemaVersion": 8, "generatedAtUtc": datetime.now(timezone.utc).isoformat(),
        "runComplete": complete, "inputRunsComplete": inputs_complete, "allTransitionsPassed": all_passed,
        "catalogEdges": len(edges), "assignedEdges": len(assigned), "resultEdges": len(latest), "counts": counts,
        "edgesWithWarnings": sum(bool(row["warnings"]) for row in rows),
        "manualFakeSignonEdges": sum("manual_fake_signon" in row["warnings"] for row in rows),
        "airborneSetupEdges": sum("airborne_setup" in row["warnings"] for row in rows),
        "setViewSuppressedEdges": sum(bool(row["view"] and row["view"]["setViewSuppressedCalls"]) for row in rows),
        "setViewSuppressedCalls": sum(len(part["viewEvents"]) for part in parts),
        "musicSuppressedEdges": sum(bool(row["music"] and row["music"]["events"]) for row in rows),
        "musicPlaySuppressedCalls": sum(event.get("command") == "play" for part in parts for event in part["musicEvents"]),
        "musicStopSuppressedCalls": sum(event.get("command") == "stop" for part in parts for event in part["musicEvents"]),
        "crosshairSuppressedEdges": sum(bool(row["crosshair"] and row["crosshair"]["events"]) for row in rows),
        "crosshairSuppressedCalls": sum(len(part["crosshairEvents"]) for part in parts),
        "unexpectedIdentityEdges": sum(bool(row["identity"] and row["identity"]["unexpectedIdentityIssues"]) for row in rows),
        "identityVerifiedPasses": sum(row["status"] == "passed" and bool(row["identity"]) and
                                      not row["identity"]["unexpectedIdentityIssues"] for row in rows),
        "landmarkVerifiedPasses": sum(row["status"] == "passed" and row["landmarkVerified"] and
                                      bool(row["position"] and row["position"]["verified"]) for row in rows),
        "mapSpawnFallbackPasses": sum(row["status"] == "passed" and row["positionVerificationMode"] == "map_spawn_fallback" and
                                      bool(row["position"] and row["position"]["verified"]) for row in rows),
        "sourceSetupPasses": {mode: sum(row["status"] == "passed" and row["sourceSetupMode"] == mode and
                                        bool(row["sourceScene"] and row["sourceScene"]["verified"]) for row in rows)
                              for mode in SOURCE_SETUP_MODES},
        "stateValidationPasses": {mode: sum(row["status"] == "passed" and row["stateValidationMode"] == mode and
                                            bool(row["state"] and row["state"]["verified"]) for row in rows)
                                  for mode in STATE_VALIDATION_MODES},
        "consistencyIssues": sorted(set(issues)), "provenanceVerified": True,
        "scope": {"engine": "Official Steam Valve HLDS", "mode": "headless dedicated server",
                  "fakeClients": 2, "realClients": 0, "playerKind": "server_simulated_players",
                  "identityMode": parts[0]["identityMode"],
                  "viewMessageMode": parts[0]["viewMessageMode"], "cameraProtocolChecked": False,
                  "musicMessageMode": parts[0]["musicMessageMode"], "musicProtocolChecked": False,
                  "crosshairMessageMode": parts[0]["crosshairMessageMode"], "crosshairProtocolChecked": False,
                  "positionVerificationModes": parts[0]["positionModes"],
                  "sourceSetupModes": parts[0]["sourceSetupModes"],
                  "stateValidationModes": parts[0]["stateValidationModes"],
                  "sourcePointStagingMayBeUsed": True,
                  "networkSignonChecked": False, "steamAuthChecked": False,
                  "farMode": first_metadata.get("farMode", 0),
                  "destinationDwellSeconds": first_metadata["dwellSeconds"],
                  "destinationDwellIsMinimum": True,
                  "manualFakeSignonMayBeUsed": True,
                  "limits": ["Both players are engine fake clients, not two real Steam players or computers",
                             "synthetic_lan_auth_adapter normalizes BOT to STEAM_ID_LAN only in this checker game DLL's callback view for its two simulated players",
                             "suppressed_for_owned_fake_probes suppresses SET_VIEW only for the checker's two owned fake slots because their headless netchannels have no initialized camera message buffer",
                             "Suppressed SET_VIEW calls are recorded as evidence; camera messages, client view changes and camera rendering are not verified",
                             "The music adapter suppresses only the two known cd play/stop CLIENT_COMMAND sites in SendMusic for the owned probes; no generic message or client-command suppression is used",
                             "Music cache changes remain active, but delivery and playback of the suppressed commands are not verified",
                             "The crosshair adapter suppresses pfnCrosshairAngle only for the two owned fake probes whose netchannels have no initialized message buffer; server autoaim calculations remain active",
                             "Suppressed crosshair-angle calls are recorded, but client crosshair delivery, HUD presentation and crosshair behavior are not verified",
                             "Raw engine auth, real slots and userids remain separate evidence; unexpected changes are not accepted as a successful identity check",
                             "Equipment and point_setup source positions are prepared by the harness; native_ambush stages both probes at the real scene entry",
                             "native_train_ride checks natural intro-train motion and passenger floor contacts without source point teleport or train repositioning; native_ambush runs the authored ambush trigger, teleport, weapon strip and delayed exit after point staging",
                             "Native source-scene evidence supplements the unchanged state, identity, position and destination-dwell requirements; only the selected source sequence is covered",
                             "Destination DwellSeconds is a minimum; native_train_ride may observe up to 30 extra seconds for a temporarily detached path to recover and remain live for one second, without changing physics or the world; end path and passenger-floor requirements remain strict",
                             "native_destination_weaponstrip is limited to the observed c5a1 trigger_auto/DelayedUse/player_weaponstrip chain: raw source and restored states remain unchanged, raw state_equal stays false, and only the independently verified native removal of weapons/ammo is accepted with preserved personal fields",
                             "An airborne source contact can be used without changing physics; source floor support is recorded separately and does not prove a walked route",
                             "map_spawn_fallback is used only without a destination landmark: expected_origin is null and landmark_verified is false; transfer evidence uses real authored/custom map-spawn references and the first-arrival group distance",
                             "The original trigger uses its native Touch or Use callback; destination arrival follows the actual deferred level change",
                             "setup_not_reached means a valid source setup was not reached; it is not a successful transition",
                             "protocol_limitation is separate from a gameplay failure and is not a successful transition",
                             "Manual ClientPutInServer may initialize preserved fake slots; this is not network sign-on evidence",
                             "No full walkthrough, source puzzle prerequisites, all scripted scenes, Steam P2P, real sign-on, reconnect or client rendering were verified"]},
        "inputs": {"catalog": _helpers.file_metadata(args.catalog), "tsv": _helpers.file_metadata(args.tsv)},
        "serverDllSha256": first_metadata["serverDllSha256"].lower(),
        "catalogStaticSummary": catalog.get("summary", {}),
        "runs": [{"runId": part["start"]["run"], "firstIndex": part["first"], "count": part["count"],
                  "complete": part["complete"], "summary": part["summary"],
                  "metadataFile": _helpers.file_metadata(part["metadataPath"]),
                  "metadata": public_fields(part["metadata"]),
                  "runtimeSha256": hashlib.sha256(part["raw"].encode("utf-8")).hexdigest(),
                  "trailingPartialLineIgnored": part["trailingPartialLineIgnored"]} for part in parts],
        "edges": rows,
    }
    output = args.output or REPO_ROOT / "artifacts/transition-check" / ("report.json" if complete else "progress.json")
    if output.suffix.lower() != ".json":
        raise ValueError("Выходной файл должен иметь расширение .json")
    raw_outputs = []
    if inputs_complete:
        for part in parts:
            base = f"run-{part['start']['run']}-edges-{part['first']}-{part['first'] + part['count'] - 1}-far-{first_metadata.get('farMode', 0)}"
            raw_outputs.append((part, output.parent / f"{base}.jsonl", output.parent / f"{base}-meta.json"))
    protected = {path.resolve() for path in [*args.inputs, *metadata_paths, args.catalog, args.tsv]}
    destinations = [output, output.with_suffix(".csv"), *(path for _, raw, meta in raw_outputs for path in (raw, meta))]
    if any(path.resolve() in protected for path in destinations) or len({path.resolve() for path in destinations}) != len(destinations):
        raise ValueError("Выходные файлы пересекаются с входными или друг с другом")
    output.parent.mkdir(parents=True, exist_ok=True)
    for part, raw_path, metadata_path in raw_outputs:
        with raw_path.open("w", encoding="utf-8", newline="") as raw_output:
            raw_output.write(part["raw"])
        metadata_path.write_text(json.dumps(public_fields(part["metadata"]), ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    if raw_outputs:
        report["rawReports"] = [{"runtime": raw.name, "metadata": metadata.name} for _, raw, metadata in raw_outputs]
    write_csv(output.with_suffix(".csv"), rows)
    output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"output": str(output), "runComplete": complete, "allTransitionsPassed": all_passed,
                      "catalogEdges": len(edges), "resultEdges": len(latest), "counts": counts}, ensure_ascii=False))


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"Отчёт переходов: {error}", file=sys.stderr)
        sys.exit(1)
