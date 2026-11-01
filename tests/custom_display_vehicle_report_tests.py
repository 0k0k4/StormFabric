"""Synthetic UI lifecycle traces only; never claim real runtime coverage."""
import importlib.util
from pathlib import Path
import unittest

path = Path(__file__).resolve().parents[1] / "tools/custom_display_vehicle_report.py"
spec = importlib.util.spec_from_file_location("vehicle_report", path)
report = importlib.util.module_from_spec(spec)
spec.loader.exec_module(report)


def trace(count=2):
    lines = ["CDVEH: PASS-THROUGH enabled max_events=4096;"]
    event = 0

    def record(e, kind, side, obj, context=0, parent=0, modal=0, valid=0, action=-1):
        lines.append(f"CDVEH event={e} phase={kind}.{side} thread=7 object={obj:x} "
                     f"context={context:x} parent={parent:x} modal={modal:x} valid={valid} action={action}")

    for _ in range(count):
        record(event, "ui.init", "before", 0x100, 0x200, valid=1)
        record(event, "ui.init", "after", 0x100, 0x200, valid=1); event += 1
        for side in ("before", "after"):
            record(event, "ui.update", side, 0x100, 0x200, 0x150, 0x300, 1)
        event += 1
        record(event, "state.action", "before", 0x50, 0x200, valid=1, action=0x61)
        record(event, "state.action", "after", 0x50, action=0x61); event += 1
        state = event; event += 1
        record(state, "state.teardown", "before", 0x50, 0x200, valid=1)
        for side in ("before", "after"):
            record(event, "ui.teardown", side, 0x100, 0x200, 0x150, valid=1)
        event += 1
        record(state, "state.teardown", "after", 0x50)
        for side in ("before", "after"):
            record(event, "ui.destroy", side, 0x100)
        event += 1
    return "\n".join(lines)


def activity_trace(count=2, visits=2):
    lines = ["CDVEH: PASS-THROUGH enabled max_events=4096; version=2;"]
    event = 0
    def record(e, kind, side, obj, context=0, parent=0, valid=0, action=-1):
        thread = 42 if kind == "ui.init" else 7
        lines.append(f"CDVEH event={e} phase={kind}.{side} thread={thread} object={obj:x} "
                     f"context={context:x} parent={parent:x} modal=0 valid={valid} action={action}")
    for _ in range(count):
        for side in ("before", "after"):
            record(event, "ui.init", side, 0x100, 0x200, valid=1)
        event += 1
        for _ in range(visits):
            record(event, "state.enter", "before", 0x50, 0x200, valid=1)
            record(event, "state.enter", "after", 0x50); event += 1
            for side in ("before", "after"):
                record(event, "ui.update", side, 0x100, 0x200, 0x150, 1)
            event += 1
            record(event, "state.action", "before", 0x50, 0x200, valid=1, action=97)
            record(event, "state.action", "after", 0x50, action=97); event += 1
            record(event, "state.leave", "before", 0x50, 0x200, valid=1)
            record(event, "state.leave", "after", 0x50); event += 1
        state = event; event += 1
        record(state, "state.teardown", "before", 0x50, 0x200, valid=1)
        for side in ("before", "after"):
            record(event, "ui.teardown", side, 0x100, 0x200, 0x150, 1)
        event += 1
        for side in ("before", "after"):
            record(event, "ui.destroy", side, 0x100)
        event += 1
        record(state, "state.teardown", "after", 0x50)
    return "\n".join(lines)


class VehicleReportTests(unittest.TestCase):
    def test_retained_activity_is_not_two_ui_lifetimes(self):
        result = report.audit(activity_trace(count=1))
        self.assertTrue(result["observer_integrity_passed"], result)
        self.assertTrue(result["activity_coverage_passed"], result)
        self.assertFalse(result["requested_coverage_passed"], result)
        self.assertEqual(result["coverage"]["complete_ui_lifetimes"], 1)
        self.assertEqual(result["activity_coverage"]["retained_ui_reentries"], 1)

    def test_two_world_lifetimes_with_retained_visits(self):
        result = report.audit(activity_trace())
        self.assertTrue(result["audit_passed"], result)
        self.assertEqual(result["activity_coverage"]["complete_active_visits"], 4)

    def test_old_trace_does_not_certify_activity(self):
        result = report.audit(trace())
        self.assertTrue(result["audit_passed"], result)
        self.assertIsNone(result["activity_coverage"])
        self.assertFalse(result["activity_coverage_passed"])

    def test_activity_must_be_versioned_and_supported(self):
        for marker in ("", "version=3;", "version=unknown;"):
            result = report.audit(activity_trace().replace("version=2;", marker))
            self.assertFalse(result["observer_integrity_passed"], result)

    def test_activity_overlap_state_thread_and_post_transition_reads_rejected(self):
        text = activity_trace()
        for before, after in (("state.leave", "state.enter"),
                              ("phase=state.leave.before thread=7", "phase=state.leave.before thread=8"),
                              ("phase=state.leave.before thread=7 object=50", "phase=state.leave.before thread=7 object=51"),
                              ("phase=state.enter.after thread=7 object=50 context=0", "phase=state.enter.after thread=7 object=50 context=200")):
            self.assertFalse(report.audit(text.replace(before, after))["observer_integrity_passed"], before)

    def test_activity_update_after_leave_rejected(self):
        lines = activity_trace(count=1).splitlines()
        update = [line for line in lines if "event=2 " in line]
        lines = [line for line in lines if "event=2 " not in line]
        after_leave = next(i for i, line in enumerate(lines) if "phase=state.leave.after" in line)
        lines[after_leave+1:after_leave+1] = update
        result = report.audit("\n".join(lines))
        self.assertTrue(any("activity update/action outside" in e for e in result["errors"]), result)

    def test_single_run_lifetimes_and_address_reuse(self):
        result = report.audit(trace())
        self.assertTrue(result["audit_passed"], result)
        self.assertEqual(result["coverage"]["complete_ui_lifetimes"], 2)
        self.assertIn("no keyboard/mouse/modality", result["limitations"])

    def test_coverage_is_distinct_from_integrity(self):
        result = report.audit(trace(1))
        self.assertTrue(result["observer_integrity_passed"], result)
        self.assertFalse(result["requested_coverage_passed"])
        self.assertFalse(result["audit_passed"])

    def test_worker_init_then_one_attached_runtime_thread(self):
        lines = trace().splitlines()
        text = "\n".join(line.replace("thread=7", "thread=42") if "phase=ui.init." in line else line
                         for line in lines)
        result = report.audit(text)
        self.assertTrue(result["audit_passed"], result)
        self.assertEqual(len(result["thread_observations"]), 2)
        self.assertTrue(all(o["init_thread"] == 42 and o["runtime_thread"] == 7
                            for o in result["thread_observations"]))
        self.assertIn("no keyboard/mouse/modality, native publication synchronization", result["limitations"])

    def test_runtime_thread_cannot_return_to_construction_worker(self):
        lines = trace().splitlines()
        for phase in ("state.action", "ui.teardown", "ui.destroy", "state.teardown"):
            changed = "\n".join(line.replace("thread=7", "thread=42")
                                if f"phase={phase}." in line else line for line in lines)
            self.assertFalse(report.audit(changed)["observer_integrity_passed"], phase)

    def test_init_must_complete_before_first_attached_update(self):
        lines = trace(1).splitlines()
        delayed = lines.pop(2)
        lines.insert(4, delayed)
        self.assertFalse(report.audit("\n".join(lines))["observer_integrity_passed"])

    def test_action_cannot_establish_runtime_thread(self):
        lines = [line for line in trace(1).splitlines() if "phase=ui.update." not in line]
        # Renumber the remaining IDs to avoid relying on the gap rejection.
        text = "\n".join(lines)
        for old in range(2, 6):
            text = text.replace(f"event={old} ", f"event={old - 1} ")
        self.assertFalse(report.audit(text)["observer_integrity_passed"])

    def test_subsequent_update_cannot_change_runtime_thread(self):
        lines = trace(1).splitlines()
        extra = [line.replace("event=1 ", "event=6 ").replace("thread=7", "thread=42")
                 for line in lines if "phase=ui.update." in line]
        lines[5:5] = extra
        result = report.audit("\n".join(lines))
        self.assertTrue(any("owner thread/context changed" in error for error in result["errors"]), result)

    def test_missing_pair_and_duplicate_pair(self):
        lines = trace().splitlines()
        for changed in (lines[:-1], lines + [lines[-1]]):
            self.assertFalse(report.audit("\n".join(changed))["observer_integrity_passed"])

    def test_combined_sessions_refused(self):
        self.assertFalse(report.audit(trace() + "\n" + trace())["observer_integrity_passed"])

    def test_thread_context_parent_and_action_tampering(self):
        for before, after in (("thread=7", "thread=8"), ("context=200", "context=201"),
                              ("parent=150", "parent=151"), ("action=97", "action=98")):
            text = trace()
            at = text.rfind(before)
            changed = text[:at] + text[at:].replace(before, after, 1)
            self.assertFalse(report.audit(changed)["observer_integrity_passed"], (before, after))

    def test_unreadable_missing_and_unknown_records(self):
        for text in ("", trace().replace("valid=1", "valid=0", 1),
                     trace().replace("phase=ui.init.before", "phase=unknown.before", 1),
                     trace().replace("thread=7", "thread=?", 1)):
            self.assertFalse(report.audit(text)["observer_integrity_passed"])

    def test_no_post_free_or_transition_reads(self):
        text = trace().replace("phase=ui.destroy.after thread=7 object=100 context=0",
                               "phase=ui.destroy.after thread=7 object=100 context=200", 1)
        self.assertFalse(report.audit(text)["observer_integrity_passed"])

    def test_missing_catalogue_is_coverage_failure_only(self):
        result = report.audit(trace().replace("action=97", "action=98"))
        self.assertTrue(result["observer_integrity_passed"], result)
        self.assertFalse(result["requested_coverage_passed"])

    def test_modal_sample_is_not_a_modality_certificate(self):
        result = report.audit(trace().replace("modal=300", "modal=0"))
        self.assertTrue(result["audit_passed"], result)
        self.assertEqual(result["coverage"]["modal_update_samples"], 0)


if __name__ == "__main__":
    unittest.main()
