import type { CSSProperties, ReactNode } from "react";
import { formatValue, resolveInstrument } from "./data";
import {
  decodePropulsionFlags,
  etsModeLabel,
  forwardComponent,
  playerClassManifest
} from "./energySemantics";
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

function playerRecord(snapshot: DashboardSnapshot | null, recordName: string): Record<string, unknown> {
  const records = snapshot?.records[recordName] ?? [];
  return records.find((record) => String(record.entity_id) === String(snapshot?.playerEntityId)) ?? records[0] ?? {};
}

function derived(snapshot: DashboardSnapshot | null, suffix: string) {
  if (!snapshot?.playerEntityId) return undefined;
  return snapshot.derived[`entities.${snapshot.playerEntityId}.${suffix}`];
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
      className={`energy-panel state-${resolved.state} ${className}`}
      onClick={onInspect}
      aria-label={`${definition.label}: ${STATE_LABELS[resolved.state]}`}
    >
      <header><span>{definition.label}</span><i>{STATE_LABELS[resolved.state]}</i></header>
      <div className="energy-panel-body">
        {unavailable ? (
          <div className="energy-unavailable">
            <strong>{STATE_LABELS[resolved.state]}</strong><span>{resolved.reason}</span>
          </div>
        ) : children}
      </div>
    </button>
  );
}

function Metric({ label, value, unit }: { label: string; value: unknown; unit?: string }) {
  const available = value !== undefined && value !== null;
  return (
    <div className="energy-metric">
      <span>{label}</span><strong>{available ? formatValue(value) : "—"}</strong>
      {unit && available && <small>{unit}</small>}
    </div>
  );
}

function ResourceGauge({
  label,
  ratio,
  current,
  maximum,
  unit
}: {
  label: string;
  ratio: unknown;
  current: unknown;
  maximum: unknown;
  unit: string;
}) {
  const numeric = typeof ratio === "number"
    ? ratio
    : typeof ratio === "string" && ratio.trim()
      ? Number(ratio)
      : Number.NaN;
  const available = Number.isFinite(numeric);
  const normalized = available ? Math.max(0, Math.min(1, numeric)) : 0;
  const style = { "--resource-ratio": `${normalized * 360}deg` } as CSSProperties;
  return (
    <div className={`resource-gauge ${available ? "" : "resource-unavailable"}`} style={style}>
      <div className="resource-arc">
        <div><strong>{available ? `${(normalized * 100).toFixed(0)}%` : "—"}</strong><span>{label}</span></div>
      </div>
      <div className="resource-values">
        <span>ACTUEL <b>{current === undefined ? "—" : formatValue(current)}</b></span>
        <span>MAX <b>{maximum === undefined ? "—" : formatValue(maximum)}</b></span>
        <small>{unit}</small>
      </div>
    </div>
  );
}

function EtsBar({ label, value }: { label: string; value: unknown }) {
  const numeric = Number(value);
  const index = Number.isInteger(numeric) ? Math.max(0, Math.min(12, numeric)) : null;
  return (
    <div className="ets-column">
      <span>{label}</span>
      <div className="ets-segments">
        {Array.from({ length: 12 }, (_, position) => (
          <i className={index !== null && position < index ? "active" : ""} key={position} />
        ))}
      </div>
      <strong>{index === null ? "ERR" : index}<small>/12</small></strong>
    </div>
  );
}

function FlagChips({ value }: { value: unknown }) {
  const flags = decodePropulsionFlags(value);
  if (flags === null) return <span className="inline-error">ERR</span>;
  if (!flags.length) return <span className="flag-chip neutral">AUCUN MODE ACTIF</span>;
  return <div className="flag-chips">{flags.map((flag) => <span className="flag-chip" key={flag}>{flag}</span>)}</div>;
}

function findDefinition(definitions: InstrumentDefinition[], id: string): InstrumentDefinition {
  const definition = definitions.find((candidate) => candidate.id === id);
  if (!definition) throw new Error(`Instrument énergie manquant: ${id}`);
  return definition;
}

export function EnergyCockpit({ definitions, snapshot, onInspect }: Props) {
  const definition = (id: string) => findDefinition(definitions, id);
  const resolved = (id: string) => resolveInstrument(definition(id), snapshot);
  const energy = playerRecord(snapshot, "ENERGY_STATE");
  const propulsion = playerRecord(snapshot, "PROPULSION_STATE");
  const shipClass = playerClassManifest(snapshot) ?? {};
  const afterburnerClass = shipClass.afterburner as Record<string, unknown> | undefined;
  const value = (suffix: string) => derived(snapshot, suffix)?.value;

  const resourcesDefinition = definition("energy-resources");
  const etsDefinition = definition("energy-ets");
  const propulsionDefinition = definition("energy-propulsion");
  const engineDefinition = definition("energy-engine");
  const performanceDefinition = definition("energy-performance");
  const futureDefinition = definition("energy-future");
  const readiness = value("afterburner_readiness");

  return (
    <section className="energy-cockpit" aria-label="Cockpit propulsion et énergie">
      <div className="energy-zone energy-ets-zone">
        <div className="zone-title"><span>01</span><strong>GESTION ETS</strong></div>
        <Panel definition={etsDefinition} resolved={resolved("energy-ets")} onInspect={() => onInspect(etsDefinition)}>
          <div className="ets-header">
            <span>MODE ETS</span><strong>{etsModeLabel(energy.ets_mode)}</strong>
          </div>
          <div className="ets-bank">
            <EtsBar label="BOUCLIERS" value={energy.ets_shields_index} />
            <EtsBar label="ARMES" value={energy.ets_weapons_index} />
            <EtsBar label="MOTEURS" value={energy.ets_engines_index} />
          </div>
          <div className="energy-flow-grid">
            <Metric label="RÉGÉN. ARMES" value={energy.weapon_regeneration_per_s} unit="/s" />
            <Metric label="RÉGÉN. BOUCLIERS" value={energy.shield_regeneration_per_s} unit="/s" />
            <Metric label="TRANSFERT → ARMES" value={energy.deferred_to_weapons} />
            <Metric label="TRANSFERT → BOUCLIERS" value={energy.deferred_to_shields} />
          </div>
        </Panel>
      </div>

      <div className="energy-zone energy-resources-zone">
        <div className="zone-title"><span>02</span><strong>RESSOURCES</strong></div>
        <Panel
          definition={resourcesDefinition}
          resolved={resolved("energy-resources")}
          onInspect={() => onInspect(resourcesDefinition)}
          className="energy-resources-panel"
        >
          <div className="resource-pair">
            <ResourceGauge
              label="ÉNERGIE ARMES"
              ratio={value("weapon_energy_ratio")}
              current={energy.weapon_energy_current}
              maximum={energy.weapon_energy_max}
              unit="unités"
            />
            <ResourceGauge
              label="CARBURANT AB"
              ratio={value("fuel_ratio")}
              current={propulsion.fuel_current}
              maximum={propulsion.fuel_max}
              unit="unités"
            />
          </div>
          <div className="readiness-strip">
            <span>AFTERBURNER</span>
            <strong>{readiness === undefined ? "—" : formatValue(readiness)}</strong>
            <Metric label="PRÊT DANS" value={value("afterburner_ready_delay_s")} unit="s" />
          </div>
        </Panel>
      </div>

      <div className="energy-zone energy-propulsion-zone">
        <div className="zone-title"><span>03</span><strong>PROPULSION</strong></div>
        <Panel
          definition={propulsionDefinition}
          resolved={resolved("energy-propulsion")}
          onInspect={() => onInspect(propulsionDefinition)}
        >
          <FlagChips value={propulsion.propulsion_flags} />
          <div className="propulsion-timers">
            <Metric label="AUTONOMIE AB" value={value("afterburner_autonomy_s")} unit="s" />
            <Metric label="RECHARGE COMPLÈTE" value={value("afterburner_recharge_s")} unit="s" />
            <Metric label="CARBURANT UTILISABLE" value={value("afterburner_usable_fuel")} />
            <Metric label="COOLDOWN" value={propulsion.cooldown_remaining_us} unit="µs" />
          </div>
          <div className="propulsion-dynamics">
            <Metric label="ACCÉLÉRATION AB" value={propulsion.forward_accel_time_const_s} unit="s" />
            <Metric
              label="VITESSE AB AVANT"
              value={forwardComponent(propulsion.afterburner_max_velocity_local)}
              unit="u/s"
            />
            <Metric label="CONSOMMATION" value={propulsion.consumption_per_s} unit="/s" />
            <Metric label="RÉCUPÉRATION" value={propulsion.recovery_per_s} unit="/s" />
          </div>
        </Panel>
      </div>

      <div className="energy-zone energy-engine-zone">
        <div className="zone-title"><span>04</span><strong>MOTEURS</strong></div>
        <Panel definition={engineDefinition} resolved={resolved("energy-engine")} onInspect={() => onInspect(engineDefinition)}>
          <div className="engine-overview">
            <ResourceGauge
              label="INTÉGRITÉ"
              ratio={value("engine_integrity_ratio")}
              current={energy.aggregate_engine_current_hits}
              maximum={energy.aggregate_engine_max_hits}
              unit="points"
            />
            <div>
              <Metric label="POWER OUTPUT" value={energy.power_output} />
              <Metric label="ENGINE WASH" value={propulsion.engine_wash_intensity} />
              <span className="raw-flag">FLAGS 0x{Number(propulsion.propulsion_flags ?? 0).toString(16).padStart(4, "0")}</span>
            </div>
          </div>
        </Panel>
      </div>

      <div className="energy-zone energy-performance-zone">
        <Panel
          definition={performanceDefinition}
          resolved={resolved("energy-performance")}
          onInspect={() => onInspect(performanceDefinition)}
        >
          <div className="performance-strip">
            <Metric label="CLASSE" value={shipClass.internal_name} />
            <Metric label="VITESSE NOMINALE" value={forwardComponent(shipClass.max_velocity)} unit="u/s" />
            <Metric label="VITESSE AB" value={forwardComponent(shipClass.afterburner_max_velocity)} unit="u/s" />
            <Metric label="VITESSE BOOSTER" value={forwardComponent(shipClass.booster_max_velocity)} unit="u/s" />
            <Metric label="ACCÉL. AVANT" value={shipClass.forward_accel_time} unit="s" />
            <Metric label="ACCÉL. AB" value={shipClass.afterburner_forward_accel_time} unit="s" />
            <Metric label="DÉCÉL. AVANT" value={shipClass.forward_decel_time} unit="s" />
            <Metric label="CAPACITÉ AB" value={afterburnerClass?.fuel_capacity} />
          </div>
        </Panel>
      </div>

      <div className="energy-zone energy-future-zone">
        <Panel definition={futureDefinition} resolved={resolved("energy-future")} onInspect={() => onInspect(futureDefinition)}>
          <></>
        </Panel>
        <div className="energy-future-list" aria-label="Données énergétiques futures non disponibles">
          {[
            "PUISSANCE MOTEUR DYNAMIQUE",
            "VITESSE MAX DISPONIBLE",
            "RÉPARTITION ETS EXACTE",
            "POUSSÉE EFFECTIVE",
            "CHARGES INSTANTANÉES",
            "RCS"
          ].map((label) => <span key={label}><b>ND</b>{label}</span>)}
        </div>
      </div>
    </section>
  );
}
