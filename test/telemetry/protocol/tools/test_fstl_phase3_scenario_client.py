#!/usr/bin/env python3
"""Focused Phase 3 replica tests for the independent FSTL client."""

from __future__ import annotations

import copy
import math
import struct
import sys
import unittest
from pathlib import Path
from typing import Any


TOOLS = Path(__file__).resolve().parent
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

import fstl_client_core as client
import fstl_reference_decoder as reference


def encoded_record(
    record_type: int, payload: bytes, *, version: int = 1
) -> bytes:
    return struct.pack("<HBBH", record_type, version, 0, len(payload)) + payload


def vlist_item(payload: bytes) -> bytes:
    return struct.pack("<BH", 1, len(payload)) + payload


def utf8(value: str) -> bytes:
    encoded = value.encode("utf-8")
    return struct.pack("<H", len(encoded)) + encoded


def radar_contact(
    contact_entity_id: int,
    *,
    visibility: int,
    flags: int = 0,
) -> dict[str, Any]:
    return {
        "recordName": "RADAR_CONTACTS",
        "recordType": 18,
        "recordVersion": 1,
        "recordFlags": flags,
        "recordLength": 0,
        "fields": {
            "entity_id": "1",
            "contact_entity_id": str(contact_entity_id),
            "visibility": visibility,
        },
    }


class FstlPhase3ScenarioClientTest(unittest.TestCase):
    def test_track_geometry_is_derived_from_raw_contact_and_flight_atoms(self) -> None:
        state = client.ConsoleState()
        state._apply_records([
            {"recordName": "FLIGHT_STATE", "recordType": 7, "recordVersion": 1, "recordFlags": 0,
             "recordLength": 0, "fields": {"entity_id": "1", "position_world": [0.0, 0.0, 0.0],
             "velocity_world": [1.0, 0.0, 0.0]}},
            {**radar_contact(101, visibility=1), "fields": {"entity_id": "1", "contact_entity_id": "101",
             "visibility": 1, "position_world": [3.0, 4.0, 0.0], "velocity_world": [4.0, 0.0, 0.0]}},
        ], True)
        derived = client.DashboardProjection(state, at_us=0).build()["derived"]
        self.assertEqual([3.0, 4.0, 0.0],
                         derived["entities.1.tracks.101.relative_position"]["value"])
        self.assertAlmostEqual(math.atan2(4.0, 3.0),
                               derived["entities.1.tracks.101.bearing_rad"]["value"])
        self.assertEqual(0.0, derived["entities.1.tracks.101.elevation_rad"]["value"])
        self.assertEqual(5.0, derived["entities.1.tracks.101.distance"]["value"])
        self.assertEqual(3.0, derived["entities.1.tracks.101.relative_speed"]["value"])
        self.assertAlmostEqual(-1.8, derived["entities.1.tracks.101.closing_speed"]["value"])
        self.assertFalse(derived["entities.1.tracks.101.ttc_s"]["available"])

    def test_track_ttc_and_navigation_eta_are_derived_from_positions_and_velocity(self) -> None:
        state = client.ConsoleState()
        state._apply_records([
            {"recordName": "FLIGHT_STATE", "recordType": 7, "recordVersion": 1, "recordFlags": 0,
             "recordLength": 0, "fields": {"entity_id": "1", "position_world": [0.0, 0.0, 0.0],
             "velocity_world": [2.0, 0.0, 0.0]}},
            {**radar_contact(101, visibility=1), "fields": {"entity_id": "1", "contact_entity_id": "101",
             "visibility": 1, "position_world": [10.0, 0.0, 0.0], "velocity_world": [1.0, 0.0, 0.0]}},
            {"recordName": "NAVIGATION_STATE", "recordType": 23, "recordVersion": 1,
             "recordFlags": 0, "recordLength": 0, "fields": {"entity_id": "1", "navpoints": [
                 {"navpoint_id": 7, "position_world": [20.0, 0.0, 0.0]}
             ]}},
        ], True)
        derived = client.DashboardProjection(state, at_us=0).build()["derived"]
        self.assertEqual(10.0, derived["entities.1.tracks.101.ttc_s"]["value"])
        self.assertEqual(20.0, derived["entities.1.navpoints.7.distance"]["value"])
        self.assertEqual(10.0, derived["entities.1.navpoints.7.eta_s"]["value"])

    def test_sensor_integrity_ratio_is_derived_without_visibility_inference(self) -> None:
        state = client.ConsoleState()
        state._apply_records([
            {"recordName": "RADAR_STATE", "recordType": 17, "recordVersion": 1,
             "recordFlags": 0, "recordLength": 0,
             "fields": {"entity_id": "1", "sensor_current_hits": 25.0,
                        "sensor_max_hits": 100.0}},
        ], True)
        derived = client.DashboardProjection(state, at_us=0).build()["derived"]
        ratio = derived["entities.1.sensor_integrity_ratio"]
        self.assertTrue(ratio["available"])
        self.assertEqual(0.25, ratio["value"])
        self.assertNotIn("visibility", ratio,
                         "the client must not turn sensor health into an authorization decision")

    def test_reference_decoder_accepts_nonempty_phase3_records(self) -> None:
        lock_item = (
            struct.pack("<HBBQI", 0x0003, 0, 1, 91, 7)
            + struct.pack("<fffQ", 1.0, 2.0, 3.0, 500_000)
        )
        lock = reference.decode_record(
            encoded_record(
                15,
                struct.pack("<QQQH", 1, 0, 42, 1)
                + vlist_item(lock_item),
            )
        )
        self.assertEqual("91", lock["fields"]["locks"][0]["target_entity_id"])
        self.assertEqual("42", lock["fields"]["producer_sample_time_us"])

        target_presence = 0x0002 | 0x0004 | 0x0040 | 0x0100 | 0x0200
        target_payload = (
            struct.pack("<QQQQB", 1, target_presence, 42, 91, 1)
            + utf8("Alpha 1")
            + struct.pack("<IIIQB", 3, 4, 5, 1_000_000, 2)
            + struct.pack("<BfffI", 1, 10.0, 20.0, 30.0, 17)
        )
        target = reference.decode_record(encoded_record(16, target_payload))
        self.assertEqual(17, target["fields"]["lead_bank_id"])
        self.assertEqual("Alpha 1", target["fields"]["revealed_identity"]["name"])
        self.assertEqual("42", target["fields"]["producer_sample_time_us"])

        contact_presence = 0x0002 | 0x0004
        contact_payload = (
            struct.pack("<QQQQBBB", 1, 91, contact_presence, 42, 1, 0, 1)
            + struct.pack(
                "<fffffffffffI",
                1.0, 2.0, 3.0,
                4.0, 5.0, 6.0,
                10.0, -20.0, 30.0,
                40.0,
                7.0,
                0,
            )
            + utf8("Alpha 1")
            + struct.pack("<I", 3)
        )
        contact = reference.decode_record(
            encoded_record(18, contact_payload, version=2)
        )
        self.assertEqual(3, contact["fields"]["revealed_class_id"])
        self.assertEqual("42", contact["fields"]["producer_sample_time_us"])
        self.assertEqual(
            [10.0, -20.0, 30.0],
            contact["fields"]["radar_local_position"],
        )
        self.assertEqual(
            40.0, contact["fields"]["radar_projection_distance"]
        )

        missile_item = (
            struct.pack("<HBBQIQ", 0, 3, 1, 101, 22, 1)
            + struct.pack(
                "<ffffffffff",
                1.0,
                2.0,
                3.0,
                1.0,
                0.0,
                0.0,
                0.0,
                4.0,
                5.0,
                6.0,
            )
        )
        threat_payload = (
            struct.pack("<QQQBQH", 1, 0x0002, 42, 2, 101, 1)
            + vlist_item(missile_item)
        )
        threat = reference.decode_record(encoded_record(19, threat_payload))
        self.assertEqual(22, threat["fields"]["incoming_missiles"][0]["weapon_class_id"])
        self.assertEqual("42", threat["fields"]["producer_sample_time_us"])

        nav_item = (
            struct.pack("<HBBI", 0, 0, 1, 9)
            + utf8("Nav Alpha")
            + struct.pack("<fff", 1.0, 2.0, 3.0)
        )
        route = struct.pack("<IHHfff", 8, 2, 0, 4.0, 5.0, 6.0)
        nav_payload = (
            struct.pack("<QQQBH", 1, 0x0005, 42, 1, 1)
            + vlist_item(nav_item)
            + struct.pack("<IHBH", 9, 1, 1, 20)
            + route
            + struct.pack("<Hf", 0, 25.0)
        )
        navigation = reference.decode_record(encoded_record(23, nav_payload))
        self.assertEqual("Nav Alpha", navigation["fields"]["navpoints"][0]["name"])
        self.assertEqual(25.0, navigation["fields"]["route_speed_limit"])
        self.assertEqual("42", navigation["fields"]["producer_sample_time_us"])

    def test_phase3_raw_inventory_keeps_wire_provenance_and_sample_times(self) -> None:
        state = client.ConsoleState()
        records = [
            {"recordName": "LOCK_STATE", "recordType": 15, "recordVersion": 1,
             "recordFlags": 0, "recordLength": 0,
             "fields": {"entity_id": "1", "producer_sample_time_us": "101"}},
            {"recordName": "TARGET_STATE", "recordType": 16, "recordVersion": 1,
             "recordFlags": 0, "recordLength": 0,
             "fields": {"entity_id": "1", "producer_sample_time_us": "102"}},
            {**radar_contact(101, visibility=1),
             "fields": {"entity_id": "1", "contact_entity_id": "101",
                        "visibility": 1, "producer_sample_time_us": "103"}},
            {"recordName": "THREAT_STATE", "recordType": 19, "recordVersion": 1,
             "recordFlags": 0, "recordLength": 0,
             "fields": {"entity_id": "1", "producer_sample_time_us": "104"}},
            {"recordName": "NAVIGATION_STATE", "recordType": 23, "recordVersion": 1,
             "recordFlags": 0, "recordLength": 0,
             "fields": {"entity_id": "1", "producer_sample_time_us": "105"}},
        ]
        state._apply_records(records, True)
        inventory = client.DashboardProjection(state, at_us=0).build()["inventory"]
        leaves = {entry["path"]: entry for entry in inventory if entry["kind"] == "A"}

        expected = {
            "LOCK_STATE": "101", "TARGET_STATE": "102", "RADAR_CONTACTS": "103",
            "THREAT_STATE": "104", "NAVIGATION_STATE": "105",
        }
        for record_name, sample_time in expected.items():
            matching = [entry for path, entry in leaves.items()
                        if path.endswith(".producer_sample_time_us")
                        and f"records.{record_name}/" in path]
            self.assertEqual(1, len(matching), record_name)
            self.assertEqual(sample_time, matching[0]["value"])
            self.assertEqual(f"wire:{record_name}:v1", matching[0]["source"])

    def test_wire_delta_delete_decodes_key_only_radar_contact(self) -> None:
        key = struct.pack("<QQ", 1, 101)
        record = struct.pack("<HBBH", 18, 1, client.RECORD_FLAG_DELETE, len(key)) + key
        payload = struct.pack("<IIQHH", 7, 1, 42, 1, 0) + record

        decoded = reference.decode_message(7, 0, payload, {})
        deleted = decoded["fields"]["records"][0]
        self.assertEqual(client.RECORD_FLAG_DELETE, deleted["recordFlags"])
        self.assertEqual(
            {"contact_entity_id": "101", "entity_id": "1"},
            deleted["fields"],
        )

        state = client.ConsoleState()
        state._apply_records([radar_contact(101, visibility=1)], True)
        state.baseline_record_instances = copy.deepcopy(state.record_instances)
        state.baseline_records = copy.deepcopy(state.records)
        state._apply_cumulative_delta_records([deleted])
        self.assertEqual({}, state.record_instances)

    def test_contacts_keep_distinct_atom_identities(self) -> None:
        state = client.ConsoleState()
        state._apply_records(
            [
                radar_contact(101, visibility=1),
                radar_contact(202, visibility=2),
            ],
            True,
        )

        self.assertEqual(
            {
                "RADAR_CONTACTS/entity_id=1/contact_entity_id=101",
                "RADAR_CONTACTS/entity_id=1/contact_entity_id=202",
            },
            set(state.record_instances),
        )
        self.assertEqual(
            "202",
            state.records["RADAR_CONTACTS"]["contact_entity_id"],
            "the legacy name-only view stays deterministic without owning identity",
        )

    def test_cumulative_delta_deletes_from_baseline_and_replaces_atom(self) -> None:
        state = client.ConsoleState()
        state._apply_records(
            [
                radar_contact(101, visibility=1),
                radar_contact(202, visibility=2),
            ],
            True,
        )
        state.baseline_record_instances = copy.deepcopy(state.record_instances)
        state.baseline_records = copy.deepcopy(state.records)

        state._apply_cumulative_delta_records(
            [
                radar_contact(
                    101,
                    visibility=1,
                    flags=client.RECORD_FLAG_DELETE,
                ),
                radar_contact(202, visibility=1),
            ]
        )

        self.assertNotIn(
            "RADAR_CONTACTS/entity_id=1/contact_entity_id=101",
            state.record_instances,
        )
        retained = state.record_instances[
            "RADAR_CONTACTS/entity_id=1/contact_entity_id=202"
        ]
        self.assertEqual(1, retained["fields"]["visibility"])
        self.assertEqual(0, retained["recordFlags"])
        self.assertEqual(
            retained["fields"], state.records["RADAR_CONTACTS"]
        )

        state._apply_cumulative_delta_records(
            [radar_contact(101, visibility=2)]
        )
        self.assertEqual(
            {
                "RADAR_CONTACTS/entity_id=1/contact_entity_id=101",
                "RADAR_CONTACTS/entity_id=1/contact_entity_id=202",
            },
            set(state.record_instances),
            "each cumulative delta is rebuilt from the immutable baseline",
        )
        self.assertEqual(
            2,
            state.record_instances[
                "RADAR_CONTACTS/entity_id=1/contact_entity_id=101"
            ]["fields"]["visibility"],
        )
        self.assertEqual(
            2,
            state.record_instances[
                "RADAR_CONTACTS/entity_id=1/contact_entity_id=202"
            ]["fields"]["visibility"],
            "omitted atoms retain their baseline value, not a prior delta value",
        )

    def test_lost_phase3_delta_converges_without_a_dangling_contact(self) -> None:
        state = client.ConsoleState()
        state._apply_records(
            [radar_contact(101, visibility=1), radar_contact(202, visibility=1)],
            True,
        )
        state.baseline_record_instances = copy.deepcopy(state.record_instances)
        state.baseline_records = copy.deepcopy(state.records)

        # A replaceable Phase 3 delta is lost in transit.  It must not mutate
        # the replica, and the next cumulative delta is applied to the ACKed
        # baseline rather than to speculative state from the lost packet.
        lost_delta = [radar_contact(101, visibility=2)]
        self.assertEqual(2, lost_delta[0]["fields"]["visibility"])

        state._apply_cumulative_delta_records(
            [
                radar_contact(101, visibility=1, flags=client.RECORD_FLAG_DELETE),
                radar_contact(202, visibility=2),
            ]
        )
        self.assertEqual(
            {"RADAR_CONTACTS/entity_id=1/contact_entity_id=202"},
            set(state.record_instances),
        )
        self.assertEqual(
            2,
            state.record_instances[
                "RADAR_CONTACTS/entity_id=1/contact_entity_id=202"
            ]["fields"]["visibility"],
        )

    def test_full_snapshot_rejects_delete_atom(self) -> None:
        state = client.ConsoleState()
        state._apply_records([radar_contact(202, visibility=2)], True)
        with self.assertRaisesRegex(
            ValueError, "DELETE is not valid in a full snapshot"
        ):
            state._apply_records(
                [
                    radar_contact(
                        101,
                        visibility=1,
                        flags=client.RECORD_FLAG_DELETE,
                    )
                ],
                True,
            )
        self.assertEqual(
            {"RADAR_CONTACTS/entity_id=1/contact_entity_id=202"},
            set(state.record_instances),
            "a rejected snapshot must not replace the last coherent image",
        )


if __name__ == "__main__":
    unittest.main()
