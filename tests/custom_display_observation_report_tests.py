import importlib.util
from pathlib import Path
import unittest

SPEC = importlib.util.spec_from_file_location("cd_report", Path(__file__).resolve().parents[1] /
                                            "tools/custom_display_observation_report.py")
report = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(report)


def fixture():
    rows = ["CDOBS: PASS-THROUGH observation enabled"]
    for event, quality in enumerate((1, 2, 3)):
        rows.append(f"CDOBS event={event} native_stack=+0x1234 caller=0000000000001234")
        for phase in ("before", "after"):
            after = phase == "after"
            rows.append(f"CDOBS event={event} phase=create.{phase} thread={event + 10} "
                        f"monitor=0000000000000040 queue=0000000000000020 quality={quality} "
                        f"caller=0000000000001234 valid=1 def=0000000000000030 "
                        f"id={report.WITNESS_ID} archive={report.WITNESS_ARCHIVE} "
                        f"pixels=32x32 scale={({1: 1, 2: 2, 3: 4}[quality] if after else 1)} "
                        f"video={('0000000000000010/0000000000000020' if after else '0000000000000000/0000000000000000')} "
                        f"overlay={('0000000000000018/0000000000000020' if after else '0000000000000000/0000000000000000')}")
    return "\n".join(rows)


def small_fixture():
    text = (fixture().replace("PASS-THROUGH", "SMALL-RESOLUTION-EXPERIMENT")
            .replace(report.WITNESS_ID, report.SMALL_ID)
            .replace(report.WITNESS_ARCHIVE, report.SMALL_ARCHIVE)
            .replace("pixels=32x32", "pixels=24x16"))
    rows = [report.SMALL_ENABLE]
    charge = 0
    for row in text.splitlines():
        if "phase=create.after" in row:
            event = int(row.split("event=")[1].split()[0])
            quality = event + 1
            scale = {1: 1, 2: 2, 3: 4}[quality]
            charge += 24 * 16 * 12 * (scale ** 2 + 1)
            rows.append(f"CDRES experiment=24x16 result=0 monitor=0000000000000040 "
                        f"queue=0000000000000020 quality={quality} thread={event + 10} "
                        f"lifetime_charge={charge}")
        rows.append(row)
    return "\n".join(rows)


class ReportTests(unittest.TestCase):
    def test_valid_multithread_observation(self):
        result = report.audit(fixture())
        self.assertTrue(result["audit_passed"], result["errors"])
        self.assertEqual(result["threads"], [10, 11, 12])
        self.assertEqual(result["events"], 3)
        self.assertTrue(result["witness_quality_coverage"])
        self.assertTrue(result["limitations"])

    def test_empty_log_cannot_pass(self):
        self.assertFalse(report.audit("")["audit_passed"])

    def test_truncated_log_cannot_pass(self):
        self.assertFalse(report.audit("\n".join(fixture().splitlines()[:-1]))["audit_passed"])

    def test_duplicate_phase_rejected(self):
        self.assertFalse(report.audit(fixture() + "\n" + fixture().splitlines()[-1])["audit_passed"])

    def test_multiple_sessions_not_merged(self):
        self.assertFalse(report.audit(fixture() + "\n" + fixture())["audit_passed"])

    def test_malformed_or_stale_evidence_rejected(self):
        for old, new in (("valid=1", "valid=0"), ("pixels=32x32", "pixels=64x32"),
                         (report.WITNESS_ARCHIVE, "f" * 32), ("quality=2", "quality=1"),
                         ("thread=10", "thread=garbage"), ("phase=create.after", "phase=release.after")):
            with self.subTest(new=new):
                self.assertFalse(report.audit(fixture().replace(old, new))["audit_passed"])

    def test_manager_thread_identity_change_in_call_rejected(self):
        for old, new in (("thread=10", "thread=99"), ("id=swml", "id=other"),
                         ("/0000000000000020", "/0000000000000099")):
            rows = fixture().splitlines()
            rows[3] = rows[3].replace(old, new)
            self.assertFalse(report.audit("\n".join(rows))["audit_passed"])

    def test_unrelated_game_log_lines_ignored(self):
        self.assertTrue(report.audit("hello\n" + fixture() + "\nworld")["audit_passed"])

    def test_small_experiment_requires_its_own_header_identity_and_dimensions(self):
        text = small_fixture()
        self.assertTrue(report.audit(text,small=True)["audit_passed"])
        self.assertFalse(report.audit(text)["audit_passed"])
        self.assertFalse(report.audit(fixture(),small=True)["audit_passed"])
        self.assertFalse(report.audit(text.replace("pixels=24x16","pixels=32x32"),small=True)["audit_passed"])

    def test_basic_success_does_not_claim_full_quality_coverage(self):
        rows = [row for row in small_fixture().splitlines()
                if not any(token in row for token in ("event=1 ", "quality=2 "))]
        # Preserve contiguous IDs, and recompute the actual lifetime contribution.
        text = "\n".join(rows).replace("event=2 ", "event=1 ")
        text = text.replace("lifetime_charge=110592", "lifetime_charge=87552")
        result = report.audit(text, small=True)
        self.assertTrue(result["lifecycle_integrity_passed"], result["errors"])
        self.assertFalse(result["audit_passed"])
        self.assertEqual(result["missing_specimen_qualities"], [2])
        self.assertEqual(result["admitted_target_bytes"], 87552)

    def test_resolution_admission_required(self):
        for text in ("\n".join(row for row in small_fixture().splitlines() if "CDRES experiment=" not in row),
                     small_fixture().replace(report.SMALL_ENABLE, "")):
            result = report.audit(text, small=True)
            self.assertFalse(result["audit_passed"])
            self.assertFalse(result["lifecycle_integrity_passed"])

    def test_resolution_admission_corruption_rejected(self):
        for old, new in (("result=0", "result=6"), ("lifetime_charge=110592", "lifetime_charge=110593"),
                         ("lifetime_charge=9216", "lifetime_charge=0"),
                         ("lifetime_charge=9216", "lifetime_charge=8388609"),
                         ("verified and pinned", "unverified")):
            with self.subTest(new=new):
                result = report.audit(small_fixture().replace(old, new), small=True)
                self.assertFalse(result["lifecycle_integrity_passed"], result)

    def test_resolution_admission_cannot_belong_to_another_call(self):
        for old, new in (("thread=10", "thread=99"), ("quality=1", "quality=3"),
                         ("monitor=0000000000000040", "monitor=0000000000000090"),
                         ("queue=0000000000000020", "queue=0000000000000090")):
            rows = small_fixture().splitlines()
            index = next(i for i, row in enumerate(rows) if "CDRES experiment=" in row)
            rows[index] = rows[index].replace(old, new)
            self.assertFalse(report.audit("\n".join(rows), small=True)["lifecycle_integrity_passed"])

    def test_duplicate_or_orphan_resolution_admission_rejected(self):
        rows = small_fixture().splitlines()
        admission = next(row for row in rows if "CDRES experiment=" in row)
        for text in (small_fixture() + "\n" + admission,
                     small_fixture().replace(admission, admission + "\n" + admission)):
            self.assertFalse(report.audit(text, small=True)["lifecycle_integrity_passed"])

    def test_charge_snapshots_need_not_follow_log_order_between_threads(self):
        # Producer 10 can log after producer 11 has already reserved its pair.
        # A snapshot is a global load, not a per-call allocation receipt.
        text = small_fixture().replace("lifetime_charge=9216", "lifetime_charge=32256")
        self.assertTrue(report.audit(text, small=True)["audit_passed"])

    def test_specimen_id_not_overwritten_by_sorted_definitions(self):
        rows = fixture().splitlines()
        rows = [row.replace(report.WITNESS_ID, "zzz_unrelated") if "event=2 " in row else row
                for row in rows]
        self.assertEqual(report.audit("\n".join(rows))["specimen_id"], report.WITNESS_ID)


if __name__ == "__main__":
    unittest.main()
