/**
 * API client.
 *
 * The token lives in localStorage so a reload does not sign the operator out
 * mid-shift. That is a deliberate trade: it survives XSS worse than an
 * httpOnly cookie would, and the right long-term answer is a cookie plus CSRF
 * protection. For a single-origin operator tool behind a VPN this is the
 * pragmatic choice, and it is isolated here so changing it touches one file.
 */

const TOKEN_KEY = 'evrest.token';

export function getToken(): string | null {
  return localStorage.getItem(TOKEN_KEY);
}
export function setToken(token: string | null): void {
  if (token) localStorage.setItem(TOKEN_KEY, token);
  else localStorage.removeItem(TOKEN_KEY);
}

export class ApiError extends Error {
  constructor(readonly status: number, message: string, readonly details?: string[]) {
    super(message);
  }
}

async function request<T>(path: string, init: RequestInit = {}): Promise<T> {
  const token = getToken();
  const res = await fetch(path, {
    ...init,
    headers: {
      'Content-Type': 'application/json',
      ...(token ? { Authorization: `Bearer ${token}` } : {}),
      ...init.headers,
    },
  });

  if (res.status === 401) {
    // The token has expired or been revoked. Clearing it here means every
    // caller does not have to handle re-authentication.
    setToken(null);
    window.location.hash = '#/login';
    throw new ApiError(401, 'Session expired, please sign in again');
  }

  if (!res.ok) {
    let message = res.statusText;
    let details: string[] | undefined;
    try {
      const body = await res.json();
      message = body.error ?? message;
      details = body.details;
    } catch { /* non-JSON error body */ }
    throw new ApiError(res.status, message, details);
  }

  if (res.status === 204) return undefined as T;
  return res.json() as Promise<T>;
}

export const api = {
  get:  <T>(path: string) => request<T>(path),
  post: <T>(path: string, body?: unknown) =>
    request<T>(path, { method: 'POST', body: JSON.stringify(body ?? {}) }),
  del:  <T>(path: string) => request<T>(path, { method: 'DELETE' }),
};

/* ---- Shapes returned by the API ---- */

export type ChargerStatus =
  | 'Available' | 'Preparing' | 'Charging' | 'SuspendedEV' | 'SuspendedEVSE'
  | 'Finishing' | 'Reserved' | 'Unavailable' | 'Faulted';

export interface Charger {
  id: string;
  name: string;
  vendor: string | null;
  model: string | null;
  firmware: string | null;
  status: ChargerStatus;
  error_code: string | null;
  vendor_error: string | null;
  online: boolean;
  last_seen: number | null;
  max_current_a: number;
  active_transaction_id?: number | null;
  active_id_tag?: string | null;
  active_started_at?: number | null;
}

export interface ChargerDetail extends Charger {
  activeTransaction: Transaction | null;
  latestSample: MeterSample | null;
}

export interface Transaction {
  id: number;
  charger_id: string;
  charger_name?: string;
  id_tag: string;
  meter_start_wh: number;
  meter_stop_wh: number | null;
  started_at: number;
  stopped_at: number | null;
  stop_reason: string | null;
  energy_wh: number | null;
}

export interface MeterSample {
  ts: number;
  energy_wh: number | null;
  power_w: number | null;
  current_a: number | null;
  voltage_v: number | null;
  offered_a: number | null;
}

export interface Tag {
  id_tag: string;
  parent_id_tag: string | null;
  label: string | null;
  status: 'Accepted' | 'Blocked' | 'Expired' | 'Invalid';
  expires_at: number | null;
  user_id: string | null;
}

export interface Summary {
  chargers: { total: number; online: number; charging: number; faulted: number };
  last30Days: {
    sessions: number;
    energyWh: number;
    daily: Array<{ day: number; sessions: number; energy_wh: number }>;
  };
}

export interface LogEntry {
  ts: number;
  direction: 'in' | 'out';
  action: string | null;
  message_id: string;
  payload: string;
}

export interface User { id: string; email: string; name: string | null; role: 'admin' | 'operator' | 'driver' }
