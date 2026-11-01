"""Read-only audit of CDOBS pass-through logs; never authorizes runtime hooks.

An object address is only an observation, not an identity across destruction or
address reuse. Empty handles do NOT prove backend/GPU deletion or queue drainage.
"""
import argparse
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path
import re

WITNESS_ID = "swml_cd_probe_v1_1x1"
WITNESS_ARCHIVE = "d7b0869911c6cc3f3204060c2854c674"
ROW = re.compile(
    r"CDOBS event=(?P<event>\d+) phase=(?P<phase>(?:create|release)\.(?:before|after)) "
    r"thread=(?P<thread>\d+) monitor=(?P<monitor>[0-9A-Fa-f]{16}) "
    r"queue=(?P<queue>[0-9A-Fa-f]{16}) quality=(?P<quality>-?\d+) "
    r"caller=(?P<caller>[0-9A-Fa-f]{16}) valid=(?P<valid>[01]) "
    r"def=(?P<definition>[0-9A-Fa-f]{16}) id=(?P<identity>.*?) archive=(?P<archive>.*?) "
    r"pixels=(?P<width>\d+)x(?P<height>\d+) scale=(?P<scale>\d+) "
    r"video=(?P<video>[0-9A-Fa-f]{16})/(?P<video_manager>[0-9A-Fa-f]{16}) "
    r"overlay=(?P<overlay>[0-9A-Fa-f]{16})/(?P<overlay_manager>[0-9A-Fa-f]{16})$"
)
NUMBERS = {"event", "thread", "quality", "valid", "width", "height", "scale"}
POINTERS = {"monitor", "queue", "caller", "definition", "video", "video_manager",
            "overlay", "overlay_manager"}
HANDLE_FIELDS = ("video", "video_manager", "overlay", "overlay_manager")
SMALL_ID = "swml_cd_v1_b1x1_p24x16"
SMALL_ARCHIVE = "26f8c812f42cd41a9173ae6030881eb9"
SMALL_ENABLE = (f"CDRES: LOCAL EXPERIMENT enabled ONLY for {SMALL_ID}/{SMALL_ARCHIVE}; "
                "24x16 <= stock 32x32; native destruction unchanged; disk fixture SHA256 "
                "verified and pinned; not a production provenance/global-budget certificate")
ADMISSION = re.compile(
    r"CDRES experiment=24x16 result=(?P<result>\d+) monitor=(?P<monitor>[0-9A-Fa-f]{16}) "
    r"queue=(?P<queue>[0-9A-Fa-f]{16}) quality=(?P<quality>-?\d+) "
    r"thread=(?P<thread>\d+) lifetime_charge=(?P<charge>\d+)$")


def audit(text, *, small=False):
    errors = []
    events = defaultdict(dict)
    stacks = {}
    enabled = 0
    small_enabled = 0
    active_creates = {}
    admissions = {}
    for line_number, line in enumerate(text.splitlines(), 1):
        header = "CDOBS: SMALL-RESOLUTION-EXPERIMENT observation enabled" if small else "CDOBS: PASS-THROUGH observation enabled"
        if header in line:
            enabled += 1
        if "CDRES: LOCAL EXPERIMENT enabled" in line:
            small_enabled += 1
            if not line.endswith(SMALL_ENABLE):
                errors.append(f"line {line_number}: unexpected resolution activation")
        if "CDRES experiment=" in line:
            match = ADMISSION.fullmatch(line[line.index("CDRES experiment="):])
            if not match:
                errors.append(f"line {line_number}: malformed resolution admission")
                continue
            admission = match.groupdict()
            for key in ("result", "quality", "thread", "charge"):
                admission[key] = int(admission[key])
            for key in ("monitor", "queue"):
                admission[key] = admission[key].lower()
            event = active_creates.get(admission["thread"])
            if event is None or event in admissions:
                errors.append(f"line {line_number}: orphan or duplicate resolution admission")
            else:
                admissions[event] = admission
            continue
        if "CDOBS event=" not in line:
            continue
        payload = line[line.index("CDOBS event="):]
        if " native_stack=" in payload:
            match = re.fullmatch(r"CDOBS event=(\d+) native_stack=(.*?) caller=([0-9A-Fa-f]{16})", payload)
            if not match or int(match[1]) in stacks:
                errors.append(f"line {line_number}: malformed or duplicate stack")
            else:
                stacks[int(match[1])] = {"frames": match[2], "caller": match[3].lower()}
            continue
        match = ROW.fullmatch(payload)
        if not match:
            errors.append(f"line {line_number}: malformed observation")
            continue
        row = match.groupdict()
        for key in NUMBERS:
            row[key] = int(row[key])
        for key in POINTERS:
            row[key] = row[key].lower()
        event, phase = row["event"], row["phase"]
        if phase == "create.before":
            if row["thread"] in active_creates:
                errors.append(f"event {event}: overlapping create on one thread")
            active_creates[row["thread"]] = event
        elif phase == "create.after":
            if active_creates.pop(row["thread"], None) != event:
                errors.append(f"event {event}: create interval mismatch")
        if phase in events[event]:
            errors.append(f"event {event}: duplicate {phase}")
        else:
            events[event][phase] = row

    if enabled != 1:
        errors.append(f"expected one observation activation, found {enabled}")
    if small and small_enabled != 1:
        errors.append(f"expected one pinned resolution activation, found {small_enabled}")
    if not small and (small_enabled or admissions):
        errors.append("resolution experiment evidence in pass-through session")
    if not events:
        errors.append("no observed lifecycle calls")
    if set(events) != set(range(len(events))):
        errors.append("event IDs are incomplete or do not start at zero")
    if set(stacks) != set(events):
        errors.append("stack/event coverage differs")
    definitions = defaultdict(lambda: {"archives": set(), "create_qualities": Counter(),
                                      "logical_sizes": set(), "threads": set(),
                                      "creates": 0, "releases": 0})
    queues, threads, operations, callsites = set(), set(), Counter(), Counter()
    empty_releases = 0
    admitted_bytes = 0
    for event, phases in sorted(events.items()):
        operations_here = {phase.split(".")[0] for phase in phases}
        if len(operations_here) != 1:
            errors.append(f"event {event}: mixed operations")
            continue
        operation = next(iter(operations_here))
        before, after = phases.get(operation + ".before"), phases.get(operation + ".after")
        if before is None or after is None:
            errors.append(f"event {event}: missing before/after pair")
            continue
        operations[operation] += 1
        for field in ("thread", "monitor", "queue", "quality", "caller", "definition", "identity", "archive"):
            if before[field] != after[field]:
                errors.append(f"event {event}: {field} changed across native call")
        if not before["valid"] or not after["valid"]:
            errors.append(f"event {event}: unreadable snapshot")
            continue
        stack = stacks.get(event)
        if stack and stack["caller"] != before["caller"]:
            errors.append(f"event {event}: stack caller differs")
        if int(before["queue"], 16) == 0 or int(before["monitor"], 16) == 0:
            errors.append(f"event {event}: null queue or monitor")
        threads.add(before["thread"])
        queues.add(before["queue"])
        callsites[(operation, stack["frames"] if stack else "<missing>")] += 1
        definition = definitions[after["identity"]]
        definition["archives"].add(after["archive"])
        definition["threads"].add(after["thread"])
        definition[operation + "s"] += 1
        if operation == "create":
            if any(int(before[field], 16) for field in HANDLE_FIELDS):
                errors.append(f"event {event}: create entered with occupied handle")
            expected_scale = {1: 1, 3: 4}.get(after["quality"], 2)
            if after["scale"] != expected_scale or not after["width"] or not after["height"]:
                errors.append(f"event {event}: native dimension/quality mismatch")
            for field in ("video", "overlay"):
                if not int(after[field], 16) or after[field + "_manager"] != after["queue"]:
                    errors.append(f"event {event}: {field} handle/manager mismatch")
            definition["create_qualities"][after["quality"]] += 1
            definition["logical_sizes"].add((after["width"], after["height"], after["scale"]))
            admission = admissions.get(event)
            if small and after["identity"] == SMALL_ID:
                if not admission:
                    errors.append(f"event {event}: missing resolution admission")
                elif admission["result"] != 0:
                    errors.append(f"event {event}: resolution request refused ({admission['result']})")
                else:
                    admitted_bytes += 24 * 16 * 12 * (expected_scale ** 2 + 1)
            if admission:
                if after["identity"] != SMALL_ID or after["archive"] != SMALL_ARCHIVE:
                    errors.append(f"event {event}: admission for unrelated definition")
                for field in ("monitor", "queue", "thread", "quality"):
                    if admission[field] != after[field]:
                        errors.append(f"event {event}: admission {field} differs")
                cost = 24 * 16 * 12 * (expected_scale ** 2 + 1)
                if not cost <= admission["charge"] <= 8 * 1024 * 1024:
                    errors.append(f"event {event}: admission charge outside experiment bounds")
        else:
            empty_releases += int(not any(int(before[field], 16) for field in HANDLE_FIELDS))
            if any(int(after[field], 16) for field in HANDLE_FIELDS):
                errors.append(f"event {event}: release did not clear handles")
            if any(before[field] != after[field] for field in ("width", "height", "scale")):
                errors.append(f"event {event}: release changed logical dimensions")

    if small and admissions and max(row["charge"] for row in admissions.values()) != admitted_bytes:
        errors.append("cumulative admission charge differs from observed requests")
    identity = SMALL_ID if small else WITNESS_ID
    archive = SMALL_ARCHIVE if small else WITNESS_ARCHIVE
    dimensions = (24,16) if small else (32,32)
    witness = definitions.get(identity)
    witness_matches = bool(witness and witness["archives"] == {archive}
                           and witness["logical_sizes"]
                           and all(size[:2] == dimensions for size in witness["logical_sizes"]))
    if not witness_matches:
        errors.append(f"expected specimen identity/archive/{dimensions} dimensions not observed")
    integrity_errors = list(errors)
    quality_coverage = bool(witness and {1, 2, 3} <= witness["create_qualities"].keys())
    if not quality_coverage:
        errors.append("witness quality enums 1, 2 and 3 not all observed")
    normalized = {}
    for definition_id, data in sorted(definitions.items()):
        normalized[definition_id] = {key: (dict(sorted(value.items())) if isinstance(value, Counter)
                                     else sorted(value) if isinstance(value, set) else value)
                                for key, value in data.items()}
    return {
        "kind": "swml-custom-display-observation-audit-v1",
        "audit_passed": not errors, "errors": errors,
        "lifecycle_integrity_passed": not integrity_errors,
        "integrity_errors": integrity_errors,
        "missing_specimen_qualities": sorted({1, 2, 3} - (witness["create_qualities"].keys() if witness else set())),
        "resolution_admissions": len(admissions), "admitted_target_bytes": admitted_bytes,
        "events": len(events), "operations": dict(operations), "threads": sorted(threads),
        "queues": sorted(queues), "empty_release_calls": empty_releases,
        "logging_cap_reached": bool(events and max(events) >= 4095),
        "specimen_id": identity, "small_resolution_experiment": small,
        "witness_matches": witness_matches, "witness_quality_coverage": quality_coverage,
        "definitions": normalized,
        "native_callsites": [{"operation": op, "frames": frames, "count": count}
                             for (op, frames), count in sorted(callsites.items())],
        "limitations": [
            "No backend consumer thread, allocation success or GPU deletion completion observed.",
            "Addresses may be reused; no cross-call object lifetime or synchronization certified.",
            "Quality enums are observed values, not certification of UI setting names.",
            "Lifecycle snapshots alone do not validate camera output, Lua dimensions, geometry or touch.",
            "Audit never authorizes installation or overrides authentication/budget gates.",
        ],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--small-experiment", action="store_true")
    args = parser.parse_args()
    raw = args.log.read_bytes()
    report = audit(raw.decode("utf-8", errors="replace"), small=args.small_experiment)
    report["log_sha256"] = hashlib.sha256(raw).hexdigest()
    print(json.dumps(report, indent=2))
    return 0 if report["audit_passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
