import { describe, expect, it } from "vitest";
import type { AvCoreConfig } from "./types";
import { validateConfig } from "./validation";

const config = (): AvCoreConfig => ({
  schemaVersion: 1,
  telemetry: { host: "127.0.0.1", port: 42042, staleAfterMs: 1000 },
  can: { nodeTimeoutMs: 3000 },
  web: { port: 8080 },
  modules: {
    warnCtrl: { installed: true },
    threatProc: { installed: true },
    sensProc: { installed: false },
    instProc: { installed: false }
  },
  alerts: {
    engine: { activateBelowPercent: 50, clearAbovePercent: 55 },
    shield: { activateBelowPercent: 30, clearAbovePercent: 35 },
    hull: { activateBelowPercent: 40, clearAbovePercent: 45 },
    weaponEnergy: { activateBelowPercent: 20, clearAbovePercent: 30 },
    afterburnerFuel: { activateBelowPercent: 20, clearAbovePercent: 25 },
    ammo: { activateBelowPercent: 20, clearAbovePercent: 25 },
    countermeasures: { activateAtOrBelowCount: 3, activateAtOrBelowPercent: 20 },
    subsystem: { activateBelowPercent: 40, clearAbovePercent: 45 }
  },
  lighting: {
    warningColor: { r: 255, g: 96, b: 0 },
    cautionColor: { r: 255, g: 96, b: 0 },
    maxBrightnessPercent: 30,
    slowFlashHz: 1,
    fastFlashHz: 4
  }
});

describe("configuration validation", () => {
  it("accepts the product defaults", () => expect(validateConfig(config())).toEqual([]));

  it("requires strict hysteresis", () => {
    const value = config();
    value.alerts.engine.clearAbovePercent = 50;
    expect(validateConfig(value).join(" ")).toContain("engine");
  });

  it("requires fast flashing to be faster", () => {
    const value = config();
    value.lighting.fastFlashHz = value.lighting.slowFlashHz;
    expect(validateConfig(value).join(" ")).toContain("cadence rapide");
  });

  it("rejects invalid ports and timeouts", () => {
    const value = config();
    value.telemetry.port = 0;
    value.can.nodeTimeoutMs = 60001;
    expect(validateConfig(value)).toHaveLength(2);
  });

  it("returns validation messages in English", () => {
    const value = config();
    value.lighting.fastFlashHz = value.lighting.slowFlashHz;
    expect(validateConfig(value, "en")).toContain("The fast flash rate must be greater than the slow rate.");
  });
});
