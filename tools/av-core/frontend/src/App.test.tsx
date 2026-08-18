// @vitest-environment jsdom
import "@testing-library/jest-dom/vitest";
import { act, cleanup, fireEvent, render, screen, waitFor } from "@testing-library/react";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import App from "./App";
import type { AvCoreConfig, AvCoreStatus } from "./types";

const config: AvCoreConfig = {
  schemaVersion: 1,
  telemetry: { host: "127.0.0.1", port: 42042, staleAfterMs: 1000 },
  can: { nodeTimeoutMs: 3000 },
  web: { port: 8080 },
  modules: { warnCtrl: { installed: true }, threatProc: { installed: true }, sensProc: { installed: false }, instProc: { installed: false } },
  alerts: {
    engine: { activateBelowPercent: 50, clearAbovePercent: 55 }, shield: { activateBelowPercent: 30, clearAbovePercent: 35 }, hull: { activateBelowPercent: 40, clearAbovePercent: 45 }, weaponEnergy: { activateBelowPercent: 20, clearAbovePercent: 30 }, afterburnerFuel: { activateBelowPercent: 20, clearAbovePercent: 25 }, ammo: { activateBelowPercent: 20, clearAbovePercent: 25 }, countermeasures: { activateAtOrBelowCount: 3, activateAtOrBelowPercent: 20 }, subsystem: { activateBelowPercent: 40, clearAbovePercent: 45 }
  },
  lighting: { warningColor: { r: 255, g: 96, b: 0 }, cautionColor: { r: 255, g: 96, b: 0 }, maxBrightnessPercent: 30, slowFlashHz: 1, fastFlashHz: 4 }
};

const status: AvCoreStatus = {
  schema: "AvCoreStatusV1", version: "0.1.0", uptimeMs: 10,
  configuration: { state: "OK", message: null }, restartRequired: false,
  telemetry: { state: "UNAVAILABLE", host: "127.0.0.1", port: 42042 },
  can: { state: "UNAVAILABLE", interface: "can0", bitrate: 1000000 },
  modules: ["WARN_CTRL", "THREAT_PROC", "SENS_PROC", "INST_PROC"].map((role) => ({ role, installed: role === "WARN_CTRL" || role === "THREAT_PROC", state: "UNAVAILABLE", protocolId: null, uid: null, firmwareVersion: null, lastHeartbeatMs: null })) as AvCoreStatus["modules"]
};

class FakeEventSource {
  static instance: FakeEventSource;
  listeners = new Map<string, (event: MessageEvent) => void>();
  onerror: (() => void) | null = null;
  constructor(public url: string) { FakeEventSource.instance = this; }
  addEventListener(name: string, callback: EventListenerOrEventListenerObject) { this.listeners.set(name, callback as (event: MessageEvent) => void); }
  close = vi.fn();
  emit(name: string, payload: unknown) { this.listeners.get(name)?.(new MessageEvent(name, { data: JSON.stringify(payload) })); }
}

function jsonResponse(payload: unknown, statusCode = 200): Response {
  return { ok: statusCode >= 200 && statusCode < 300, status: statusCode, statusText: "", json: async () => payload, text: async () => JSON.stringify(payload) } as Response;
}

describe("AV CORE application", () => {
  beforeEach(() => {
    window.location.hash = "";
    localStorage.setItem("av-core-language", "fr");
    vi.stubGlobal("EventSource", FakeEventSource);
    vi.stubGlobal("fetch", vi.fn(async (path: string, init?: RequestInit) => {
      if (path === "/api/config" && init?.method === "PUT") return jsonResponse({ config: JSON.parse(init.body as string), restartRequired: false });
      if (path === "/api/config") return jsonResponse(config);
      return jsonResponse(status);
    }));
  });

  afterEach(() => {
    cleanup();
    vi.unstubAllGlobals();
  });

  it("navigates across four pages without a night profile", async () => {
    render(<App />);
    await screen.findByRole("heading", { name: "Modules" });
    fireEvent.click(screen.getByRole("button", { name: /Alertes/ }));
    expect(await screen.findByRole("heading", { name: "Alertes" })).toBeInTheDocument();
    fireEvent.click(screen.getByRole("button", { name: /Éclairage/ }));
    expect(await screen.findByRole("heading", { name: "Éclairage" })).toBeInTheDocument();
    expect(screen.queryByText(/nocturne/i)).not.toBeInTheDocument();
    expect(screen.getByRole("button", { name: "Tester tous les voyants" })).toBeDisabled();
    fireEvent.click(screen.getByRole("button", { name: /Système/ }));
    expect(await screen.findByRole("heading", { name: "Système" })).toBeInTheDocument();
  });

  it("saves the complete configuration after changing an installed role", async () => {
    render(<App />);
    const checkbox = (await screen.findAllByRole("checkbox", { name: /Installé dans le cockpit/i }))[0];
    fireEvent.click(checkbox);
    fireEvent.click(screen.getByRole("button", { name: "Enregistrer et appliquer" }));
    await waitFor(() => expect(fetch).toHaveBeenCalledWith("/api/config", expect.objectContaining({ method: "PUT" })));
    const call = vi.mocked(fetch).mock.calls.find((entry) => entry[1]?.method === "PUT")!;
    expect(JSON.parse(call[1]!.body as string).modules.warnCtrl.installed).toBe(false);
  });

  it("rejects invalid hysteresis before calling the API", async () => {
    render(<App />);
    await screen.findByRole("heading", { name: "Modules" });
    fireEvent.click(screen.getByRole("button", { name: /Alertes/ }));
    const clear = await screen.findByRole("spinbutton", { name: "Extinction ENG" });
    fireEvent.change(clear, { target: { value: "50" } });
    fireEvent.click(screen.getByRole("button", { name: "Enregistrer et appliquer" }));
    expect(await screen.findByText(/extinction engine/i)).toBeInTheDocument();
    expect(vi.mocked(fetch).mock.calls.filter((entry) => entry[1]?.method === "PUT")).toHaveLength(0);
  });

  it("updates the permanent status strip from SSE", async () => {
    render(<App />);
    await screen.findByRole("heading", { name: "Modules" });
    const next = { ...status, configuration: { state: "ERROR" as const, message: "Configuration invalide" } };
    act(() => FakeEventSource.instance.emit("status", next));
    expect(await screen.findByText("CONFIG ERROR")).toBeInTheDocument();
    expect(screen.getByText("Configuration invalide")).toBeInTheDocument();
  });

  it("shows when saving requires a service restart", async () => {
    vi.mocked(fetch).mockImplementation(async (path: string | URL | Request, init?: RequestInit) => {
      if (path === "/api/config" && init?.method === "PUT") {
        return jsonResponse({ config: JSON.parse(init.body as string), restartRequired: true });
      }
      if (path === "/api/config") return jsonResponse(config);
      return jsonResponse(status);
    });
    render(<App />);
    await screen.findByRole("heading", { name: "Modules" });
    fireEvent.click(screen.getByRole("button", { name: "Enregistrer et appliquer" }));
    expect(await screen.findByText(/redémarrage requis pour le port HTTP/i)).toBeInTheDocument();
  });

  it("offers all supported languages and remembers the selection", async () => {
    const first = render(<App />);
    await screen.findByRole("heading", { name: "Modules" });
    const selector = screen.getByRole("combobox", { name: "Langue" });
    expect(selector.querySelectorAll("option")).toHaveLength(6);
    expect(Array.from(selector.querySelectorAll("option"), (option) => option.textContent)).toEqual([
      "Deutsch", "English", "Español", "Français", "Italiano", "Português"
    ]);
    for (const [code, saveLabel] of [
      ["es", "Guardar y aplicar"],
      ["pt", "Guardar e aplicar"],
      ["it", "Salva e applica"],
      ["de", "Speichern und anwenden"],
      ["en", "Save and apply"]
    ]) {
      fireEvent.change(selector, { target: { value: code } });
      expect(await screen.findByRole("button", { name: saveLabel })).toBeInTheDocument();
    }
    expect(await screen.findByRole("button", { name: "Save and apply" })).toBeInTheDocument();
    expect(screen.getByRole("combobox", { name: "Language" })).toHaveValue("en");
    expect(screen.getAllByText("UNAVAILABLE").length).toBeGreaterThan(0);
    expect(localStorage.getItem("av-core-language")).toBe("en");

    first.unmount();
    render(<App />);
    expect(await screen.findByRole("button", { name: "Save and apply" })).toBeInTheDocument();
  });
});
