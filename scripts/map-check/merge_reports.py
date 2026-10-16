import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import sys

sys.dont_write_bytecode = True
from report import REPO_ROOT, STATES, build_report, file_metadata, read_records, select_run, source_metadata, write_csv


def load_part(path, run_id, dll_path, catalog, audit):
    records, trailing_partial = read_records(path)
    selected = select_run(records, run_id)
    start = selected[0][0]
    first = start.get("first_index")
    if first not in (0, 24, 48, 72) or start.get("total") != 24:
        raise ValueError(f"Ожидается диапазон из 24 карт с first_index 0/24/48/72: {path.name}")
    report = build_report(selected, catalog, audit, trailing_partial)
    summary = report["harnessSummary"]
    if not summary or summary.get("status") != "complete" or summary.get("total") != 24:
        raise ValueError(f"Диапазон {first}..{first + 23} не имеет итогового summary complete")
    results = [record for record, _ in selected if record.get("type") == "map_result"]
    names = [row["map"] for row in report["maps"][first:first + 24]]
    if len(results) != 24 or len({record["map"] for record in results}) != 24 or \
            {record["map"] for record in results} != set(names):
        raise ValueError(f"Диапазон {first}..{first + 23} должен содержать ровно 24 ожидаемых map_result")
    if report["checkedMaps"] != 24 or report["consistencyIssues"]:
        raise ValueError(f"Несогласованные результаты диапазона {first}..{first + 23}")
    expected_boot = {state: report["counts"]["boot"][state] for state in ("passed", "failed")}
    expected_scene = {state: report["counts"]["scene"][state] for state in ("passed", "failed")}
    expected_scene["checked"] = expected_scene["passed"] + expected_scene["failed"]
    if summary.get("boot_checks") != expected_boot or summary.get("scene_checks") != expected_scene:
        raise ValueError(f"Счётчики boot/scene не совпадают с map_result диапазона {first}..{first + 23}")
    if any(row["status"] == "not_checked" for row in report["maps"][first:first + 24]):
        raise ValueError(f"В диапазоне {first}..{first + 23} остались непроверенные карты")
    if not dll_path.is_file():
        raise ValueError(f"Для диапазона {first}..{first + 23} не найдена проверочная DLL; передайте --server-dll")
    return {"first": first, "report": report, "selected": selected, "start": start,
            "dll": file_metadata(dll_path), "input": path,
            "raw": "".join(raw for _, raw in selected)}


def main():
    parser = argparse.ArgumentParser(description="Объединение четырёх завершённых проверок HLDS по 24 карты")
    parser.add_argument("inputs", type=Path, nargs=4, help="Четыре файла map-check.jsonl")
    parser.add_argument("--run-ids", type=int, nargs=4, help="runId каждого входного файла в том же порядке")
    parser.add_argument("--server-dll", type=Path, nargs=4,
                        help="Четыре проверенные DLL, иначе dlls/hl.dll рядом с каждым JSONL")
    parser.add_argument("--static-audit", type=Path, default=REPO_ROOT / "artifacts/runtime/campaign-entry-audit.json")
    parser.add_argument("--catalog", type=Path, default=REPO_ROOT / "game/hlcoop/campaign.json")
    parser.add_argument("--source-root", type=Path, help="Необязательный SHA256 текущих исходников")
    parser.add_argument("--output", type=Path, default=REPO_ROOT / "artifacts/map-check/report.json")
    args = parser.parse_args()
    if len({path.resolve() for path in args.inputs}) != 4:
        raise ValueError("Нужны четыре различных входных JSONL")
    if args.output.suffix.lower() != ".json":
        raise ValueError("Выходной отчёт должен иметь расширение .json")
    catalog = json.loads(args.catalog.read_text(encoding="utf-8-sig"))
    audit = json.loads(args.static_audit.read_text(encoding="utf-8-sig"))
    dll_paths = args.server_dll or [path.parent / "dlls/hl.dll" for path in args.inputs]
    run_ids = args.run_ids or [None] * 4
    parts = [load_part(path, run_id, dll, catalog, audit)
             for path, run_id, dll in zip(args.inputs, run_ids, dll_paths)]
    parts.sort(key=lambda part: part["first"])
    if [part["first"] for part in parts] != [0, 24, 48, 72]:
        raise ValueError("Диапазоны должны стыковаться: 0..23, 24..47, 48..71, 72..95")
    if len({part["dll"]["sha256"] for part in parts}) != 1:
        raise ValueError("Четыре сервера проверяли разные серверные DLL по SHA256")
    first_scope = parts[0]["report"]["scope"]
    for part in parts[1:]:
        if part["report"]["scope"] != first_scope:
            raise ValueError("У четырёх прогонов различаются условия проверки")
    if first_scope["fakeClients"] != 1:
        raise ValueError("Ожидается один fake client на каждом сервере")

    # Берём только назначенные каждому серверу карты, не добавляя старые baseline и повторы
    rows = [row for part in parts for row in part["report"]["maps"][part["first"]:part["first"] + 24]]
    catalog_names = [name for chapter in catalog["chapters"] for name in chapter["maps"]]
    if len(rows) != 96 or [row["map"] for row in rows] != catalog_names or \
            len({row["map"] for row in rows}) != 96:
        raise ValueError("Объединённый отчёт должен содержать ровно 96 уникальных карт каталога")
    counts = {field: {state: sum(row[field] == state for row in rows) for state in STATES}
              for field in ("status", "boot", "scene")}
    for part in parts:
        run_id = part["report"]["runId"]
        part["rawName"] = f"run-{run_id}-maps-{part['first']:02d}-{part['first'] + 23:02d}.jsonl"
    input_paths = {path.resolve() for path in [*args.inputs, *dll_paths, args.static_audit, args.catalog]}
    output_paths = [args.output, args.output.with_suffix(".csv"),
                    *(args.output.parent / part["rawName"] for part in parts)]
    if any(path.resolve() in input_paths for path in output_paths):
        raise ValueError("Выходные файлы не должны перезаписывать входные данные")
    if len({path.resolve() for path in output_paths}) != 6:
        raise ValueError("Имена выходных файлов пересекаются")

    scope = dict(first_scope)
    scope["parallelServers"] = 4
    scope["mapsPerServer"] = 24
    scope["physicsSpeedChanged"] = False
    terminal = {
        "status": "complete", "total": 96, "checked": 96,
        "passed": counts["status"]["passed"], "failed": counts["status"]["failed"],
        "warnings": sum(part["report"]["harnessSummary"]["warnings"] for part in parts),
        "boot_checks": {"passed": counts["boot"]["passed"], "failed": counts["boot"]["failed"]},
        "scene_checks": {"checked": counts["scene"]["passed"] + counts["scene"]["failed"],
                         "passed": counts["scene"]["passed"], "failed": counts["scene"]["failed"]},
    }
    report = {
        "schemaVersion": 1, "generatedAtUtc": datetime.now(timezone.utc).isoformat(),
        "runComplete": True, "runStatus": "complete", "totalMaps": 96, "checkedMaps": 96,
        "counts": counts, "mapsWithWarnings": sum(bool(row["warnings"]) for row in rows),
        "harnessSummary": terminal, "consistencyIssues": [], "scope": scope,
        "serverDll": {**parts[0]["dll"], "copiesVerified": 4},
        "inputs": {"staticAudit": file_metadata(args.static_audit), "catalog": file_metadata(args.catalog)},
        "runs": [{"runId": part["report"]["runId"], "firstIndex": part["first"], "count": 24,
                  "rawReport": part["rawName"], "runtimeSha256": hashlib.sha256(part["raw"].encode("utf-8")).hexdigest(),
                  "serverDllSha256": part["dll"]["sha256"],
                  "harnessSummary": part["report"]["harnessSummary"]} for part in parts],
        "staticAudit": parts[0]["report"]["staticAudit"], "maps": rows,
    }
    if args.source_root:
        report["currentSourceTree"] = source_metadata(args.source_root.resolve())
    args.output.parent.mkdir(parents=True, exist_ok=True)
    for part in parts:
        with (args.output.parent / part["rawName"]).open("w", encoding="utf-8", newline="") as output:
            output.write(part["raw"])
    write_csv(args.output.with_suffix(".csv"), rows)
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"output": str(args.output), "runComplete": True, "checked": 96,
                      "counts": counts, "mapsWithWarnings": report["mapsWithWarnings"],
                      "serverDllSha256": report["serverDll"]["sha256"]}, ensure_ascii=False))


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"Объединение отчётов: {error}", file=sys.stderr)
        sys.exit(1)
