import type { AutomationRule, Dashboard, Device, DeviceRegistration, LoginResponse } from '@/types';

/**
 * Thin REST client for the C++ hub. In dev the default base URL is relative so
 * the Vite proxy handles it; VITE_API_BASE_URL overrides it for a deployed UI.
 */
const BASE_URL = import.meta.env.VITE_API_BASE_URL ?? '/api/v1';

const TOKEN_KEY = 'homestr.token';

export class ApiError extends Error {
  constructor(
    readonly status: number,
    message: string,
  ) {
    super(message);
    this.name = 'ApiError';
  }
}

export const getToken = (): string | null => localStorage.getItem(TOKEN_KEY);

export const setToken = (token: string): void => localStorage.setItem(TOKEN_KEY, token);

export const clearToken = (): void => localStorage.removeItem(TOKEN_KEY);

async function request<T>(path: string, init: RequestInit = {}): Promise<T> {
  const token = getToken();
  const headers = new Headers(init.headers);
  headers.set('Accept', 'application/json');
  if (init.body !== undefined) {
    headers.set('Content-Type', 'application/json');
  }
  if (token !== null) {
    headers.set('Authorization', `Bearer ${token}`);
  }

  const response = await fetch(`${BASE_URL}${path}`, { ...init, headers });

  if (response.status === 204) {
    return undefined as T;
  }

  const text = await response.text();
  const body: unknown = text.length > 0 ? JSON.parse(text) : null;

  if (!response.ok) {
    const message =
      typeof body === 'object' && body !== null && 'error' in body
        ? String(body.error)
        : response.statusText;
    throw new ApiError(response.status, message);
  }

  return body as T;
}

export const api = {
  /** POST /auth/login -- stores the bearer token on success. */
  async login(email: string, password: string): Promise<LoginResponse> {
    const result = await request<LoginResponse>('/auth/login', {
      method: 'POST',
      body: JSON.stringify({ email, password }),
    });
    setToken(result.token);
    return result;
  },

  logout(): void {
    clearToken();
  },

  getDashboard: (): Promise<Dashboard> => request<Dashboard>('/dashboard'),

  async listDevices(): Promise<Device[]> {
    const { devices } = await request<{ devices: Device[] }>('/devices');
    return devices;
  },

  getDevice: (deviceId: string): Promise<Device> =>
    request<Device>(`/devices/${encodeURIComponent(deviceId)}`),

  /** GET /devices/discover -- unregistered devices seen on the LAN. */
  async discover(): Promise<DeviceRegistration[]> {
    const { discovered } = await request<{ discovered: DeviceRegistration[] }>('/devices/discover');
    return discovered;
  },

  registerDevice: (device: DeviceRegistration): Promise<{ registered: string }> =>
    request<{ registered: string }>('/devices', {
      method: 'POST',
      body: JSON.stringify(device),
    }),

  unregisterDevice: (deviceId: string): Promise<void> =>
    request<void>(`/devices/${encodeURIComponent(deviceId)}`, { method: 'DELETE' }),

  /** POST /devices/{id}/commands -- returns the device's new state. */
  sendCommand: (
    deviceId: string,
    command: string,
    params: Record<string, unknown> = {},
  ): Promise<Device> =>
    request<Device>(`/devices/${encodeURIComponent(deviceId)}/commands`, {
      method: 'POST',
      body: JSON.stringify({ command, params }),
    }),

  async listRules(): Promise<AutomationRule[]> {
    const { rules } = await request<{ rules: AutomationRule[] }>('/rules');
    return rules;
  },

  createRule: (
    rule: Omit<AutomationRule, 'enabled'> & { enabled?: boolean },
  ): Promise<AutomationRule> =>
    request<AutomationRule>('/rules', { method: 'POST', body: JSON.stringify(rule) }),

  setRuleEnabled: (ruleId: string, enabled: boolean): Promise<{ id: string; enabled: boolean }> =>
    request<{ id: string; enabled: boolean }>(`/rules/${encodeURIComponent(ruleId)}/enabled`, {
      method: 'PUT',
      body: JSON.stringify({ enabled }),
    }),

  triggerRule: (ruleId: string): Promise<{ fired: string[] }> =>
    request<{ fired: string[] }>(`/rules/${encodeURIComponent(ruleId)}/trigger`, {
      method: 'POST',
    }),
};
