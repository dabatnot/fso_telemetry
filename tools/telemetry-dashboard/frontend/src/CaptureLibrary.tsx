import { useEffect, useMemo, useRef, useState } from "react";
import { useI18n } from "./i18n";
import type { CaptureLibraryItem, DashboardSnapshot } from "./types";

interface CaptureLibraryProps {
  open: boolean;
  snapshot: DashboardSnapshot | null;
  onClose: () => void;
  onChanged: () => void;
  onLoad: (id: string) => void;
  onStart: (name: string, expectedDurationUs?: number) => void;
  onStop: () => void;
  onReturnLive: () => void;
  notify: (message: string) => void;
}

function bytes(value?: number | null): string {
  if (value === undefined || value === null) return "—";
  const units = ["o", "Kio", "Mio", "Gio", "Tio"];
  let current = value;
  let unit = 0;
  while (current >= 1024 && unit < units.length - 1) { current /= 1024; unit += 1; }
  return `${current.toFixed(unit < 2 ? 0 : 2)} ${units[unit]}`;
}

function elapsed(valueUs: number): string {
  const seconds = Math.floor(valueUs / 1_000_000);
  return `${Math.floor(seconds / 3600).toString().padStart(2, "0")}:${Math.floor((seconds % 3600) / 60).toString().padStart(2, "0")}:${(seconds % 60).toString().padStart(2, "0")}`;
}

export function CaptureLibrary({ open, snapshot, onClose, onChanged, onLoad, onStart, onStop, onReturnLive, notify }: CaptureLibraryProps) {
  const { t } = useI18n();
  const [items, setItems] = useState<CaptureLibraryItem[]>([]);
  const [query, setQuery] = useState("");
  const [name, setName] = useState("");
  const [expectedMinutes, setExpectedMinutes] = useState("");
  const fileRef = useRef<HTMLInputElement>(null);

  const refresh = async () => {
    const response = await fetch(`/api/captures?q=${encodeURIComponent(query)}`);
    if (!response.ok) throw new Error(await response.text());
    setItems(await response.json() as CaptureLibraryItem[]);
  };

  useEffect(() => {
    if (!open) return;
    void refresh().catch((error) => notify(String(error)));
  }, [open, query]);

  useEffect(() => {
    if (!open) return;
    const close = (event: KeyboardEvent) => { if (event.key === "Escape") onClose(); };
    document.addEventListener("keydown", close);
    return () => document.removeEventListener("keydown", close);
  }, [open, onClose]);

  const active = snapshot?.capture.active ?? false;
  const capture = snapshot?.capture;
  const projections = useMemo(() => [
    [t("10 MIN"), capture?.projectedBytes10Minutes],
    [t("1 HEURE"), capture?.projectedBytes1Hour],
    [t("ESTIMATION FINALE"), capture?.projectedFinalBytes]
  ] as const, [capture, t]);

  if (!open) return null;
  return (
    <div className="capture-library-backdrop" onPointerDown={(event) => { if (event.target === event.currentTarget) onClose(); }}>
      <section className="capture-library" role="dialog" aria-modal="true" aria-labelledby="capture-library-title">
        <header><div><span>{t("ATELIER DE REPLAY")}</span><h2 id="capture-library-title">{t("Bibliothèque des captures")}</h2></div><button onClick={onClose} aria-label={t("Fermer")}>×</button></header>
        <div className="capture-recorder">
          <div>
            <strong>{t(active ? "ENREGISTREMENT EN COURS" : "NOUVELLE CAPTURE")}</strong>
            {active ? <span>{capture?.path}</span> : <span>{t("La capture reste ouverte pendant les reconnexions de FSO.")}</span>}
          </div>
          {active ? (
            <>
              <dl>
                <div><dt>{t("TAILLE")}</dt><dd>{bytes(capture?.bytes)}</dd></div>
                <div><dt>{t("DURÉE")}</dt><dd>{elapsed(capture?.durationUs ?? 0)}</dd></div>
                <div><dt>{t("DÉBIT 60 S")}</dt><dd>{bytes(capture?.rollingBytesPerSecond)}/s</dd></div>
                <div><dt>{t("ESPACE LIBRE")}</dt><dd>{bytes(capture?.freeBytes)}</dd></div>
                {projections.map(([label, value]) => <div key={label}><dt>{label}</dt><dd>{bytes(value)}</dd></div>)}
              </dl>
              {capture?.warning && <p className="capture-warning" role="alert">{t(capture.warning)}</p>}
              <button className="danger" onClick={onStop}>{t("ARRÊTER ET FINALISER")}</button>
            </>
          ) : snapshot?.mode === "live" ? (
            <div className="capture-start-form">
              <label>{t("Nom")}<input value={name} maxLength={128} onChange={(event) => setName(event.currentTarget.value)} /></label>
              <label>{t("Durée prévue (minutes)")}<input type="number" min="1" value={expectedMinutes} onChange={(event) => setExpectedMinutes(event.currentTarget.value)} /></label>
              <button onClick={() => onStart(name.trim(), expectedMinutes ? Number(expectedMinutes) * 60_000_000 : undefined)}>{t("DÉMARRER LA CAPTURE")}</button>
            </div>
          ) : (
            <div className="capture-replay-notice" role="status">
              <p>{t(snapshot?.replay.playing
                ? "Une capture est actuellement rejouée. Les données affichées ne proviennent pas directement de FSO."
                : "Le replay est en pause. Les données affichées ne proviennent pas directement de FSO.")}</p>
              <button onClick={onReturnLive}>{t("RETOUR AU DIRECT")}</button>
            </div>
          )}
        </div>
        <div className="capture-library-tools">
          <input type="search" value={query} onChange={(event) => setQuery(event.currentTarget.value)} placeholder={t("Rechercher une capture")} />
          <input ref={fileRef} hidden type="file" accept=".fstlcap,.jsonl" onChange={async (event) => {
            const file = event.currentTarget.files?.[0];
            if (!file) return;
            const response = await fetch(`/api/captures/import?filename=${encodeURIComponent(file.name)}`, { method: "POST", body: file });
            if (!response.ok) notify(await response.text()); else { notify(t("Capture importée")); await refresh(); onChanged(); }
            event.currentTarget.value = "";
          }} />
          <button onClick={() => fileRef.current?.click()}>{t("IMPORTER")}</button>
        </div>
        <div className="capture-table" role="table" aria-label={t("Captures enregistrées")}>
          {items.map((item) => (
            <article key={item.id} role="row">
              <div><strong>{item.name}</strong><span>{item.createdAtUtc ?? "—"}</span><small>{item.mission ?? t("Mission inconnue")}</small></div>
              <dl>
                <div><dt>{t("DURÉE")}</dt><dd>{elapsed(item.durationUs)}</dd></div>
                <div><dt>{t("TAILLE")}</dt><dd>{bytes(item.sizeBytes)}</dd></div>
                <div><dt>{t("PAQUETS")}</dt><dd>{item.packetCount.toLocaleString()}</dd></div>
                <div><dt>{t("SESSIONS")}</dt><dd>{item.sessionCount}</dd></div>
              </dl>
              <span className={item.complete ? "capture-complete" : "capture-incomplete"}>{t(item.complete ? "COMPLÈTE" : "INCOMPLÈTE")}</span>
              <div className="capture-row-actions">
                <button onClick={() => onLoad(item.id)}>{t("CHARGER")}</button>
                <a href={`/api/captures/${item.id}/download`} download>{t("TÉLÉCHARGER")}</a>
                <button onClick={async () => {
                  const next = window.prompt(t("Nouveau nom"), item.name)?.trim();
                  if (!next) return;
                  const response = await fetch(`/api/captures/${item.id}`, { method: "PATCH", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ name: next }) });
                  if (!response.ok) notify(await response.text()); else await refresh();
                }}>{t("RENOMMER")}</button>
                <button className="danger" onClick={async () => {
                  if (!window.confirm(t("Supprimer définitivement cette capture ?"))) return;
                  const response = await fetch(`/api/captures/${item.id}`, { method: "DELETE" });
                  if (!response.ok) notify(await response.text()); else { await refresh(); onChanged(); }
                }}>{t("SUPPRIMER")}</button>
              </div>
            </article>
          ))}
          {!items.length && <p>{t("Aucune capture")}</p>}
        </div>
      </section>
    </div>
  );
}
