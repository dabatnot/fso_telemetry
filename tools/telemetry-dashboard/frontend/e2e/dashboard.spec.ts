import { expect, test } from "@playwright/test";

async function mockEnergySnapshot(page: import("@playwright/test").Page) {
  const snapshot = {
    schema: "DashboardSnapshotV1",
    publishedAtUtc: "2026-08-03T10:00:00Z",
    mode: "live",
    connection: {
      status: "Live",
      host: "127.0.0.1",
      port: 42042,
      sessionId: "42",
      lastLiveObservedUtc: "2026-08-03T10:00:00Z",
      staleReason: null
    },
    session: {},
    mission: { phase: "ACTIVE" },
    playerEntityId: "7",
    records: {
      SHIP_IDENTITY: [{ entity_id: "7", ship_class_id: 3 }],
      ENERGY_STATE: [{
        entity_id: "7", presence: "55", producer_sample_time_us: "1000000",
        ets_mode: 1, ets_shields_index: 4, ets_weapons_index: 8, ets_engines_index: 4,
        weapon_energy_current: 50, weapon_energy_max: 100,
        weapon_regeneration_per_s: 5, shield_regeneration_per_s: 6,
        deferred_to_weapons: -2, deferred_to_shields: 2, power_output: 1,
        aggregate_engine_current_hits: 50, aggregate_engine_max_hits: 100
      }],
      PROPULSION_STATE: [{
        entity_id: "7", presence: "31", producer_sample_time_us: "1000000",
        propulsion_flags: 0x25, fuel_current: 40, fuel_max: 100,
        consumption_per_s: 10, recovery_per_s: 5, minimum_to_engage: 25,
        cooldown_remaining_us: "0", time_since_last_stop_us: "1000000",
        fuel_at_last_engagement: 80, forward_accel_time_const_s: 0.8,
        afterburner_max_velocity_local: [0, 0, 140], engine_wash_intensity: 0
      }]
    },
    recordInstances: {},
    manifest: {
      id: 1,
      records: {
        "CLASS_MANIFEST/class_id=3": {
          recordName: "CLASS_MANIFEST",
          fields: {
            class_id: 3, internal_name: "GTF Test",
            max_velocity: [0, 0, 80], afterburner_max_velocity: [0, 0, 140],
            booster_max_velocity: [0, 0, 0], max_rotational_velocity: [1, 1, 1],
            max_rear_velocity: 20, forward_accel_time: 2.4,
            afterburner_forward_accel_time: 0.8, booster_forward_accel_time: 0,
            forward_decel_time: 2, slide_accel_time: 0, slide_decel_time: 0,
            afterburner: { fuel_capacity: 100, burn_rate: 10, recover_rate: 5, minimum_start_fuel: 25 }
          }
        }
      }
    },
    derived: {
      "entities.7.weapon_energy_ratio": { available: true, value: 0.5 },
      "entities.7.fuel_ratio": { available: true, value: 0.4 },
      "entities.7.engine_integrity_ratio": { available: true, value: 0.5 },
      "entities.7.afterburner_autonomy_s": { available: true, value: 4 },
      "entities.7.afterburner_recharge_s": { available: false, reason: "afterburner-active", value: null },
      "entities.7.afterburner_usable_fuel": { available: true, value: 15 },
      "entities.7.afterburner_ready_delay_s": { available: true, value: 0 },
      "entities.7.afterburner_readiness": { available: true, value: "ACTIF" }
    },
    transport: { synchronized: true, baseline: 1, deltaSequence: 1, manifestId: 1 },
    quality: { packets: 1, transportGapCount: 0, decodeErrorCount: 0, resyncCount: 0, channels: [] },
    capture: { active: false, path: null },
    replay: { path: null, playing: false, speed: 1, position: 0, packetCount: 0 }
  };
  await page.addInitScript((payload) => {
    class MockWebSocket {
      static OPEN = 1;
      static CLOSED = 3;
      readyState = MockWebSocket.OPEN;
      onopen: ((event: Event) => void) | null = null;
      onmessage: ((event: MessageEvent) => void) | null = null;
      onclose: ((event: CloseEvent) => void) | null = null;
      onerror: ((event: Event) => void) | null = null;
      constructor() {
        setTimeout(() => {
          this.onopen?.(new Event("open"));
          this.onmessage?.(new MessageEvent("message", { data: JSON.stringify(payload) }));
        }, 0);
      }
      close() {
        this.readyState = MockWebSocket.CLOSED;
        this.onclose?.(new CloseEvent("close"));
      }
    }
    Object.defineProperty(window, "WebSocket", { value: MockWebSocket });
  }, snapshot);
}

async function mockIntegritySnapshot(page: import("@playwright/test").Page) {
  const subsystemNames = [
    "Moteur principal", "Tourelle avant", "Senseurs", "Réacteur", "Radar",
    "Navigation", "Communications", "Armement primaire", "Propulseur gauche",
    "Propulseur droit", "Hangar", "Contrôle de manœuvre", "Tourelle arrière",
    "Générateur auxiliaire"
  ];
  const subsystemTypes = [1, 2, 7, 8, 3, 4, 5, 6, 1, 1, 10, 9, 2, 13];
  const subsystemRecords = subsystemNames.map((_, index) => ({
    entity_id: "7",
    subsystem_id: index + 1,
    canonical_index: index,
    type: subsystemTypes[index],
    current_hits: index === 1 ? 0 : index === 2 ? 30 : 100 - index * 3,
    max_hits: index === 13 ? 0 : 100,
    subsystem_flags: index === 2 ? 0x21 : 0,
    producer_sample_time_us: "1000000",
    presence: index === 1 || index === 12 ? "128" : "0",
    ...(index === 1 || index === 12
      ? { turret: { cooldown_remaining_us: index === 1 ? "0" : "250000" } }
      : {})
  }));
  const snapshot = {
    schema: "DashboardSnapshotV1",
    publishedAtUtc: "2026-08-03T10:00:00Z",
    mode: "live",
    connection: {
      status: "Live",
      host: "127.0.0.1",
      port: 42042,
      sessionId: "43",
      lastLiveObservedUtc: "2026-08-03T10:00:00Z",
      staleReason: null
    },
    session: {},
    mission: { phase: "ACTIVE" },
    playerEntityId: "7",
    records: {
      SHIP_IDENTITY: [{ entity_id: "7", ship_class_id: 4 }],
      ENTITY_LIFECYCLE: [{
        entity_id: "7", presence: "0", producer_sample_time_us: "1000000",
        lifecycle_phase: 1, lifecycle_flags: 0
      }],
      DAMAGE_STATE: [{
        entity_id: "7", presence: "6", producer_sample_time_us: "1000000",
        hull_strength: 25, dynamic_max_hull: 100, protection_flags: 0x0004,
        armor_id: 9, guardian_threshold: 10
      }],
      SHIELD_STATE: [{
        entity_id: "7", presence: "7", producer_sample_time_us: "1000000",
        has_shields: 1, segment_count: 4, reserved: 0,
        segment_current_hits: [100, 25, 50, 0], segment_max_hits: [100, 50, 100, 100],
        recharge_max: 225, regeneration_per_s: 25, deferred_energy_transfer: 5
      }],
      SUBSYSTEM_STATE: [
        ...subsystemRecords,
        {
          entity_id: "99", subsystem_id: 99, canonical_index: 0, type: 1,
          current_hits: 0, max_hits: 100, subsystem_flags: 1,
          producer_sample_time_us: "1000000", presence: "0"
        }
      ]
    },
    recordInstances: {},
    manifest: {
      id: 2,
      records: {
        "CLASS_MANIFEST/class_id=4": {
          recordName: "CLASS_MANIFEST",
          fields: {
            class_id: 4,
            internal_name: "GTF Integrity",
            subsystem_definitions: subsystemNames.map((name, index) => ({
              subsystem_id: index + 1,
              canonical_index: index,
              type: subsystemTypes[index],
              internal_name: name,
              local_position: [index, 0, 0],
              radius: 1,
              max_hits: index === 13 ? 0 : 100,
              static_flags: 0
            }))
          }
        }
      }
    },
    derived: {
      "entities.7.lifecycle_label": { available: true, value: "ACTIVE" },
      "entities.7.hull_ratio": { available: true, value: 0.25 },
      "entities.7.hull_missing_hits": { available: true, value: 75 },
      "entities.7.hull_damage_ratio": { available: true, value: 0.75 },
      "entities.7.guardian_margin": { available: true, value: 15 },
      "entities.7.shield_current_total": { available: true, value: 175 },
      "entities.7.shield_max_total": { available: true, value: 350 },
      "entities.7.shield_ratio": { available: true, value: 0.5 },
      "entities.7.shield_segment_ratios": { available: true, value: [1, 0.5, 0.5, 0] },
      "entities.7.shield_weakest_segment_index": { available: true, value: 3 },
      "entities.7.shield_weakest_segment_ratio": { available: true, value: 0 },
      "entities.7.shield_deficit": { available: true, value: 175 },
      "entities.7.shield_recharge_eta_s": { available: true, value: 7 },
      ...Object.fromEntries(subsystemRecords.flatMap((record) => {
        const prefix = `entities.7.subsystems.${record.subsystem_id}`;
        const maximum = Number(record.max_hits);
        const current = Number(record.current_hits);
        return [
          [`${prefix}.integrity_ratio`, {
            available: maximum > 0,
            reason: maximum > 0 ? null : "missing-or-nonpositive-denominator",
            value: maximum > 0 ? current / maximum : null
          }],
          [`${prefix}.destroyed`, {
            available: true,
            value: maximum > 0 && current <= 0
          }],
          [`${prefix}.missing_hits`, {
            available: true,
            value: Math.max(maximum - current, 0)
          }]
        ];
      }))
    },
    transport: { synchronized: true, baseline: 2, deltaSequence: 4, manifestId: 2 },
    quality: { packets: 10, transportGapCount: 0, decodeErrorCount: 0, resyncCount: 0, channels: [] },
    capture: { active: false, path: null },
    replay: { path: null, playing: false, speed: 1, position: 0, packetCount: 0 }
  };
  await page.addInitScript((payload) => {
    class MockWebSocket {
      static OPEN = 1;
      static CLOSED = 3;
      readyState = MockWebSocket.OPEN;
      onopen: ((event: Event) => void) | null = null;
      onmessage: ((event: MessageEvent) => void) | null = null;
      onclose: ((event: CloseEvent) => void) | null = null;
      onerror: ((event: Event) => void) | null = null;
      constructor() {
        setTimeout(() => {
          this.onopen?.(new Event("open"));
          this.onmessage?.(new MessageEvent("message", { data: JSON.stringify(payload) }));
        }, 0);
      }
      close() {
        this.readyState = MockWebSocket.CLOSED;
        this.onclose?.(new CloseEvent("close"));
      }
    }
    Object.defineProperty(window, "WebSocket", { value: MockWebSocket });
  }, snapshot);
}

async function mockWeaponSnapshot(page: import("@playwright/test").Page) {
  const primaryBanks = [
    {
      presence: 33, canonical_index: 0, bank_id: 11, weapon_class_id: 101,
      cooldown_remaining_us: "500000", primary_slot: 0, fire_point: 0,
      simultaneous_slots: 2, pattern_id: 1,
      ammunition: { current: 40, initial: 80, capacity: 80, rearm_remaining_us: "2000000" },
      fof_cooldown_remaining_us: "100000"
    },
    {
      presence: 0, canonical_index: 1, bank_id: 12, weapon_class_id: 102,
      cooldown_remaining_us: "0", primary_slot: 1, fire_point: 1,
      simultaneous_slots: 1, pattern_id: 0
    }
  ];
  const secondaryBanks = [
    {
      presence: 1, canonical_index: 0, bank_id: 21, weapon_class_id: 201,
      cooldown_remaining_us: "0", secondary_slot: 0,
      ammunition: { current: 8, initial: 16, capacity: 16 }
    },
    {
      presence: 5, canonical_index: 1, bank_id: 22, weapon_class_id: 202,
      cooldown_remaining_us: "750000", secondary_slot: 1,
      ammunition: { current: 2, initial: 6, capacity: 6 },
      burst: { counter: 1, reserved: 0, seed: 42 }
    }
  ];
  const snapshot = {
    schema: "DashboardSnapshotV1",
    publishedAtUtc: "2026-08-03T10:00:00Z",
    mode: "live",
    connection: {
      status: "Live", host: "127.0.0.1", port: 42042, sessionId: "44",
      lastLiveObservedUtc: "2026-08-03T10:00:00Z", staleReason: null
    },
    session: {},
    mission: { phase: "ACTIVE" },
    playerEntityId: "7",
    records: {
      SHIP_IDENTITY: [{ entity_id: "7", ship_class_id: 5 }],
      WEAPON_STATE: [{
        entity_id: "7", presence: "255", producer_sample_time_us: "1000000",
        primary_bank_count: 2, secondary_bank_count: 2, tertiary_bank_count: 1,
        current_primary_bank_id: 11, current_secondary_bank_id: 22,
        current_tertiary_bank_id: 31, weapon_flags: 0x008f,
        previous_primary_bank_id: 12, previous_secondary_bank_id: 21,
        targeting_laser_bank_id: 11,
        swarm: { remaining: 3, origin_bank_id: 22 },
        remote_detonation_remaining_us: "1250000",
        primary_banks: primaryBanks,
        secondary_banks: secondaryBanks,
        tertiary: {
          bank_id: 31, ammunition_current: 3, ammunition_initial: 6,
          ammunition_capacity: 6, cooldown_remaining_us: "0",
          rearm_remaining_us: "0"
        },
        countermeasure: {
          presence: 1, flags: 1, weapon_class_id: 301,
          current: 4, maximum: 8, cooldown_remaining_us: "0"
        }
      }],
      ENERGY_STATE: [{
        entity_id: "7", presence: "1", producer_sample_time_us: "1000000",
        weapon_energy_current: 62, weapon_energy_max: 100
      }],
      CONTROL_STATE: [{
        entity_id: "7", producer_sample_time_us: "1000000",
        fire_primary_count: 12, fire_secondary_count: 4, fire_countermeasure_count: 1
      }],
      SUBSYSTEM_STATE: [{
        entity_id: "7", subsystem_id: 9, canonical_index: 0, type: 2,
        current_hits: 90, max_hits: 100, subsystem_flags: 0,
        producer_sample_time_us: "1000000", presence: "128",
        turret: { target_entity_id: "42", cooldown_remaining_us: "250000" }
      }]
    },
    recordInstances: {},
    manifest: {
      id: 3,
      records: {
        "CLASS_MANIFEST/class_id=5": {
          recordName: "CLASS_MANIFEST",
          fields: {
            class_id: 5, internal_name: "GTF Weapons",
            banks: [...primaryBanks, ...secondaryBanks].map((bank) => ({
              bank_id: bank.bank_id,
              canonical_index: bank.canonical_index,
              weapon_class_id: bank.weapon_class_id,
              fire_points: [[0, 0, 1], [1, 0, 1]]
            })),
            subsystem_definitions: [{
              subsystem_id: 9, canonical_index: 0, type: 2, internal_name: "Tourelle dorsale"
            }]
          }
        },
        "WEAPON_MANIFEST/weapon_class_id=101": {
          recordName: "WEAPON_MANIFEST",
          fields: {
            weapon_class_id: 101, title: "Mekhu HL-7D", internal_name: "Mekhu",
            subtype: 1, weapon_flags: 2, max_speed: 900,
            ranges: { minimum: 0, optimal: 700, maximum: 1200 },
            fire: { wait_us: "250000", energy_consumed: 1.5 },
            damage: { amount: 18, damage_type_id: 1, effect_flags: 0 }
          }
        },
        "WEAPON_MANIFEST/weapon_class_id=102": {
          recordName: "WEAPON_MANIFEST",
          fields: {
            weapon_class_id: 102, internal_name: "Prometheus S",
            subtype: 1, weapon_flags: 0,
            fire: { wait_us: "180000", energy_consumed: 2.2 },
            damage: { amount: 24 }, ranges: { minimum: 0, optimal: 800, maximum: 1400 }
          }
        },
        "WEAPON_MANIFEST/weapon_class_id=201": {
          recordName: "WEAPON_MANIFEST",
          fields: {
            weapon_class_id: 201, internal_name: "Harpoon", subtype: 2,
            weapon_flags: 0x40, fire: { wait_us: "1000000", energy_consumed: 0 },
            damage: { amount: 65 }, ranges: { minimum: 100, optimal: 1100, maximum: 1800 },
            guidance: { type: 2, fov_rad: 0.4 }, lock: { time_us: "1500000", fov_rad: 0.2 }
          }
        },
        "WEAPON_MANIFEST/weapon_class_id=202": {
          recordName: "WEAPON_MANIFEST",
          fields: {
            weapon_class_id: 202, title: "Tornado", internal_name: "Tornado",
            subtype: 2, weapon_flags: 0x50,
            fire: { wait_us: "750000", energy_consumed: 0 },
            damage: { amount: 42 }, ranges: { minimum: 100, optimal: 900, maximum: 1500 },
            guidance: { type: 4, fov_rad: 0.5 }, lock: { time_us: "1200000", fov_rad: 0.2 },
            burst: { count: 4, interval_us: "80000" },
            swarm: { count: 4, shots_per_trigger: 1 }
          }
        },
        "WEAPON_MANIFEST/weapon_class_id=301": {
          recordName: "WEAPON_MANIFEST",
          fields: {
            weapon_class_id: 301, title: "Chaff", internal_name: "Chaff",
            subtype: 4, weapon_flags: 0x20
          }
        }
      }
    },
    derived: {
      "entities.7.primary_banks[0].ammo_ratio": { available: true, value: 0.5 },
      "entities.7.primary_banks[0].cooldown_s": { available: true, value: 0.5 },
      "entities.7.primary_banks[0].rearm_s": { available: true, value: 2 },
      "entities.7.primary_banks[0].state": { available: true, value: "RECHARGE" },
      "entities.7.primary_banks[1].ammo_ratio": { available: false, reason: "bank-does-not-use-ammunition", value: null },
      "entities.7.primary_banks[1].cooldown_s": { available: true, value: 0 },
      "entities.7.primary_banks[1].state": { available: true, value: "DISPONIBLE" },
      "entities.7.secondary_banks[0].ammo_ratio": { available: true, value: 0.5 },
      "entities.7.secondary_banks[0].cooldown_s": { available: true, value: 0 },
      "entities.7.secondary_banks[0].state": { available: true, value: "DISPONIBLE" },
      "entities.7.secondary_banks[1].ammo_ratio": { available: true, value: 1 / 3 },
      "entities.7.secondary_banks[1].cooldown_s": { available: true, value: 0.75 },
      "entities.7.secondary_banks[1].state": { available: true, value: "RECHARGE" },
      "weapon_classes.101.nominal_rate_hz": { available: true, value: 4 },
      "weapon_classes.102.nominal_rate_hz": { available: true, value: 5.555 },
      "weapon_classes.201.nominal_rate_hz": { available: true, value: 1 },
      "weapon_classes.202.nominal_rate_hz": { available: true, value: 1.333 },
      "entities.7.tertiary.state": { available: true, value: "DISPONIBLE" },
      "entities.7.countermeasure.quantity_ratio": { available: true, value: 0.5 },
      "entities.7.countermeasure.state": { available: true, value: "DISPONIBLE" }
    },
    transport: { synchronized: true, baseline: 3, deltaSequence: 7, manifestId: 3 },
    quality: { packets: 10, transportGapCount: 0, decodeErrorCount: 0, resyncCount: 0, channels: [] },
    capture: { active: false, path: null },
    replay: { path: null, playing: false, speed: 1, position: 0, packetCount: 0 }
  };
  await page.addInitScript((payload) => {
    class MockWebSocket {
      static OPEN = 1;
      static CLOSED = 3;
      readyState = MockWebSocket.OPEN;
      onopen: ((event: Event) => void) | null = null;
      onmessage: ((event: MessageEvent) => void) | null = null;
      onclose: ((event: CloseEvent) => void) | null = null;
      onerror: ((event: Event) => void) | null = null;
      constructor() {
        setTimeout(() => {
          this.onopen?.(new Event("open"));
          this.onmessage?.(new MessageEvent("message", { data: JSON.stringify(payload) }));
        }, 0);
      }
      close() {
        this.readyState = MockWebSocket.CLOSED;
        this.onclose?.(new CloseEvent("close"));
      }
    }
    Object.defineProperty(window, "WebSocket", { value: MockWebSocket });
  }, snapshot);
}

async function mockSupportSnapshot(page: import("@playwright/test").Page) {
  const identities = Array.from({ length: 10 }, (_, index) => ({
    entity_id: String(index + 1),
    ship_class_id: index === 2 ? 8 : 7,
    internal_name: index === 0 ? "GTF Joueur" : index === 1 ? "Support Alpha" : `Docké ${index + 1}`
  }));
  const docking = identities.map((identity, index) => ({
    entity_id: identity.entity_id,
    presence: "0",
    producer_sample_time_us: "1000000",
    phase: index === 0 ? 3 : 0,
    group_leader_entity_id: "1",
    relations: index === 0
      ? identities.slice(1).map((remote, remoteIndex) => ({
          item_version: 1, item_size: 32, remote_entity_id: remote.entity_id,
          local_dockpoint: remoteIndex + 1, remote_dockpoint: 0,
          local_dock_bay_name: `Port ${remoteIndex + 1}`,
          remote_dock_bay_name: "Port principal"
        }))
      : [{
          item_version: 1, item_size: 32, remote_entity_id: "1",
          local_dockpoint: 0, remote_dockpoint: index,
          local_dock_bay_name: "Port principal", remote_dock_bay_name: `Port ${index}`
        }]
  }));
  const snapshot = {
    schema: "DashboardSnapshotV1",
    publishedAtUtc: "2026-08-03T10:00:00Z",
    mode: "live",
    connection: {
      status: "Live", host: "127.0.0.1", port: 42042, sessionId: "45",
      lastLiveObservedUtc: "2026-08-03T10:00:00Z", staleReason: null
    },
    session: {},
    mission: { phase: "ACTIVE" },
    playerEntityId: "1",
    records: {
      SHIP_IDENTITY: identities,
      ENTITY_LIFECYCLE: identities.map((identity) => ({
        entity_id: identity.entity_id, lifecycle_phase: 1, lifecycle_flags: 0
      })),
      SUPPORT_STATE: [{
        entity_id: "1", presence: "1", producer_sample_time_us: "1000000",
        phase: 2, support_flags: 3, support_entity_id: "2", reserved: "000000"
      }],
      DOCKING_STATE: docking,
      CARGO_SCAN_STATE: [{
        entity_id: "1", presence: "31", producer_sample_time_us: "1000000",
        scan_phase: 2, disclosure: 1, target_entity_id: "3",
        target_subsystem_id: 9, elapsed_us: "2500000", required_us: "10000000",
        validity_flags: 7, cargo_text: "Munitions médicales"
      }],
      FLIGHT_STATE: [
        {
          entity_id: "1", producer_sample_time_us: "1000000",
          position_world: [0, 0, 0], velocity_world: [0, 0, 0]
        },
        {
          entity_id: "2", producer_sample_time_us: "1000000",
          position_world: [300, 400, 0], velocity_world: [-30, -40, 0]
        }
      ],
      DAMAGE_STATE: [{
        entity_id: "1", producer_sample_time_us: "1000000",
        hull_strength: 50, dynamic_max_hull: 100
      }],
      SHIELD_STATE: [{
        entity_id: "1", producer_sample_time_us: "1000000",
        has_shields: 1, segment_current_hits: [50, 50, 50, 50],
        segment_max_hits: [100, 100, 100, 100]
      }],
      SUBSYSTEM_STATE: [{
        entity_id: "1", subsystem_id: 4, current_hits: 25, max_hits: 100
      }],
      WEAPON_STATE: [{
        entity_id: "1", producer_sample_time_us: "1000000",
        primary_banks: [{ ammunition: { current: 5, initial: 10 } }],
        secondary_banks: [{ ammunition: { current: 2, initial: 8 } }],
        tertiary: { ammunition_current: 0, ammunition_initial: 4 },
        countermeasure: { current: 3, maximum: 6 }
      }]
    },
    recordInstances: {},
    manifest: {
      id: 4,
      records: {
        "CLASS_MANIFEST/class_id=8": {
          recordName: "CLASS_MANIFEST",
          fields: {
            class_id: 8,
            scan: { required_time_us: "10000000", maximum_distance: 1000, maximum_angle_rad: 0.317 },
            subsystem_definitions: [{ subsystem_id: 9, internal_name: "Conteneur central" }]
          }
        }
      }
    },
    derived: {
      "entities.1.hull_ratio": { available: true, value: 0.5 },
      "entities.1.shield_ratio": { available: true, value: 0.5 },
      "entities.1.subsystems.4.integrity_ratio": { available: true, value: 0.25 },
      "entities.1.primary_banks[0].ammo_ratio": { available: true, value: 0.5 },
      "entities.1.secondary_banks[0].ammo_ratio": { available: true, value: 0.25 },
      "entities.1.tertiary.ammo_ratio": { available: true, value: 0 },
      "entities.1.countermeasure.quantity_ratio": { available: true, value: 0.5 },
      "entities.1.cargo.progress_ratio": { available: true, value: 0.25 },
      "entities.1.cargo.remaining_us": { available: true, value: 7500000 },
      "entities.1.support.distance": { available: true, value: 500 },
      "entities.1.support.relative_speed": { available: true, value: 50 },
      "entities.1.support.closing_speed": { available: true, value: 50 }
    },
    transport: { synchronized: true, baseline: 4, deltaSequence: 8, manifestId: 4 },
    quality: { packets: 10, transportGapCount: 0, decodeErrorCount: 0, resyncCount: 0, channels: [] },
    capture: { active: false, path: null },
    replay: { path: null, playing: false, speed: 1, position: 0, packetCount: 0 }
  };
  await page.addInitScript((payload) => {
    class MockWebSocket {
      static OPEN = 1;
      static CLOSED = 3;
      readyState = MockWebSocket.OPEN;
      onopen: ((event: Event) => void) | null = null;
      onmessage: ((event: MessageEvent) => void) | null = null;
      onclose: ((event: CloseEvent) => void) | null = null;
      onerror: ((event: Event) => void) | null = null;
      constructor() {
        setTimeout(() => {
          this.onopen?.(new Event("open"));
          this.onmessage?.(new MessageEvent("message", { data: JSON.stringify(payload) }));
        }, 0);
      }
      close() {
        this.readyState = MockWebSocket.CLOSED;
        this.onclose?.(new CloseEvent("close"));
      }
    }
    Object.defineProperty(window, "WebSocket", { value: MockWebSocket });
  }, snapshot);
}

async function mockTacticalSnapshot(
  page: import("@playwright/test").Page,
  contactCount = 24
) {
  const contacts = Array.from({ length: contactCount }, (_, index) => {
    const id = String(101 + index);
    const angle = (index / Math.max(contactCount, 1)) * Math.PI * 2;
    const distance = 200 + (index % 12) * 70;
    return {
      entity_id: "1",
      contact_entity_id: id,
      presence: index < 4 ? "2" : "0",
      producer_sample_time_us: "1000000",
      object_type: index === 1 ? 2 : 1,
      category: index === 1 ? 2 : 1,
      visibility: index % 9 === 0 ? 2 : 1,
      position_world: [
        Math.sin(angle) * distance,
        (index % 3 - 1) * 20,
        Math.cos(angle) * distance
      ],
      velocity_world: [0, 0, index === 1 ? -120 : -10],
      radius: index === 1 ? 2 : 12,
      contact_flags: index === 0 ? 0x02 : index === 1 ? 0xe0 : 0,
      ...(index < 4 ? { revealed_name: index === 0 ? "Cible Alpha" : `Contact ${id}` } : {})
    };
  });
  const derived = Object.fromEntries(contacts.flatMap((contact, index) => {
    const id = String(contact.contact_entity_id);
    const distance = Math.hypot(
      Number(contact.position_world[0]),
      Number(contact.position_world[1]),
      Number(contact.position_world[2])
    );
    const rawX = Number(contact.position_world[0]) / 1000;
    const rawY = -Number(contact.position_world[2]) / 1000;
    const radius = Math.hypot(rawX, rawY);
    const divisor = Math.max(1, radius);
    const prefix = `entities.1.tracks.${id}`;
    return [
      [`${prefix}.distance`, { available: true, value: distance }],
      [`${prefix}.relative_speed`, { available: true, value: index === 1 ? 120 : 10 }],
      [`${prefix}.closing_speed`, { available: true, value: index === 1 ? 115 : 8 }],
      [`${prefix}.ttc_s`, { available: true, value: index === 1 ? distance / 115 : distance / 8 }],
      [`${prefix}.age_us`, { available: true, value: 0 }],
      [`${prefix}.bearing_local_rad`, { available: true, value: Math.atan2(rawX, -rawY) }],
      [`${prefix}.elevation_local_rad`, { available: true, value: Number(contact.position_world[1]) / Math.max(distance, 1) }],
      [`${prefix}.scope_clamped_position`, { available: true, value: [rawX / divisor, rawY / divisor] }],
      [`${prefix}.scope_in_range`, { available: true, value: radius <= 1 }]
    ];
  }));
  const snapshot = {
    schema: "DashboardSnapshotV1",
    publishedAtUtc: "2026-08-06T12:00:00Z",
    mode: "live",
    connection: {
      status: "Live",
      host: "127.0.0.1",
      port: 42042,
      sessionId: "46",
      lastLiveObservedUtc: "2026-08-06T12:00:00Z",
      staleReason: null
    },
    session: {},
    mission: { phase: "ACTIVE" },
    playerEntityId: "1",
    records: {
      FLIGHT_STATE: [{
        entity_id: "1", producer_sample_time_us: "1000000",
        position_world: [0, 0, 0], velocity_world: [0, 0, 0],
        orientation_local_to_world: [1, 0, 0, 0]
      }],
      RADAR_STATE: [{
        entity_id: "1", presence: "12", producer_sample_time_us: "1000000",
        radar_mode: 0, selected_range: 1000, sensor_state: 2,
        sensor_current_hits: 80, sensor_max_hits: 100,
        awacs_intensity: 0.75, awacs_range: 1500,
        emp_intensity: 0.2, emp_remaining_us: "2500000"
      }],
      RADAR_CONTACTS: contacts,
      TARGET_STATE: [{
        entity_id: "1", presence: "902", producer_sample_time_us: "1000000",
        current_target_entity_id: "101", previous_target_entity_id: "103",
        revealed_identity: { object_type: 1, name: "Cible Alpha", class_id: 7, team_id: 2, iff_id: 3 },
        time_on_target_us: "3200000", target_subsystem_id: 4,
        distance_trend: 1, speed_trend: 2, in_cone: true,
        lead_world: [20, 0, 300], lead_bank_id: 21
      }],
      LOCK_STATE: [{
        entity_id: "1", presence: "0", producer_sample_time_us: "1000000",
        locks: [{
          item_version: 1, item_size: 40, presence: 3,
          locked: false, target_in_lock_cone: true,
          target_entity_id: "101", subsystem_id: 4,
          world_position: [0, 0, 300], time_to_lock_remaining_us: "1000000"
        }]
      }],
      THREAT_STATE: [{
        entity_id: "1", presence: "4", producer_sample_time_us: "1000000",
        threat_level: 2, nearest_homing_entity_id: "102",
        incoming_missiles: [{
          item_version: 1, item_size: 80, presence: 0,
          guidance_type: 3, radar_visibility: 1,
          entity_id: "102", weapon_class_id: 9, target_entity_id: "1",
          position_world: [0, 0, 600], orientation_local_to_world: [1, 0, 0, 0],
          velocity_world: [0, 0, -120]
        }]
      }],
      HUD_ALERT_STATE: [{
        entity_id: "1", presence: "0", producer_sample_time_us: "1000000",
        primary_fire_threat_active: false, missile_lock_state: 1
      }]
    },
    recordInstances: {},
    manifest: {
      id: 5,
      records: {
        "WEAPON_MANIFEST/weapon_class_id=9": {
          recordName: "WEAPON_MANIFEST",
          fields: { weapon_class_id: 9, title: "Harpoon entrant", internal_name: "Harpoon", lock: { time_us: "2000000" } }
        },
        "CLASS_MANIFEST/class_id=7": {
          recordName: "CLASS_MANIFEST",
          fields: {
            class_id: 7,
            internal_name: "GTF Tactical",
            subsystem_definitions: [{ subsystem_id: 4, internal_name: "Moteurs" }]
          }
        }
      }
    },
    derived: {
      ...derived,
      "entities.1.sensor_integrity_ratio": { available: true, value: 0.8 },
      "entities.1.target.distance": { available: true, value: 200 },
      "entities.1.locks[0].progress": { available: true, value: 0.5 },
      "entities.1.missiles.102.distance": { available: true, value: 600 },
      "entities.1.missiles.102.relative_speed": { available: true, value: 120 },
      "entities.1.missiles.102.closing_speed": { available: true, value: 120 },
      "entities.1.missiles.102.ttc_s": { available: true, value: 5 }
    },
    transport: { synchronized: true, baseline: 5, deltaSequence: 9, manifestId: 5 },
    quality: { packets: 20, transportGapCount: 0, decodeErrorCount: 0, resyncCount: 0, channels: [] },
    capture: { active: false, path: null },
    replay: { path: null, playing: false, speed: 1, position: 0, packetCount: 0 }
  };
  await page.addInitScript((payload) => {
    class MockWebSocket {
      static OPEN = 1;
      static CLOSED = 3;
      readyState = MockWebSocket.OPEN;
      onopen: ((event: Event) => void) | null = null;
      onmessage: ((event: MessageEvent) => void) | null = null;
      onclose: ((event: CloseEvent) => void) | null = null;
      onerror: ((event: Event) => void) | null = null;
      constructor() {
        setTimeout(() => {
          this.onopen?.(new Event("open"));
          this.onmessage?.(new MessageEvent("message", { data: JSON.stringify(payload) }));
        }, 0);
      }
      close() {
        this.readyState = MockWebSocket.CLOSED;
        this.onclose?.(new CloseEvent("close"));
      }
    }
    Object.defineProperty(window, "WebSocket", { value: MockWebSocket });
  }, snapshot);
}

test("all cockpit tabs remain reachable and tactical future data is ND", async ({ page }) => {
  await page.goto("/");
  const tabs = page.getByRole("navigation", { name: "Sections du cockpit" }).getByRole("button");
  await expect(tabs).toHaveCount(10);
  await page.getByRole("button", { name: /Tactique/ }).click();
  await expect(page.getByRole("heading", { name: "Tactique" })).toBeVisible();
  await expect(page.getByText("Brouillage global quantifié", { exact: true })).toBeVisible();
  await expect(page.locator(".tactical-future").getByText("ND", { exact: true })).toBeVisible();
});

test("the 1920x1080 tactical cockpit keeps its five combat zones readable", async ({ page }) => {
  await mockTacticalSnapshot(page);
  await page.goto("/");
  await page.getByRole("button", { name: /Tactique/ }).click();
  await expect(page.getByRole("heading", { name: "Tactique" })).toBeVisible();
  const dimensions = await page.evaluate(() => ([
    document.documentElement.scrollWidth,
    document.documentElement.clientWidth,
    document.documentElement.scrollHeight,
    document.documentElement.clientHeight,
    document.querySelector("main")?.scrollHeight ?? 0,
    document.querySelector("main")?.clientHeight ?? 0,
    document.querySelector(".tactical-scope")?.getBoundingClientRect().width ?? 0,
    document.querySelector(".tactical-target")?.getBoundingClientRect().width ?? 0
  ]));
  expect(dimensions[0]).toBeLessThanOrEqual(dimensions[1]);
  expect(dimensions[2]).toBeLessThanOrEqual(dimensions[3]);
  expect(dimensions[4]).toBeLessThanOrEqual(dimensions[5]);
  expect(dimensions[6]).toBeGreaterThan(dimensions[7]);
  await expect(page.getByText("COURTE", { exact: true })).toBeVisible();
  await expect(page.getByText("Cible Alpha", { exact: true }).first()).toBeVisible();
  await expect(page.locator(".tactical-target .target-card > strong")).toHaveText("Cible Alpha");
  await expect(page.locator(".tactical-target .target-class")).toHaveText("GTF Tactical");
  const identityLinesFit = await page.locator(".tactical-target").evaluate((panel) =>
    Array.from(panel.querySelectorAll<HTMLElement>(".target-card > strong, .target-class"))
      .every((line) => line.clientHeight >= line.scrollHeight)
  );
  expect(identityLinesFit).toBe(true);
  await expect(page.getByText("ACQUISITION", { exact: true })).toBeVisible();
  await expect(page.getByText("LOCK EN COURS", { exact: true })).toBeVisible();
  await expect(page.getByText("Harpoon entrant", { exact: true })).toBeVisible();
  await expect(page.getByLabel(/Scope radar affichant 24 contacts/)).toBeVisible();
});

test("tactical contacts, locks and missiles remain keyboard inspectable", async ({ page }) => {
  await mockTacticalSnapshot(page);
  await page.goto("/");
  await page.getByRole("button", { name: /Tactique/ }).click();
  const contact = page.locator(".scope-contact-list").getByRole("button", { name: /Cible Alpha/ });
  await contact.focus();
  await page.keyboard.press("Enter");
  await expect(page.getByText("INSPECTION TACTIQUE", { exact: true })).toBeVisible();
  await expect(page.getByText("RADAR_CONTACTS", { exact: true })).toBeVisible();
  await page.getByRole("button", { name: "Fermer" }).click();
  const lock = page.locator(".lock-list").getByRole("button", { name: /Cible Alpha/ });
  await lock.focus();
  await page.keyboard.press("Enter");
  await expect(page.getByText("LOCK_STATE", { exact: true })).toBeVisible();
  await page.getByRole("button", { name: "Fermer" }).click();
  const missile = page.locator(".missile-rack").getByRole("button", { name: /Harpoon entrant/ });
  await missile.focus();
  await page.keyboard.press("Enter");
  await expect(page.getByText("THREAT_STATE", { exact: true })).toBeVisible();
  await expect(page.getByText("Manifeste autorisé", { exact: true })).toBeVisible();
});

test("the canvas scope accepts the full 4096-contact contract without DOM growth", async ({ page }) => {
  test.setTimeout(30_000);
  await mockTacticalSnapshot(page, 4096);
  await page.goto("/");
  await page.getByRole("button", { name: /Tactique/ }).click();
  await expect(page.getByLabel(/Scope radar affichant 4096 contacts/)).toBeVisible();
  await expect(page.locator(".scope-contact-list > button")).toHaveCount(9);
  const state = await page.evaluate(() => ({
    canvasCount: document.querySelectorAll(".tactical-scope canvas").length,
    contactDomCount: document.querySelectorAll(".scope-contact-list > button").length,
    mainOverflow: (document.querySelector("main")?.scrollHeight ?? 0) >
      (document.querySelector("main")?.clientHeight ?? 0)
  }));
  expect(state.canvasCount).toBe(1);
  expect(state.contactDomCount).toBe(9);
  expect(state.mainOverflow).toBe(false);
});

test("the 1920x1080 Pilotage cockpit fits without scrolling and keeps attitude dominant", async ({ page }) => {
  await page.goto("/");
  const dimensions = await page.evaluate(() => ({
    scrollWidth: document.documentElement.scrollWidth,
    clientWidth: document.documentElement.clientWidth,
    scrollHeight: document.documentElement.scrollHeight,
    clientHeight: document.documentElement.clientHeight,
    mainScrollHeight: document.querySelector("main")?.scrollHeight ?? 0,
    mainClientHeight: document.querySelector("main")?.clientHeight ?? 0,
    attitude: document.querySelector(".attitude-zone")?.getBoundingClientRect().toJSON(),
    movement: document.querySelector(".movement-zone")?.getBoundingClientRect().toJSON()
  }));
  expect(dimensions.scrollWidth).toBeLessThanOrEqual(dimensions.clientWidth);
  expect(dimensions.scrollHeight).toBeLessThanOrEqual(dimensions.clientHeight);
  expect(dimensions.mainScrollHeight).toBeLessThanOrEqual(dimensions.mainClientHeight);
  expect(dimensions.attitude?.height).toBeGreaterThan((dimensions.movement?.height ?? 0) * 1.8);
  expect(dimensions.attitude?.width).toBeGreaterThan(dimensions.movement?.width ?? 0);
  await expect(page.getByText("COORDONNÉES HUD")).toBeVisible();
  await expect(page.getByText("HORIZON PLANÉTAIRE")).toBeVisible();
});

test("Pilotage instruments are keyboard inspectable and expose technical detail", async ({ page }) => {
  await page.goto("/");
  const attitude = page.getByRole("button", { name: /Sphère d’attitude inertielle:/ });
  await attitude.focus();
  await page.keyboard.press("Enter");
  await expect(page.getByText("INSPECTION INSTRUMENT", { exact: true })).toBeVisible();
  await expect(page.getByText("Valeur brute", { exact: true })).toBeVisible();
  await expect(page.getByText("Entité", { exact: true })).toBeVisible();
  await expect(page.getByText("Présence", { exact: true })).toBeVisible();
});

test("the 1920x1080 energy cockpit is readable without scrolling", async ({ page }) => {
  await mockEnergySnapshot(page);
  await page.goto("/");
  await page.getByRole("button", { name: /Propulsion & énergie/ }).click();
  await expect(page.getByRole("heading", { name: "Propulsion & énergie" })).toBeVisible();
  const dimensions = await page.evaluate(() => ({
    scrollWidth: document.documentElement.scrollWidth,
    clientWidth: document.documentElement.clientWidth,
    scrollHeight: document.documentElement.scrollHeight,
    clientHeight: document.documentElement.clientHeight,
    mainScrollHeight: document.querySelector("main")?.scrollHeight ?? 0,
    mainClientHeight: document.querySelector("main")?.clientHeight ?? 0,
    resources: document.querySelector(".energy-resources-zone")?.getBoundingClientRect().toJSON(),
    ets: document.querySelector(".energy-ets-zone")?.getBoundingClientRect().toJSON()
  }));
  expect(dimensions.scrollWidth).toBeLessThanOrEqual(dimensions.clientWidth);
  expect(dimensions.scrollHeight).toBeLessThanOrEqual(dimensions.clientHeight);
  expect(dimensions.mainScrollHeight).toBeLessThanOrEqual(dimensions.mainClientHeight);
  expect(dimensions.resources?.height).toBeGreaterThan((dimensions.ets?.height ?? 0) * 1.8);
  expect(dimensions.resources?.width).toBeGreaterThan(dimensions.ets?.width ?? 0);
  await expect(page.getByRole("button", { name: /Performances nominales de classe:/ })).toBeVisible();
  await expect(page.getByText("PUISSANCE MOTEUR DYNAMIQUE")).toBeVisible();
  await expect(page.getByText("RCS")).toBeVisible();
  await expect(page.getByText("50%", { exact: true })).toHaveCount(2);
  await expect(page.getByText("40%", { exact: true })).toBeVisible();
  await expect(page.getByText("ACTIF", { exact: true })).toBeVisible();
});

test("energy panels are keyboard inspectable with associated raw values", async ({ page }) => {
  await page.goto("/");
  await page.getByRole("button", { name: /Propulsion & énergie/ }).click();
  const resources = page.getByRole("button", { name: /Réserves énergétiques:/ });
  await resources.focus();
  await page.keyboard.press("Enter");
  await expect(page.getByText("INSPECTION INSTRUMENT", { exact: true })).toBeVisible();
  await expect(page.getByText("Valeurs associées", { exact: true })).toBeVisible();
  await expect(page.getByText("ENERGY_STATE.entity_id", { exact: true })).toBeVisible();
});

test("the 1920x1080 integrity cockpit stays readable and player-scoped", async ({ page }) => {
  await mockIntegritySnapshot(page);
  await page.goto("/");
  await page.getByRole("button", { name: /Intégrité/ }).click();
  await expect(page.getByRole("heading", { name: "Intégrité" })).toBeVisible();
  const dimensions = await page.evaluate(() => ({
    scrollWidth: document.documentElement.scrollWidth,
    clientWidth: document.documentElement.clientWidth,
    scrollHeight: document.documentElement.scrollHeight,
    clientHeight: document.documentElement.clientHeight,
    mainScrollHeight: document.querySelector("main")?.scrollHeight ?? 0,
    mainClientHeight: document.querySelector("main")?.clientHeight ?? 0,
    core: document.querySelector(".integrity-core-zone")?.getBoundingClientRect().toJSON(),
    protection: document.querySelector(".integrity-protection-zone")?.getBoundingClientRect().toJSON()
  }));
  expect(dimensions.scrollWidth).toBeLessThanOrEqual(dimensions.clientWidth);
  expect(dimensions.scrollHeight).toBeLessThanOrEqual(dimensions.clientHeight);
  expect(dimensions.mainScrollHeight).toBeLessThanOrEqual(dimensions.mainClientHeight);
  expect(dimensions.core?.height).toBeGreaterThan((dimensions.protection?.height ?? 0) * 1.8);
  expect(dimensions.core?.width).toBeGreaterThan(dimensions.protection?.width ?? 0);
  await expect(page.getByText("25%", { exact: true })).toBeVisible();
  const quadrantLabels = page.locator(".shield-quadrant-labels");
  await expect(quadrantLabels.getByText("AVANT", { exact: true })).toBeVisible();
  await expect(quadrantLabels.getByText("ARRIÈRE", { exact: true })).toBeVisible();
  await expect(quadrantLabels.getByText("GAUCHE", { exact: true })).toBeVisible();
  await expect(quadrantLabels.getByText("DROITE", { exact: true })).toBeVisible();
  await expect(page.getByText("4 QUADRANTS", { exact: true })).toBeVisible();
  await expect(page.getByText("PLAFOND RECHARGEABLE", { exact: true })).toBeVisible();
  await expect(page.getByText("225", { exact: true })).toBeVisible();
  await expect(page.getByText("HP/s", { exact: true })).toBeVisible();
  await expect(page.getByText("+ 2 AUTRES · OUVRIR LA LISTE COMPLÈTE", { exact: true })).toBeVisible();
  await expect(page.getByText("SOUS-SYSTÈME 99")).toHaveCount(0);
  await expect(page.getByText(/DIRECTION \/ POSITION IMPACT/)).toBeVisible();
});

test("integrity subsystems remain exhaustive, filterable and keyboard inspectable", async ({ page }) => {
  await mockIntegritySnapshot(page);
  await page.goto("/");
  await page.getByRole("button", { name: /Intégrité/ }).click();
  const destroyedTurret = page.getByRole("button", { name: "Inspecter Tourelle avant" });
  await destroyedTurret.focus();
  await page.keyboard.press("Enter");
  await expect(page.getByText("INSPECTION SOUS-SYSTÈME", { exact: true })).toBeVisible();
  await expect(page.getByText("0 / 100", { exact: true })).toBeVisible();
  await expect(page.getByText("ID sous-système", { exact: true })).toBeVisible();
  await page.getByRole("button", { name: "← LISTE COMPLÈTE" }).click();
  await expect(page.getByText("14 SYSTÈMES · 1 DÉTRUITS", { exact: true })).toBeVisible();
  const filter = page.getByRole("searchbox", { name: "FILTRER PAR NOM, TYPE OU ÉTAT" });
  await filter.fill("auxiliaire");
  await expect(page.getByRole("button", { name: /Générateur auxiliaire/ })).toBeVisible();
  await expect(page.getByRole("button", { name: /Moteur principal/ })).toHaveCount(0);
});

test("the 1920x1080 weapons cockpit is readable and keeps selected weapons dominant", async ({ page }) => {
  await mockWeaponSnapshot(page);
  await page.goto("/");
  await page.getByRole("button", { name: /Armement/ }).click();
  await expect(page.getByRole("heading", { name: "Armement" })).toBeVisible();
  const dimensions = await page.evaluate(() => ({
    scrollWidth: document.documentElement.scrollWidth,
    clientWidth: document.documentElement.clientWidth,
    scrollHeight: document.documentElement.scrollHeight,
    clientHeight: document.documentElement.clientHeight,
    mainScrollHeight: document.querySelector("main")?.scrollHeight ?? 0,
    mainClientHeight: document.querySelector("main")?.clientHeight ?? 0,
    selected: document.querySelector(".weapon-selected-zone")?.getBoundingClientRect().toJSON(),
    primary: document.querySelector(".weapon-primary-zone")?.getBoundingClientRect().toJSON()
  }));
  expect(dimensions.scrollWidth).toBeLessThanOrEqual(dimensions.clientWidth);
  expect(dimensions.scrollHeight).toBeLessThanOrEqual(dimensions.clientHeight);
  expect(dimensions.mainScrollHeight).toBeLessThanOrEqual(dimensions.mainClientHeight);
  expect(dimensions.selected?.width).toBeGreaterThan((dimensions.primary?.width ?? 0) * 1.5);
  const selectedZone = page.locator(".weapon-selected-zone");
  await expect(selectedZone.getByText("Mekhu HL-7D", { exact: true })).toBeVisible();
  await expect(selectedZone.getByText("Tornado", { exact: true })).toBeVisible();
  await expect(page.getByText("DÉGÂTS NOMINAUX", { exact: true })).toHaveCount(2);
  await expect(page.getByText("CADENCE NOMINALE", { exact: true })).toHaveCount(2);
  await expect(page.getByText(/PROGRESSION LOCK/)).toBeVisible();
  await expect(page.getByText(/TIRS \/ IMPACTS CONFIRMÉS/)).toBeVisible();
  await expect(page.getByText("Tourelle dorsale", { exact: true })).toBeVisible();
});

test("the 1920x1080 support cockpit separates service, docking and cargo without scrolling", async ({ page }) => {
  await mockSupportSnapshot(page);
  await page.goto("/");
  await page.getByRole("button", { name: /Support · Docking · Cargo/ }).click();
  await expect(page.getByRole("heading", { name: "Support · Docking · Cargo" })).toBeVisible();
  const dimensions = await page.evaluate(() => ({
    scrollWidth: document.documentElement.scrollWidth,
    clientWidth: document.documentElement.clientWidth,
    scrollHeight: document.documentElement.scrollHeight,
    clientHeight: document.documentElement.clientHeight,
    mainScrollHeight: document.querySelector("main")?.scrollHeight ?? 0,
    mainClientHeight: document.querySelector("main")?.clientHeight ?? 0
  }));
  expect(dimensions.scrollWidth).toBeLessThanOrEqual(dimensions.clientWidth);
  expect(dimensions.scrollHeight).toBeLessThanOrEqual(dimensions.clientHeight);
  expect(dimensions.mainScrollHeight).toBeLessThanOrEqual(dimensions.mainClientHeight);
  await expect(page.locator(".support-identity").getByText("Support Alpha", { exact: true })).toBeVisible();
  await expect(page.getByText("EN APPROCHE", { exact: true })).toBeVisible();
  await expect(page.getByText("MESURES GÉOMÉTRIQUES · AUCUNE ETA DÉDUITE", { exact: true })).toBeVisible();
  await expect(page.locator(".cargo-progress-ring").getByText("25%", { exact: true })).toBeVisible();
  await expect(page.getByText("7.5 s", { exact: true })).toBeVisible();
  await expect(page.getByText("+ 2 AUTRES · COMPOSANTE COMPLÈTE", { exact: true })).toBeVisible();
  await expect(page.getByText(/ETA SUPPORT \/ INTERVENTION/)).toBeVisible();
  await expect(page.getByText(/PROGRESSION GLOBALE/)).toBeVisible();
});

test("support relations and cargo remain keyboard inspectable with raw detail", async ({ page }) => {
  await mockSupportSnapshot(page);
  await page.goto("/");
  await page.getByRole("button", { name: /Support · Docking · Cargo/ }).click();
  const support = page.getByRole("button", { name: /Support Alpha/ }).first();
  await support.focus();
  await page.keyboard.press("Enter");
  await expect(page.getByText("INSPECTION ENTITÉ SUPPORT / DOCKING", { exact: true })).toBeVisible();
  await expect(page.getByText("DOCKING_STATE", { exact: true })).toBeVisible();
  await page.getByRole("button", { name: "Fermer" }).click();
  const scanner = page.locator(".cargo-scanner");
  await scanner.focus();
  await page.keyboard.press("Enter");
  await expect(page.getByText("INSPECTION SCANNER CARGO", { exact: true })).toBeVisible();
  await expect(page.getByText("Conteneur central", { exact: true }).last()).toBeVisible();
  await expect(page.getByText("CARGO_SCAN_STATE brut", { exact: true })).toBeVisible();
});

test("weapon banks and turrets are keyboard inspectable with authoritative detail", async ({ page }) => {
  await mockWeaponSnapshot(page);
  await page.goto("/");
  await page.getByRole("button", { name: /Armement/ }).click();
  const selected = page.getByRole("button", { name: "Inspecter l’arme Mekhu HL-7D" });
  await expect(selected).toHaveCount(1);
  await selected.focus();
  await page.keyboard.press("Enter");
  await expect(page.getByText("INSPECTION BANQUE", { exact: true })).toBeVisible();
  await expect(page.getByText("Manifeste de l’arme", { exact: true })).toBeVisible();
  await expect(page.getByText("Définition de banque et géométrie", { exact: true })).toBeVisible();
  await page.getByRole("button", { name: "Fermer" }).click();
  const turret = page.getByRole("button", { name: /Tourelle dorsale/ });
  await expect(turret).toHaveCount(1);
  await turret.focus();
  await page.keyboard.press("Enter");
  await expect(page.getByText("INSPECTION SOUS-SYSTÈME", { exact: true })).toBeVisible();
});
