import { describe, expect, it } from "vitest";
import { resolveInstrument } from "./data";
import type { DashboardSnapshot, InstrumentDefinition } from "./types";

const baseSnapshot: DashboardSnapshot = {
  schema: "DashboardSnapshotV1",
  publishedAtUtc: "2026-01-01T00:00:00.000000Z",
  mode: "live",
  connection: {
    status: "Live",
    host: "127.0.0.1",
    port: 42042,
    sessionId: "1",
    lastLiveObservedUtc: null,
    staleReason: null
  },
  session: {},
  mission: { paused: 0 },
  playerEntityId: "7",
  records: {
    CONTROL_STATE: [{ entity_id: "7", pitch: 0, producer_sample_time_us: "1000" }]
  },
  recordInstances: {},
  manifest: { id: 1, records: {} },
  derived: {},
  transport: { synchronized: true, baseline: 1, deltaSequence: 1, manifestId: 1 },
  quality: {
    packets: 1,
    transportGapCount: 0,
    decodeErrorCount: 0,
    resyncCount: 0,
    channels: [{
      recordName: "CONTROL_STATE",
      identity: "CONTROL_STATE/entity_id=7",
      configuredHz: 30,
      observedHz: 30,
      ageUs: 0,
      intervalP50Us: 33334,
      intervalP95Us: 33334,
      intervalMaxUs: 33334,
      jitterUs: 0,
      gapCount: 0,
      repeatedSamples: 0,
      updates: 2,
      lastProducerSampleUs: "1000"
    }]
  },
  capture: { active: false, path: null },
  replay: { path: null, playing: false, speed: 1, position: 0, packetCount: 0 }
};

const pitch: InstrumentDefinition = {
  id: "pitch",
  tab: "flight",
  label: "Tangage",
  source: "record:CONTROL_STATE.pitch",
  component: "bar"
};

describe("instrument availability", () => {
  it("preserves a real zero as live data", () => {
    const result = resolveInstrument(pitch, baseSnapshot);
    expect(result.state).toBe("live");
    expect(result.value).toBe(0);
    expect(result.ageUs).toBe(0);
  });

  it("uses ND only for a declared unavailable source", () => {
    const result = resolveInstrument(
      { ...pitch, source: "future:LOCK_STATE", availability: "nd", reason: "future" },
      baseSnapshot
    );
    expect(result.state).toBe("nd");
    expect(result.reason).toBe("future");
  });

  it("promotes an ND placeholder when its authoritative future record arrives", () => {
    const futureSnapshot = {
      ...baseSnapshot,
      records: {
        ...baseSnapshot.records,
        LOCK_STATE: [{ entity_id: "7", lock_progress: 0.5 }]
      }
    };
    const result = resolveInstrument(
      { ...pitch, source: "future:LOCK_STATE", availability: "nd", reason: "future" },
      futureSnapshot
    );
    expect(result.state).toBe("live");
    expect(result.value).toEqual([{ entity_id: "7", lock_progress: 0.5 }]);
  });

  it("uses not applicable for a supported optional field", () => {
    const result = resolveInstrument({ ...pitch, source: "record:CONTROL_STATE.optional" }, baseSnapshot);
    expect(result.state).toBe("not_applicable");
  });

  it("keeps the last value but marks a stale connection", () => {
    const result = resolveInstrument(
      pitch,
      { ...baseSnapshot, connection: { ...baseSnapshot.connection, status: "Stale" } }
    );
    expect(result.state).toBe("stale");
    expect(result.value).toBe(0);
  });

  it("shows waiting before a session is synchronized", () => {
    const result = resolveInstrument(pitch, null);
    expect(result.state).toBe("waiting");
  });

  it("marks an invalid attitude quaternion as ERR", () => {
    const snapshot = {
      ...baseSnapshot,
      records: {
        ...baseSnapshot.records,
        FLIGHT_STATE: [{
          entity_id: "7",
          presence: "0",
          producer_sample_time_us: "1000",
          orientation_local_to_world: [0, 0, 0, 0]
        }]
      }
    };
    const result = resolveInstrument({
      id: "attitude",
      tab: "flight",
      label: "Attitude",
      source: "record:FLIGHT_STATE.orientation_local_to_world",
      component: "attitude"
    }, snapshot);
    expect(result.state).toBe("invalid");
    expect(result.reason).toContain("quaternion");
  });
});
