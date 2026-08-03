import type { CSSProperties, ReactNode } from "react";
import { formatValue, resolveInstrument } from "./data";
import {
  decodeFlags,
  LIFECYCLE_FLAGS,
  playerRecord,
  PROTECTION_FLAGS,
  SHIELD_ARC_PATH_LENGTH,
  SHIELD_ARC_SPAN,
  shieldArcGeometry,
  shieldQuadrantLabel,
  subsystemViews
} from "./integritySemantics";
import type {
  DashboardSnapshot,
  InspectionTarget,
  InstrumentDefinition,
  InstrumentValue
} from "./types";

interface Props {
  definitions: InstrumentDefinition[];
  snapshot: DashboardSnapshot | null;
  onInspect: (target: InspectionTarget) => void;
}

const STATE_LABELS: Record<string, string> = {
  live: "LIVE",
  stale: "STALE",
  nd: "ND",
  not_applicable: "—",
  invalid: "ERR",
  waiting: "EN ATTENTE"
};

function definitionById(definitions: InstrumentDefinition[], id: string): InstrumentDefinition {
  const definition = definitions.find((candidate) => candidate.id === id);
  if (!definition) throw new Error(`Instrument intégrité manquant: ${id}`);
  return definition;
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
    <section className={`integrity-panel state-${resolved.state} ${className}`}>
      <header>
        <button onClick={onInspect} aria-label={`Inspecter ${definition.label}`}>
          {definition.label}
        </button>
        <i>{STATE_LABELS[resolved.state]}</i>
      </header>
      <div className="integrity-panel-body">
        {unavailable ? (
          <div className="integrity-unavailable">
            <strong>{STATE_LABELS[resolved.state]}</strong><span>{resolved.reason}</span>
          </div>
        ) : children}
      </div>
    </section>
  );
}

function Metric({ label, value, unit }: { label: string; value: unknown; unit?: string }) {
  const available = value !== undefined && value !== null;
  return (
    <div className="integrity-metric">
      <span>{label}</span>
      <strong>{available ? formatValue(value) : "—"}</strong>
      {available && unit ? <small>{unit}</small> : null}
    </div>
  );
}

function Chips({ values, empty }: { values: string[] | null; empty: string }) {
  if (values === null) return <span className="inline-error">ERR</span>;
  if (!values.length) return <span className="integrity-chip neutral">{empty}</span>;
  return (
    <div className="integrity-chips">
      {values.map((value) => <span className="integrity-chip" key={value}>{value}</span>)}
    </div>
  );
}

function IntegrityCore({
  snapshot,
  damage,
  shield
}: {
  snapshot: DashboardSnapshot | null;
  damage: Record<string, unknown>;
  shield: Record<string, unknown>;
}) {
  const hullRatio = derived(snapshot, "hull_ratio")?.value;
  const shieldRatio = derived(snapshot, "shield_ratio")?.value;
  const segmentRatios = derived(snapshot, "shield_segment_ratios")?.value;
  const hull = typeof hullRatio === "number" ? Math.max(0, Math.min(1, hullRatio)) : null;
  const shieldTotal = typeof shieldRatio === "number"
    ? Math.max(0, Math.min(1, shieldRatio))
    : null;
  const segments = Array.isArray(segmentRatios)
    ? segmentRatios.map(Number).filter(Number.isFinite)
    : [];
  const hasShields = Boolean(shield.has_shields);
  const standardShield = hasShields && segments.length === 4;
  const hullStyle = {
    "--hull-ratio": `${(hull ?? 0) * 360}deg`
  } as CSSProperties;
  const quadrantLayout = [
    { index: 1, label: "AVANT", rotation: 270 },
    { index: 0, label: "DROITE", rotation: 0 },
    { index: 2, label: "ARRIÈRE", rotation: 90 },
    { index: 3, label: "GAUCHE", rotation: 180 }
  ];
  const weakestIndex = derived(snapshot, "shield_weakest_segment_index")?.value;
  return (
    <div className="integrity-core-display">
      <div className={`shield-orbit ${hasShields ? "" : "shield-orbit-absent"}`}>
        {standardShield ? (
          <>
            <svg className="shield-quadrant-arcs" viewBox="0 0 360 360" aria-hidden="true">
              {quadrantLayout.map((quadrant) => {
                const geometry = shieldArcGeometry(
                  quadrant.rotation,
                  segments[quadrant.index]
                );
                return (
                  <g key={quadrant.index}>
                    <circle
                      className="shield-quadrant-track"
                      cx="180"
                      cy="180"
                      r="142"
                      pathLength={SHIELD_ARC_PATH_LENGTH}
                      transform={`rotate(${geometry.trackRotation} 180 180)`}
                      style={{
                        strokeDasharray: `${SHIELD_ARC_SPAN} ${SHIELD_ARC_PATH_LENGTH}`
                      }}
                    />
                    <circle
                      className="shield-quadrant-value"
                      cx="180"
                      cy="180"
                      r="142"
                      pathLength={SHIELD_ARC_PATH_LENGTH}
                      transform={`rotate(${geometry.valueRotation} 180 180)`}
                      style={{
                        strokeDasharray: `${geometry.valueLength} ${SHIELD_ARC_PATH_LENGTH}`
                      }}
                    />
                  </g>
                );
              })}
            </svg>
            <div className="shield-quadrant-labels">
              {quadrantLayout.map((quadrant) => (
                <span className={`quadrant-${quadrant.label.toLocaleLowerCase("fr")}`} key={quadrant.index}>
                  <b>{quadrant.label}</b>
                  <strong>{(segments[quadrant.index] * 100).toFixed(0)}%</strong>
                </span>
              ))}
            </div>
          </>
        ) : hasShields ? (
          <div className="shield-layout-error">
            <strong>ERR</strong>
            <span>{segments.length} segments · configuration non prise en charge</span>
          </div>
        ) : (
          <b className="shield-absent-label">AUCUN<br />BOUCLIER</b>
        )}
        <div className="hull-core" style={hullStyle}>
          <div>
            <strong>{hull === null ? "—" : `${(hull * 100).toFixed(0)}%`}</strong>
            <span>COQUE</span>
          </div>
        </div>
      </div>
      <div className="integrity-core-values">
        <Metric label="COQUE" value={damage.hull_strength} unit="HP" />
        <Metric label="MAX DYNAMIQUE" value={damage.dynamic_max_hull} unit="HP" />
        <Metric label="HP MANQUANTS" value={derived(snapshot, "hull_missing_hits")?.value} unit="HP" />
        <Metric label="BOUCLIER" value={derived(snapshot, "shield_current_total")?.value} unit="HP" />
        <Metric label="MAX BOUCLIER" value={derived(snapshot, "shield_max_total")?.value} unit="HP" />
        <Metric label="CHARGE TOTALE" value={shieldTotal === null ? null : shieldTotal * 100} unit="%" />
      </div>
      <div className="weakest-segment">
        <span>QUADRANT LE PLUS FAIBLE</span>
        <strong>
          {weakestIndex === undefined
            ? "—"
            : shieldQuadrantLabel(weakestIndex)}
        </strong>
        <b>
          {typeof derived(snapshot, "shield_weakest_segment_ratio")?.value === "number"
            ? `${(Number(derived(snapshot, "shield_weakest_segment_ratio")?.value) * 100).toFixed(0)}%`
            : "—"}
        </b>
      </div>
      <span className="segment-count">
        {hasShields ? `${segments.length} QUADRANTS` : "— AUCUN BOUCLIER"}
      </span>
    </div>
  );
}

export function IntegrityCockpit({ definitions, snapshot, onInspect }: Props) {
  const definition = (id: string) => definitionById(definitions, id);
  const resolved = (id: string) => resolveInstrument(definition(id), snapshot);
  const damage = playerRecord(snapshot, "DAMAGE_STATE") ?? {};
  const shield = playerRecord(snapshot, "SHIELD_STATE") ?? {};
  const lifecycle = playerRecord(snapshot, "ENTITY_LIFECYCLE") ?? {};
  const systems = subsystemViews(snapshot);
  const visibleSystems = systems.slice(0, 12);
  const coreDefinition = definition("integrity-core");
  const protectionDefinition = definition("integrity-protection");
  const recoveryDefinition = definition("integrity-recovery");
  const systemsDefinition = definition("integrity-subsystems");
  const futureDefinition = definition("integrity-future");

  return (
    <section className="integrity-cockpit" aria-label="Cockpit d’intégrité">
      <div className="integrity-zone integrity-protection-zone">
        <div className="zone-title"><span>01</span><strong>PROTECTIONS</strong></div>
        <Panel
          definition={protectionDefinition}
          resolved={resolved("integrity-protection")}
          onInspect={() => onInspect({ definition: protectionDefinition })}
        >
          <div className="lifecycle-strip">
            <span>ÉTAT VAISSEAU</span>
            <strong>{derived(snapshot, "lifecycle_label")?.value?.toString() ?? "—"}</strong>
          </div>
          <Chips
            values={decodeFlags(damage.protection_flags, PROTECTION_FLAGS)}
            empty="AUCUNE PROTECTION"
          />
          <Chips
            values={decodeFlags(lifecycle.lifecycle_flags, LIFECYCLE_FLAGS)}
            empty="CYCLE NOMINAL"
          />
          <div className="protection-values">
            <Metric label="ARMURE" value={damage.armor_id === undefined ? null : `ID ${damage.armor_id}`} />
            <Metric label="SEUIL GUARDIAN" value={damage.guardian_threshold} unit="HP" />
            <Metric label="MARGE GUARDIAN" value={derived(snapshot, "guardian_margin")?.value} unit="HP" />
            <Metric label="DÉGÂTS" value={
              typeof derived(snapshot, "hull_damage_ratio")?.value === "number"
                ? Number(derived(snapshot, "hull_damage_ratio")?.value) * 100
                : null
            } unit="%" />
          </div>
        </Panel>
      </div>

      <div className="integrity-zone integrity-core-zone">
        <div className="zone-title"><span>02</span><strong>STRUCTURE & BOUCLIERS</strong></div>
        <Panel
          definition={coreDefinition}
          resolved={resolved("integrity-core")}
          onInspect={() => onInspect({ definition: coreDefinition })}
          className="integrity-core-panel"
        >
          <IntegrityCore snapshot={snapshot} damage={damage} shield={shield} />
        </Panel>
      </div>

      <div className="integrity-zone integrity-recovery-zone">
        <div className="zone-title"><span>03</span><strong>RÉCUPÉRATION</strong></div>
        <Panel
          definition={recoveryDefinition}
          resolved={resolved("integrity-recovery")}
          onInspect={() => onInspect({ definition: recoveryDefinition })}
        >
          {!shield.has_shields ? (
            <div className="integrity-not-applicable"><strong>—</strong><span>aucun bouclier</span></div>
          ) : (
            <div className="recovery-grid">
              <Metric label="DÉFICIT" value={derived(snapshot, "shield_deficit")?.value} unit="HP" />
              <Metric label="RÉGÉNÉRATION" value={shield.regeneration_per_s} unit="HP/s" />
              <Metric label="PLAFOND RECHARGEABLE" value={shield.recharge_max} unit="HP" />
              <Metric label="TRANSFERT DIFFÉRÉ" value={shield.deferred_energy_transfer} />
              <Metric
                label="RECHARGE ESTIMÉE"
                value={derived(snapshot, "shield_recharge_eta_s")?.value}
                unit="s"
              />
            </div>
          )}
          <p className="estimate-note">ESTIMATION AU TAUX INSTANTANÉ COURANT</p>
        </Panel>
      </div>

      <div className="integrity-zone integrity-subsystems-zone">
        <div className="zone-title"><span>04</span><strong>SOUS-SYSTÈMES</strong></div>
        <Panel
          definition={systemsDefinition}
          resolved={resolved("integrity-subsystems")}
          onInspect={() => onInspect({ definition: systemsDefinition, kind: "subsystem-list" })}
        >
          <div className="subsystem-summary">
            <span>{systems.length} TOTAL</span>
            <span>{systems.filter((system) => system.destroyed).length} DÉTRUITS</span>
            <span>{systems.filter((system) => system.alert).length} ALERTES</span>
          </div>
          <div className="subsystem-matrix">
            {visibleSystems.map((system) => (
              <button
                className={`subsystem-tile ${system.destroyed ? "destroyed" : ""} ${system.alert ? "alert" : ""}`}
                key={`${system.entityId}-${system.subsystemId}`}
                onClick={() => onInspect({
                  definition: systemsDefinition,
                  kind: "subsystem",
                  record: system.record,
                  title: system.name
                })}
                aria-label={`Inspecter ${system.name}`}
              >
                <span><b>{system.name}</b><small>{system.typeLabel}</small></span>
                <i>
                  {system.ratio === null ? "—" : `${(system.ratio * 100).toFixed(0)}%`}
                </i>
                <em><u style={{ width: `${(system.ratio ?? 0) * 100}%` }} /></em>
                <strong>
                  {system.destroyed
                    ? "DÉTRUIT"
                    : system.flags.includes("PERTURBÉ")
                      ? "PERTURBÉ"
                      : system.flags.includes("MOUVEMENT VERROUILLÉ")
                        ? "MVT VERROUILLÉ"
                        : system.cooldownUs !== null
                          ? `CD ${formatValue(system.cooldownUs)} µs`
                          : "NOMINAL"}
                </strong>
              </button>
            ))}
          </div>
          {systems.length > 12 ? (
            <button
              className="subsystem-more"
              onClick={() => onInspect({ definition: systemsDefinition, kind: "subsystem-list" })}
            >
              + {systems.length - 12} AUTRES · OUVRIR LA LISTE COMPLÈTE
            </button>
          ) : null}
          {!systems.length ? <div className="integrity-not-applicable">— aucun sous-système</div> : null}
        </Panel>
      </div>

      <div className="integrity-zone integrity-future-zone">
        <Panel
          definition={futureDefinition}
          resolved={resolved("integrity-future")}
          onInspect={() => onInspect({ definition: futureDefinition })}
        ><span /></Panel>
        <div className="integrity-future-list" aria-label="Données d’intégrité futures non disponibles">
          {[
            "DIRECTION / POSITION IMPACT",
            "DERNIÈRE SOURCE",
            "DERNIÈRE ARME",
            "DÉGÂTS CUMULÉS",
            "CONTRIBUTEURS"
          ].map((label) => <span key={label}><b>ND</b>{label}</span>)}
        </div>
      </div>
    </section>
  );
}
