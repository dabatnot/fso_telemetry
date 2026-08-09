import { useEffect, useRef, useState } from "react";
import { useI18n } from "./i18n";
import type { DashboardSnapshot } from "./types";

type MenuName = "session" | "replay" | "data" | "display";

interface ControlDockProps {
  snapshot: DashboardSnapshot | null;
  renderFps: number;
  displayFrozen: boolean;
  onToggleFreeze: () => void;
  onOpenDiagnostics: () => void;
  onResync: () => void;
  onReconnect: () => void;
  onToggleCapture: () => void;
  onExport: () => void;
  onReplayControl: (control: { playing?: boolean; speed?: number; position?: number }) => void;
}

function sessionValue(value: unknown): string {
  return value === null || value === undefined || value === "" ? "—" : String(value);
}

export function ControlDock({
  snapshot,
  renderFps,
  displayFrozen,
  onToggleFreeze,
  onOpenDiagnostics,
  onResync,
  onReconnect,
  onToggleCapture,
  onExport,
  onReplayControl
}: ControlDockProps) {
  const { t } = useI18n();
  const [openMenu, setOpenMenu] = useState<MenuName | null>(null);
  const [confirmReconnect, setConfirmReconnect] = useState(false);
  const [fullscreen, setFullscreen] = useState(Boolean(document.fullscreenElement));
  const dockRef = useRef<HTMLDivElement>(null);
  const menuRef = useRef<HTMLDivElement>(null);
  const replay = snapshot?.mode === "replay";
  const recoveryState = snapshot?.connection.recoveryState ?? "idle";
  const status = snapshot?.connection.status ?? "Disconnected";
  const sessionAvailable = snapshot?.connection.sessionId !== undefined
    && snapshot.connection.sessionId !== "0"
    && recoveryState !== "reconnecting";

  useEffect(() => {
    const closeOnOutside = (event: PointerEvent) => {
      if (dockRef.current && !dockRef.current.contains(event.target as Node)) {
        setOpenMenu(null);
        setConfirmReconnect(false);
      }
    };
    const closeOnEscape = (event: KeyboardEvent) => {
      if (event.key === "Escape") {
        setOpenMenu(null);
        setConfirmReconnect(false);
      }
    };
    document.addEventListener("pointerdown", closeOnOutside);
    document.addEventListener("keydown", closeOnEscape);
    return () => {
      document.removeEventListener("pointerdown", closeOnOutside);
      document.removeEventListener("keydown", closeOnEscape);
    };
  }, []);

  useEffect(() => {
    if (openMenu !== null) {
      window.setTimeout(() => {
        menuRef.current?.querySelector<HTMLElement>("button:not(:disabled), input:not(:disabled)")?.focus();
      }, 0);
    }
  }, [openMenu]);

  useEffect(() => {
    const update = () => setFullscreen(Boolean(document.fullscreenElement));
    document.addEventListener("fullscreenchange", update);
    return () => document.removeEventListener("fullscreenchange", update);
  }, []);

  const toggleMenu = (menu: MenuName) => {
    setConfirmReconnect(false);
    setOpenMenu((current) => current === menu ? null : menu);
  };

  const runAndClose = (callback: () => void) => {
    setOpenMenu(null);
    setConfirmReconnect(false);
    callback();
  };

  const reconnect = () => {
    if (status === "Live" && recoveryState === "idle" && !confirmReconnect) {
      setConfirmReconnect(true);
      return;
    }
    runAndClose(onReconnect);
  };

  const toggleFullscreen = () => {
    runAndClose(() => {
      if (document.fullscreenElement) {
        void document.exitFullscreen();
      } else {
        void document.documentElement.requestFullscreen();
      }
    });
  };

  const sessionMenu = !replay && openMenu === "session" ? (
    <div className="dock-menu session-menu" id="dock-session-menu" role="menu" ref={menuRef}>
      <div className="dock-menu-title">{t("ÉTAT DE SESSION")}</div>
      <dl className="dock-session-details">
        <dt>{t("ÉTAT")}</dt><dd>{t(recoveryState === "idle" ? status : recoveryState.toUpperCase())}</dd>
        <dt>{t("SESSION")}</dt><dd>{sessionValue(snapshot?.connection.sessionId)}</dd>
        <dt>{t("BASELINE")}</dt><dd>{snapshot?.transport.baseline ?? 0}</dd>
        <dt>{t("DERNIER LIVE")}</dt><dd>{sessionValue(snapshot?.connection.lastLiveObservedUtc)}</dd>
        <dt>{t("RAISON STALE")}</dt><dd>{sessionValue(snapshot?.connection.staleReason)}</dd>
      </dl>
      <button role="menuitem" disabled={!sessionAvailable} onClick={() => runAndClose(onResync)}>
        {t("RESYNCHRONISER")}
      </button>
      {confirmReconnect ? (
        <div className="dock-confirm" role="alert">
          <span>{t("La session est Live. Confirmer la reconnexion ?")}</span>
          <div>
            <button role="menuitem" className="danger" onClick={() => runAndClose(onReconnect)}>{t("CONFIRMER")}</button>
            <button role="menuitem" onClick={() => setConfirmReconnect(false)}>{t("ANNULER")}</button>
          </div>
        </div>
      ) : (
        <button role="menuitem" className={status === "Live" ? "" : "danger"} onClick={reconnect}>
          {t("RECONNECTER")}
        </button>
      )}
      <button role="menuitem" onClick={() => runAndClose(onOpenDiagnostics)}>{t("OUVRIR LE DIAGNOSTIC")}</button>
    </div>
  ) : null;

  const replayMenu = replay && openMenu === "replay" ? (
    <div className="dock-menu replay-menu" id="dock-replay-menu" role="menu" ref={menuRef}>
      <div className="dock-menu-title">{t("CONTRÔLES DU REPLAY")}</div>
      <button role="menuitem" onClick={() => runAndClose(() => onReplayControl({ playing: !snapshot?.replay.playing }))}>
        {snapshot?.replay.playing ? t("PAUSE") : t("LECTURE")}
      </button>
      <div className="dock-speed-row" aria-label={t("Vitesse")}>
        {[0.5, 1, 2, 4].map((speed) => (
          <button
            role="menuitem"
            className={snapshot?.replay.speed === speed ? "active" : ""}
            key={speed}
            onClick={() => runAndClose(() => onReplayControl({ speed }))}
          >{speed}×</button>
        ))}
      </div>
      <label className="dock-replay-position">
        <span>{t("Position du replay")}</span>
        <input
          type="range"
          min="0"
          max={snapshot?.replay.packetCount ?? 0}
          value={snapshot?.replay.position ?? 0}
          aria-label={t("Position du replay")}
          onChange={(event) => onReplayControl({ position: Number(event.currentTarget.value) })}
        />
        <strong>{snapshot?.replay.position ?? 0} / {snapshot?.replay.packetCount ?? 0}</strong>
      </label>
    </div>
  ) : null;

  const dataMenu = openMenu === "data" ? (
    <div className="dock-menu" id="dock-data-menu" role="menu" ref={menuRef}>
      <div className="dock-menu-title">{t("GESTION DES DONNÉES")}</div>
      {!replay && (
        <button role="menuitem" className={snapshot?.capture.active ? "danger" : ""} onClick={() => runAndClose(onToggleCapture)}>
          {t(snapshot?.capture.active ? "■ ARRÊTER CAPTURE" : "● CAPTURER")}
        </button>
      )}
      <button role="menuitem" onClick={() => runAndClose(onExport)}>{t("EXPORTER")}</button>
    </div>
  ) : null;

  const displayMenu = openMenu === "display" ? (
    <div className="dock-menu" id="dock-display-menu" role="menu" ref={menuRef}>
      <div className="dock-menu-title">{t("OPTIONS D’AFFICHAGE")}</div>
      <button role="menuitem" disabled={replay} onClick={() => runAndClose(onToggleFreeze)}>
        {t(displayFrozen ? "REPRENDRE L’AFFICHAGE" : "FIGER L’AFFICHAGE")}
      </button>
      <button role="menuitem" onClick={toggleFullscreen}>
        {t(fullscreen ? "QUITTER LE PLEIN ÉCRAN" : "PLEIN ÉCRAN")}
      </button>
    </div>
  ) : null;

  return (
    <footer className="control-dock">
      <div className="dock-status">
        <span>BASELINE <strong>{snapshot?.transport.baseline ?? 0}</strong></span>
        <span>DELTA <strong>{snapshot?.transport.deltaSequence ?? 0}</strong></span>
        <span>{t("MANIFESTE")} <strong>{snapshot?.transport.manifestId ?? 0}</strong></span>
        <span>{t("RENDU")} <strong>{renderFps.toFixed(0)} FPS</strong></span>
      </div>
      <div className="dock-actions" ref={dockRef}>
        <div className="dock-menu-anchor">
          <button
            className={openMenu === (replay ? "replay" : "session") ? "active" : ""}
            aria-haspopup="menu"
            aria-expanded={openMenu === (replay ? "replay" : "session")}
            aria-controls={replay ? "dock-replay-menu" : "dock-session-menu"}
            onClick={() => toggleMenu(replay ? "replay" : "session")}
          >
            {!replay && <i className={`dock-connection-dot status-${status.toLowerCase()} recovery-${recoveryState}`} />}
            {t(replay ? "REPLAY" : "SESSION")}
          </button>
          {sessionMenu}
          {replayMenu}
        </div>
        <div className="dock-menu-anchor">
          <button
            className={openMenu === "data" ? "active" : ""}
            aria-haspopup="menu"
            aria-expanded={openMenu === "data"}
            aria-controls="dock-data-menu"
            onClick={() => toggleMenu("data")}
          >
            {t("DONNÉES")}{snapshot?.capture.active ? <b className="dock-rec">REC</b> : null}
          </button>
          {dataMenu}
        </div>
        <div className="dock-menu-anchor">
          <button
            className={openMenu === "display" ? "active" : ""}
            aria-haspopup="menu"
            aria-expanded={openMenu === "display"}
            aria-controls="dock-display-menu"
            onClick={() => toggleMenu("display")}
          >
            {t("AFFICHAGE")}{displayFrozen ? <b className="dock-frozen">{t("FIGÉ")}</b> : null}
          </button>
          {displayMenu}
        </div>
      </div>
    </footer>
  );
}
