import { describe, expect, it } from "vitest";
import catalogData from "./instrument-catalog.json";
import { resolveInstrument } from "./data";
import {
  bankState,
  bankViews,
  countermeasureState,
  decodeBitmap,
  FIRING_PATTERNS,
  GUIDANCE_TYPES,
  selectedBank,
  selectedBankIsInvalid,
  turretViews,
  visibleBanks,
  WEAPON_CLASS_FLAGS,
  WEAPON_GLOBAL_FLAGS,
  weaponDisplayName
} from "./weaponSemantics";
import type { DashboardSnapshot, InstrumentDefinition } from "./types";

function snapshot(): DashboardSnapshot {
  const primary = Array.from({ length: 7 }, (_, index) => ({
    presence: index === 0 ? 1 : 0,
    canonical_index: index,
    bank_id: 10 + index,
    weapon_class_id: 100 + index,
    cooldown_remaining_us: index === 0 ? "500000" : "0",
    primary_slot: 0,
    fire_point: index,
    simultaneous_slots: 1,
    pattern_id: index === 0 ? 1 : 0,
    ...(index === 0 ? { ammunition: { current: 5, initial: 10, capacity: 10 } } : {})
  }));
  const result: DashboardSnapshot = {
    schema: "DashboardSnapshotV1",
    publishedAtUtc: "2026-08-03T10:00:00Z",
    mode: "live",
    connection: {
      status: "Live", host: "127.0.0.1", port: 42042, sessionId: "1",
      lastLiveObservedUtc: null, staleReason: null
    },
    session: {},
    mission: {},
    playerEntityId: "7",
    records: {
      SHIP_IDENTITY: [{ entity_id: "7", ship_class_id: 3 }],
      WEAPON_STATE: [{
        entity_id: "7",
        presence: "255",
        producer_sample_time_us: "1000000",
        primary_bank_count: 7,
        secondary_bank_count: 1,
        tertiary_bank_count: 0,
        current_primary_bank_id: 16,
        current_secondary_bank_id: 20,
        current_tertiary_bank_id: 0,
        weapon_flags: 0x0005,
        primary_banks: primary,
        secondary_banks: [{
          presence: 1,
          canonical_index: 0,
          bank_id: 20,
          weapon_class_id: 200,
          cooldown_remaining_us: "0",
          secondary_slot: 0,
          ammunition: { current: 0, initial: 8, capacity: 8 }
        }],
        countermeasure: {
          presence: 1, flags: 0, weapon_class_id: 300,
          current: 2, maximum: 4, cooldown_remaining_us: "0"
        }
      }],
      SUBSYSTEM_STATE: [
        {
          entity_id: "7", subsystem_id: 1, canonical_index: 1, type: 2,
          current_hits: 80, max_hits: 100, subsystem_flags: 0,
          turret: { target_entity_id: "42", cooldown_remaining_us: "0" }
        },
        {
          entity_id: "7", subsystem_id: 2, canonical_index: 0, type: 2,
          current_hits: 80, max_hits: 100, subsystem_flags: 0x20,
          turret: { target_entity_id: "0", cooldown_remaining_us: "500000" }
        },
        {
          entity_id: "99", subsystem_id: 3, canonical_index: 0, type: 2,
          current_hits: 80, max_hits: 100, subsystem_flags: 0,
          turret: { target_entity_id: "42", cooldown_remaining_us: "0" }
        }
      ]
    },
    recordInstances: {},
    manifest: { id: 1, records: {} },
    derived: {
      "entities.7.primary_banks[0].ammo_ratio": { available: true, value: 0.5 },
      "entities.7.primary_banks[0].cooldown_s": { available: true, value: 0.5 },
      "entities.7.primary_banks[0].state": { available: true, value: "RECHARGE" },
      "entities.7.secondary_banks[0].ammo_ratio": { available: true, value: 0 },
      "entities.7.secondary_banks[0].cooldown_s": { available: true, value: 0 },
      "entities.7.secondary_banks[0].state": { available: true, value: "VIDE" }
    },
    transport: { synchronized: true, baseline: 1, deltaSequence: 1, manifestId: 1 },
    quality: {
      packets: 1, transportGapCount: 0, decodeErrorCount: 0, resyncCount: 0,
      channels: []
    },
    capture: { active: false, path: null },
    replay: { path: null, playing: false, speed: 1, position: 0, packetCount: 0 }
  };
  result.manifest.records["CLASS_MANIFEST/class_id=3"] = {
    recordName: "CLASS_MANIFEST",
    fields: {
      class_id: 3,
      banks: primary.map((bank) => ({
        bank_id: bank.bank_id, canonical_index: bank.canonical_index,
        family: 0, weapon_class_id: bank.weapon_class_id, fire_points: [[0, 0, 1]]
      })),
      subsystem_definitions: [
        { subsystem_id: 1, canonical_index: 1, type: 2, internal_name: "Tourelle avant" },
        { subsystem_id: 2, canonical_index: 0, type: 2, internal_name: "Tourelle verrouillée" }
      ]
    }
  };
  for (let index = 0; index < 7; index += 1) {
    result.manifest.records[`WEAPON_MANIFEST/weapon_class_id=${100 + index}`] = {
      recordName: "WEAPON_MANIFEST",
      fields: {
        weapon_class_id: 100 + index,
        internal_name: `Primary ${index + 1}`,
        ...(index === 6 ? { title: "Prometheus S" } : {}),
        subtype: 1,
        weapon_flags: index === 0 ? 0x0002 : 0,
        fire: { wait_us: "250000", energy_consumed: 2 },
        damage: { amount: 10 }
      }
    };
  }
  for (const [id, name, subtype] of [[200, "Harpoon", 2], [300, "Chaff", 4]] as const) {
    result.manifest.records[`WEAPON_MANIFEST/weapon_class_id=${id}`] = {
      recordName: "WEAPON_MANIFEST",
      fields: { weapon_class_id: id, internal_name: name, subtype, weapon_flags: 0 }
    };
  }
  return result;
}

describe("weapon cockpit semantics", () => {
  it("decodes closed registries and preserves manifest display-name precedence", () => {
    expect(decodeBitmap(0x0085, WEAPON_GLOBAL_FLAGS)).toEqual([
      "PRIMAIRES LIÉES", "GÂCHETTE PRIMAIRE", "DÉTONATEURS DISTANTS"
    ]);
    expect(decodeBitmap(0x0043, WEAPON_CLASS_FLAGS)).toEqual([
      "BOMBE", "BALISTIQUE", "GUIDÉE"
    ]);
    expect(decodeBitmap(Number.NaN, WEAPON_GLOBAL_FLAGS)).toBeNull();
    expect(GUIDANCE_TYPES[2]).toBe("ASPECT");
    expect(FIRING_PATTERNS[4]).toBe("ALÉATOIRE SANS RÉPÉTITION");
    expect(weaponDisplayName({ title: "  Harpoon  ", internal_name: "Harpoon#1" })).toBe("Harpoon");
    expect(weaponDisplayName({ internal_name: "Prometheus" })).toBe("Prometheus");
    expect(weaponDisplayName(null)).toContain("ERR");
  });

  it("resolves selections, manifests and keeps an off-page selected bank visible", () => {
    const value = snapshot();
    const banks = bankViews(value, "primary");
    expect(banks).toHaveLength(7);
    expect(selectedBank(value, "primary")?.name).toBe("Prometheus S");
    expect(selectedBankIsInvalid(value, "primary")).toBe(false);
    expect(banks[0]).toMatchObject({
      name: "Primary 1", subtype: "PRIMAIRE", ammoRatio: 0.5,
      state: "RECHARGE", cooldownS: 0.5
    });
    expect(visibleBanks(banks)).toHaveLength(6);
    expect(visibleBanks(banks).map((bank) => bank.bankId)).toContain("16");
  });

  it("maps bank and countermeasure availability without claiming a confirmed shot", () => {
    const ammunition = { ammunition: { current: 2, initial: 4 }, cooldown_remaining_us: 0 };
    expect(bankState("primary", 0, ammunition)).toBe("DISPONIBLE");
    expect(bankState("primary", 0x10, ammunition)).toBe("VERROUILLÉE");
    expect(bankState("secondary", 0, { ...ammunition, ammunition: { current: 0, initial: 4 } })).toBe("VIDE");
    expect(bankState("primary", 0, { ...ammunition, cooldown_remaining_us: 1 })).toBe("RECHARGE");
    expect(countermeasureState({ flags: 0, current: 2, cooldown_remaining_us: 0 })).toBe("DISPONIBLE");
    expect(countermeasureState({ flags: 2, current: 2, cooldown_remaining_us: 0 })).toBe("VERROUILLÉE");
    expect(countermeasureState({ flags: 0, current: 0, cooldown_remaining_us: 0 })).toBe("VIDE");
    expect(countermeasureState({ flags: 0, current: 2, cooldown_remaining_us: 1 })).toBe("RECHARGE");
  });

  it("filters player turrets and sorts locked alerts before active targets", () => {
    const turrets = turretViews(snapshot());
    expect(turrets).toHaveLength(2);
    expect(turrets.map((turret) => turret.name)).toEqual([
      "Tourelle verrouillée", "Tourelle avant"
    ]);
    expect(turrets[0].locked).toBe(true);
    expect(turrets[1].targetActive).toBe(true);
  });

  it("detects an invalid selector and preserves dashboard availability states", () => {
    const value = snapshot();
    value.records.WEAPON_STATE[0].current_primary_bank_id = 999;
    expect(selectedBankIsInvalid(value, "primary")).toBe(true);
    const definition = (catalogData as InstrumentDefinition[]).find(
      (item) => item.id === "weapon-selected"
    )!;
    value.records.WEAPON_STATE[0].current_primary_bank_id = 0;
    expect(resolveInstrument(definition, value).value).toBe(0);
    value.connection.status = "Stale";
    expect(resolveInstrument(definition, value).state).toBe("stale");
    expect(resolveInstrument(definition, null).state).toBe("waiting");
    const future = (catalogData as InstrumentDefinition[]).find(
      (item) => item.id === "weapon-future"
    )!;
    expect(resolveInstrument(future, value).state).toBe("nd");
  });
});

describe("weapon catalog coverage", () => {
  it("classifies all runtime, manifest, cross-record and future groups", () => {
    const definitions = (catalogData as InstrumentDefinition[]).filter(
      (item) => item.tab === "weapons"
    );
    expect(definitions.map((item) => item.id)).toEqual([
      "weapon-modes", "weapon-primary-rack", "weapon-selected",
      "weapon-secondary-rack", "weapon-reserves", "weapon-turrets", "weapon-future"
    ]);
    const classified = new Set(definitions.flatMap((item) => [
      ...(item.consumedFields ?? []),
      ...(item.detailFields ?? []),
      ...(item.reservedFields ?? [])
    ]));
    const required = [
      "WEAPON_STATE.entity_id", "WEAPON_STATE.presence",
      "WEAPON_STATE.producer_sample_time_us", "WEAPON_STATE.weapon_flags",
      "WEAPON_STATE.primary_bank_count", "WEAPON_STATE.secondary_bank_count",
      "WEAPON_STATE.tertiary_bank_count", "WEAPON_STATE.current_primary_bank_id",
      "WEAPON_STATE.current_secondary_bank_id", "WEAPON_STATE.current_tertiary_bank_id",
      "WEAPON_STATE.primary_banks[].bank_id", "WEAPON_STATE.primary_banks[].weapon_class_id",
      "WEAPON_STATE.primary_banks[].ammunition.current",
      "WEAPON_STATE.secondary_banks[].bank_id", "WEAPON_STATE.secondary_banks[].weapon_class_id",
      "WEAPON_STATE.secondary_banks[].ammunition.current",
      "WEAPON_STATE.tertiary.ammunition_current", "WEAPON_STATE.swarm.remaining",
      "WEAPON_STATE.countermeasure.current", "WEAPON_STATE.countermeasure.maximum",
      "WEAPON_MANIFEST.internal_name", "WEAPON_MANIFEST.weapon_flags",
      "WEAPON_MANIFEST.fire.wait_us", "WEAPON_MANIFEST.damage.amount",
      "WEAPON_MANIFEST.guidance.type", "WEAPON_MANIFEST.lock.time_us",
      "ENERGY_STATE.weapon_energy_current", "CONTROL_STATE.fire_primary_count",
      "SUBSYSTEM_STATE.turret.banks", "future.LOCK_PROGRESS",
      "future.CONFIRMED_WEAPON_EVENTS"
    ];
    expect(required.filter((field) => !classified.has(field))).toEqual([]);
  });
});
