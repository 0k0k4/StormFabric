"""Single-run, read-only vehicle UI observer audit. Not a modality/UI certificate."""
import argparse
import hashlib
import json
from pathlib import Path
import re

MARKER = re.compile(r"CDVEH: PASS-THROUGH enabled max_events=(\d+);")
RECORD = re.compile(
    r"CDVEH event=(\d+) phase=(\S+) thread=(\d+) object=([0-9a-fA-Fx]+) "
    r"context=([0-9a-fA-Fx]+) parent=([0-9a-fA-Fx]+) modal=([0-9a-fA-Fx]+) "
    r"valid=([01]) action=(-?\d+)\s*$")
KINDS = {"ui.init", "ui.update", "ui.teardown", "ui.destroy", "state.teardown", "state.action",
         "state.enter", "state.leave"}


def audit_activity(records, errors):
    """Version 2 only: retained UI lifetime is distinct from active visits."""
    owners, pending = {}, {}
    visits, reentries = 0, 0
    for r in records:
        kind, side, obj = r["kind"], r["side"], r["object"]
        def fail(message):
            errors.append(f"line {r['line']}: activity {message}")
        if kind == "ui.init" and side == "before":
            owners[obj] = dict(context=r["context"], ready=False, status="inactive",
                               state=None, thread=None, visits=0, torn=False)
        if kind == "ui.init" and side == "after" and obj in owners:
            owners[obj]["ready"] = True
        matches = [o for o in owners.values() if o["context"] == r["context"]]
        owner = matches[0] if len(matches) == 1 else None
        if kind in {"state.enter", "state.leave"}:
            if side == "before":
                if owner is None or not owner["ready"] or owner["torn"]:
                    fail("transition without initialized live UI")
                    continue
                expected = "inactive" if kind == "state.enter" else "active"
                if owner["status"] != expected:
                    fail("overlapping/reversed enter/leave")
                if owner["state"] is None and kind == "state.enter":
                    owner["state"], owner["thread"] = obj, r["thread"]
                if owner["state"] != obj or owner["thread"] != r["thread"]:
                    fail("state/thread changed")
                owner["status"] = "entering" if kind == "state.enter" else "leaving"
                pending[r["event"]] = owner
            else:
                owner = pending.pop(r["event"], None)
                if owner is None:
                    fail("completion without transition")
                    continue
                if kind == "state.enter":
                    owner["status"] = "active"
                else:
                    owner["status"] = "inactive"
                    reentries += int(owner["visits"] > 0)
                    owner["visits"] += 1
                    visits += 1
        elif kind in {"ui.update", "state.action"}:
            # Transition/action .after records intentionally contain no context.
            if kind == "state.action" and side == "after":
                continue
            if owner is None or owner["status"] != "active" or owner["thread"] != r["thread"]:
                fail("update/action outside active visit or wrong thread")
            elif kind == "state.action" and owner["state"] != obj:
                fail("action state changed")
        elif kind == "state.teardown" and side == "before":
            if owner is None or owner["status"] != "inactive":
                fail("state teardown before leave completed")
            elif owner["state"] is not None and (owner["state"] != obj or owner["thread"] != r["thread"]):
                fail("teardown state/thread changed")
        elif kind == "ui.teardown" and side == "before" and obj in owners:
            owner = owners[obj]
            if owner["status"] != "inactive":
                fail("UI teardown before leave completed")
            owner["torn"] = True
        elif kind == "ui.destroy" and side == "after":
            owners.pop(obj, None)
    if pending:
        errors.append("activity transitions still open at end of log")
    return dict(complete_active_visits=visits, retained_ui_reentries=reentries)


def audit(text):
    errors, events, records, markers, versions = [], {}, [], [], []
    for line_number, line in enumerate(text.splitlines(), 1):
        if match := MARKER.search(line):
            markers.append(int(match[1]))
            version = re.search(r"; version=(\d+);", line)
            versions.append(int(version[1]) if version else 1)
            if "version=" in line and version is None:
                errors.append(f"line {line_number}: malformed observer version")
        if "CDVEH event=" not in line:
            continue
        match = RECORD.search(line)
        if not match:
            errors.append(f"line {line_number}: malformed observer record")
            continue
        event, phase, thread, obj, context, parent, modal, valid, action = match.groups()
        kind, _, side = phase.rpartition(".")
        r = dict(event=int(event), kind=kind, side=side, thread=int(thread), object=int(obj, 16),
                 context=int(context, 16), parent=int(parent, 16), modal=int(modal, 16),
                 valid=int(valid), action=int(action), line=line_number)
        if kind not in KINDS or side not in {"before", "after"}:
            errors.append(f"line {line_number}: unknown phase")
        records.append(r)
        events.setdefault(r["event"], []).append(r)
    if len(markers) != 1 or (markers and markers[0] != 4096):
        errors.append("expected exactly one current observer activation; do not combine sessions")
    version = versions[0] if len(versions) == 1 else 0
    if version not in {1, 2}:
        errors.append("unsupported observer version")
    if version != 2 and any(r["kind"] in {"state.enter", "state.leave"} for r in records):
        errors.append("activity records require observer version 2")
    if not events or sorted(events) != list(range(len(events))):
        errors.append("missing/non-contiguous observer event IDs")
    if len(events) >= 4096:
        errors.append("observer cap reached; lifecycle trace may be incomplete")
    for event, pair in events.items():
        if len(pair) != 2 or [r["side"] for r in pair] != ["before", "after"]:
            errors.append(f"event {event}: missing/duplicate/reversed pair")
            continue
        before, after = pair
        if any(before[k] != after[k] for k in ("kind", "object", "thread", "action")):
            errors.append(f"event {event}: pair identity/thread/action mismatch")
        if not before["object"] or not before["thread"] or not -(2**31) <= before["action"] < 2**31:
            errors.append(f"event {event}: invalid identity/action")
        if before["kind"] != "state.action" and before["action"] != -1:
            errors.append(f"event {event}: unexpected action payload")
        if before["kind"] in {"ui.init", "ui.update", "ui.teardown"}:
            if not before["valid"] or not after["valid"] or not before["context"] or before["context"] != after["context"]:
                errors.append(f"event {event}: unreadable/changed UI context")
        elif after["valid"] or after["context"] or after["parent"] or after["modal"]:
            errors.append(f"event {event}: unexpected post-destruction/transition read")
        if before["kind"].startswith("state.") and (not before["valid"] or not before["context"]):
            errors.append(f"event {event}: unreadable state context")
        if before["kind"] == "ui.destroy" and any(before[k] for k in ("context", "parent", "modal", "valid")):
            errors.append(f"event {event}: destructor must not infer a context from a dismantled tree")

    live, completed, attached, catalogue, modal_samples, state_intervals = {}, 0, 0, 0, 0, []
    thread_observations = []
    for r in records:
        kind, side, obj = r["kind"], r["side"], r["object"]
        if kind == "ui.init" and side == "before":
            if obj in live:
                errors.append(f"line {r['line']}: UI reinitialized without observed destruction")
            # Native state creation runs through thread_task_state_create. Do
            # not bind attached UI work to that worker. The first attached
            # update establishes the observed runtime thread, never an action
            # or a teardown. This is trace consistency, not a publication fence
            # or permission for a native dialog adapter to transfer ownership.
            live[obj] = dict(context=r["context"], init_thread=r["thread"], thread=None, initialized=False,
                              torn=False, teardown_end=0, parent=0)
        if kind.startswith("ui."):
            owner = live.get(obj)
            if owner is None:
                errors.append(f"line {r['line']}: UI event without observed init")
                continue
            if (kind == "ui.update" and side == "before" and owner["thread"] is None and
                    owner["initialized"] and not owner["torn"] and r["parent"]):
                owner["thread"] = r["thread"]
            expected_thread = owner["init_thread"] if kind == "ui.init" else owner["thread"]
            if r["thread"] != expected_thread or (kind != "ui.destroy" and r["context"] != owner["context"]):
                errors.append(f"line {r['line']}: UI owner thread/context changed")
            if kind in {"ui.update", "ui.teardown"} and owner["parent"] and owner["parent"] != r["parent"]:
                errors.append(f"line {r['line']}: UI parent changed")
            if kind == "ui.init" and side == "after":
                if owner["initialized"] or owner["thread"] is not None:
                    errors.append(f"line {r['line']}: repeated/late init completion")
                owner["initialized"] = True
            elif kind == "ui.update":
                if not owner["initialized"] or owner["torn"] or not r["parent"]:
                    errors.append(f"line {r['line']}: update outside attached live UI")
                owner["parent"] = r["parent"]
                modal_samples += int(bool(r["modal"]))
            elif kind == "ui.teardown":
                if not owner["initialized"]:
                    errors.append(f"line {r['line']}: teardown before init completed")
                owner["torn"] = True
                if side == "after":
                    owner["teardown_end"] = r["line"]
            elif kind == "ui.destroy":
                if not owner["teardown_end"]:
                    errors.append(f"line {r['line']}: destruction without completed native teardown")
                if side == "after":
                    attached += int(bool(owner["parent"]))
                    thread_observations.append(dict(object=f"{obj:x}", context=f"{owner['context']:x}",
                                                    init_thread=owner["init_thread"], runtime_thread=owner["thread"]))
                    del live[obj]
                    completed += 1
        elif kind == "state.action" and side == "before":
            if not any(o["context"] == r["context"] and o["thread"] == r["thread"] and
                       o["initialized"] and not o["torn"] for o in live.values()):
                errors.append(f"line {r['line']}: action outside observed live UI")
            catalogue += int(r["action"] == 0x61)
        elif kind == "state.teardown" and side == "before":
            pair = events.get(r["event"], [])
            if len(pair) == 2:
                state_intervals.append((r, pair[1]))
    if live:
        errors.append("UI lifetimes still open at end of log")
    for before, after in state_intervals:
        if not any(r["kind"] == "ui.teardown" and r["side"] == "after" and
                   r["context"] == before["context"] and r["thread"] == before["thread"] and
                   before["line"] < r["line"] < after["line"] for r in records):
            errors.append(f"event {before['event']}: no same-context child teardown inside state teardown")
    coverage = dict(complete_ui_lifetimes=completed, attached_ui_lifetimes=attached,
                    catalogue_actions_0x61=catalogue,
                    state_teardown_intervals=len(state_intervals), modal_update_samples=modal_samples)
    covered = completed >= 2 and attached >= 2 and catalogue > 0 and len(state_intervals) >= 2
    activity_coverage = audit_activity(records, errors) if version == 2 else None
    activity_covered = bool(activity_coverage and activity_coverage["complete_active_visits"] >= 2 and
                            activity_coverage["retained_ui_reentries"] >= 1)
    if version == 2:
        covered = covered and activity_covered
    return dict(observer_integrity_passed=not errors, requested_coverage_passed=covered,
                 audit_passed=not errors and covered, coverage=coverage, errors=errors,
                 thread_observations=thread_observations,
                 observer_version=version, activity_coverage=activity_coverage,
                 activity_coverage_passed=activity_covered,
                 limitations="Sampled updates; no keyboard/mouse/modality, native publication synchronization, custom-dialog, registration or placement certificate.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    args = parser.parse_args()
    data = args.log.read_bytes()
    result = audit(data.decode("utf-8-sig", errors="replace"))
    result["log_sha256"] = hashlib.sha256(data).hexdigest()
    print(json.dumps(result, indent=2))
    return 0 if result["audit_passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
