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

    def test_real_retained_evidence(self):
        base = json.loads((ROOT / "tools/discovery_contracts.json").read_text())
        extra = json.loads((ROOT / "tools/custom_display_runtime_contracts.json").read_text())
        result = merge(base, extra)
        all_entries = result["entries"] + result["witnesses"]
        self.assertEqual(len(all_entries), len({e["rva"] for e in all_entries}))
        by_key = {e["rva"]: e for e in all_entries}
        self.assertIn("custom_display_targets", by_key[0x366010]["groups"])
        self.assertIn("video", by_key[0x366010]["groups"])
        self.assertIn("custom_display_targets", by_key[0x20da70]["groups"])
        self.assertIn("custom_display_registration", by_key[0x316770]["groups"])
        table = next(t for t in result["tables"] if t["rva"] == 0xb70ea0)
        self.assertEqual(table["slots"], [{"slot": 0, "target": 0x367c80},
                                          {"slot": 89, "target": 0x366010},
                                          {"slot": 90, "target": 0x366100}])

    def test_widget_contracts_extend_without_weakening_existing_evidence(self):
        base = json.loads((ROOT / "tools/discovery_contracts.json").read_text())
        base = merge(base, json.loads((ROOT / "tools/custom_display_runtime_contracts.json").read_text()))
        widgets = json.loads((ROOT / "tools/custom_display_dialog_widget_contracts.json").read_text())
        self.assertEqual(len(widgets["entries"]), 29)
        self.assertEqual(len(widgets["witnesses"]), 37)
        self.assertEqual(len(widgets["tables"]), 3)
        result = merge(base, widgets)
        by_key = {e["rva"]: e for e in result["entries"] + result["witnesses"]}
        for entry in base["entries"] + base["witnesses"]:
            actual = by_key[entry["rva"]]
            for field in ("bytes", "mask", "refs", "start", "name"):
                self.assertEqual(actual[field], entry[field])
            self.assertTrue(set(entry["groups"]) <= set(actual["groups"]))
        for entry in widgets["entries"]:
            self.assertIn("custom_display_dialog_widgets", by_key[entry["rva"]]["groups"])
        self.assertEqual(len(by_key), len(result["entries"]) + len(result["witnesses"]))
        basic = next(t for t in result["tables"] if t["rva"] == 0xb0b5c8)
        self.assertIn({"slot": 37, "target": 0x250300}, basic["slots"])
        self.assertIn({"slot": 21, "target": 0x25a9e0}, basic["slots"])

    def test_catalogue_route_keeps_selection_getter_authenticated(self):
        base = json.loads((ROOT / "tools/discovery_contracts.json").read_text())
        for file in ("custom_display_runtime_contracts.json", "custom_display_dialog_widget_contracts.json",
                     "custom_display_vehicle_observer_contracts.json", "custom_display_vehicle_activity_contracts.json",
                     "custom_display_dialog_construction_contracts.json"):
            base = merge(base, json.loads((ROOT / "tools" / file).read_text()))
        extra = json.loads((ROOT / "tools/custom_display_catalogue_contracts.json").read_text())
        result = merge(base, extra)
        by_key = {e["rva"]: e for e in result["entries"] + result["witnesses"]}
        self.assertIn("custom_display_catalogue_route", by_key[0x611050]["groups"])
        self.assertIn("custom_display_catalogue_route", by_key[0x7fa7a0]["groups"])
        for entry in base["entries"] + base["witnesses"]:
            for field in ("bytes", "mask", "refs", "start", "name"):
                self.assertEqual(by_key[entry["rva"]][field], entry[field])
            self.assertTrue(set(entry["groups"]) <= set(by_key[entry["rva"]]["groups"]))

    def test_dialog_construction_preserves_prior_contracts(self):
        base = json.loads((ROOT / "tools/discovery_contracts.json").read_text())
        for file in ("custom_display_runtime_contracts.json", "custom_display_dialog_widget_contracts.json",
                     "custom_display_vehicle_observer_contracts.json", "custom_display_vehicle_activity_contracts.json"):
            base = merge(base, json.loads((ROOT / "tools" / file).read_text()))
        extra = json.loads((ROOT / "tools/custom_display_dialog_construction_contracts.json").read_text())
        self.assertEqual(len(extra["entries"]), 15)
        self.assertEqual(len(extra["witnesses"]), 44)
        result = merge(base, extra)
        by_key = {e["rva"]: e for e in result["entries"] + result["witnesses"]}
        for entry in base["entries"] + base["witnesses"]:
            for field in ("bytes", "mask", "refs", "start", "name"):
                self.assertEqual(by_key[entry["rva"]][field], entry[field])
            self.assertTrue(set(entry["groups"]) <= set(by_key[entry["rva"]]["groups"]))
        basic = next(t for t in result["tables"] if t["rva"] == 0xb0b5c8)
        self.assertIn({"slot": 37, "target": 0x250300}, basic["slots"])
        self.assertIn("custom_display_dialog_construction", basic["groups"])

    def test_vehicle_activity_preserves_prior_groups_and_slots(self):
        base = json.loads((ROOT / "tools/discovery_contracts.json").read_text())
        for file in ("custom_display_runtime_contracts.json", "custom_display_dialog_widget_contracts.json",
                     "custom_display_vehicle_observer_contracts.json"):
            base = merge(base, json.loads((ROOT / "tools" / file).read_text()))
        extra = json.loads((ROOT / "tools/custom_display_vehicle_activity_contracts.json").read_text())
        self.assertEqual(len(extra["entries"]), 3)
        self.assertEqual(len(extra["witnesses"]), 39)
        result = merge(base, extra)
        by_key = {e["rva"]: e for e in result["entries"] + result["witnesses"]}
        for entry in base["entries"] + base["witnesses"]:
            for field in ("bytes", "mask", "refs", "start", "name"):
                self.assertEqual(by_key[entry["rva"]][field], entry[field])
            self.assertTrue(set(entry["groups"]) <= set(by_key[entry["rva"]]["groups"]))
        state = next(t for t in result["tables"] if t["rva"] == 0xb44838)
        self.assertEqual({s["slot"] for s in state["slots"]}, {2, 3, 4, 20})
        self.assertEqual(set(state["groups"]), {"custom_display_vehicle_observer", "custom_display_vehicle_activity"})

    def test_native_input_state_preserves_all_previous_contracts(self):
        base = json.loads((ROOT / "tools/discovery_contracts.json").read_text())
        for file in ("custom_display_runtime_contracts.json", "custom_display_dialog_widget_contracts.json",
                     "custom_display_vehicle_observer_contracts.json", "custom_display_vehicle_activity_contracts.json",
                     "custom_display_dialog_construction_contracts.json", "custom_display_catalogue_contracts.json",
                     "custom_display_dialog_callback_contracts.json", "custom_display_dialog_input_contracts.json"):
            base = merge(base, json.loads((ROOT / "tools" / file).read_text()))
        extra = json.loads((ROOT / "tools/custom_display_native_input_state_contracts.json").read_text())
        self.assertEqual(len(extra["entries"]), 4)
        result = merge(base, extra)
        by_key = {e["rva"]: e for e in result["entries"] + result["witnesses"]}
        for entry in base["entries"] + base["witnesses"]:
            actual = by_key[entry["rva"]]
            for field in ("bytes", "mask", "refs", "start", "name"):
                self.assertEqual(actual[field], entry[field])
            self.assertTrue(set(entry["groups"]) <= set(actual["groups"]))
        self.assertEqual(result["tables"], base["tables"])
        self.assertEqual(len(result["entries"]), 147)
        self.assertEqual(len(result["witnesses"]), 481)
        self.assertEqual(len(by_key), 628)
        for key in (0x1bf810, 0x97090, 0x1e2e20, 0x1e1bc0):
            self.assertIn("custom_display_native_input_state", by_key[key]["groups"])

    def test_vehicle_observer_only_adds_evidence(self):
        base = json.loads((ROOT / "tools/discovery_contracts.json").read_text())
        for file in ("custom_display_runtime_contracts.json", "custom_display_dialog_widget_contracts.json"):
            base = merge(base, json.loads((ROOT / "tools" / file).read_text()))
        extra = json.loads((ROOT / "tools/custom_display_vehicle_observer_contracts.json").read_text())
        self.assertEqual(len(extra["entries"]), 6)
        self.assertEqual(len(extra["witnesses"]), 269)
        result = merge(base, extra)
        by_key = {e["rva"]: e for e in result["entries"] + result["witnesses"]}
        for entry in base["entries"] + base["witnesses"]:
            for field in ("bytes", "mask", "refs", "start", "name"):
                self.assertEqual(by_key[entry["rva"]][field], entry[field])
            self.assertTrue(set(entry["groups"]) <= set(by_key[entry["rva"]]["groups"]))
        ui = next(t for t in result["tables"] if t["rva"] == 0xb99940)
        self.assertEqual([s["slot"] for s in ui["slots"]], [0, 1, 2, 3])
        state = next(t for t in result["tables"] if t["rva"] == 0xb44838)
        self.assertEqual(state["slots"], [{"slot": 2, "target": 0x7cf750},
                                          {"slot": 20, "target": 0x7fa7a0}])


if __name__ == "__main__":
    unittest.main()
