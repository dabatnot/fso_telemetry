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
  entityName,
  entityRecord,
  serviceRows
} from "./supportSemantics";
import type { DashboardSnapshot, InspectionTarget } from "./types";

interface Props {
  target: InspectionTarget;
  snapshot: DashboardSnapshot | null;
  onSelect: (target: InspectionTarget) => void;
}

function RawValues({
  title,
  values
}: {
  title: string;
  values: Record<string, unknown> | null | undefined;
}) {
  if (!values) return null;
  return (
    <div className="detail-values">
      <h3>{title}</h3>
      {Object.entries(values).map(([field, value]) => (
        <div key={field}>
          <span>{field}</span>
          <code>{value === undefined ? "—" : JSON.stringify(value)}</code>
        </div>
      ))}
    </div>
  );
}

function quality(
  snapshot: DashboardSnapshot | null,
  recordName: string,
  entityId?: string
) {
  return snapshot?.quality.channels.find(
    (channel) => channel.recordName === recordName &&
      (!entityId || channel.identity.includes(`entity_id=${entityId}`))
  );
}

function DetailState({ snapshot }: { snapshot: DashboardSnapshot | null }) {
  const state = snapshot?.connection.status === "Stale" ? "stale" : "live";
  return <div className={`detail-state state-${state}`}>{state.toUpperCase()}</div>;
}

export function SupportInspection({ target, snapshot, onSelect }: Props) {
  if (target.kind === "docking-component") {
    const nodes = dockingNodes(snapshot);
    const relations = dockingRelations(snapshot);
    return (
      <>
        <span className="eyebrow">INSPECTION EXHAUSTIVE</span>
        <h2>Composante d’amarrage</h2>
        <DetailState snapshot={snapshot} />
        <p className="detail-summary">
          {nodes.length} entités · {relations.length / 2} relations réciproques
        </p>
        <div className="subsystem-detail-list">
          {nodes.map((node) => (
            <button
              key={node.entityId}
              onClick={() => onSelect({
                definition: target.definition,
                kind: "support-entity",
                entityId: node.entityId,
                title: node.name
              })}
            >
              <span>
                <strong>{node.name}</strong>
                <small>ENTITÉ {node.entityId}</small>
              </span>
              <b>{node.player ? "JOUEUR" : node.support ? "SUPPORT" : node.leader ? "LEADER" : "DOCKÉ"}</b>
            </button>
          ))}
        </div>
        <div className="detail-values">
          <h3>Relations publiées</h3>
          {relations.map((relation, index) => (
            <button
              className="detail-relation-button"
              key={`${relation.subjectId}-${relation.remoteId}-${index}`}
              onClick={() => onSelect({
                definition: target.definition,
                kind: "docking-relation",
                entityId: relation.subjectId,
                remoteEntityId: relation.remoteId,
                record: relation.record,
                title: `${relation.localBay} ↔ ${relation.remoteBay}`
              })}
            >
              {relation.subjectName} · {relation.localBay} → {relation.remoteName} · {relation.remoteBay}
            </button>
          ))}
        </div>
      </>
    );
  }

  if (target.kind === "docking-relation") {
    const relation = dockingRelations(snapshot).find(
      (candidate) =>
        candidate.subjectId === String(target.entityId) &&
        candidate.remoteId === String(target.remoteEntityId)
    );
    if (!relation) {
      return (
        <>
          <span className="eyebrow">INSPECTION RELATION</span>
          <h2>{target.title ?? "Relation d’amarrage"}</h2>
          <div className="detail-state state-invalid">ERR</div>
          <p className="detail-empty">Relation absente du snapshot courant.</p>
        </>
      );
    }
    return (
      <>
        <button className="detail-back" onClick={() => onSelect({
          definition: target.definition,
          kind: "docking-component"
        })}>← COMPOSANTE</button>
        <span className="eyebrow">INSPECTION RELATION</span>
        <h2>{relation.subjectName} ↔ {relation.remoteName}</h2>
        <DetailState snapshot={snapshot} />
        <dl>
          <dt>Entité locale</dt><dd>{relation.subjectName} · {relation.subjectId}</dd>
          <dt>Entité distante</dt><dd>{relation.remoteName} · {relation.remoteId}</dd>
          <dt>Point local</dt><dd>{relation.localBay} · index {relation.localDockpoint ?? "—"}</dd>
          <dt>Point distant</dt><dd>{relation.remoteBay} · index {relation.remoteDockpoint ?? "—"}</dd>
          <dt>Relation inverse</dt><dd>{relation.reciprocal ? "PRÉSENTE" : "ERR · ABSENTE"}</dd>
        </dl>
        <RawValues title="Relation brute" values={relation.relation} />
        <RawValues title="DOCKING_STATE local" values={relation.record} />
        <RawValues
          title="DOCKING_STATE distant"
          values={entityRecord(snapshot, "DOCKING_STATE", relation.remoteId)}
        />
      </>
    );
  }

  if (target.kind === "cargo-target") {
    const cargo = cargoState(snapshot);
    const player = String(snapshot?.playerEntityId ?? "");
    const scanQuality = quality(snapshot, "CARGO_SCAN_STATE", player);
    const targetId = cargo?.target_entity_id;
    const targetName = targetId === undefined
      ? "— cible non exposée"
      : entityName(snapshot, targetId);
    return (
      <>
        <span className="eyebrow">INSPECTION SCANNER CARGO</span>
        <h2>{targetName}</h2>
        <DetailState snapshot={snapshot} />
        <dl>
          <dt>Phase</dt><dd>{decodeClosed(cargo?.scan_phase, CARGO_PHASES) ?? "ERR"}</dd>
          <dt>Divulgation</dt><dd>{decodeClosed(cargo?.disclosure, DISCLOSURE_STATES) ?? "ERR"}</dd>
          <dt>Sous-système</dt><dd>{cargoSubsystemName(snapshot, targetId, cargo?.target_subsystem_id)}</dd>
          <dt>Progression</dt><dd>{String(cargo?.elapsed_us ?? "—")} / {String(cargo?.required_us ?? "—")} µs</dd>
          <dt>Conditions</dt><dd>{decodeFlags(cargo?.validity_flags, CARGO_VALIDITY_FLAGS)?.join(", ") || "—"}</dd>
          <dt>Contenu</dt><dd>{String(cargo?.cargo_text ?? "— masqué")}</dd>
          <dt>Échantillon</dt><dd>{String(cargo?.producer_sample_time_us ?? "—")}</dd>
          <dt>Âge estimé</dt><dd>{scanQuality?.ageUs === null || scanQuality?.ageUs === undefined ? "—" : `${scanQuality.ageUs} µs`}</dd>
          <dt>Cadence observée</dt><dd>{scanQuality?.observedHz === null || scanQuality?.observedHz === undefined ? "—" : `${scanQuality.observedHz.toFixed(2)} Hz`}</dd>
        </dl>
        <RawValues title="CARGO_SCAN_STATE brut" values={cargo} />
        <RawValues title="Identité de la cible" values={entityRecord(snapshot, "SHIP_IDENTITY", targetId)} />
      </>
    );
  }

  const entityId = target.entityId ?? String(snapshot?.playerEntityId ?? "");
  const identity = entityRecord(snapshot, "SHIP_IDENTITY", entityId);
  const lifecycle = entityRecord(snapshot, "ENTITY_LIFECYCLE", entityId);
  const support = entityRecord(snapshot, "SUPPORT_STATE", entityId);
  const docking = entityRecord(snapshot, "DOCKING_STATE", entityId);
  const flight = entityRecord(snapshot, "FLIGHT_STATE", entityId);
  const supportQuality = quality(snapshot, "SUPPORT_STATE", entityId);
  const supportFlags = decodeFlags(support?.support_flags, SUPPORT_FLAGS);
  return (
    <>
      <span className="eyebrow">INSPECTION ENTITÉ SUPPORT / DOCKING</span>
      <h2>{entityName(snapshot, entityId)}</h2>
      <DetailState snapshot={snapshot} />
      <dl>
        <dt>Entité</dt><dd>{entityId || "—"}</dd>
        <dt>Phase support</dt><dd>{decodeClosed(support?.phase, SUPPORT_PHASES) ?? "—"}</dd>
        <dt>Flags support</dt><dd>{supportFlags?.join(", ") || "aucun"}</dd>
        <dt>Support assigné</dt><dd>{support?.support_entity_id === undefined ? "—" :
          entityName(snapshot, support.support_entity_id)}</dd>
        <dt>Phase docking</dt><dd>{decodeClosed(docking?.phase, DOCKING_PHASES) ?? "—"}</dd>
        <dt>Relations directes</dt><dd>{Array.isArray(docking?.relations) ? docking.relations.length : 0}</dd>
        <dt>Échantillon</dt><dd>{String(support?.producer_sample_time_us ?? "—")}</dd>
        <dt>Âge estimé</dt><dd>{supportQuality?.ageUs === null || supportQuality?.ageUs === undefined ? "—" : `${supportQuality.ageUs} µs`}</dd>
        <dt>Cadence observée</dt><dd>{supportQuality?.observedHz === null || supportQuality?.observedHz === undefined ? "—" : `${supportQuality.observedHz.toFixed(2)} Hz`}</dd>
      </dl>
      {entityId === String(snapshot?.playerEntityId ?? "") ? (
        <div className="detail-values">
          <h3>État courant à restaurer</h3>
          {serviceRows(snapshot).map((row) => (
            <div key={row.id}><span>{row.label}</span><code>{row.value}</code></div>
          ))}
        </div>
      ) : null}
      <RawValues title="SHIP_IDENTITY" values={identity} />
      <RawValues title="ENTITY_LIFECYCLE" values={lifecycle} />
      <RawValues title="SUPPORT_STATE" values={support} />
      <RawValues title="DOCKING_STATE" values={docking} />
      <RawValues title="FLIGHT_STATE" values={flight} />
    </>
  );
}
