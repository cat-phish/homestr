import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

import { api, clearToken, getToken, setToken } from '@/api/client';
import { subscribe } from '@/api/websocket';

class FakeSocket {
  static instances: FakeSocket[] = [];
  onopen: (() => void) | null = null;
  onclose: ((event: CloseEvent) => void) | null = null;
  onmessage: ((event: MessageEvent<string>) => void) | null = null;
  onerror: (() => void) | null = null;
  send = vi.fn();
  close = vi.fn();
  constructor(
    readonly url: string,
    readonly protocols: string[],
  ) {
    FakeSocket.instances.push(this);
  }
}

let unsubscribe: (() => void) | undefined;

beforeEach(() => {
  vi.useFakeTimers();
  vi.stubGlobal('WebSocket', FakeSocket);
  FakeSocket.instances = [];
  clearToken();
});

afterEach(() => {
  unsubscribe?.();
  unsubscribe = undefined;
  vi.restoreAllMocks();
  vi.unstubAllGlobals();
  vi.useRealTimers();
});

describe('authenticated WebSocket', () => {
  it('offers credentials as a subprotocol without putting them in the URL', () => {
    setToken('session-token');
    unsubscribe = subscribe(vi.fn(), { url: 'ws://localhost/api/v1/ws' });
    expect(FakeSocket.instances[0]?.url).toBe('ws://localhost/api/v1/ws');
    expect(FakeSocket.instances[0]?.protocols).toEqual(['homestr', 'bearer.session-token']);
  });

  it('ends authentication when the server closes an expired session', () => {
    setToken('expired-token');
    unsubscribe = subscribe(vi.fn());
    FakeSocket.instances[0]?.onclose?.({ code: 4001 } as CloseEvent);
    expect(getToken()).toBeNull();
  });

  it('detects a rejected session even when the browser hides the handshake status', async () => {
    setToken('expired-token');
    vi.stubGlobal(
      'fetch',
      vi
        .fn()
        .mockResolvedValue(
          new Response(JSON.stringify({ error: 'authentication required' }), { status: 401 }),
        ),
    );
    unsubscribe = subscribe(vi.fn());
    FakeSocket.instances[0]?.onclose?.({ code: 1006 } as CloseEvent);
    await vi.advanceTimersByTimeAsync(1000);
    expect(getToken()).toBeNull();
    expect(FakeSocket.instances).toHaveLength(1);
  });

  it('does not reconnect after unmounting while a session check is pending', async () => {
    setToken('session-token');
    let resolve!: (value: { id: string; name: string; email: string; role: 'user' }) => void;
    vi.spyOn(api, 'me').mockReturnValue(
      new Promise((done) => {
        resolve = done;
      }),
    );
    unsubscribe = subscribe(vi.fn());
    FakeSocket.instances[0]?.onclose?.({ code: 1006 } as CloseEvent);
    unsubscribe();
    resolve({ id: 'u1', name: 'Ada', email: 'ada@example.com', role: 'user' });
    await vi.advanceTimersByTimeAsync(1000);
    expect(FakeSocket.instances).toHaveLength(1);
  });
});

describe('connection loss reporting', () => {
  it('reports each lost connection and reconnects', async () => {
    setToken('session-token');
    vi.spyOn(api, 'me').mockResolvedValue({
      id: 'u1',
      name: 'Ada',
      email: 'ada@example.com',
      role: 'user',
    });
    const onReconnecting = vi.fn();
    unsubscribe = subscribe(vi.fn(), { onReconnecting });
    FakeSocket.instances[0]?.onopen?.();
    expect(onReconnecting).not.toHaveBeenCalled();

    FakeSocket.instances[0]?.onclose?.({ code: 1006 } as CloseEvent);
    expect(onReconnecting).toHaveBeenCalledTimes(1);
    await vi.advanceTimersByTimeAsync(1000);
    expect(FakeSocket.instances).toHaveLength(2);

    FakeSocket.instances[1]?.onclose?.({ code: 1006 } as CloseEvent);
    expect(onReconnecting).toHaveBeenCalledTimes(2);
  });

  it('reports a failed first connection', () => {
    setToken('session-token');
    const onReconnecting = vi.fn();
    unsubscribe = subscribe(vi.fn(), { onReconnecting });
    FakeSocket.instances[0]?.onclose?.({ code: 1006 } as CloseEvent);
    expect(onReconnecting).toHaveBeenCalledOnce();
  });

  it('does not report a rejected session or a deliberate close', () => {
    setToken('session-token');
    const onReconnecting = vi.fn();
    unsubscribe = subscribe(vi.fn(), { onReconnecting });
    FakeSocket.instances[0]?.onclose?.({ code: 4001 } as CloseEvent);
    expect(onReconnecting).not.toHaveBeenCalled();

    setToken('session-token');
    unsubscribe();
    unsubscribe = subscribe(vi.fn(), { onReconnecting });
    const socket = FakeSocket.instances[1];
    unsubscribe();
    socket?.onclose?.({ code: 1000 } as CloseEvent);
    expect(onReconnecting).not.toHaveBeenCalled();
  });
});
