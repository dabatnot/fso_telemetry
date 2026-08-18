export type ModuleKey = "warnCtrl" | "threatProc" | "sensProc" | "instProc";
export type ModuleRole = "WARN_CTRL" | "THREAT_PROC" | "SENS_PROC" | "INST_PROC";
export type ThresholdKey =
  | "engine"
  | "shield"
  | "hull"
  | "weaponEnergy"
  | "afterburnerFuel"
  | "ammo"
  | "subsystem";

export interface PercentThreshold {
  activateBelowPercent: number;
  clearAbovePercent: number;
}

export interface RgbColor {
  r: number;
  g: number;
  b: number;
}

export interface AvCoreConfig {
  schemaVersion: 1;
  telemetry: { host: string; port: number; staleAfterMs: number };
  can: { nodeTimeoutMs: number };
  web: { port: number };
  modules: Record<ModuleKey, { installed: boolean }>;
  alerts: Record<ThresholdKey, PercentThreshold> & {
    countermeasures: {
      activateAtOrBelowCount: number;
      activateAtOrBelowPercent: number;
    };
  };
  lighting: {
    warningColor: RgbColor;
    cautionColor: RgbColor;
    maxBrightnessPercent: number;
    slowFlashHz: number;
    fastFlashHz: number;
  };
}

export interface ModuleStatus {
  role: ModuleRole;
  installed: boolean;
  state: "UNAVAILABLE";
  protocolId: number | null;
  uid: string | null;
  firmwareVersion: string | null;
  lastHeartbeatMs: number | null;
}

export interface AvCoreStatus {
  schema: "AvCoreStatusV1";
  version: string;
  uptimeMs: number;
  configuration: { state: "OK" | "ERROR"; message: string | null };
  restartRequired: boolean;
  telemetry: { state: "UNAVAILABLE"; host: string; port: number };
  can: { state: "UNAVAILABLE"; interface: "can0"; bitrate: 1000000 };
  modules: ModuleStatus[];
}

export interface ConfigUpdateResponse {
  config: AvCoreConfig;
  restartRequired: boolean;
}
