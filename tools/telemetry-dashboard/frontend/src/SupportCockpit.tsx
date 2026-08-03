import type { ReactNode } from "react";
import { resolveInstrument } from "./data";
import {
  CARGO_PHASES,
  CARGO_VALIDITY_FLAGS,
  DISCLOSURE_STATES,
  DOCKING_PHASES,
  SUPPORT_FLAGS,
  SUPPORT_PHASES,
  cargoState,
  cargoSubsystemName,
  decodeClosed,
  decodeFlags,
  dockingNodes,
  dockingRelations,
  dockingState,
  entityName,
  entityReferenceIsInvalid,
  playerDockingRelations,
  serviceRows,
  supportReferenceInvalid,
  supportState,
  visibleDockingNodes
} from "./supportSemantics";
import type {
  DashboardSnapshot,
  InspectionTarget,
  InstrumentDefinition
} from "./types";

interface Props {
  definitions: InstrumentDefinition[];
  snapshot: DashboardSnapshot | null;
  onInspect: (target: InspectionTarget) => void;
}

function definitionById(definitions: InstrumentDefinition[], id: string) {
  const definition = definitions.find((candidate) => candidate.id === id);
  if (!definition) throw new Error(`instrument manquant: ${id}`);
  return definition;
}

function derivedNumber(snapshot: DashboardSnapshot | null, key: string): number | null {
  const item = snapshot?.derived[key];
  const numeric = Number(item?.value);
  return item?.available && Number.isFinite(numeric) ? numeric : null;
}

function Panel({
  title,
  definition,
  snapshot,
  onInspect,
  children,
  className = ""
}: {
  title: string;
  definition: InstrumentDefinition;
  snapshot: DashboardSnapshot | null;
  onInspect: Props["onInspect"];
  children: ReactNode;
  className?: string;
}) {
  const resolved = resolveInstrument(definition, snapshot);
  const unavailable = !["live", "stale"].includes(resolved.state);
  const labels: Record<string, string> = {
    nd: "ND",
    not_applicable: "—",
    invalid: "ERR",
    waiting: "EN ATTENTE"
  };
  return (
    <section className={`support-panel state-${resolved.state} ${className}`}>
      <button
        className="support-panel-header"
        onClick={() => onInspect({ definition })}
        aria-label={`Inspecter ${title}: ${resolved.state}`}
      >
        <span>{title}</span>
        <i>{resolved.state === "live" ? "LIVE" : resolved.state === "stale" ? "STALE" : labels[resolved.state]}</i>
      </button>
      <div className="support-panel-body">
        {unavailable ? (
          <div className="support-unavailable">
            <strong>{labels[resolved.state]}</strong>
            <span>{resolved.reason}</span>
          </div>
        ) : children}
      </div>
    </section>
  );
}

function SupportOverview({
  snapshot,
  definition,
  onInspect
}: {
  snapshot: DashboardSnapshot | null;
  definition: InstrumentDefinition;
  onInspect: Props["onInspect"];
}) {
  const support = supportState(snapshot);
  const phase = decodeClosed(support?.phase, SUPPORT_PHASES);
  const flags = decodeFlags(support?.support_flags, SUPPORT_FLAGS);
  const supportId = support?.support_entity_id;
  const invalid = supportReferenceInvalid(snapshot) || phase === null || flags === null;
  const assigned = supportId !== undefined && supportId !== null;
  return (
    <div className={`support-overview ${invalid ? "invalid" : ""}`}>
      <button
        className="support-identity"
        onClick={() => onInspect({
          definition,
          kind: "support-entity",
          entityId: supportId === undefined ? undefined : String(supportId),
          title: assigned ? entityName(snapshot, supportId) : "Aucun support assigné"
        })}
      >
        <span>VAISSEAU DE SUPPORT</span>
        <strong>{invalid ? "ERR" : assigned ? entityName(snapshot, supportId) : "— AUCUN SUPPORT ASSIGNÉ"}</strong>
        <small>{assigned ? `ENTITÉ ${String(supportId)}` : "aucune affectation active"}</small>
      </button>
      <div className="support-phase">
        <span>PHASE ACTUELLE</span>
        <strong>{invalid ? "ERR" : phase}</strong>
        <small>état courant · aucun verdict de réussite</small>
      </div>
      <div className="support-flag-strip">
        {SUPPORT_FLAGS.map((flag) => {
          const active = flags?.includes(flag.label) ?? false;
          return (
            <span key={flag.label} className={active ? "active" : ""}>
              <i />{flag.label}
            </span>
          );
        })}
      </div>
    </div>
  );
}

function ServiceMatrix({ snapshot }: { snapshot: DashboardSnapshot | null }) {
  return (
    <div className="support-service-matrix">
      {serviceRows(snapshot).map((row) => (
        <div key={row.id} className={row.ratio === null ? "not-applicable" : ""}>
          <span>{row.label}</span>
          <strong>{row.value}</strong>
          <div className="support-service-track">
            <i style={{ width: `${(row.ratio ?? 0) * 100}%` }} />
          </div>
          <small>{row.reason ?? "état courant · pas une promesse de remise à niveau"}</small>
        </div>
      ))}
    </div>
  );
}

function Approach({ snapshot }: { snapshot: DashboardSnapshot | null }) {
  const player = String(snapshot?.playerEntityId ?? "");
  const support = supportState(snapshot);
  const assigned = support?.support_entity_id !== undefined;
  const distance = derivedNumber(snapshot, `entities.${player}.support.distance`);
  const relative = derivedNumber(snapshot, `entities.${player}.support.relative_speed`);
  const closing = derivedNumber(snapshot, `entities.${player}.support.closing_speed`);
  const metric = (label: string, value: number | null, unit: string) => (
    <div>
      <span>{label}</span>
      <strong>{value === null ? "—" : value.toLocaleString("fr-FR", { maximumFractionDigits: 1 })}</strong>
      <small>{value === null ? "indisponible" : unit}</small>
    </div>
  );
  if (!assigned) return <div className="approach-empty">— AUCUN SUPPORT ASSIGNÉ</div>;
  return (
    <>
      <div className="approach-vector" aria-hidden="true">
        <div className="approach-player">JOUEUR</div>
        <div className={`approach-line ${(closing ?? 0) > 0 ? "closing" : "opening"}`}>
          <i />
        </div>
        <div className="approach-support">SUPPORT</div>
      </div>
      <div className="approach-metrics">
        {metric("DISTANCE", distance, "unités monde")}
        {metric("VITESSE RELATIVE", relative, "unités monde/s")}
        {metric("RAPPROCHEMENT", closing, "positif = approche")}
      </div>
      <p>MESURES GÉOMÉTRIQUES · AUCUNE ETA DÉDUITE</p>
    </>
  );
}

function DockingDiagram({
  snapshot,
  definition,
  onInspect
}: {
  snapshot: DashboardSnapshot | null;
  definition: InstrumentDefinition;
  onInspect: Props["onInspect"];
}) {
  const state = dockingState(snapshot);
  const phase = decodeClosed(state?.phase, DOCKING_PHASES);
  const nodes = visibleDockingNodes(snapshot);
  const allNodes = dockingNodes(snapshot);
  const visibleIds = new Set(nodes.map((node) => node.entityId));
  const relations = dockingRelations(snapshot);
  const edges = relations.filter((relation, index) => {
    if (!visibleIds.has(relation.subjectId) || !visibleIds.has(relation.remoteId)) return false;
    const key = [relation.subjectId, relation.remoteId].sort().join(":");
    return relations.findIndex((candidate) =>
      [candidate.subjectId, candidate.remoteId].sort().join(":") === key
    ) === index;
  });
  const support = supportState(snapshot);
  const playerId = String(snapshot?.playerEntityId ?? "");
  const supportId = String(support?.support_entity_id ?? "");
  const supportDocked = edges.some((edge) =>
    [edge.subjectId, edge.remoteId].includes(playerId) &&
    [edge.subjectId, edge.remoteId].includes(supportId)
  );
  return (
    <div className="docking-diagram">
      <div className="docking-summary">
        <span>PHASE <strong>{phase ?? "ERR"}</strong></span>
        <span>RELATIONS <strong>{playerDockingRelations(snapshot).length}</strong></span>
        <span>LEADER <strong>{state?.group_leader_entity_id &&
          String(state.group_leader_entity_id) !== "0"
          ? entityName(snapshot, state.group_leader_entity_id)
          : "—"}</strong></span>
      </div>
      <div className="docking-graph">
        <svg viewBox="0 0 100 100" preserveAspectRatio="none" aria-hidden="true">
          {edges.map((edge, index) => {
            const from = nodes.findIndex((node) => node.entityId === edge.subjectId);
            const to = nodes.findIndex((node) => node.entityId === edge.remoteId);
            const x1 = 12 + (from % 4) * 25;
            const y1 = from < 4 ? 24 : 76;
            const x2 = 12 + (to % 4) * 25;
            const y2 = to < 4 ? 24 : 76;
            return <line key={`${edge.subjectId}-${edge.remoteId}-${index}`} x1={x1} y1={y1} x2={x2} y2={y2} />;
          })}
          {supportId && supportId !== "0" && !supportDocked && visibleIds.has(supportId) ? (
            <line className="support-assignment" x1="12" y1="24"
              x2={12 + Math.max(0, nodes.findIndex((node) => node.entityId === supportId) % 4) * 25}
              y2={nodes.findIndex((node) => node.entityId === supportId) < 4 ? 24 : 76} />
          ) : null}
        </svg>
        {nodes.map((node, index) => (
          <button
            key={node.entityId}
            className={[
              "docking-node",
              node.player ? "player" : "",
              node.support ? "support" : "",
              node.leader ? "leader" : ""
            ].filter(Boolean).join(" ")}
            style={{
              left: `${4 + (index % 4) * 25}%`,
              top: index < 4 ? "8%" : "60%"
            }}
            onClick={() => onInspect({
              definition,
              kind: "support-entity",
              entityId: node.entityId,
              title: node.name
            })}
          >
            <strong>{node.name}</strong>
            <span>{node.player ? "JOUEUR" : node.support ? "SUPPORT" : "DOCKÉ"}</span>
          </button>
        ))}
      </div>
      {allNodes.length > nodes.length ? (
        <button
          className="support-more"
          onClick={() => onInspect({
            definition,
            kind: "docking-component",
            title: "Composante d’amarrage"
          })}
        >
          + {allNodes.length - nodes.length} AUTRES · COMPOSANTE COMPLÈTE
        </button>
      ) : null}
      <div className="docking-ports">
        {playerDockingRelations(snapshot).slice(0, 3).map((relation) => (
          <button
            key={`${relation.subjectId}-${relation.remoteId}`}
            onClick={() => onInspect({
              definition,
              kind: "docking-relation",
              entityId: relation.subjectId,
              remoteEntityId: relation.remoteId,
              record: relation.record,
              title: `${relation.localBay} ↔ ${relation.remoteBay}`
            })}
          >
            <span>{relation.remoteName}</span>
            <strong>{relation.localBay} ↔ {relation.remoteBay}</strong>
          </button>
        ))}
        {!playerDockingRelations(snapshot).length ? <span>— AUCUNE RELATION MATÉRIALISÉE</span> : null}
      </div>
    </div>
  );
}

function CargoScanner({
  snapshot,
  definition,
  onInspect
}: {
  snapshot: DashboardSnapshot | null;
  definition: InstrumentDefinition;
  onInspect: Props["onInspect"];
}) {
  const cargo = cargoState(snapshot);
  const phase = decodeClosed(cargo?.scan_phase, CARGO_PHASES);
  const disclosure = decodeClosed(cargo?.disclosure, DISCLOSURE_STATES);
  const player = String(snapshot?.playerEntityId ?? "");
  const ratio = derivedNumber(snapshot, `entities.${player}.cargo.progress_ratio`);
  const remainingUs = derivedNumber(snapshot, `entities.${player}.cargo.remaining_us`);
  const targetId = cargo?.target_entity_id;
  const hidden = Number(cargo?.scan_phase) === 0 && Number(cargo?.disclosure) === 0;
  const invalidTarget = entityReferenceIsInvalid(snapshot, targetId);
  const flags = decodeFlags(cargo?.validity_flags, CARGO_VALIDITY_FLAGS);
  const invalid = phase === null || disclosure === null || invalidTarget ||
    (cargo?.validity_flags !== undefined && flags === null);
  return (
    <button
      className={`cargo-scanner ${invalid ? "invalid" : ""}`}
      onClick={() => onInspect({
        definition,
        kind: "cargo-target",
        entityId: targetId === undefined ? undefined : String(targetId),
        subsystemId: cargo?.target_subsystem_id === undefined
          ? undefined
          : String(cargo.target_subsystem_id),
        record: cargo ?? undefined,
        title: hidden ? "Cible cargo non exposée" : entityName(snapshot, targetId)
      })}
    >
      <div className="cargo-target">
        <span>CIBLE CARGO</span>
        <strong>{invalid ? "ERR" : hidden ? "— CIBLE NON EXPOSÉE" : entityName(snapshot, targetId)}</strong>
        <small>{hidden ? "NOT_SCANNABLE · HIDDEN" :
          cargoSubsystemName(snapshot, targetId, cargo?.target_subsystem_id)}</small>
      </div>
      <div className="cargo-progress">
        <div>
          <span>PHASE</span><strong>{invalid ? "ERR" : phase}</strong>
          <small>{disclosure}</small>
        </div>
        <div className="cargo-progress-ring">
          <svg viewBox="0 0 120 120" aria-hidden="true">
            <circle cx="60" cy="60" r="48" />
            <circle className="value" cx="60" cy="60" r="48"
              style={{ strokeDashoffset: 302 - 302 * (ratio ?? 0) }} />
          </svg>
          <strong>{ratio === null ? "—" : `${Math.round(ratio * 100)}%`}</strong>
        </div>
        <div>
          <span>RESTANT</span>
          <strong>{remainingUs === null ? "—" : `${(remainingUs / 1_000_000).toFixed(1)} s`}</strong>
          <small>{Number(cargo?.scan_phase) === 1 ? "progression figée" : "accumulation théorique"}</small>
        </div>
      </div>
      <div className="cargo-validity">
        {CARGO_VALIDITY_FLAGS.map((flag) => (
          <span key={flag.label} className={flags?.includes(flag.label) ? "active" : ""}>
            <i />{flag.label}
          </span>
        ))}
      </div>
      <div className="cargo-disclosure">
        <span>CONTENU DIVULGUÉ</span>
        <strong>{Number(cargo?.disclosure) === 1 ? String(cargo?.cargo_text ?? "ERR") : "— MASQUÉ"}</strong>
      </div>
    </button>
  );
}

function FutureStrip({ definition }: { definition: InstrumentDefinition }) {
  const labels = [
    "ETA SUPPORT / INTERVENTION",
    "PROGRESSION GLOBALE",
    "DÉBITS ET CAUSES",
    "VERDICT DE RÉUSSITE",
    "ALIGNEMENT / ETA DOCKING",
    "DISTANCE / ANGLE CARGO",
    "ÉTAT DÉTAILLÉ DES CAPTEURS",
    "DOCKING GLOBAL"
  ];
  return (
    <section className="support-future-strip">
      <header><span>{definition.label}</span><i>ND</i></header>
      <div>{labels.map((label) => <span key={label}><b>ND</b>{label}</span>)}</div>
    </section>
  );
}

export function SupportCockpit({ definitions, snapshot, onInspect }: Props) {
  const overview = definitionById(definitions, "support-overview");
  const service = definitionById(definitions, "support-service");
  const approach = definitionById(definitions, "support-approach");
  const docking = definitionById(definitions, "support-docking");
  const cargo = definitionById(definitions, "support-cargo");
  const future = definitionById(definitions, "support-future");
  return (
    <section className="support-cockpit" aria-label="Cockpit support docking cargo">
      <Panel title="Support assigné et phase" definition={overview} snapshot={snapshot}
        onInspect={onInspect} className="support-overview-zone">
        <SupportOverview snapshot={snapshot} definition={overview} onInspect={onInspect} />
      </Panel>
      <Panel title="État courant à restaurer" definition={service} snapshot={snapshot}
        onInspect={onInspect} className="support-service-zone">
        <ServiceMatrix snapshot={snapshot} />
      </Panel>
      <Panel title="Approche relative" definition={approach} snapshot={snapshot}
        onInspect={onInspect} className="support-approach-zone">
        <Approach snapshot={snapshot} />
      </Panel>
      <Panel title="Composante d’amarrage" definition={docking} snapshot={snapshot}
        onInspect={onInspect} className="support-docking-zone">
        <DockingDiagram snapshot={snapshot} definition={docking} onInspect={onInspect} />
      </Panel>
      <Panel title="Scanner cargo" definition={cargo} snapshot={snapshot}
        onInspect={onInspect} className="support-cargo-zone">
        <CargoScanner snapshot={snapshot} definition={cargo} onInspect={onInspect} />
      </Panel>
      <FutureStrip definition={future} />
    </section>
  );
}
