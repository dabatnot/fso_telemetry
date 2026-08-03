import { describe, expect, it } from "vitest";
import catalogData from "./instrument-catalog.json";
import { CONTROL_FLAGS, PHYSICS_MODE_FLAGS, controlModeLabel, decodeFlags } from "./flightSemantics";
import type { InstrumentDefinition } from "./types";

describe("FSTL flight semantics", () => {
  it("decodes closed physics and control flag registers", () => {
    expect(decodeFlags(0x21, CONTROL_FLAGS)).toEqual(["MATCH SPEED", "AFTERBURNER DEMANDÉ"]);
    expect(decodeFlags(0x804, PHYSICS_MODE_FLAGS)).toEqual(["GLIDE", "ORIENTATION VERROUILLÉE"]);
    expect(decodeFlags(0, CONTROL_FLAGS)).toEqual([]);
    expect(decodeFlags(Number.NaN, CONTROL_FLAGS)).toBeNull();
  });

  it("labels every closed control mode", () => {
    expect([0, 1, 2, 3, 4].map(controlModeLabel)).toEqual([
      "INCONNU", "VAISSEAU", "VUE", "FLIGHT CURSOR", "AUTOPILOTE"
    ]);
  });
});

describe("Pilotage catalog coverage", () => {
  it("classifies every known FLIGHT_STATE and CONTROL_STATE field", () => {
    const expected = [
      "FLIGHT_STATE.entity_id", "FLIGHT_STATE.presence", "FLIGHT_STATE.producer_sample_time_us",
      "FLIGHT_STATE.position_world", "FLIGHT_STATE.orientation_local_to_world", "FLIGHT_STATE.velocity_world",
      "FLIGHT_STATE.rotational_velocity_local", "FLIGHT_STATE.radius", "FLIGHT_STATE.physics_mode_flags",
      "FLIGHT_STATE.desired_velocity_world",
      "CONTROL_STATE.entity_id", "CONTROL_STATE.presence", "CONTROL_STATE.producer_sample_time_us",
      "CONTROL_STATE.pitch", "CONTROL_STATE.heading", "CONTROL_STATE.bank", "CONTROL_STATE.vertical",
      "CONTROL_STATE.sideways", "CONTROL_STATE.forward", "CONTROL_STATE.control_mode", "CONTROL_STATE.control_flags",
      "CONTROL_STATE.forward_cruise_percent", "CONTROL_STATE.fire_primary_count",
      "CONTROL_STATE.fire_secondary_count", "CONTROL_STATE.fire_countermeasure_count",
      "CONTROL_STATE.flight_cursor.cursor_pitch_rad", "CONTROL_STATE.flight_cursor.cursor_yaw_rad",
      "CONTROL_STATE.flight_cursor.cursor_sensitivity", "CONTROL_STATE.flight_cursor.cursor_deadzone"
    ];
    const definitions = (catalogData as InstrumentDefinition[]).filter((item) => item.tab === "flight");
    const classified = new Set(definitions.flatMap((item) => [
      ...(item.consumedFields ?? []),
      ...(item.detailFields ?? []),
      ...(item.reservedFields ?? [])
    ]));
    expect(expected.filter((field) => !classified.has(field))).toEqual([]);
  });
});
