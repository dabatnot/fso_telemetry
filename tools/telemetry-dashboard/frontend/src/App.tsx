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
      setToast(`Erreur — ${error instanceof Error ? error.message : String(error)}`);
    }
    window.setTimeout(() => setToast(null), 5000);
  };

  return (
    <div className="dashboard-shell">
      <header className="command-bar">
        <div className="brand">
          <div className="brand-mark"><i /><i /><i /></div>
          <div><strong>FSO // SIMPIT LAB</strong><span>VALIDATION TÉLÉMÉTRIE</span></div>
        </div>
        <div className="mission-strip">
          <div><span>MISSION</span><strong>{formatValue(snapshot?.mission.phase ?? "—")}</strong></div>
          <div><span>SESSION</span><strong>{snapshot?.connection.sessionId ?? "0"}</strong></div>
          <div><span>JOUEUR</span><strong>{snapshot?.playerEntityId ?? "—"}</strong></div>
          <div><span>MODE</span><strong>{snapshot?.mode?.toUpperCase() ?? "LIVE"}</strong></div>
        </div>
        <div className="connection-block">
          <div className={`connection-light status-${connection.toLowerCase()}`} />
          <div><span>{socketOnline ? connection : "BRIDGE HORS LIGNE"}</span><small>{snapshot?.connection.host ?? "127.0.0.1"}:{snapshot?.connection.port ?? 42042}</small></div>
        </div>
      </header>

      <nav className="tab-rail" aria-label="Sections du cockpit">
        {TABS.map(([id, label], index) => (
          <button
            key={id}
            className={tab === id ? "active" : ""}
            onClick={() => setTab(id)}
            aria-current={tab === id ? "page" : undefined}
          >
            <i>{String(index + 1).padStart(2, "0")}</i><span>{label}</span>
          </button>
        ))}
      </nav>

      <main>
        <section className="section-heading">
          <div><span>MODULE ACTIF</span><h1>{TABS.find(([id]) => id === tab)?.[1]}</h1></div>
          <div className="section-stats">
            <span><i className="live-dot" />{instruments.filter((item) => item.availability !== "nd").length} SOURCES</span>
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
            <button className="detail-close" onClick={() => setSelected(null)} aria-label="Fermer">×</button>
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
            ) : (() => {
          const definition = selected.definition;
          const resolved = resolveInstrument(definition, snapshot);
          return (
            <>
              <span className="eyebrow">INSPECTION INSTRUMENT</span>
              <h2>{definition.label}</h2>
              <div className={`detail-state state-${resolved.state}`}>{resolved.state.toUpperCase()}</div>
              <dl>
                <dt>Valeur</dt><dd>{resolved.value === null ? "—" : formatValue(resolved.value)}</dd>
                <dt>Valeur brute</dt><dd>{resolved.value === null ? "—" : <code>{JSON.stringify(resolved.value)}</code>}</dd>
                <dt>Source</dt><dd><code>{definition.source}</code></dd>
                <dt>Formule</dt><dd>{definition.formula ?? "valeur directe"}</dd>
                <dt>Entité</dt><dd>{resolved.rawRecord?.entity_id === undefined ? "—" : String(resolved.rawRecord.entity_id)}</dd>
                <dt>Présence</dt><dd>{resolved.rawRecord?.presence === undefined ? "—" : String(resolved.rawRecord.presence)}</dd>
                <dt>Champs couverts</dt><dd>{definition.consumedFields?.join(", ") ?? "source directe"}</dd>
                <dt>Échantillon</dt><dd>{resolved.sampleTimeUs ?? "—"}</dd>
                <dt>Âge estimé</dt><dd>{resolved.ageUs === undefined ? "—" : `${resolved.ageUs} µs`}</dd>
                <dt>Cadence observée</dt><dd>{resolved.observedHz === undefined ? "—" : `${resolved.observedHz.toFixed(2)} Hz`}</dd>
                <dt>Raison</dt><dd>{resolved.reason ?? "aucun écart"}</dd>
              </dl>
              {definition.detailFields?.length ? (
                <div className="detail-values">
                  <h3>Valeurs associées</h3>
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
          <span>MANIFESTE <strong>{snapshot?.transport.manifestId ?? 0}</strong></span>
          <span>RENDU <strong>{renderFps.toFixed(0)} FPS</strong></span>
        </div>
        <div className="dock-actions">
          {snapshot?.mode === "replay" ? (
            <>
              <button onClick={() => action("Replay", () => post("/api/replay/control", { playing: !snapshot.replay.playing }))}>
                {snapshot.replay.playing ? "PAUSE" : "LECTURE"}
              </button>
              {[0.5, 1, 2, 4].map((speed) => (
                <button className={snapshot.replay.speed === speed ? "active" : ""} key={speed}
                  onClick={() => action("Vitesse", () => post("/api/replay/control", { speed }))}>{speed}×</button>
              ))}
              <input
                className="replay-slider"
                type="range"
                min="0"
                max={snapshot.replay.packetCount}
                value={snapshot.replay.position}
                aria-label="Position du replay"
                onChange={(event) =>
                  action("Position", () =>
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
                snapshot?.capture.active ? "Capture arrêtée" : "Capture démarrée",
                () => post(snapshot?.capture.active ? "/api/capture/stop" : "/api/capture/start")
              )}
            >
              {snapshot?.capture.active ? "■ ARRÊTER CAPTURE" : "● CAPTURER"}
            </button>
          )}
          <button onClick={() => action("Exports créés", () => post("/api/export"))}>EXPORTER</button>
        </div>
      </footer>
      {toast && <div className="toast">{toast}</div>}
    </div>
  );
}
