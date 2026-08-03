import type { CSSProperties, ReactNode } from "react";
import { formatValue, resolveInstrument } from "./data";
import { attitudeFromQuaternion, trajectoryFromLocalVelocity } from "./flightMath";
import { CONTROL_FLAGS, PHYSICS_MODE_FLAGS, controlModeLabel, decodeFlags } from "./flightSemantics";
import type { DashboardSnapshot, InstrumentDefinition, InstrumentValue } from "./types";

interface Props {
  definitions: InstrumentDefinition[];
  snapshot: DashboardSnapshot | null;
  onInspect: (definition: InstrumentDefinition) => void;
}

const STATE_LABELS: Record<string, string> = {
  live: "LIVE",
  stale: "STALE",
  nd: "ND",
  not_applicable: "—",
  invalid: "ERR",
  waiting: "EN ATTENTE"
};

function playerRecord(snapshot: DashboardSnapshot | null, recordName: string): Record<string, unknown> | null {
  const records = snapshot?.records[recordName] ?? [];
  return records.find((record) => String(record.entity_id) === String(snapshot?.playerEntityId)) ?? records[0] ?? null;
}

function derivedValue(snapshot: DashboardSnapshot | null, suffix: string): unknown {
  if (!snapshot?.playerEntityId) return undefined;
  return snapshot.derived[`entities.${snapshot.playerEntityId}.${suffix}`]?.value;
}

function Panel({
  definition,
  resolved,
  onInspect,
  className = "",
  children
}: {
  definition: InstrumentDefinition;
  resolved: InstrumentValue;
  onInspect: () => void;
  className?: string;
  children: ReactNode;
}) {
  const unavailable = resolved.state !== "live" && resolved.state !== "stale";
  return (
    <button
      className={`flight-panel state-${resolved.state} ${className}`}
      onClick={onInspect}
      aria-label={`${definition.label}: ${STATE_LABELS[resolved.state]}`}
    >
      <header><span>{definition.label}</span><i>{STATE_LABELS[resolved.state]}</i></header>
      <div className="flight-panel-body">
        {unavailable ? (
          <div className="flight-unavailable"><strong>{STATE_LABELS[resolved.state]}</strong><span>{resolved.reason}</span></div>
        ) : children}
      </div>
    </button>
  );
}

function Metric({ label, value, unit }: { label: string; value: unknown; unit?: string }) {
  return (
    <div className="cockpit-metric">
      <span>{label}</span><strong>{formatValue(value)}</strong>{unit && <small>{unit}</small>}
    </div>
  );
}

function VectorRows({ label, value, unit }: { label: string; value: unknown; unit: string }) {
  const vector = Array.isArray(value) ? value : [];
  return (
    <div className="cockpit-vector">
      <span className="vector-title">{label}</span>
      {["X", "Y", "Z"].map((axis, index) => (
        <span key={axis}><i>{axis}</i><strong>{formatValue(vector[index])}</strong><small>{unit}</small></span>
      ))}
    </div>
  );
}

function AttitudeSphere({
  quaternion,
  velocityLocal
}: {
  quaternion: unknown;
  velocityLocal: unknown;
}) {
  const attitude = attitudeFromQuaternion(quaternion);
  if (!attitude) return null;
  const trajectory = trajectoryFromLocalVelocity(velocityLocal);
  const horizonStyle = {
    "--horizon-roll": `${-attitude.rollDeg}deg`,
    "--horizon-pitch": `${Math.max(-42, Math.min(42, attitude.pitchDeg)) * 1.75}px`
  } as CSSProperties;
  const markerStyle = trajectory ? {
    "--marker-x": `${trajectory.xPercent * 0.42}%`,
    "--marker-y": `${trajectory.yPercent * 0.42}%`
  } as CSSProperties : undefined;
  const heading = ((attitude.headingDeg % 360) + 360) % 360;
  return (
    <div className="attitude-cluster">
      <div className="heading-tape" aria-label={`Cap ${heading.toFixed(1)} degrés`}>
        <span>CAP</span><strong>{heading.toFixed(1)}°</strong>
      </div>
      <div className="attitude-sphere" style={horizonStyle}>
        <div className="attitude-world">
          <div className="attitude-sky" />
          <div className="attitude-ground" />
          <div className="pitch-ladder">
            {[-30, -20, -10, 0, 10, 20, 30].map((pitch) => (
              <i key={pitch} style={{ top: `${50 - pitch * 1.6}%` }}><span>{Math.abs(pitch)}</span></i>
            ))}
          </div>
        </div>
        <div className="roll-scale">
          {[-60, -30, 0, 30, 60].map((roll) => <i key={roll} style={{ transform: `rotate(${roll}deg)` }} />)}
        </div>
        <div className="craft-reference"><i /><b /><i /></div>
        {trajectory ? (
          <div className="trajectory-marker" style={markerStyle} aria-label={`Dérive ${trajectory.driftDeg.toFixed(1)} degrés`}>
            <i /><b /><i />
          </div>
        ) : <div className="trajectory-unavailable" title="Vitesse quasi nulle">TRAJ —</div>}
      </div>
      <div className="attitude-readouts">
        <Metric label="TANGAGE" value={`${attitude.pitchDeg.toFixed(1)}°`} />
        <Metric label="ROULIS" value={`${attitude.rollDeg.toFixed(1)}°`} />
        <Metric label="DÉRIVE" value={trajectory ? `${trajectory.driftDeg.toFixed(1)}°` : "—"} />
      </div>
    </div>
  );
}

function AxisBars({ record }: { record: Record<string, unknown> }) {
  const axes: Array<[string, string]> = [
    ["pitch", "TANGAGE"], ["heading", "LACET"], ["bank", "ROULIS"],
    ["vertical", "VERTICAL"], ["sideways", "LATÉRAL"], ["forward", "LONGITUDINAL"]
  ];
  return (
    <div className="axis-bank">
      {axes.map(([field, label]) => {
        const value = Number(record[field]);
        const finite = Number.isFinite(value);
        const position = finite ? (Math.max(-1, Math.min(1, value)) + 1) * 50 : 50;
        return (
          <div className="axis-row" key={field}>
            <span>{label}</span>
            <div><i style={{ left: `${position}%` }} /></div>
            <strong>{finite ? value.toFixed(2) : "ERR"}</strong>
          </div>
        );
      })}
    </div>
  );
}

function FlagChips({ value, kind }: { value: unknown; kind: "control" | "physics" }) {
  const definitions = kind === "control" ? CONTROL_FLAGS : PHYSICS_MODE_FLAGS;
  const flags = decodeFlags(value, definitions);
  if (flags === null) return <span className="inline-error">ERR</span>;
  if (!flags.length) return <span className="flag-chip neutral">AUCUN</span>;
  return <div className="flag-chips">{flags.map((flag) => <span className="flag-chip" key={flag}>{flag}</span>)}</div>;
}

function findDefinition(definitions: InstrumentDefinition[], id: string): InstrumentDefinition {
  const definition = definitions.find((candidate) => candidate.id === id);
  if (!definition) throw new Error(`Instrument Pilotage manquant: ${id}`);
  return definition;
}

export function PilotageCockpit({ definitions, snapshot, onInspect }: Props) {
  const definition = (id: string) => findDefinition(definitions, id);
  const resolved = (id: string) => resolveInstrument(definition(id), snapshot);
  const flight = playerRecord(snapshot, "FLIGHT_STATE") ?? {};
  const control = playerRecord(snapshot, "CONTROL_STATE") ?? {};
  const velocityLocal = derivedValue(snapshot, "velocity_local");
  const trajectory = trajectoryFromLocalVelocity(velocityLocal);

  const attitudeDefinition = definition("attitude");
  const attitudeResolved = resolved("attitude");
  const movementDefinition = definition("flight-movement");
  const movementResolved = resolved("flight-movement");
  const controlsDefinition = definition("flight-controls");
  const controlsResolved = resolved("flight-controls");
  const activityDefinition = definition("flight-activity");
  const activityResolved = resolved("flight-activity");
  const cursorDefinition = definition("flight-cursor");
  const cursorResolved = resolved("flight-cursor");
  const statusDefinition = definition("flight-status");
  const statusResolved = resolved("flight-status");
  const futureDefinition = definition("flight-future");
  const futureResolved = resolved("flight-future");

  const cursor = control.flight_cursor as Record<string, unknown> | undefined;
  return (
    <section className="flight-cockpit" aria-label="Cockpit de pilotage">
      <div className="flight-zone movement-zone">
        <div className="zone-title"><span>01</span><strong>MOUVEMENT</strong></div>
        <Panel definition={movementDefinition} resolved={movementResolved} onInspect={() => onInspect(movementDefinition)}>
          <div className="movement-grid">
            <Metric label="VITESSE" value={derivedValue(snapshot, "speed")} unit="u/s" />
            <Metric label="DÉRIVE" value={trajectory ? trajectory.driftDeg : "—"} unit={trajectory ? "°" : undefined} />
            <VectorRows label="VITESSE LOCALE" value={velocityLocal} unit="u/s" />
            <VectorRows label="VITESSE MONDE" value={flight.velocity_world} unit="u/s" />
            <VectorRows label="POSITION MONDE" value={flight.position_world} unit="u" />
            <VectorRows label="VITESSE ANGULAIRE" value={flight.rotational_velocity_local} unit="rad/s" />
          </div>
        </Panel>
      </div>

      <div className="flight-zone attitude-zone">
        <div className="zone-title"><span>02</span><strong>ATTITUDE INERTIELLE</strong></div>
        <Panel definition={attitudeDefinition} resolved={attitudeResolved} onInspect={() => onInspect(attitudeDefinition)} className="attitude-panel">
          <AttitudeSphere quaternion={attitudeResolved.value} velocityLocal={velocityLocal} />
        </Panel>
      </div>

      <div className="flight-zone command-zone">
        <div className="zone-title"><span>03</span><strong>COMMANDES</strong></div>
        <Panel definition={controlsDefinition} resolved={controlsResolved} onInspect={() => onInspect(controlsDefinition)}>
          <AxisBars record={control} />
          <div className="control-summary">
            <Metric label="CROISIÈRE" value={control.forward_cruise_percent ?? "—"} unit="%" />
            <Metric label="MODE" value={controlModeLabel(control.control_mode)} />
          </div>
          <FlagChips value={control.control_flags} kind="control" />
        </Panel>
      </div>

      <div className="flight-zone status-zone">
        <div className="zone-title"><span>04</span><strong>ÉTATS DE VOL</strong></div>
        <Panel definition={statusDefinition} resolved={statusResolved} onInspect={() => onInspect(statusDefinition)}>
          <div className="status-summary">
            <Metric label="RAYON" value={flight.radius} unit="u" />
            <span className="raw-flag">BRUT 0x{Number(flight.physics_mode_flags ?? 0).toString(16).padStart(8, "0")}</span>
          </div>
          <FlagChips value={flight.physics_mode_flags} kind="physics" />
        </Panel>
      </div>

      <div className="flight-zone activity-zone">
        <div className="zone-title"><span>05</span><strong>ACTIVITÉ & CURSEUR</strong></div>
        <div className="activity-split">
          <Panel definition={activityDefinition} resolved={activityResolved} onInspect={() => onInspect(activityDefinition)}>
            <div className="pulse-counters">
              <Metric label="DEMANDE PRIMAIRE" value={control.fire_primary_count} />
              <Metric label="DEMANDE SECONDAIRE" value={control.fire_secondary_count} />
              <Metric label="DEMANDE CONTRE-MESURE" value={control.fire_countermeasure_count} />
            </div>
            <small className="semantic-note">Demandes du tick source · pas des tirs confirmés</small>
          </Panel>
          <Panel definition={cursorDefinition} resolved={cursorResolved} onInspect={() => onInspect(cursorDefinition)}>
            {cursor && <div className="cursor-grid">
              <Metric label="TANGAGE" value={cursor.cursor_pitch_rad} unit="rad" />
              <Metric label="LACET" value={cursor.cursor_yaw_rad} unit="rad" />
              <Metric label="SENSIBILITÉ" value={cursor.cursor_sensitivity} />
              <Metric label="DEADZONE" value={cursor.cursor_deadzone} />
            </div>}
          </Panel>
        </div>
      </div>

      <div className="flight-zone future-zone">
        <Panel definition={futureDefinition} resolved={futureResolved} onInspect={() => onInspect(futureDefinition)}>
          <></>
        </Panel>
        <div className="future-nd-list" aria-label="Données futures non disponibles">
          {["COORDONNÉES HUD", "RÉFÉRENCE GRAVITATIONNELLE", "HORIZON PLANÉTAIRE", "VITESSE DÉSIRÉE", "ACCÉLÉRATION"].map((label) =>
            <span key={label}><b>ND</b>{label}</span>
          )}
        </div>
      </div>
    </section>
  );
}
