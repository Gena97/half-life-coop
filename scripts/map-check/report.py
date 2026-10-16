import argparse
from collections import defaultdict
import csv
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import sys


REPO_ROOT = Path(__file__).resolve().parents[2]
STATES = ("passed", "failed", "not_checked")
SAMPLE_KEYS = (
    "phase", "elapsed", "game_time", "player_exists", "health", "armor", "pending", "solid",
    "movetype", "flags", "frozen", "clear_hull", "origin", "velocity", "story_busy",
    "damage_entity_class", "elevator_doors", "entry_actor_exists", "initial_actor_ready",
    "entry_actor_health", "entry_actor_route", "entry_actor_origin", "train_door_exists",
    "train_door_opened", "train_door_state", "train_door_origin", "train_door_target",
    "intro_box_remaining", "entry_space_cleared",
)


def read_records(path):
    with path.open(encoding="utf-8-sig", newline="") as source:
        lines = source.read().splitlines(keepends=True)
    records = []
    trailing_partial = False
    for index, line in enumerate(lines):
        if not line.strip():
            continue
        try:
            record = json.loads(line)
        except json.JSONDecodeError:
            if index == len(lines) - 1 and not line.endswith(("\r", "\n")):
                trailing_partial = True
                break
            raise ValueError(f"Некорректный JSONL в строке {index + 1}") from None
        if not isinstance(record, dict):
            raise ValueError(f"В строке {index + 1} ожидается объект JSON")
        records.append((record, line))
    return records, trailing_partial


def select_run(records, requested_id):
    starts = [index for index, (record, _) in enumerate(records)
              if record.get("type") == "start" and
              (requested_id is None or record.get("run") == requested_id)]
    if not starts:
        raise ValueError("Не найден start выбранного прогона")
    first = starts[-1]
    run_id = records[first][0]["run"]
    last = next((index for index in range(first + 1, len(records))
                 if records[index][0].get("type") == "start"), len(records))
    # Повторные и остановленные запуски не объединяем даже при одинаковом runId
    return [(record, raw) for record, raw in records[first:last] if record.get("run") == run_id]


def sample_fields(sample):
    return {key: sample[key] for key in SAMPLE_KEYS if key in sample}


def ready(sample):
    return bool(sample.get("player_exists") and sample.get("health", 0) > 0 and
                not sample.get("pending", True) and sample.get("solid") == 3)


def file_metadata(path):
    data = path.read_bytes()
    return {"file": path.name, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}


def source_metadata(root):
    digest = hashlib.sha256()
    files = set()
    for directory in ("dlls", "common", "engine", "pm_shared", "game_shared", "public",
                      "projects/vs2019", "scripts/map-check"):
        files.update(path for path in (root / directory).rglob("*")
                     if path.is_file() and path.suffix.lower() in (".cpp", ".h", ".def", ".vcxproj"))
    if not files:
        raise ValueError("В source-root не найдены исходники DLL")
    for path in sorted(files, key=lambda item: item.relative_to(root).as_posix()):
        digest.update(path.relative_to(root).as_posix().encode("utf-8") + b"\0")
        digest.update(hashlib.sha256(path.read_bytes()).digest())
    return {"sha256": digest.hexdigest(), "files": len(files),
            "scope": "Current source files at report generation; not proof of a matching build"}


def build_report(selected, catalog, audit, trailing_partial):
    start = selected[0][0]
    terminal = next((record for record, _ in reversed(selected) if record.get("type") == "summary"), None)
    chapters = [(number, chapter, name) for number, chapter in enumerate(catalog["chapters"], 1)
                for name in chapter["maps"]]
    names = [name for _, _, name in chapters]
    if len(names) != 96 or len(set(names)) != 96:
        raise ValueError("Каталог должен содержать ровно 96 различных карт")
    static_maps = {record["map"]: record for record in audit["maps"]}
    if set(static_maps) != set(names):
        raise ValueError("Карты статического аудита не совпадают с каталогом")
    pending_samples = defaultdict(list)
    latest_results = {}
    phases = {}
    issues = []
    for record, _ in selected:
        name = record.get("map")
        if not name:
            continue
        if name not in names:
            issues.append(f"unknown_map:{name}")
            continue
        if record.get("index") != names.index(name):
            issues.append(f"catalog_index_mismatch:{name}")
        kind = record.get("type")
        if kind == "map_queued":
            pending_samples[name] = []
            phases[name] = "queued"
        elif kind == "map_loaded":
            phases[name] = "loaded"
        elif kind == "sample":
            pending_samples[name].append(record)
            phases[name] = "running"
        elif kind == "map_result":
            latest_results[name] = (record, list(pending_samples[name]))
            phases[name] = "finished"

    rows = []
    for index, (chapter_number, chapter, name) in enumerate(chapters):
        result, samples = latest_results.get(name, ({}, pending_samples[name]))
        initial = next((sample for sample in samples if sample.get("phase") == "spawn"), None)
        warnings = []
        if result.get("passive_death"):
            warnings.append("passive_death_after_spawn")
        if result.get("frozen"):
            warnings.append("frozen_at_end")
        if initial and not ready(initial) and any(ready(sample) for sample in samples):
            warnings.append("initial_spawn_not_ready")
        if result.get("reason") not in (None, "startup_observed", "entry_scene_not_completed",
                                       "spawn_pending_or_missing", "blocked_hull",
                                       "passive_death_after_spawn", "scripted_control_or_frozen"):
            warnings.append(result["reason"])
        if result and result.get("observed_map") != name:
            warnings.append("observed_map_mismatch")
            issues.append(f"observed_map_mismatch:{name}")
        warning_samples = [sample_fields(sample) for sample in samples if
                           sample.get("frozen") or (sample.get("player_exists") and
                           (sample.get("health", 0) <= 0 or sample.get("pending") or
                            (ready(sample) and not sample.get("clear_hull"))))]
        static = static_maps[name]
        starts = static.get("nativeStarts", [])
        row = {
            "index": index, "map": name, "chapter": chapter_number, "chapterId": chapter["id"],
            "chapterTitle": chapter["title"], "phase": phases.get(name, "not_started"),
            "status": result.get("status", "not_checked"), "boot": result.get("boot", "not_checked"),
            "scene": result.get("scene", "not_checked"), "reason": result.get("reason", "not_checked"),
            "observedMap": result.get("observed_map"),
            "dwellSeconds": result.get("map_dwell_seconds", max(start["dwell_seconds"], 45)
                                       if name == "c0a0e" else start["dwell_seconds"]),
            "initialSpawn": sample_fields(initial) if initial else None,
            "samples": {"count": len(samples), "ready": sum(ready(sample) for sample in samples),
                        "clearHull": sum(bool(sample.get("clear_hull")) for sample in samples),
                        "last": sample_fields(samples[-1]) if samples else None},
            "warnings": warnings, "warningSamples": warning_samples,
            "staticAudit": {"findingIds": static.get("findingIds", []),
                            "assessment": static.get("staticAssessment"),
                            "nativeStartCount": len(starts),
                            "primaryStart": starts[0] if starts else None,
                            "incomingChangeTargets": static.get("incomingChangeTargets", [])},
        }
        for field in ("status", "boot", "scene"):
            if row[field] not in STATES:
                issues.append(f"invalid_{field}:{name}")
        rows.append(row)

    counts = {field: {state: sum(row[field] == state for row in rows) for state in STATES}
              for field in ("status", "boot", "scene")}
    checked = len(latest_results)
    if terminal:
        expected = {"checked": checked, "passed": counts["status"]["passed"],
                    "failed": counts["status"]["failed"]}
        for key, value in expected.items():
            if terminal.get(key) != value:
                issues.append(f"summary_{key}_mismatch")
    complete = bool(terminal and terminal.get("status") == "complete" and
                    start.get("first_index") == 0 and start.get("total") == 96 and
                    terminal.get("total") == 96 and checked == 96 and not issues)
    return {
        "schemaVersion": 1, "generatedAtUtc": datetime.now(timezone.utc).isoformat(),
        "runId": start["run"], "runComplete": complete,
        "runStatus": terminal.get("status") if terminal else "in_progress",
        "totalMaps": 96, "checkedMaps": checked, "counts": counts,
        "mapsWithWarnings": sum(bool(row["warnings"]) for row in rows),
        "harnessSummary": terminal, "consistencyIssues": sorted(set(issues)),
        "trailingPartialLineIgnored": trailing_partial,
        "scope": {"engine": "Stock Valve HLDS", "mode": "headless dedicated server",
                  "fakeClients": start.get("fake_clients", 1), "directStart": True,
                  "dwellSeconds": start["dwell_seconds"], "dwellOverrides": {"c0a0e": max(45, start["dwell_seconds"])},
                  "sceneScope": start.get("scene_scope", "").split(","),
                  "limits": ["Boot means a live spawned player and a clear hull were observed",
                             "Scene checks cover only the listed entry conditions; other scenes are not_checked",
                             "No full level walkthrough, boss victory, puzzle solving or all natural transitions",
                             "No real client rendering, HUD, camera or Steam Networking between two PCs",
                             "Passive death while standing still is a warning and does not erase a successful spawn",
                             "Static audit is BSP/source analysis; its older pending labels are not runtime results"]},
        "staticAudit": {"summary": audit.get("summary", {}), "runtimeVerifiedByThisAudit": False},
        "maps": rows,
    }


def write_csv(path, rows):
    fields = ("index", "map", "chapter", "chapterTitle", "status", "boot", "scene", "reason",
              "dwellSeconds", "initialSpawn", "samples", "warnings", "warningSamples", "staticFindingIds")
    with path.open("w", newline="", encoding="utf-8-sig") as output:
        writer = csv.DictWriter(output, fieldnames=fields)
        writer.writeheader()
        for row in rows:
            record = {field: row[field] for field in fields if field in row}
            for field in ("initialSpawn", "samples", "warnings", "warningSamples"):
                record[field] = json.dumps(record[field], ensure_ascii=False, separators=(",", ":"))
            record["staticFindingIds"] = ";".join(row["staticAudit"]["findingIds"])
            writer.writerow(record)


def main():
    parser = argparse.ArgumentParser(description="Отчёт по существующему прогону 96 карт без управления сервером")
    parser.add_argument("input", type=Path, help="Путь к map-check.jsonl")
    parser.add_argument("--run-id", type=int, help="Конкретный runId, иначе последний start в файле")
    parser.add_argument("--static-audit", type=Path, default=REPO_ROOT / "artifacts/runtime/campaign-entry-audit.json")
    parser.add_argument("--catalog", type=Path, default=REPO_ROOT / "game/hlcoop/campaign.json")
    parser.add_argument("--output", type=Path, help="JSON отчёт, по умолчанию report.json или progress.json")
    parser.add_argument("--server-dll", type=Path, help="Необязательный SHA256 проверенной серверной DLL")
    parser.add_argument("--source-root", type=Path, help="Необязательный SHA256 текущих исходников")
    args = parser.parse_args()
    records, trailing_partial = read_records(args.input)
    selected = select_run(records, args.run_id)
    catalog = json.loads(args.catalog.read_text(encoding="utf-8-sig"))
    audit = json.loads(args.static_audit.read_text(encoding="utf-8-sig"))
    report = build_report(selected, catalog, audit, trailing_partial)
    report["inputs"] = {"runtimeSha256": hashlib.sha256("".join(raw for _, raw in selected).encode("utf-8")).hexdigest(),
                        "staticAudit": file_metadata(args.static_audit), "catalog": file_metadata(args.catalog)}
    if args.server_dll:
        report["serverDll"] = file_metadata(args.server_dll)
    if args.source_root:
        report["currentSourceTree"] = source_metadata(args.source_root.resolve())
    output = args.output or REPO_ROOT / "artifacts/map-check" / ("report.json" if report["runComplete"] else "progress.json")
    input_paths = {path.resolve() for path in (args.input, args.static_audit, args.catalog)}
    if args.server_dll:
        input_paths.add(args.server_dll.resolve())
    output_paths = [output]
    if report["runComplete"]:
        output_paths.extend((output.with_suffix(".csv"), output.parent / f"run-{report['runId']}.jsonl"))
    if any(path.resolve() in input_paths for path in output_paths):
        raise ValueError("Файлы отчёта не должны перезаписывать входные данные")
    output.parent.mkdir(parents=True, exist_ok=True)
    if report["runComplete"]:
        raw_path = output.parent / f"run-{report['runId']}.jsonl"
        with raw_path.open("w", encoding="utf-8", newline="") as raw_output:
            raw_output.write("".join(raw for _, raw in selected))
        write_csv(output.with_suffix(".csv"), report["maps"])
        report["rawReport"] = raw_path.name
    output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"output": str(output), "runId": report["runId"], "runComplete": report["runComplete"],
                      "checked": report["checkedMaps"], "counts": report["counts"],
                      "mapsWithWarnings": report["mapsWithWarnings"]}, ensure_ascii=False))


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"Отчёт: {error}", file=sys.stderr)
        sys.exit(1)
