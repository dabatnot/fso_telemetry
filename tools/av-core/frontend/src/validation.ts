import type { AvCoreConfig, ThresholdKey } from "./types";
import { translate, type Language } from "./i18n";

const THRESHOLDS: ThresholdKey[] = [
  "engine",
  "shield",
  "hull",
  "weaponEnergy",
  "afterburnerFuel",
  "ammo",
  "subsystem"
];

function inRange(value: number, minimum: number, maximum: number): boolean {
  return Number.isFinite(value) && value >= minimum && value <= maximum;
}

export function validateConfig(config: AvCoreConfig, language: Language = "fr"): string[] {
  const errors: string[] = [];
  if (!config.telemetry.host.trim()) errors.push(translate(language, "requiredHost"));
  if (!inRange(config.telemetry.port, 1, 65535)) errors.push(translate(language, "fstlPortRange"));
  if (!inRange(config.web.port, 1, 65535)) errors.push(translate(language, "httpPortRange"));
  if (!inRange(config.telemetry.staleAfterMs, 1, 60000)) errors.push(translate(language, "telemetryTimeoutRange"));
  if (!inRange(config.can.nodeTimeoutMs, 1, 60000)) errors.push(translate(language, "moduleTimeoutRange"));

  for (const key of THRESHOLDS) {
    const threshold = config.alerts[key];
    if (!inRange(threshold.activateBelowPercent, 0, 100) || !inRange(threshold.clearAbovePercent, 0, 100)) {
      errors.push(translate(language, "thresholdRange", { name: key }));
    } else if (threshold.clearAbovePercent <= threshold.activateBelowPercent) {
      errors.push(translate(language, "hysteresis", { name: key }));
    }
  }

  if (!inRange(config.alerts.countermeasures.activateAtOrBelowCount, 0, 255)) {
    errors.push(translate(language, "countermeasureCountRange"));
  }
  if (!inRange(config.alerts.countermeasures.activateAtOrBelowPercent, 0, 100)) {
    errors.push(translate(language, "countermeasurePercentRange"));
  }
  if (!inRange(config.lighting.maxBrightnessPercent, 0, 100)) {
    errors.push(translate(language, "brightnessRange"));
  }
  if (!inRange(config.lighting.slowFlashHz, 0.25, 10) || !inRange(config.lighting.fastFlashHz, 0.25, 10)) {
    errors.push(translate(language, "flashRange"));
  } else if (config.lighting.fastFlashHz <= config.lighting.slowFlashHz) {
    errors.push(translate(language, "flashOrder"));
  }
  return errors;
}
