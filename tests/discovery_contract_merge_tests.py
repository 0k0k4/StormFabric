"""Evidence merge regression tests; no game execution or writes."""
import importlib.util
import json
from copy import deepcopy
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("contract_merge", ROOT / "tools/discovery_contract_merge.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
merge = module.merge_contracts


def fixture():
    return {"reference_sha256": "reference", "entries": [
        {"rva": 1, "start": 1, "name": "original", "groups": ["video"],
         "bytes": "c3", "mask": "ff", "refs": []}],
        "witnesses": [{"rva": 2, "start": 2, "name": "callee", "groups": [],
                       "bytes": "c3", "mask": "ff", "refs": []}],
        "tables": [{"rva": 100, "type": "monitor", "group": "video",
                    "slots": [{"slot": 0, "target": 1}]}]}


class MergeTests(unittest.TestCase):
    def test_preserves_inputs_names_and_group_membership(self):
        base, extra = fixture(), fixture()
        extra["entries"][0]["name"] = "new name"
        extra["entries"][0]["groups"] = ["targets"]
        before = deepcopy((base, extra))
        result = merge(base, extra)
        self.assertEqual((base, extra), before)
        self.assertEqual(result["entries"][0]["name"], "original")
        self.assertEqual(result["entries"][0]["groups"], ["video", "targets"])
        self.assertEqual(len(result["entries"]), 1)

    def test_promotes_witness_without_duplicate_identity(self):
        extra = fixture()
        extra["witnesses"][0]["groups"] = ["targets"]
        result = merge(fixture(), extra)
        self.assertEqual([e["rva"] for e in result["entries"]], [1, 2])
        self.assertEqual(result["witnesses"], [])

    def test_rejects_changed_reference(self):
        extra = fixture(); extra["reference_sha256"] = "different"
        with self.assertRaises(ValueError):
            merge(fixture(), extra)

    def test_rejects_weakened_or_changed_contract(self):
        for field, value in (("start", 3), ("bytes", "90"), ("mask", "00"),
                             ("refs", [{"offset": 0, "end": 4, "target": 8, "flow": True}])):
            with self.subTest(field=field):
                extra = fixture(); extra["entries"][0][field] = value
                with self.assertRaises(ValueError):
                    merge(fixture(), extra)

    def test_rejects_duplicate_base_identity(self):
        base = fixture(); base["witnesses"].append(deepcopy(base["entries"][0]))
        with self.assertRaises(ValueError):
            merge(base, fixture())

    def test_combines_vtable_slots_without_losing_old_requirements(self):
        extra = fixture()
        extra["tables"][0]["group"] = "targets"
        extra["tables"][0]["slots"] = [{"slot": 89, "target": 2}]
        result = merge(fixture(), extra)
        self.assertEqual(result["tables"][0]["groups"], ["video", "targets"])
        self.assertEqual(result["tables"][0]["slots"],
                         [{"slot": 0, "target": 1}, {"slot": 89, "target": 2}])

    def test_rejects_changed_rtti_and_conflicting_slot(self):
        for field, value in (("type", "different"), ("slots", [{"slot": 0, "target": 2}])):
            extra = fixture(); extra["tables"][0][field] = value
            with self.assertRaises(ValueError):
                merge(fixture(), extra)


if __name__ == "__main__":
    unittest.main()
