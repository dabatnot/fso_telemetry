import { describe, expect, it } from "vitest";
import catalog from "./instrument-catalog.json";
import { resolveInstrument } from "./data";
import {
  CONTACT_FLAGS,
  GUIDANCE_TYPES,
  HUD_WARNING_KINDS,
  RADAR_BLIP_TYPES,
  RADAR_CATEGORIES,
  RADAR_MODES,
  RADAR_VISIBILITY,
  SENSOR_STATES,
  THREAT_LEVELS,
  contactVisibilityAlpha,
  contactViews,
  decodeContactFlags,
  hudAlertView,
  lockViews,
  missileViews,
  prioritizedContacts,
  targetClassDisplayName,
  targetDisplayName,
  targetHudTypeLabel,
  targetHudColor,
  targetReferenceInvalid,
  subsystemNameForTarget
} from "./tacticalSemantics";
import type { DashboardSnapshot, InstrumentDefinition } from "./types";

function snapshot(): DashboardSnapshot {
  return {
    schema: "DashboardSnapshotV1",
    publishedAtUtc: "2026-08-06T12:00:00Z",
    mode: "live",
    connection: {
      status: "Live",
      host: "127.0.0.1",
      port: 42042,
      sessionId: "3",
      lastLiveObservedUtc: "2026-08-06T12:00:00Z",
      staleReason: null
    },
    session: {},
    mission: { phase: "ACTIVE" },
    playerEntityId: "1",
    records: {
      SHIP_IDENTITY: [
        { entity_id: "1", internal_name: "Joueur" },
        { entity_id: "101", internal_name: "NOM SECRET À NE PAS UTILISER" }
      ],
      FLIGHT_STATE: [{
        entity_id: "1",
        producer_sample_time_us: "1000000",
        position_world: [0, 0, 0],
        velocity_world: [0, 0, 10],
        orientation_local_to_world: [1, 0, 0, 0]
      }],
      TARGET_STATE: [{
        entity_id: "1",
        presence: "772",
        producer_sample_time_us: "1000000",
        current_target_entity_id: "101",
        time_on_target_us: "2000000",
        in_cone: true,
        exact_hud_distance: 0
      }],
      RADAR_STATE: [{
        entity_id: "1",
        presence: "0",
        producer_sample_time_us: "1000000",
        radar_mode: 0,
        selected_range: 1000,
        sensor_state: 2,
        sensor_current_hits: 50,
        sensor_max_hits: 100
      }],
      RADAR_CONTACTS: [
        {
          entity_id: "1",
          contact_entity_id: "101",
          presence: "0",
          producer_sample_time_us: "1000000",
          object_type: 1,
          category: 1,
          visibility: 1,
          position_world: [100, 10, 500],
          velocity_world: [0, 0, -20],
          radius: 10,
          contact_flags: 0x02
        },
        {
          entity_id: "1",
          contact_entity_id: "202",
          presence: "2",
          producer_sample_time_us: "1000000",
          object_type: 2,
          category: 2,
          visibility: 2,
          position_world: [-200, 0, 800],
          velocity_world: [0, 0, -100],
          radius: 2,
          contact_flags: 0xe0,
          revealed_name: "Missile détecté"
        }
      ],
      LOCK_STATE: [{
        entity_id: "1",
        presence: "0",
        producer_sample_time_us: "1000000",
        locks: [{
          item_version: 1,
          item_size: 40,
          presence: 2,
          locked: false,
          target_in_lock_cone: true,
          target_entity_id: "101",
          world_position: [100, 10, 500],
          time_to_lock_remaining_us: "1000000"
        }]
      }],
      THREAT_STATE: [{
        entity_id: "1",
        presence: "4",
        producer_sample_time_us: "1000000",
        threat_level: 2,
        nearest_homing_entity_id: "202",
        incoming_missiles: [{
          item_version: 1,
          item_size: 80,
          presence: 0,
          guidance_type: 3,
          radar_visibility: 2,
          entity_id: "202",
          weapon_class_id: 9,
          target_entity_id: "1",
          position_world: [-200, 0, 800],
          orientation_local_to_world: [1, 0, 0, 0],
          velocity_world: [0, 0, -100]
        }]
      }]
    },
    recordInstances: {},
    manifest: {
      id: 4,
      records: {
        "CLASS_MANIFEST/class_id=3": {
          recordName: "CLASS_MANIFEST",
          fields: {
            class_id: 3,
            internal_name: "GTF Myrmidon",
            subsystems: [{
              subsystem_id: 4,
              internal_name: "engine",
              hud_name: "Moteurs"
            }]
          }
        },
        "WEAPON_MANIFEST/weapon_class_id=9": {
          recordName: "WEAPON_MANIFEST",
          fields: {
            weapon_class_id: 9,
            internal_name: "Missile test",
            guidance: { type: 3 },
            lock: { time_us: "2000000", fov_rad: 0.3 }
          }
        }
      }
    },
    derived: {
      "entities.1.sensor_integrity_ratio": { available: true, value: 0.5 },
      "entities.1.target.distance": { available: true, value: 0 },
      "entities.1.tracks.101.distance": { available: true, value: 510 },
      "entities.1.tracks.101.relative_speed": { available: true, value: 30 },
      "entities.1.tracks.101.closing_speed": { available: true, value: 29 },
      "entities.1.tracks.101.ttc_s": { available: true, value: 17.6 },
      "entities.1.tracks.101.age_us": { available: true, value: 0 },
      "entities.1.tracks.101.bearing_local_rad": { available: true, value: 0.197 },
      "entities.1.tracks.101.elevation_local_rad": { available: true, value: 0.019 },
      "entities.1.tracks.101.scope_clamped_position": { available: true, value: [0.1, -0.5] },
      "entities.1.tracks.101.scope_in_range": { available: true, value: true },
      "entities.1.tracks.202.distance": { available: true, value: 824 },
      "entities.1.tracks.202.relative_speed": { available: true, value: 110 },
      "entities.1.tracks.202.closing_speed": { available: true, value: 106 },
      "entities.1.tracks.202.ttc_s": { available: true, value: 7.8 },
      "entities.1.tracks.202.age_us": { available: true, value: 0 },
      "entities.1.tracks.202.bearing_local_rad": { available: true, value: -0.245 },
      "entities.1.tracks.202.elevation_local_rad": { available: true, value: 0 },
      "entities.1.tracks.202.scope_clamped_position": { available: true, value: [-0.2, -0.8] },
      "entities.1.tracks.202.scope_in_range": { available: true, value: true },
      "entities.1.locks[0].progress": { available: true, value: 0.5 },
      "entities.1.missiles.202.distance": { available: true, value: 824 },
      "entities.1.missiles.202.relative_speed": { available: true, value: 110 },
      "entities.1.missiles.202.closing_speed": { available: true, value: 106 },
      "entities.1.missiles.202.ttc_s": { available: true, value: 7.8 }
    },
    transport: { synchronized: true, baseline: 1, deltaSequence: 1, manifestId: 4 },
    quality: { packets: 4, transportGapCount: 0, decodeErrorCount: 0, resyncCount: 0, channels: [] },
    capture: { active: false, path: null },
    replay: { path: null, playing: false, speed: 1, position: 0, packetCount: 0 }
  };
}

describe("tactical closed registries", () => {
  it("decodes every Phase 3 tactical enum and contact flag", () => {
    expect(Object.keys(RADAR_MODES)).toHaveLength(4);
    expect(Object.keys(SENSOR_STATES)).toHaveLength(3);
    expect(Object.keys(RADAR_VISIBILITY)).toHaveLength(3);
    expect(Object.keys(RADAR_CATEGORIES)).toHaveLength(8);
    expect(Object.keys(THREAT_LEVELS)).toHaveLength(4);
    expect(Object.keys(GUIDANCE_TYPES)).toHaveLength(6);
    expect(HUD_WARNING_KINDS).toEqual({
      1: "LAUNCH",
      2: "EVADED",
      3: "COLLISION",
      4: "BLAST",
      5: "ENGINE WASH",
      6: "EMP",
      7: "OTHER"
    });
    expect(RADAR_BLIP_TYPES).toEqual({
      0: "NŒUD DE SAUT",
      1: "NAVBUOY/CARGO",
      2: "BOMBE",
      3: "WARP",
      4: "TAGUÉ",
      5: "VAISSEAU NORMAL"
    });
    expect(decodeContactFlags(0xff)).toEqual(CONTACT_FLAGS.map((flag) => flag.label));
    expect(decodeContactFlags(0x100)).toBeNull();
  });
});

describe("tactical display semantics", () => {
  it("keeps primary fire and acquired missile lock simultaneous with authoritative timing", () => {
    const value = snapshot();
    value.records.HUD_ALERT_STATE = [{
      entity_id: "1",
      presence: "1",
      producer_sample_time_us: "1000000",
      primary_fire_threat_active: true,
      missile_lock_state: 2,
      warning_kind: 1,
      warning_text: "Launch",
      warning_remaining_us: "750000",
      warning_instance_id: "9"
    }];
    expect(hudAlertView(value)).toMatchObject({
      primaryFireActive: true,
      primaryBlinkMs: 180,
      missileLockState: 2,
      lockBlinkMs: 90,
      provenance: "authoritative-v1",
      warning: {
        kindCode: 1,
        kind: "LAUNCH",
        text: "Launch",
        remainingUs: 750000,
        instanceId: "9"
      }
    });
  });

  it("uses 180 ms for a lock attempt and never invents an incomplete warning", () => {
    const value = snapshot();
    value.records.HUD_ALERT_STATE = [{
      entity_id: "1",
      presence: "1",
      producer_sample_time_us: "1000000",
      primary_fire_threat_active: false,
      missile_lock_state: 1,
      warning_kind: 3
    }];
    expect(hudAlertView(value)).toMatchObject({
      primaryFireActive: false,
      primaryBlinkMs: null,
      missileLockState: 1,
      lockBlinkMs: 180,
      warning: null,
      provenance: "authoritative-v1"
    });
  });

  it("falls back to aggregated threats only for replay and never invents warning text", () => {
    const legacy = snapshot();
    legacy.mode = "replay";
    legacy.replay.captureSchema = "FSTL-dashboard-capture-v1";
    expect(hudAlertView(legacy)).toMatchObject({
      primaryFireActive: false,
      missileLockState: 1,
      lockBlinkMs: 180,
      warning: null,
      provenance: "legacy-aggregated"
    });
    legacy.mode = "live";
    expect(hudAlertView(legacy)).toMatchObject({
      primaryFireActive: false,
      missileLockState: 0,
      warning: null,
      provenance: "missing-authoritative"
    });
    legacy.mode = "replay";
    legacy.replay.captureSchema = "FSTL-dashboard-capture-v2";
    legacy.replay.contractFeatures = ["HUD_ALERT_STATE/v1"];
    expect(hudAlertView(legacy).provenance).toBe("missing-authoritative");
  });

  it("does not leak an unrevealed identity from SHIP_IDENTITY", () => {
    const view = contactViews(snapshot()).find((contact) => contact.id === "101");
    expect(view?.name).toBe("CONTACT 101");
    expect(view?.typeLabel).toBeNull();
    expect(targetDisplayName(snapshot())).toBe("CONTACT 101");
  });

  it("uses the authoritative v3 HUD type label without manifest fallback", () => {
    const value = snapshot();
    const contact = value.records.RADAR_CONTACTS[0];
    contact.revealed_name = "Alpha 2";
    contact.revealed_class_id = 3;
    contact.hud_type_label = "GTF Ulysses (alt)";
    value.recordInstances["RADAR_CONTACTS/entity_id=1/contact_entity_id=101"] = {
      recordName: "RADAR_CONTACTS",
      recordVersion: 3,
      fields: contact
    };
    expect(contactViews(value).find((item) => item.id === "101")).toMatchObject({
      name: "Alpha 2",
      typeLabel: "GTF Ulysses (alt)"
    });
    delete contact.hud_type_label;
    expect(contactViews(value).find((item) => item.id === "101")?.typeLabel).toBeNull();
  });

  it("resolves the class manifest only for legacy radar captures", () => {
    const value = snapshot();
    value.records.RADAR_CONTACTS[0].revealed_class_id = 3;
    expect(contactViews(value).find((item) => item.id === "101")?.typeLabel)
      .toBe("GTF Myrmidon");
  });

  it("uses the authoritative v4 radar RGBA and blip type without target recoloring", () => {
    const value = snapshot();
    const contact = value.records.RADAR_CONTACTS[0];
    contact.contact_flags = 0x03;
    contact.radar_blip_color = [12, 34, 56, 128];
    contact.radar_blip_type = 5;
    value.recordInstances["RADAR_CONTACTS/entity_id=1/contact_entity_id=101"] = {
      recordName: "RADAR_CONTACTS",
      recordVersion: 4,
      fields: contact
    };
    expect(contactViews(value).find((item) => item.id === "101")).toMatchObject({
      blipType: "VAISSEAU NORMAL",
      blipTypeCode: 5,
      bright: true,
      color: {
        rgba: [12, 34, 56, 128],
        css: "#0c223880",
        provenance: "authoritative-v4"
      }
    });
  });

  it("uses neutral missing-authoritative-color instead of deriving a v4 radar color", () => {
    const value = snapshot();
    const contact = value.records.RADAR_CONTACTS[0];
    value.recordInstances["RADAR_CONTACTS/entity_id=1/contact_entity_id=101"] = {
      recordName: "RADAR_CONTACTS",
      recordVersion: 4,
      fields: contact
    };
    expect(contactViews(value).find((item) => item.id === "101")?.color).toEqual({
      rgba: null,
      css: "#70e4d1",
      provenance: "missing-authoritative-color"
    });
  });

  it("dims distorted contacts without replacing their authoritative hue", () => {
    const value = snapshot();
    const contact = value.records.RADAR_CONTACTS[0];
    contact.visibility = 2;
    contact.radar_blip_color = [200, 30, 90, 180];
    contact.radar_blip_type = 4;
    value.recordInstances["RADAR_CONTACTS/entity_id=1/contact_entity_id=101"] = {
      recordName: "RADAR_CONTACTS",
      recordVersion: 4,
      fields: contact
    };
    expect(contactViews(value).find((item) => item.id === "101")?.color.css)
      .toBe("#c81e5ab4");
    expect(contactVisibilityAlpha(2)).toBe(0.6);
  });

  it("keeps the historical contact palette only for v1-v3 captures", () => {
    const value = snapshot();
    const contact = value.records.RADAR_CONTACTS[0];
    for (const recordVersion of [1, 2, 3]) {
      value.recordInstances["RADAR_CONTACTS/entity_id=1/contact_entity_id=101"] = {
        recordName: "RADAR_CONTACTS",
        recordVersion,
        fields: contact
      };
      expect(contactViews(value).find((item) => item.id === "101")?.color).toEqual({
        rgba: null,
        css: "#ffd466",
        provenance: "legacy-v1-v3"
      });
    }
    expect(contactViews(value).find((item) => item.id === "202")?.color.css)
      .toBe("#ff6b55");
  });

  it("resolves a revealed target class from the CLASS_MANIFEST internal name", () => {
    const value = snapshot();
    value.records.TARGET_STATE[0].revealed_identity = {
      name: "Alpha 2", class_id: 3
    };
    expect(targetDisplayName(value)).toBe("Alpha 2");
    expect(targetClassDisplayName(value)).toBe("GTF Myrmidon");
  });

  it("uses authoritative target and lock subsystem labels in TARGET_STATE v5", () => {
    const value = snapshot();
    const target = value.records.TARGET_STATE[0];
    target.revealed_identity = { name: "Alpha 2", class_id: 0, object_type: 1 };
    target.hud_target_subsystem_label = "Laser turret";
    target.hud_lock_subsystem_label = "Navigation";
    value.recordInstances["TARGET_STATE/entity_id=1"] = {
      recordName: "TARGET_STATE",
      recordVersion: 5,
      fields: target
    };
    expect(subsystemNameForTarget(value, undefined, target.hud_target_subsystem_label))
      .toBe("Laser turret");
    expect(subsystemNameForTarget(value, undefined, target.hud_lock_subsystem_label))
      .toBe("Navigation");
  });

  it("resolves historical target subsystem IDs from CLASS_MANIFEST.subsystems", () => {
    const value = snapshot();
    value.records.TARGET_STATE[0].revealed_identity = {
      name: "Alpha 2", class_id: 3, object_type: 1
    };
    value.recordInstances["TARGET_STATE/entity_id=1"] = {
      recordName: "TARGET_STATE",
      recordVersion: 4,
      fields: value.records.TARGET_STATE[0]
    };
    expect(subsystemNameForTarget(value, 4)).toBe("Moteurs");
  });

  it("keeps authoritative zero distance and prioritizes target and threat contacts", () => {
    const value = snapshot();
    expect(value.derived["entities.1.target.distance"].value).toBe(0);
    const contacts = prioritizedContacts(value);
    expect(new Set(contacts.slice(0, 2).map((contact) => contact.id))).toEqual(
      new Set(["101", "202"])
    );
    expect(contacts.every((contact) => !contact.invalid)).toBe(true);
  });

	it("uses the latest TARGET_STATE instead of a retained radar target flag", () => {
		const value = snapshot();
		value.records.TARGET_STATE[0].producer_sample_time_us = "1100000";
		value.records.TARGET_STATE[0].current_target_entity_id = "202";
		// RADAR_CONTACTS is deliberately the older systemsHz sample: 101 still
		// carries CURRENT_TARGET while 202 does not.
		const contacts = prioritizedContacts(value);
		expect(contacts[0].id).toBe("202");
		expect(contacts.find((contact) => contact.id === "202")?.current).toBe(true);
		expect(contacts.find((contact) => contact.id === "101")?.current).toBe(false);
		const retained = contacts.find((contact) => contact.id === "101");
		expect(retained).toBeDefined();
		expect((retained?.flagBits ?? 0) & 0x02).toBe(0x02);
	});

  it("uses authoritative target RGBA in v4 and neutralizes a missing v4 color", () => {
    const value = snapshot();
    const target = value.records.TARGET_STATE[0];
    target.hud_target_color = [9, 180, 70, 255];
    value.recordInstances["TARGET_STATE/entity_id=1"] = {
      recordName: "TARGET_STATE",
      recordVersion: 4,
      fields: target
    };
    expect(targetHudColor(value)).toEqual({
      rgba: [9, 180, 70, 255],
      css: "#09b446ff",
      provenance: "authoritative-v4"
    });
    delete target.hud_target_color;
    expect(targetHudColor(value)).toEqual({
      rgba: null,
      css: "#70e4d1",
      provenance: "missing-authoritative-color"
    });
  });

  it("keeps the historical target accent for v1-v3 captures", () => {
    const value = snapshot();
    for (const recordVersion of [1, 2, 3]) {
      value.recordInstances["TARGET_STATE/entity_id=1"] = {
        recordName: "TARGET_STATE",
        recordVersion,
        fields: value.records.TARGET_STATE[0]
      };
      expect(targetHudColor(value)).toEqual({
        rgba: null,
        css: "#ffd466",
        provenance: "legacy-v1-v3"
      });
    }
  });

  it("resolves lock progression and missile estimates without making them authoritative", () => {
    const value = snapshot();
    expect(lockViews(value)[0]).toMatchObject({
      targetId: "101",
      progress: 0.5,
      remainingS: 1,
      locked: false
    });
    expect(missileViews(value)[0]).toMatchObject({
      id: "202",
      name: "Missile test",
      guidance: "HOMING",
      ttcS: 7.8
    });
  });

  it("keeps a HUD target valid when it deliberately has no radar contact", () => {
    const value = snapshot();
    value.records.RADAR_CONTACTS = value.records.RADAR_CONTACTS.filter(
      (record) => record.contact_entity_id !== "101"
    );
    expect(targetReferenceInvalid(value)).toBe(false);
    value.transport.synchronized = false;
    expect(targetReferenceInvalid(value)).toBe(false);
  });

  it("uses the v3 HUD type label when a non-ship target has no class manifest", () => {
    const value = snapshot();
    value.records.TARGET_STATE[0].revealed_identity = { name: "Harpoon" };
    value.records.TARGET_STATE[0].hud_type_label = "impact: 4.0 sec";
    expect(targetClassDisplayName(value)).toBeNull();
    expect(targetDisplayName(value)).toBe("Harpoon");
    expect(targetHudTypeLabel(value)).toBe("impact: 4.0 sec");
  });

  it("keeps the authoritative v3 ship class label when its manifest entry is absent", () => {
    const value = snapshot();
    value.records.TARGET_STATE[0].revealed_identity = {
      name: "Alpha 2", class_id: 0
    };
    value.records.TARGET_STATE[0].hud_type_label = "GTF Myrmidon";
    expect(targetDisplayName(value)).toBe("Alpha 2");
    expect(targetClassDisplayName(value)).toBeNull();
    expect(targetHudTypeLabel(value)).toBe("GTF Myrmidon");
  });

  it("preserves live zero, stale, waiting and ND states", () => {
    const definitions = catalog.filter((item) => item.tab === "tactical") as InstrumentDefinition[];
    const targetDefinition = definitions.find((item) => item.id === "tactical-target")!;
    const futureDefinition = definitions.find((item) => item.id === "tactical-future")!;
    const value = snapshot();
    value.records.TARGET_STATE[0].current_target_entity_id = 0;
    expect(resolveInstrument(targetDefinition, value)).toMatchObject({ state: "live", value: 0 });
    expect(resolveInstrument(
      targetDefinition,
      { ...value, connection: { ...value.connection, status: "Stale" } }
    ).state).toBe("stale");
    expect(resolveInstrument(targetDefinition, null).state).toBe("waiting");
    expect(resolveInstrument(futureDefinition, value).state).toBe("nd");
  });
});

describe("tactical catalog coverage", () => {
  it("classifies all tactical fields and replaces the former future cards", () => {
    const definitions = catalog.filter((item) => item.tab === "tactical");
    expect(definitions.map((item) => item.id)).toEqual([
      "tactical-sensors",
      "tactical-scope",
      "tactical-target",
      "tactical-locks",
      "tactical-threats",
      "tactical-future"
    ]);
    const covered = new Set(definitions.flatMap((definition) => [
      ...(definition.consumedFields ?? []),
      ...(definition.detailFields ?? []),
      ...(definition.reservedFields ?? [])
    ]));
    for (const field of [
      "TARGET_STATE.current_target_entity_id",
      "TARGET_STATE.exact_hud_distance",
      "LOCK_STATE.locks[].time_to_lock_remaining_us",
      "RADAR_STATE.jamming_intensity",
      "RADAR_STATE.last_contact_time_us",
      "RADAR_CONTACTS.confidence",
      "RADAR_CONTACTS.hud_type_label",
      "RADAR_CONTACTS.radar_blip_color",
      "RADAR_CONTACTS.radar_blip_type",
      "TARGET_STATE.hud_target_color",
      "THREAT_STATE.threat_level",
      "THREAT_STATE.incoming_missiles[].orientation_local_to_world"
    ]) expect(covered.has(field), field).toBe(true);
    expect(definitions.at(-1)?.availability).toBe("nd");
  });
});
