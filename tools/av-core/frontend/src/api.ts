import type { AvCoreConfig, AvCoreStatus, ConfigUpdateResponse } from "./types";

async function request<T>(path: string, init?: RequestInit): Promise<T> {
  const response = await fetch(path, init);
  if (!response.ok) {
    let detail = response.statusText;
    try {
      const payload = await response.json() as { detail?: unknown };
      detail = typeof payload.detail === "string" ? payload.detail : JSON.stringify(payload.detail);
    } catch {
      detail = await response.text() || detail;
    }
    throw new Error(detail);
  }
  return response.json() as Promise<T>;
}

export function loadConfig(): Promise<AvCoreConfig> {
  return request<AvCoreConfig>("/api/config");
}

export function loadStatus(): Promise<AvCoreStatus> {
  return request<AvCoreStatus>("/api/status");
}

export function saveConfig(config: AvCoreConfig): Promise<ConfigUpdateResponse> {
  return request<ConfigUpdateResponse>("/api/config", {
    method: "PUT",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(config)
  });
}
