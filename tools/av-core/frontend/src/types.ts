export type ModuleKey = "warnCtrl" | "threatProc" | "sensProc" | "instProc";
export type ModuleRole = "WARN_CTRL" | "THREAT_PROC" | "SENS_PROC" | "INST_PROC";
export type WarnCtrlLamp =
  | "MASTER_WARNING" | "FIRE" | "MISSILE" | "BLAST" | "COLLISION" | "EMP"
  | "MASTER_CAUTION" | "ENG" | "SENS" | "SHIELD" | "HULL" | "WEP_EN"
  | "AB_FUEL" | "AMMO" | "CM_LOW" | "SUBSYS" | "AV_CORE" | "FLT_DATA"
  | "AV_BUS" | "SENS_PROC" | "THREAT_PROC" | "INST_PROC" | "WARN_CTRL";
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
  state: "ONLINE" | "DEGRADED" | "ABSENT" | "CAN_ERROR" | "UNAVAILABLE";
  protocolId: number | null;
  uid: string | null;
  firmwareVersion: string | null;
  lastHeartbeatMs: number | null;
}

export type TelemetryState = "READY" | "LIVE" | "STALE" | "DISCONNECTED";
export type CautionState = "ACTIVE" | "CLEAR" | "UNAVAILABLE";

export interface PercentCautionStatus {
  state: CautionState;
  valuePercent: number | null;
}

export interface AvCoreCockpitStatus {
  available: boolean;
  warnings: {
    available: boolean;
    master: boolean;
    fire: boolean;
    missile: boolean;
    blast: boolean;
    collision: boolean;
    emp: boolean;
  };
  cautions: {
    master: boolean;
    engine: PercentCautionStatus;
    sensor: { state: CautionState; sensorState: "ONLINE" | "DEGRADED" | "OFFLINE" | null };
    shield: PercentCautionStatus;
    hull: PercentCautionStatus;
    weaponEnergy: PercentCautionStatus;
    afterburnerFuel: PercentCautionStatus;
    ammo: PercentCautionStatus;
    countermeasures: PercentCautionStatus & { valueCount: number | null };
    subsystem: PercentCautionStatus;
  };
  threat: {
    available: boolean;
    sectorMask: number;
    incomingMissileCount: number;
    lockState: "NONE" | "ATTEMPT" | "ACQUIRED";
  };
}

export interface AvCoreStatus {
  schema: "AvCoreStatusV1";
  version: string;
  uptimeMs: number;
  configuration: { state: "OK" | "ERROR"; message: string | null };
  restartRequired: boolean;
  telemetry: {
    state: TelemetryState;
    host: string;
    port: number;
    sessionId: string | null;
    lastLiveAgeMs: number | null;
    error: string | null;
  };
  mission?: {
    active: boolean;
    paused: boolean;
    generation: number | null;
    timeCompression: number | null;
  };
  cockpit: AvCoreCockpitStatus;
  can: {
    state: "OK" | "ERROR" | "BUS_OFF" | "UNAVAILABLE";
    interface: "can0";
    bitrate: 1000000;
    receiveErrors: number;
    transmitErrors: number;
    error: string | null;
  };
  modules: ModuleStatus[];
}

export interface ConfigUpdateResponse {
  config: AvCoreConfig;
  restartRequired: boolean;
}
