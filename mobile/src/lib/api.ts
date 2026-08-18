/**
 * API client for the CSMS.
 *
 * The base URL is configurable at runtime because a phone will be pointed at a
 * different host depending on whether it is on the site's Wi-Fi, on cellular
 * through a public endpoint, or on a bench emulator. Hard-coding it would make
 * the app unusable in two of those three cases.
 */
import AsyncStorage from '@react-native-async-storage/async-storage';
import Constants from 'expo-constants';

const TOKEN_KEY = 'evrest.token';
const BASE_KEY = 'evrest.apiBase';

let cachedToken: string | null = null;
let cachedBase: string | null = null;

const DEFAULT_BASE =
  (Constants.expoConfig?.extra as { apiBaseUrl?: string } | undefined)?.apiBaseUrl
  ?? 'http://10.0.2.2:9221';

export async function getApiBase(): Promise<string> {
  if (cachedBase !== null) return cachedBase;
  cachedBase = (await AsyncStorage.getItem(BASE_KEY)) ?? DEFAULT_BASE;
  return cachedBase;
}

export async function setApiBase(url: string): Promise<void> {
  // Trailing slashes produce "//api/..." paths that some proxies reject.
  cachedBase = url.replace(/\/+$/, '');
  await AsyncStorage.setItem(BASE_KEY, cachedBase);
}

export async function getToken(): Promise<string | null> {
  if (cachedToken !== null) return cachedToken;
  cachedToken = await AsyncStorage.getItem(TOKEN_KEY);
  return cachedToken;
}

export async function setToken(token: string | null): Promise<void> {
  cachedToken = token;
  if (token) await AsyncStorage.setItem(TOKEN_KEY, token);
  else await AsyncStorage.removeItem(TOKEN_KEY);
}

export class ApiError extends Error {
  constructor(readonly status: number, message: string) { super(message); }
}

/** Callback fired when the server rejects our token, so the UI can sign out. */
let onUnauthorized: (() => void) | null = null;
export function setUnauthorizedHandler(fn: (() => void) | null): void {
  onUnauthorized = fn;
}

async function request<T>(path: string, init: RequestInit = {}): Promise<T> {
  const base = await getApiBase();
  const token = await getToken();

  let res: Response;
  try {
    res = await fetch(`${base}${path}`, {
      ...init,
      headers: {
        'Content-Type': 'application/json',
        ...(token ? { Authorization: `Bearer ${token}` } : {}),
        ...init.headers,
      },
    });
  } catch {
    // fetch rejects on DNS failure, refused connection and TLS problems alike.
    // On a phone the cause is nearly always "wrong server address" or "no
    // signal", so say that rather than surfacing a bare TypeError.
    throw new ApiError(0, `Cannot reach ${base}. Check the server address and your connection.`);
  }

  if (res.status === 401) {
    await setToken(null);
    onUnauthorized?.();
    throw new ApiError(401, 'Session expired — please sign in again');
  }

  if (!res.ok) {
    let message = `Request failed (${res.status})`;
    try {
      const body = await res.json();
      if (body?.error) message = body.error;
    } catch { /* non-JSON body */ }
    throw new ApiError(res.status, message);
  }

  if (res.status === 204) return undefined as T;
  return (await res.json()) as T;
}

export const api = {
  get: <T>(path: string) => request<T>(path),
  post: <T>(path: string, body?: unknown) =>
    request<T>(path, { method: 'POST', body: JSON.stringify(body ?? {}) }),
};

/* ---- Shapes ---- */

export type ChargerStatus =
  | 'Available' | 'Preparing' | 'Charging' | 'SuspendedEV' | 'SuspendedEVSE'
  | 'Finishing' | 'Reserved' | 'Unavailable' | 'Faulted';

export interface Charger {
  id: string;
  name: string;
  status: ChargerStatus;
  online: boolean;
  max_current_a: number;
  error_code: string | null;
  vendor_error: string | null;
  active_transaction_id?: number | null;
  active_id_tag?: string | null;
  active_started_at?: number | null;
}

export interface MeterSample {
  ts: number;
  energy_wh: number | null;
  power_w: number | null;
  current_a: number | null;
  voltage_v: number | null;
  offered_a: number | null;
}

export interface Transaction {
  id: number;
  charger_id: string;
  charger_name?: string;
  id_tag: string;
  meter_start_wh: number;
  started_at: number;
  stopped_at: number | null;
  stop_reason: string | null;
  energy_wh: number | null;
}

export interface ChargerDetail extends Charger {
  activeTransaction: Transaction | null;
  latestSample: MeterSample | null;
}

export interface Tag {
  id_tag: string;
  label: string | null;
  status: string;
}

export interface User {
  id: string; email: string; name: string | null;
  role: 'admin' | 'operator' | 'driver';
}
