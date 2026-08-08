import { useEffect, useMemo, useRef, type MouseEvent, type ReactNode } from "react";
import { resolveInstrument } from "./data";
import {
  lockViews,
  missileViews,
  prioritizedContacts,
  radarState,
  sensorLabels,
  subsystemNameForTarget,
  targetClassDisplayName,
  targetContact,
  targetDisplayName,
  targetHudTypeLabel,
  targetRecordVersion,
  targetReferenceInvalid,
  targetState,
  threatState,
  type ContactView
} from "./tacticalSemantics";
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
  if (!definition) throw new Error(`instrument tactique manquant: ${id}`);
  return definition;
}

function finite(value: unknown): number | null {
  const numeric = Number(value);
  return Number.isFinite(numeric) ? numeric : null;
}

function formatNumber(value: number | null, digits = 0): string {
  return value === null
    ? "—"
    : value.toLocaleString("fr-FR", { maximumFractionDigits: digits });
}

function formatDurationUs(value: unknown): string {
  const microseconds = finite(value);
  return microseconds === null ? "—" : `${formatNumber(microseconds / 1_000_000, 1)} s`;
}

function TacticalPanel({
  definition,
  snapshot,
  title,
  className,
  onInspect,
  children
}: {
  definition: InstrumentDefinition;
  snapshot: DashboardSnapshot | null;
  title: string;
  className: string;
  onInspect: Props["onInspect"];
  children: ReactNode;
}) {
  const resolved = resolveInstrument(definition, snapshot);
  const unavailable = !["live", "stale"].includes(resolved.state);
  const label: Record<string, string> = {
    nd: "ND",
    not_applicable: "—",
    invalid: "ERR",
    waiting: "EN ATTENTE"
  };
  return (
    <section className={`tactical-panel ${className} state-${resolved.state}`}>
      <button
        className="tactical-panel-header"
        onClick={() => onInspect({ definition })}
        aria-label={`Inspecter ${title}: ${resolved.state}`}
      >
        <span>{title}</span>
        <i>{resolved.state === "live" ? "LIVE" : resolved.state === "stale" ? "STALE" : label[resolved.state]}</i>
      </button>
      <div className="tactical-panel-body">
        {unavailable ? (
          <div className="tactical-unavailable">
            <strong>{label[resolved.state]}</strong>
            <span>{resolved.reason}</span>
          </div>
        ) : children}
      </div>
    </section>
  );
}

function SensorStrip({
  snapshot,
  definition,
  onInspect
}: {
  snapshot: DashboardSnapshot | null;
  definition: InstrumentDefinition;
  onInspect: Props["onInspect"];
}) {
  const radar = radarState(snapshot);
  const labels = sensorLabels(snapshot);
  const player = String(snapshot?.playerEntityId ?? "");
  const ratioItem = snapshot?.derived[`entities.${player}.sensor_integrity_ratio`];
  const ratio = ratioItem?.available ? finite(ratioItem.value) : null;
  const current = finite(radar?.sensor_current_hits);
  const maximum = finite(radar?.sensor_max_hits);
  const range = finite(radar?.selected_range);
  const emp = finite(radar?.emp_intensity);
  const invalid = labels.mode === null || labels.state === null || ratio === null;
  return (
    <TacticalPanel
      definition={definition}
      snapshot={snapshot}
      title="CAPTEURS · PORTÉE · ENVIRONNEMENT"
      className="tactical-sensors"
      onInspect={onInspect}
    >
      <div className={`sensor-strip ${invalid ? "invalid" : ""}`}>
        <div><span>MODE RADAR</span><strong>{labels.mode ?? "ERR"}</strong></div>
        <div><span>PORTÉE</span><strong>{formatNumber(range, 0)}</strong><small>unités monde</small></div>
        <div className="sensor-health">
          <span>CAPTEURS</span>
          <strong>{labels.state ?? "ERR"}</strong>
          <div><i style={{ width: `${Math.max(0, Math.min(1, ratio ?? 0)) * 100}%` }} /></div>
          <small>{formatNumber(current, 1)} / {formatNumber(maximum, 1)} HP</small>
        </div>
        <div><span>AWACS</span><strong>{radar?.awacs_intensity === undefined ? "—" : formatNumber(finite(radar.awacs_intensity), 2)}</strong><small>{radar?.awacs_range === undefined ? "non applicable" : `portée ${formatNumber(finite(radar.awacs_range))}`}</small></div>
        <div className={(emp ?? 0) > 0 ? "alert" : ""}>
          <span>EMP</span><strong>{radar?.emp_intensity === undefined ? "—" : formatNumber(emp, 2)}</strong>
          <small>{radar?.emp_remaining_us === undefined ? "non applicable" : formatDurationUs(radar.emp_remaining_us)}</small>
        </div>
      </div>
    </TacticalPanel>
  );
}

type HitPoint = { contact: ContactView; x: number; y: number };

function drawContact(
  context: CanvasRenderingContext2D,
  contact: ContactView,
  x: number,
  y: number
) {
  const target = (contact.flagBits & 0x02) !== 0;
  const bomb = (contact.flagBits & 0x20) !== 0;
  const threat = (contact.flagBits & 0x80) !== 0;
  const homing = (contact.flagBits & 0x40) !== 0;
  context.save();
  context.translate(x, y);
  context.globalAlpha = contact.visibilityCode === 0 ? 0.35 : 1;
  context.strokeStyle = threat || bomb ? "#ff6b55" : target ? "#ffd466" : "#70e4d1";
  context.fillStyle = context.strokeStyle;
  context.lineWidth = target ? 2.6 : 1.5;
  context.setLineDash(contact.visibilityCode === 2 ? [3, 3] : []);
  const size = target ? 8 : threat || bomb ? 6 : 4.5;
  context.beginPath();
  if (bomb || homing) {
    context.moveTo(0, -size);
    context.lineTo(size, 0);
    context.lineTo(0, size);
    context.lineTo(-size, 0);
    context.closePath();
  } else if (contact.category === "VAISSEAU") {
    context.rect(-size, -size, size * 2, size * 2);
  } else if (contact.category === "ASTÉROÏDE" || contact.category === "DÉBRIS") {
    context.arc(0, 0, size, 0, Math.PI * 2);
  } else {
    context.moveTo(0, -size);
    context.lineTo(size, size);
    context.lineTo(-size, size);
    context.closePath();
  }
  context.stroke();
  if (target) {
    context.beginPath();
    context.moveTo(-12, -12); context.lineTo(-6, -12); context.lineTo(-12, -6);
    context.moveTo(12, -12); context.lineTo(6, -12); context.lineTo(12, -6);
    context.moveTo(-12, 12); context.lineTo(-6, 12); context.lineTo(-12, 6);
    context.moveTo(12, 12); context.lineTo(6, 12); context.lineTo(12, 6);
    context.stroke();
  }
  if (contact.elevationRad !== null && Math.abs(contact.elevationRad) > 0.04) {
    context.beginPath();
    const direction = contact.elevationRad > 0 ? -1 : 1;
    context.moveTo(-3, direction * 10);
    context.lineTo(0, direction * 13);
    context.lineTo(3, direction * 10);
    context.stroke();
  }
  if (contact.inRange === false) {
    context.fillRect(-1.5, -1.5, 3, 3);
  }
  context.restore();
}

function RadarScope({
  snapshot,
  definition,
  onInspect
}: {
  snapshot: DashboardSnapshot | null;
  definition: InstrumentDefinition;
  onInspect: Props["onInspect"];
}) {
  const canvas = useRef<HTMLCanvasElement>(null);
  const hits = useRef<HitPoint[]>([]);
  const contacts = useMemo(() => prioritizedContacts(snapshot), [snapshot]);
  const visibleList = contacts.slice(0, 8);

  useEffect(() => {
    const element = canvas.current;
    if (!element) return;
    const render = () => {
      const bounds = element.getBoundingClientRect();
      const dpr = Math.max(window.devicePixelRatio || 1, 1);
      const width = Math.max(1, Math.round(bounds.width * dpr));
      const height = Math.max(1, Math.round(bounds.height * dpr));
      if (element.width !== width || element.height !== height) {
        element.width = width;
        element.height = height;
      }
      const context = element.getContext("2d");
      if (!context) return;
      context.setTransform(dpr, 0, 0, dpr, 0, 0);
      context.clearRect(0, 0, bounds.width, bounds.height);
      const centerX = bounds.width / 2;
      const centerY = bounds.height / 2;
      const radius = Math.max(20, Math.min(bounds.width, bounds.height) / 2 - 18);
      context.strokeStyle = "rgba(112, 228, 209, .24)";
      context.fillStyle = "rgba(7, 22, 28, .76)";
      context.lineWidth = 1;
      context.beginPath();
      context.arc(centerX, centerY, radius, 0, Math.PI * 2);
      context.fill();
      context.stroke();
      for (const fraction of [0.25, 0.5, 0.75]) {
        context.beginPath();
        context.arc(centerX, centerY, radius * fraction, 0, Math.PI * 2);
        context.stroke();
      }
      context.beginPath();
      context.moveTo(centerX - radius, centerY);
      context.lineTo(centerX + radius, centerY);
      context.moveTo(centerX, centerY - radius);
      context.lineTo(centerX, centerY + radius);
      context.stroke();
      context.fillStyle = "#d5f8f1";
      context.font = "13px ui-monospace, monospace";
      context.textAlign = "center";
      // Match FSO's standard directional radar: the nose is at the centre,
      // while the disc direction tells the pilot which way to turn or pitch.
      context.fillText("HAUT", centerX, centerY - radius + 12);
      context.fillText("BAS", centerX, centerY + radius - 5);
      context.textAlign = "left";
      context.fillText("DROITE", centerX + radius - 40, centerY - 6);
      context.textAlign = "right";
      context.fillText("GAUCHE", centerX - radius + 40, centerY - 6);
      context.textAlign = "center";
      context.fillText("AVANT", centerX, centerY + 20);
      context.beginPath();
      context.moveTo(centerX, centerY - 6);
      context.lineTo(centerX - 5, centerY + 5);
      context.lineTo(centerX + 5, centerY + 5);
      context.closePath();
      context.fill();
      hits.current = [];
      for (const contact of contacts) {
        if (!contact.scope || contact.invalid) continue;
        const x = centerX + contact.scope[0] * radius;
        const y = centerY + contact.scope[1] * radius;
        drawContact(context, contact, x, y);
        hits.current.push({ contact, x, y });
      }
    };
    render();
    const observer = new ResizeObserver(render);
    observer.observe(element);
    return () => observer.disconnect();
  }, [contacts]);

  const chooseAt = (event: MouseEvent<HTMLCanvasElement>) => {
    const bounds = event.currentTarget.getBoundingClientRect();
    const x = event.clientX - bounds.left;
    const y = event.clientY - bounds.top;
    const nearest = hits.current
      .map((hit) => ({ hit, distance: Math.hypot(hit.x - x, hit.y - y) }))
      .filter((candidate) => candidate.distance <= 14)
      .sort((left, right) => left.distance - right.distance)[0];
    if (nearest) {
      onInspect({
        definition,
        kind: "radar-contact",
        entityId: nearest.hit.contact.id,
        record: nearest.hit.contact.record,
        title: nearest.hit.contact.name
      });
    }
  };

  return (
    <TacticalPanel
      definition={definition}
      snapshot={snapshot}
      title={`SCOPE RADAR · ${contacts.length} PISTE${contacts.length > 1 ? "S" : ""}`}
      className="tactical-scope"
      onInspect={onInspect}
    >
      <div className="scope-layout">
        <canvas
          ref={canvas}
          onClick={chooseAt}
          aria-label={`Scope radar affichant ${contacts.length} contacts`}
        />
        <div className="scope-contact-list" aria-label="Contacts radar prioritaires">
          {visibleList.map((contact) => (
            <button
              key={contact.id}
              className={`${(contact.flagBits & 0x02) ? "current" : ""} ${contact.invalid ? "invalid" : ""}`}
              onClick={() => onInspect({
                definition,
                kind: "radar-contact",
                entityId: contact.id,
                record: contact.record,
                title: contact.name
              })}
            >
              <span>{contact.name}</span>
              <strong>{contact.invalid ? "ERR" : formatNumber(contact.distance)}</strong>
              {contact.typeLabel && (
                <small className="contact-type">{contact.typeLabel}</small>
              )}
              <small className="contact-status">
                {contact.category} · {contact.visibility}
              </small>
            </button>
          ))}
          {!contacts.length && <div className="tactical-empty">AUCUN CONTACT AUTORISÉ</div>}
          {contacts.length > visibleList.length && (
            <button
              className="scope-more"
              onClick={() => onInspect({
                definition,
                kind: "radar-contact",
                title: "Tous les contacts"
              })}
            >
              + {contacts.length - visibleList.length} AUTRES
            </button>
          )}
        </div>
      </div>
    </TacticalPanel>
  );
}

const TRENDS: Record<number, string> = {
  0: "INCONNUE",
  1: "DIMINUE",
  2: "STABLE",
  3: "AUGMENTE"
};

function hudTrendSuffix(value: unknown, hide = false): string {
  if (hide) return "";
  if (Number(value) === 1) return "−";
  if (Number(value) === 3) return "+";
  return "";
}

function TargetPanel({
  snapshot,
  definition,
  onInspect
}: {
  snapshot: DashboardSnapshot | null;
  definition: InstrumentDefinition;
  onInspect: Props["onInspect"];
}) {
  const target = targetState(snapshot);
  const contact = targetContact(snapshot);
  const player = String(snapshot?.playerEntityId ?? "");
  const distanceItem = snapshot?.derived[`entities.${player}.target.distance`];
  const distance = distanceItem?.available ? finite(distanceItem.value) : null;
  const hudSpeedItem = snapshot?.derived[`entities.${player}.target.hud_speed`];
  const hudSpeed = hudSpeedItem?.available ? finite(hudSpeedItem.value) : null;
  const targetVersion = targetRecordVersion(snapshot);
  const targetClass = targetClassDisplayName(snapshot);
  const targetHudLabel = targetHudTypeLabel(snapshot);
  const targetId = String(target?.current_target_entity_id ?? "0");
  const invalid = targetReferenceInvalid(snapshot);
  const targetSubsystem = subsystemNameForTarget(snapshot, target?.target_subsystem_id);
  const lockSubsystem = subsystemNameForTarget(snapshot, target?.lock_subsystem_id);
  return (
    <TacticalPanel
      definition={definition}
      snapshot={snapshot}
      title="CIBLE SÉLECTIONNÉE"
      className="tactical-target"
      onInspect={onInspect}
    >
      {targetId === "0" ? (
        <div className="target-empty">— AUCUNE CIBLE</div>
      ) : (
        <button
          className={`target-card ${invalid ? "invalid" : ""}`}
          onClick={() => onInspect({
            definition,
            kind: "target",
            entityId: targetId,
            record: target ?? undefined,
            title: targetDisplayName(snapshot)
          })}
        >
          <span className="target-kicker">ENTITÉ {targetId}</span>
          <strong>{invalid ? "ERR · RÉFÉRENCE INCONNUE" : targetDisplayName(snapshot)}</strong>
          {(targetClass ?? targetHudLabel) !== null && <span className="target-class">{targetClass ?? targetHudLabel}</span>}
          <div className="target-hud-readout" aria-label="Informations HUD FSO">
            <span>D : {formatNumber(distance)}{hudTrendSuffix(target?.distance_trend)}</span>
            <span>S : {formatNumber(hudSpeed)}{hudTrendSuffix(target?.speed_trend, (hudSpeed ?? 0) <= 1)}</span>
            {targetVersion !== null && targetVersion < 2 && <i>COMPAT. CAPTURE V1</i>}
          </div>
          <div className="target-primary-metrics">
            <div><span>DISTANCE</span><strong>{formatNumber(distance)}</strong></div>
            <div><span>RAPPROCHEMENT</span><strong>{formatNumber(contact?.closingSpeed ?? null, 1)}</strong></div>
            <div><span>TEMPS CIBLE</span><strong>{formatDurationUs(target?.time_on_target_us)}</strong></div>
          </div>
          <div className="target-badges">
            <i>{contact?.category ?? "CONTACT"}</i>
            {target?.in_cone !== undefined && <i>{target.in_cone ? "DANS LE CÔNE" : "HORS CÔNE"}</i>}
            {target?.lead_world !== undefined && <i>LEAD · BANQUE {String(target.lead_bank_id)}</i>}
          </div>
          <dl>
            <dt>Tendance distance</dt><dd>{TRENDS[Number(target?.distance_trend)] ?? "—"}</dd>
            <dt>Tendance vitesse</dt><dd>{TRENDS[Number(target?.speed_trend)] ?? "—"}</dd>
            <dt>Sous-système ciblé</dt><dd>{targetSubsystem ?? "—"}</dd>
            <dt>Sous-système lock</dt><dd>{lockSubsystem ?? "—"}</dd>
            <dt>Cible précédente</dt><dd>{target?.previous_target_entity_id === undefined ? "—" : String(target.previous_target_entity_id)}</dd>
          </dl>
          {target?.last_stealth_position !== undefined && (
            <div className="stealth-memory">PISTE FURTIVE MÉMORISÉE · OBSERVATION ANCIENNE</div>
          )}
        </button>
      )}
    </TacticalPanel>
  );
}

function LockPanel({
  snapshot,
  definition,
  onInspect
}: {
  snapshot: DashboardSnapshot | null;
  definition: InstrumentDefinition;
  onInspect: Props["onInspect"];
}) {
  const locks = lockViews(snapshot);
  const visible = locks.slice(0, 8);
  return (
    <TacticalPanel
      definition={definition}
      snapshot={snapshot}
      title={`VERROUILLAGES · ${locks.length}`}
      className="tactical-locks"
      onInspect={onInspect}
    >
      <div className="lock-list">
        {visible.map((lock) => (
          <button
            key={`${lock.targetId}-${lock.index}`}
            className={`${lock.locked ? "acquired" : lock.attempt ? "attempt" : ""} ${lock.invalid ? "invalid" : ""}`}
            onClick={() => onInspect({
              definition,
              kind: "lock-point",
              entityId: lock.targetId,
              record: lock.record,
              title: lock.targetName
            })}
          >
            <div>
              <span>{lock.targetName}</span>
              <strong>{lock.invalid ? "ERR" : lock.locked ? "ACQUIS" : lock.attempt ? "ACQUISITION" : "SANS TENTATIVE"}</strong>
            </div>
            <div className="lock-progress">
              <i style={{ width: `${Math.max(0, Math.min(1, lock.progress ?? 0)) * 100}%` }} />
            </div>
            <small>
              {lock.inCone ? "DANS LE CÔNE" : "HORS CÔNE"} ·
              {lock.remainingS === null ? " progression —" : ` ${formatNumber(lock.remainingS, 1)} s`}
            </small>
          </button>
        ))}
        {!locks.length && <div className="tactical-empty">AUCUN LOCK</div>}
        {locks.length > visible.length && (
          <button
            className="lock-more"
            onClick={() => onInspect({ definition, kind: "lock-list", title: "Tous les verrouillages" })}
          >
            + {locks.length - visible.length} AUTRES
          </button>
        )}
      </div>
    </TacticalPanel>
  );
}

function ThreatPanel({
  snapshot,
  definition,
  onInspect
}: {
  snapshot: DashboardSnapshot | null;
  definition: InstrumentDefinition;
  onInspect: Props["onInspect"];
}) {
  const threat = threatState(snapshot);
  const labels = sensorLabels(snapshot);
  const missiles = missileViews(snapshot);
  const visible = missiles.slice(0, 6);
  const level = Number(threat?.threat_level ?? 0);
  return (
    <TacticalPanel
      definition={definition}
      snapshot={snapshot}
      title="MENACES ENTRANTES"
      className={`tactical-threats threat-level-${level}`}
      onInspect={onInspect}
    >
      <div className="threat-strip">
        <button
          className="threat-level"
          onClick={() => onInspect({ definition, record: threat ?? undefined })}
        >
          <span>NIVEAU</span>
          <strong>{labels.threat ?? "ERR"}</strong>
          <small>{missiles.length} MISSILE{missiles.length > 1 ? "S" : ""}</small>
        </button>
        <div className="missile-rack">
          {visible.map((missile) => (
            <button
              key={missile.id}
              className={missile.invalid ? "invalid" : ""}
              onClick={() => onInspect({
                definition,
                kind: "incoming-missile",
                entityId: missile.id,
                record: missile.record,
                title: missile.name
              })}
            >
              <span>{missile.name}</span>
              <strong>{missile.invalid ? "ERR" : missile.ttcS === null ? "—" : `${formatNumber(missile.ttcS, 1)} s EST.`}</strong>
              <small>{missile.guidance} · {formatNumber(missile.distance)} u</small>
            </button>
          ))}
          {!missiles.length && <div className="tactical-empty">AUCUN MISSILE ENTRANT</div>}
          {missiles.length > visible.length && (
            <button
              className="missile-more"
              onClick={() => onInspect({ definition, kind: "missile-list", title: "Tous les missiles" })}
            >
              + {missiles.length - visible.length}
            </button>
          )}
        </div>
      </div>
    </TacticalPanel>
  );
}

function FuturePanel({ definition }: { definition: InstrumentDefinition }) {
  const items = [
    "Brouillage global quantifié",
    "Historique global de visibilité",
    "Impact et probabilité autoritaires",
    "Prédiction du résultat d’un tir",
    "Coordonnées HUD projetées",
    "Vidéo de cible H.264"
  ];
  return (
    <section className="tactical-future" aria-label="Données tactiques non disponibles">
      <strong>ND</strong>
      {items.map((item) => <span key={item}>{item}</span>)}
      <small>{definition.reason}</small>
    </section>
  );
}

export function TacticalCockpit({ definitions, snapshot, onInspect }: Props) {
  const sensors = definitionById(definitions, "tactical-sensors");
  const scope = definitionById(definitions, "tactical-scope");
  const target = definitionById(definitions, "tactical-target");
  const locks = definitionById(definitions, "tactical-locks");
  const threats = definitionById(definitions, "tactical-threats");
  const future = definitionById(definitions, "tactical-future");
  return (
    <section className="tactical-cockpit">
      <SensorStrip snapshot={snapshot} definition={sensors} onInspect={onInspect} />
      <TargetPanel snapshot={snapshot} definition={target} onInspect={onInspect} />
      <RadarScope snapshot={snapshot} definition={scope} onInspect={onInspect} />
      <LockPanel snapshot={snapshot} definition={locks} onInspect={onInspect} />
      <ThreatPanel snapshot={snapshot} definition={threats} onInspect={onInspect} />
      <FuturePanel definition={future} />
    </section>
  );
}
