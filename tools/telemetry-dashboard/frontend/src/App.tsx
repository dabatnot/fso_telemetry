import { useEffect, useMemo, useRef, useState } from "react";
import catalogData from "./instrument-catalog.json";
import { resolveDetailFields, resolveInstrument, formatValue } from "./data";
import { InstrumentCard } from "./InstrumentCard";
import { PilotageCockpit } from "./PilotageCockpit";
import { EnergyCockpit } from "./EnergyCockpit";
import { IntegrityCockpit } from "./IntegrityCockpit";
import { IntegrityInspection } from "./IntegrityInspection";
import { WeaponsCockpit } from "./WeaponsCockpit";
import { WeaponInspection } from "./WeaponInspection";
import { SupportCockpit } from "./SupportCockpit";
import { SupportInspection } from "./SupportInspection";
import { TacticalCockpit } from "./TacticalCockpit";
import { TacticalInspection } from "./TacticalInspection";
import { LanguageSelector, useI18n } from "./i18n";
import type { DashboardSnapshot, InspectionTarget, InstrumentDefinition } from "./types";
import "./styles.css";

const catalog = catalogData as InstrumentDefinition[];

const TABS = [
  ["flight", "Pilotage"],
  ["energy", "Propulsion & énergie"],
  ["integrity", "Intégrité"],
  ["weapons", "Armement"],
  ["support", "Support · Docking · Cargo"],
  ["mission", "Mission & entités"],
  ["config", "Configuration"],
  ["tactical", "Tactique"],
  ["comms", "Communications & effets"],
  ["diagnostic", "Diagnostic télémétrie"]
] as const;

async function post(path: string, body?: unknown): Promise<unknown> {
  const response = await fetch(path, {
    method: "POST",
    headers: body ? { "Content-Type": "application/json" } : undefined,
    body: body ? JSON.stringify(body) : undefined
  });
  if (!response.ok) {
    const detail = await response.text();
    throw new Error(detail || response.statusText);
  }
  return response.json();
}

function useDashboardSocket() {
  const [snapshot, setSnapshot] = useState<DashboardSnapshot | null>(null);
  const [socketOnline, setSocketOnline] = useState(false);
  useEffect(() => {
    let stopped = false;
    let socket: WebSocket | null = null;
    let retry: number | null = null;
    const connect = () => {
      const protocol = location.protocol === "https:" ? "wss:" : "ws:";
      socket = new WebSocket(`${protocol}//${location.host}/api/ws`);
      socket.onopen = () => setSocketOnline(true);
      socket.onmessage = (event) => {
        const next = JSON.parse(event.data) as DashboardSnapshot;
        if (next.schema === "DashboardSnapshotV1") setSnapshot(next);
      };
      socket.onerror = () => socket?.close();
      socket.onclose = () => {
        setSocketOnline(false);
        if (!stopped) retry = window.setTimeout(connect, 1000);
      };
    };
    connect();
    return () => {
      stopped = true;
      if (retry !== null) window.clearTimeout(retry);
      socket?.close();
    };
  }, []);
  return { snapshot, socketOnline };
}

function useRenderFps() {
  const [fps, setFps] = useState(0);
  const frames = useRef(0);
  const previous = useRef(performance.now());
  useEffect(() => {
    let frame = 0;
    const tick = (now: number) => {
      frames.current += 1;
      if (now - previous.current >= 1000) {
        setFps((frames.current * 1000) / (now - previous.current));
        frames.current = 0;
        previous.current = now;
      }
      frame = requestAnimationFrame(tick);
    };
    frame = requestAnimationFrame(tick);
    return () => cancelAnimationFrame(frame);
  }, []);
  return fps;
}

export default function App() {
  const { t } = useI18n();
  const { snapshot, socketOnline } = useDashboardSocket();
  const renderFps = useRenderFps();
  const [tab, setTab] = useState("flight");
  const [selected, setSelected] = useState<InspectionTarget | null>(null);
  const [toast, setToast] = useState<string | null>(null);
  const instruments = useMemo(() => catalog.filter((item) => item.tab === tab), [tab]);
  const connection = snapshot?.connection.status ?? "Synchronizing";

  const action = async (label: string, callback: () => Promise<unknown>) => {
    try {
      const result = await callback();
      setToast(`${label} — ${JSON.stringify(result)}`);
    } catch (error) {
      setToast(`${t("Erreur")} — ${error instanceof Error ? error.message : String(error)}`);
    }
    window.setTimeout(() => setToast(null), 5000);
  };

  return (
    <div className="dashboard-shell">
      <header className="command-bar">
        <div className="brand">
          <div className="brand-mark"><i /><i /><i /></div>
          <div><strong>FSO // SIMPIT LAB</strong><span>{t("VALIDATION TÉLÉMÉTRIE")}</span></div>
        </div>
        <div className="mission-strip">
          <div><span>{t("MISSION")}</span><strong>{formatValue(snapshot?.mission.phase ?? "—")}</strong></div>
          <div><span>{t("SESSION")}</span><strong>{snapshot?.connection.sessionId ?? "0"}</strong></div>
          <div><span>{t("JOUEUR")}</span><strong>{snapshot?.playerEntityId ?? "—"}</strong></div>
          <div><span>{t("MODE")}</span><strong>{snapshot?.mode?.toUpperCase() ?? "LIVE"}</strong></div>
        </div>
        <div className="topbar-actions">
          <LanguageSelector />
          <div className="connection-block">
            <div className={`connection-light status-${connection.toLowerCase()}`} />
            <div><span>{t(socketOnline ? connection : "BRIDGE HORS LIGNE")}</span><small>{snapshot?.connection.host ?? "127.0.0.1"}:{snapshot?.connection.port ?? 42042}</small></div>
          </div>
        </div>
      </header>

      <nav className="tab-rail" aria-label={t("Sections du cockpit")} data-heading={t("SYSTÈMES")}>
        {TABS.map(([id, label], index) => (
          <button
            key={id}
            className={tab === id ? "active" : ""}
            onClick={() => setTab(id)}
            aria-current={tab === id ? "page" : undefined}
          >
            <i>{String(index + 1).padStart(2, "0")}</i><span>{t(label)}</span>
          </button>
        ))}
      </nav>

      <main>
        <section className="section-heading">
          <div><span>{t("MODULE ACTIF")}</span><h1>{t(TABS.find(([id]) => id === tab)?.[1] ?? "")}</h1></div>
          <div className="section-stats">
            <span><i className="live-dot" />{instruments.filter((item) => item.availability !== "nd").length} {t("SOURCES")}</span>
            <span className="nd-count">{instruments.filter((item) => item.availability === "nd").length} ND</span>
          </div>
        </section>
        {tab === "flight" ? (
          <PilotageCockpit
            definitions={instruments}
            snapshot={snapshot}
            onInspect={(definition) => setSelected({ definition })}
          />
        ) : tab === "energy" ? (
          <EnergyCockpit
            definitions={instruments}
            snapshot={snapshot}
            onInspect={(definition) => setSelected({ definition })}
          />
        ) : tab === "integrity" ? (
          <IntegrityCockpit definitions={instruments} snapshot={snapshot} onInspect={setSelected} />
        ) : tab === "weapons" ? (
          <WeaponsCockpit definitions={instruments} snapshot={snapshot} onInspect={setSelected} />
        ) : tab === "support" ? (
          <SupportCockpit definitions={instruments} snapshot={snapshot} onInspect={setSelected} />
        ) : tab === "tactical" ? (
          <TacticalCockpit definitions={instruments} snapshot={snapshot} onInspect={setSelected} />
        ) : (
          <section className={`instrument-grid tab-${tab}`}>
            {instruments.map((definition) => {
              const resolved = resolveInstrument(definition, snapshot);
              return (
                <InstrumentCard
                  key={definition.id}
                  definition={definition}
                  resolved={resolved}
                  snapshot={snapshot}
                  onInspect={() => setSelected({ definition })}
                />
              );
            })}
          </section>
        )}
      </main>

      <aside className={`detail-panel ${selected ? "open" : ""}`} aria-hidden={!selected}>
        {selected && (
          <>
            <button className="detail-close" onClick={() => setSelected(null)} aria-label={t("Fermer")}>×</button>
            {selected.kind === "subsystem" || selected.kind === "subsystem-list" ? (
              <IntegrityInspection target={selected} snapshot={snapshot} onSelect={setSelected} />
            ) : selected.kind === "weapon-bank" ||
                selected.kind === "weapon-bank-list" ||
                selected.kind === "turret-list" ? (
              <WeaponInspection target={selected} snapshot={snapshot} onSelect={setSelected} />
            ) : selected.kind === "support-entity" ||
                selected.kind === "docking-relation" ||
                selected.kind === "docking-component" ||
                selected.kind === "cargo-target" ? (
              <SupportInspection target={selected} snapshot={snapshot} onSelect={setSelected} />
            ) : selected.kind === "target" ||
                selected.kind === "radar-contact" ||
                selected.kind === "lock-point" ||
                selected.kind === "lock-list" ||
                selected.kind === "incoming-missile" ||
                selected.kind === "missile-list" ? (
              <TacticalInspection target={selected} snapshot={snapshot} onSelect={setSelected} />
            ) : (() => {
          const definition = selected.definition;
          const resolved = resolveInstrument(definition, snapshot);
          return (
            <>
              <span className="eyebrow">{t("INSPECTION INSTRUMENT")}</span>
              <h2>{t(definition.label)}</h2>
              <div className={`detail-state state-${resolved.state}`}>{resolved.state.toUpperCase()}</div>
              <dl>
                <dt>{t("Valeur")}</dt><dd>{resolved.value === null ? "—" : formatValue(resolved.value)}</dd>
                <dt>{t("Valeur brute")}</dt><dd>{resolved.value === null ? "—" : <code>{JSON.stringify(resolved.value)}</code>}</dd>
                <dt>{t("Source")}</dt><dd><code>{definition.source}</code></dd>
                <dt>{t("Formule")}</dt><dd>{t(definition.formula ?? "valeur directe")}</dd>
                <dt>{t("Entité")}</dt><dd>{resolved.rawRecord?.entity_id === undefined ? "—" : String(resolved.rawRecord.entity_id)}</dd>
                <dt>{t("Présence")}</dt><dd>{resolved.rawRecord?.presence === undefined ? "—" : String(resolved.rawRecord.presence)}</dd>
                <dt>{t("Champs couverts")}</dt><dd>{definition.consumedFields?.join(", ") ?? t("source directe")}</dd>
                <dt>{t("Échantillon")}</dt><dd>{resolved.sampleTimeUs ?? "—"}</dd>
                <dt>{t("Âge estimé")}</dt><dd>{resolved.ageUs === undefined ? "—" : `${resolved.ageUs} µs`}</dd>
                <dt>{t("Cadence observée")}</dt><dd>{resolved.observedHz === undefined ? "—" : `${resolved.observedHz.toFixed(2)} Hz`}</dd>
                <dt>{t("Raison")}</dt><dd>{t(resolved.reason ?? "aucun écart")}</dd>
              </dl>
              {definition.detailFields?.length ? (
                <div className="detail-values">
                  <h3>{t("Valeurs associées")}</h3>
                  {resolveDetailFields(definition, snapshot).map(({ field, value }) => (
                    <div key={field}>
                      <span>{field}</span><code>{value === undefined ? "—" : JSON.stringify(value)}</code>
                    </div>
                  ))}
                </div>
              ) : null}
            </>
          );
            })()}
          </>
        )}
      </aside>

      <footer className="control-dock">
        <div className="dock-status">
          <span>BASELINE <strong>{snapshot?.transport.baseline ?? 0}</strong></span>
          <span>DELTA <strong>{snapshot?.transport.deltaSequence ?? 0}</strong></span>
          <span>{t("MANIFESTE")} <strong>{snapshot?.transport.manifestId ?? 0}</strong></span>
          <span>{t("RENDU")} <strong>{renderFps.toFixed(0)} FPS</strong></span>
        </div>
        <div className="dock-actions">
          {snapshot?.mode === "replay" ? (
            <>
              <button onClick={() => action("Replay", () => post("/api/replay/control", { playing: !snapshot.replay.playing }))}>
                {snapshot.replay.playing ? "PAUSE" : t("LECTURE")}
              </button>
              {[0.5, 1, 2, 4].map((speed) => (
                <button className={snapshot.replay.speed === speed ? "active" : ""} key={speed}
                  onClick={() => action(t("Vitesse"), () => post("/api/replay/control", { speed }))}>{speed}×</button>
              ))}
              <input
                className="replay-slider"
                type="range"
                min="0"
                max={snapshot.replay.packetCount}
                value={snapshot.replay.position}
                aria-label={t("Position du replay")}
                onChange={(event) =>
                  action(t("Position"), () =>
                    post("/api/replay/control", { position: Number(event.currentTarget.value) })
                  )
                }
              />
              <span>{snapshot.replay.position} / {snapshot.replay.packetCount}</span>
            </>
          ) : (
            <button
              className={snapshot?.capture.active ? "danger" : ""}
              onClick={() => action(
                t(snapshot?.capture.active ? "Capture arrêtée" : "Capture démarrée"),
                () => post(snapshot?.capture.active ? "/api/capture/stop" : "/api/capture/start")
              )}
            >
              {t(snapshot?.capture.active ? "■ ARRÊTER CAPTURE" : "● CAPTURER")}
            </button>
          )}
          <button onClick={() => action(t("Exports créés"), () => post("/api/export"))}>{t("EXPORTER")}</button>
        </div>
      </footer>
      {toast && <div className="toast">{toast}</div>}
    </div>
  );
}
