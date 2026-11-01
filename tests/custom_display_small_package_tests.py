from pathlib import Path
import sys
import unittest

sys.path.insert(0,str(Path(__file__).resolve().parents[1] / "tools"))
from package_custom_display_small_experiment import prepare_vehicle, IDENTITY


class VehicleFixtureTests(unittest.TestCase):
    def fixture(self):
        return (b'<vehicle data_version="3"><bodies><body><components>'
                b'<c d="swml_cd_probe_v1_1x1"><o><vp y="1"/></o></c>'
                b'<c d="camera_small"/><c d="battery_medium"/><c d="gate_bool_constant"/>'
                b'</components></body></bodies><logic_node_links><logic_node_link type="6">'
                b'<voxel_pos_0 x="-1"/><voxel_pos_1 y="1"/></logic_node_link></logic_node_links></vehicle>')

    def test_only_definition_identity_replaced_no_wiring_rewrite(self):
        raw = self.fixture()
        result = prepare_vehicle(raw)
        self.assertEqual(result.replace(IDENTITY.encode(),b"swml_cd_probe_v1_1x1"),raw)
        self.assertIn(b'<voxel_pos_0 x="-1"/>',result)

    def test_witness_required_once(self):
        raw = self.fixture()
        for data in (raw.replace(b'swml_cd_probe_v1_1x1',b'monitor_1'),
                     raw.replace(b'<c d="camera_small"/>',b'<c d="camera_small"/><c d="swml_cd_probe_v1_1x1"/>')):
            with self.assertRaises(ValueError): prepare_vehicle(data)

    def test_connected_camera_required(self):
        for data in (self.fixture().replace(b'camera_small',b'monitor_1'),
                     self.fixture().replace(b'logic_node_link type',b'unrelated type').replace(b'</logic_node_link>',b'</unrelated>')):
            with self.assertRaises(ValueError): prepare_vehicle(data)

    def test_unexpected_definition_refused(self):
        for definition in (b'swml_cd_other',b'unknown_custom_component'):
            with self.assertRaises(ValueError):
                prepare_vehicle(self.fixture().replace(b'battery_medium',definition))

    def test_wrong_root_and_large_input_refused(self):
        for data in (b'<other/>',b' '*(8*1024*1024+1)):
            with self.assertRaises(ValueError): prepare_vehicle(data)


if __name__ == "__main__":
    unittest.main()
