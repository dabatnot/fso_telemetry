export type Availability = "live" | "nd" | "not_applicable" | "stale" | "invalid" | "waiting";

export interface InstrumentDefinition {
  id: string;
  tab: string;
  label: string;
  source: string;
  unit?: string;
  component: "radial" | "bar" | "vector" | "status" | "segments" | "list" | "diagram" | "timeline" | "diagnostic"
    | "attitude" | "axes" | "flags" | "counters" | "cursor" | "nd-panel";
  availability?: "nd";
  reason?: string;
  min?: number;
  max?: number;
  formula?: string;
  group?: "attitude" | "movement" | "commands" | "activity" | "status" | "future"
    | "resources" | "ets" | "propulsion" | "engine" | "performance"
    | "core" | "protection" | "recovery" | "subsystems"
    | "modes" | "primary" | "selected" | "secondary" | "reserves" | "turrets"
    | "overview" | "service" | "approach" | "docking" | "cargo"
    | "sensors" | "scope" | "target" | "locks" | "threats";
  order?: number;
  size?: "hero" | "wide" | "compact";
  consumedFields?: string[];
  detailFields?: string[];
  reservedFields?: string[];
}

export interface InspectionTarget {
  definition: InstrumentDefinition;
  kind?: "instrument" | "subsystem" | "subsystem-list"
    | "weapon-bank" | "weapon-bank-list" | "turret-list"
    | "support-entity" | "docking-relation" | "docking-component" | "cargo-target"
    | "target" | "radar-contact" | "lock-point" | "lock-list"
    | "incoming-missile" | "missile-list";
  record?: Record<string, unknown>;
  title?: string;
  family?: "primary" | "secondary" | "tertiary" | "turret";
  bankId?: string;
  subsystemId?: string;
  entityId?: string;
  remoteEntityId?: string;
}

export interface InstrumentValue {
  state: Availability;
  value: unknown;
  reason?: string;
  source: string;
  sampleTimeUs?: string;
  ageUs?: number;
  observedHz?: number;
  rawRecord?: Record<string, unknown>;
}

export interface ChannelQuality {
  recordName: string;
  identity: string;
  configuredHz: number;
  observedHz: number | null;
  ageUs: number | null;
  intervalP50Us: number | null;
  intervalP95Us: number | null;
  intervalMaxUs: number | null;
  jitterUs: number | null;
  gapCount: number;
  repeatedSamples: number;
  updates: number;
  lastProducerSampleUs: string | null;
}

export interface DashboardSnapshot {
  schema: "DashboardSnapshotV1";
  publishedAtUtc: string;
  mode: "live" | "replay";
  connection: {
    status: string;
    recoveryState?: "idle" | "resyncing" | "reconnecting";
    host: string;
    port: number;
    sessionId: string;
    lastLiveObservedUtc: string | null;
    staleReason: string | null;
    error?: string;
  };
  session: Record<string, unknown>;
  mission: Record<string, unknown>;
  playerEntityId: string | null;
  records: Record<string, Array<Record<string, unknown>>>;
  recordInstances: Record<string, {
    recordName: string;
    recordVersion?: number;
    fields: Record<string, unknown>;
  }>;
  manifest: { id: number; records: Record<string, unknown> };
  derived: Record<string, { available: boolean; reason?: string; value: unknown }>;
  transport: {
    synchronized: boolean;
    baseline: number;
    deltaSequence: number;
    manifestId: number;
    requiredManifestId?: number;
  };
  quality: {
    packets: number;
    transportGapCount: number;
    decodeErrorCount: number;
    resyncCount: number;
    channels: ChannelQuality[];
  };
  capture: {
    active: boolean;
    path: string | null;
    id?: string | null;
    bytes?: number;
    durationUs?: number;
    packetCount?: number;
    rollingBytesPerSecond?: number;
    projectedBytes10Minutes?: number;
    projectedBytes1Hour?: number;
    projectedFinalBytes?: number | null;
    freeBytes?: number;
    secondsUntilLimit?: number | null;
    warning?: string | null;
  };
  replay: {
    path: string | null;
    playing: boolean;
    speed: number;
    position: number;
    packetCount: number;
    positionUs?: number;
    durationUs?: number;
    timelineUnit?: "microseconds";
    seeking?: boolean;
    previewing?: boolean;
    previewPositionUs?: number | null;
    captureId?: string | null;
    activeRangeId?: string | null;
    loop?: boolean;
    ranges?: CaptureRange[];
    sessionBoundaries?: CaptureSessionBoundary[];
    captureSchema?: string | null;
    contractFeatures?: string[];
  };
  replayUdp?: ReplayUdpState;
}

export interface CaptureRange {
  id: string;
  name: string;
  sessionIndex: number;
  startUs: number;
  endUs: number;
  createdAtUtc?: string;
  updatedAtUtc?: string;
}

export interface CaptureSessionBoundary {
  sessionIndex: number;
  sessionId: string;
  startUs: number;
  endUs: number | null;
  reason: string;
}

export interface ReplayUdpState {
  running: boolean;
  bindHost: string;
  port: number;
  lanEnabled: boolean;
  clientCount: number;
  maxClients: number;
  endpoint: string | null;
  error: string | null;
  refusedClients?: number;
  rejectedCommands?: number;
}

export interface CaptureLibraryItem {
  id: string;
  name: string;
  path: string;
  mission?: string | null;
  createdAtUtc?: string | null;
  durationUs: number;
  sizeBytes: number;
  packetCount: number;
  sessionCount: number;
  complete: boolean;
  schema: string;
}
