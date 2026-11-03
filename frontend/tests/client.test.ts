import { beforeEach, describe, expect, it, vi } from 'vitest';

import { api, ApiError, clearToken, getToken } from '@/api/client';
import type { Light } from '@/types';

const jsonResponse = (body: unknown, status = 200): Response =>
  new Response(JSON.stringify(body), {
    status,
    headers: { 'Content-Type': 'application/json' },
  });

const stubFetch = (response: Response) => {
  const fetchMock = vi.fn<typeof fetch>().mockResolvedValue(response);
  vi.stubGlobal('fetch', fetchMock);
  return fetchMock;
};

const light: Light = {
  id: 'light-1',
  name: 'Kitchen Ceiling',
  ipAddress: '192.168.1.40',
  type: 'light',
  status: 'ONLINE',
  powerOn: true,
  brightness: 42,
  color: '#FFAA00',
};

describe('api.login', () => {
  beforeEach(clearToken);

  it('posts the credentials and stores the returned token', async () => {
    const fetchMock = stubFetch(
      jsonResponse({
        token: 'tok-123',
        user: { id: 'u1', name: 'Ada', email: 'ada@homestr.local', role: 'admin' },
      }),
    );

    const result = await api.login('ada@homestr.local', 'secret');

    expect(result.user.role).toBe('admin');
    expect(getToken()).toBe('tok-123');

    const [url, init] = fetchMock.mock.calls[0] ?? [];
    expect(url).toBe('/api/v1/auth/login');
    expect(init?.method).toBe('POST');
    expect(JSON.parse(init?.body as string)).toEqual({
      email: 'ada@homestr.local',
      password: 'secret',
    });
  });

  it('raises ApiError carrying the backend message on 401', async () => {
    stubFetch(jsonResponse({ error: 'invalid credentials' }, 401));

    await expect(api.login('ada@homestr.local', 'wrong')).rejects.toThrowError(
      new ApiError(401, 'invalid credentials'),
    );
    expect(getToken()).toBeNull();
  });
});

describe('authenticated requests', () => {
  it('attaches the bearer token once one is stored', async () => {
    stubFetch(
      jsonResponse({
        token: 'tok-abc',
        user: { id: 'u1', name: 'Ada', email: 'a@b', role: 'user' },
      }),
    );
    await api.login('a@b', 'pw');

    const fetchMock = stubFetch(jsonResponse({ devices: [light] }));
    await api.listDevices();

    const headers = new Headers(fetchMock.mock.calls[0]?.[1]?.headers);
    expect(headers.get('Authorization')).toBe('Bearer tok-abc');
  });
});

describe('api.sendCommand', () => {
  it('sends the command envelope and returns the new device state', async () => {
    const fetchMock = stubFetch(jsonResponse(light));

    const updated = await api.sendCommand('light-1', 'setBrightness', { level: 42 });

    expect(updated).toEqual(light);
    expect(JSON.parse(fetchMock.mock.calls[0]?.[1]?.body as string)).toEqual({
      command: 'setBrightness',
      params: { level: 42 },
    });
  });

  it('percent-encodes the device id in the path', async () => {
    const fetchMock = stubFetch(jsonResponse(light));

    await api.sendCommand('living room/1', 'turnOn');

    expect(fetchMock.mock.calls[0]?.[0]).toBe('/api/v1/devices/living%20room%2F1/commands');
  });
});

describe('api.unregisterDevice', () => {
  it('treats 204 as success without parsing a body', async () => {
    stubFetch(new Response(null, { status: 204 }));

    await expect(api.unregisterDevice('light-1')).resolves.toBeUndefined();
  });
});

describe('api.listRules', () => {
  it('unwraps the rules envelope', async () => {
    stubFetch(
      jsonResponse({
        rules: [
          {
            id: 'rule-1',
            name: 'Nightly',
            enabled: true,
            triggerType: 'SCHEDULE',
            trigger: { cron: '0 22 * * *' },
            actions: [],
          },
        ],
      }),
    );

    const rules = await api.listRules();

    expect(rules).toHaveLength(1);
    expect(rules[0]?.triggerType).toBe('SCHEDULE');
  });
});

describe('session lifecycle', () => {
  beforeEach(clearToken);

  it('invalidates authentication on a protected 401', async () => {
    localStorage.setItem('homestr.token', 'expired-token');
    const listener = vi.fn();
    window.addEventListener('homestr:unauthorized', listener);
    stubFetch(jsonResponse({ error: 'authentication required' }, 401));
    await expect(api.getDashboard()).rejects.toThrowError(ApiError);
    expect(getToken()).toBeNull();
    expect(listener).toHaveBeenCalledOnce();
    window.removeEventListener('homestr:unauthorized', listener);
  });

  it('revokes the server session before clearing the local token', async () => {
    localStorage.setItem('homestr.token', 'active-token');
    const fetchMock = stubFetch(new Response(null, { status: 204 }));
    await api.logout();
    expect(fetchMock.mock.calls[0]?.[0]).toBe('/api/v1/auth/logout');
    expect(fetchMock.mock.calls[0]?.[1]?.method).toBe('POST');
    expect(new Headers(fetchMock.mock.calls[0]?.[1]?.headers).get('Authorization')).toBe(
      'Bearer active-token',
    );
    expect(getToken()).toBeNull();
  });

  it('retains the token when logout fails so it can be retried', async () => {
    localStorage.setItem('homestr.token', 'active-token');
    stubFetch(jsonResponse({ error: 'could not close session' }, 503));
    await expect(api.logout()).rejects.toThrowError(ApiError);
    expect(getToken()).toBe('active-token');
  });
});
