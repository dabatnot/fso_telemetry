from __future__ import annotations

import sys
import unittest
from pathlib import Path


BACKEND_ROOT = Path(__file__).resolve().parents[2] / "backend"
sys.path.insert(0, str(BACKEND_ROOT))

from av_core.can_protocol import (  # noqa: E402
    LAMP_IDS,
    LampTestTarget,
    decode_caution_state,
    decode_lighting_state,
    decode_node_status,
    decode_warning_state,
    encode_caution_state,
    encode_lamp_test,
    encode_lighting_configuration,
    encode_warning_state,
)
from av_core.models import AvCoreConfig, CockpitStatus, LampTestRequest  # noqa: E402


class CanProtocolTest(unittest.TestCase):
    def test_warning_and_caution_masks_are_canonical(self) -> None:
        cockpit = CockpitStatus(available=True)
        cockpit.warnings.fire = True
        cockpit.warnings.emp = True
        cockpit.cautions.engine.state = "ACTIVE"
        cockpit.cautions.subsystem.state = "ACTIVE"
        self.assertEqual(0x11, decode_warning_state(encode_warning_state(cockpit)))
        cockpit_mask, diagnostic_mask = decode_caution_state(
            encode_caution_state(cockpit, {"FLT_DATA": 2, "THREAT_PROC": 1})
        )
        self.assertEqual(0x101, cockpit_mask)
        self.assertEqual((2 << 2) | (1 << 8), diagnostic_mask)

    def test_lighting_configuration_preserves_colors_rates_and_limit(self) -> None:
        commands = encode_lighting_configuration(AvCoreConfig().lighting)
        self.assertEqual(3, len(commands))
        self.assertTrue(all(len(command) == 8 for command in commands))
        self.assertEqual(bytes((1, 1, 255, 96, 0)), commands[0][:5])
        self.assertEqual(30, commands[2][2])
        self.assertEqual(100, int.from_bytes(commands[2][3:5], "little"))
        self.assertEqual(400, int.from_bytes(commands[2][5:7], "little"))

    def test_lamp_test_is_bounded_to_two_seconds_and_known_lamps(self) -> None:
        payload = encode_lamp_test(LampTestRequest(target="LAMP", lamp="MISSILE"))
        self.assertEqual(LampTestTarget.LAMP, payload[2])
        self.assertEqual(LAMP_IDS["MISSILE"], payload[3])
        self.assertEqual(2000, int.from_bytes(payload[4:6], "little"))

    def test_node_and_lighting_state_decoders_reject_invalid_frames(self) -> None:
        node = decode_node_status(0x700, bytes((1, 0, 0, 4, 0, 0x12, 0x34, 0x56)))
        self.assertEqual("WARN_CTRL", node.role)
        self.assertEqual("0.4.0", node.firmware_version)
        self.assertEqual("563412", node.uid)
        state = decode_lighting_state(bytes((1, 30, 0, 0xFF, 1, 0, 0, 0)))
        self.assertEqual(30, state.brightness_percent)
        idle = decode_lighting_state(bytes((1, 30, 0xFF, 0xFF, 0, 0, 0, 0)))
        self.assertEqual(LampTestTarget.NONE, idle.test_target)
        with self.assertRaises(ValueError):
            decode_warning_state(bytes((1, 0x80, 0, 0, 0, 0, 0, 0)))
        with self.assertRaises(ValueError):
            decode_node_status(0x701, bytes((1, 3, 0, 0, 0, 0, 0, 0)))


if __name__ == "__main__":
    unittest.main()
