import type { ServerEvent } from '@/types';

const DEFAULT_PATH = '/api/v1/ws';
const PING_INTERVAL_MS = 25_000;
const MAX_BACKOFF_MS = 30_000;

const defaultUrl = (): string => {
  const configured = import.meta.env.VITE_WS_URL;
  if (typeof configured === 'string' && configured.length > 0) {
    return configured;
  }
  // Same host as the page, so the Vite proxy (dev) or the reverse proxy (prod)
  // handles the upgrade.
  const scheme = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
  return `${scheme}//${window.location.host}${DEFAULT_PATH}`;
};

/**
 * Auto-reconnecting subscription to the hub's push channel. Returns an
 * unsubscribe function that closes the socket and stops reconnecting.
 */
export function subscribe(
  onEvent: (event: ServerEvent) => void,
  url: string = defaultUrl(),
): () => void {
  let socket: WebSocket | null = null;
  let pingTimer: ReturnType<typeof setInterval> | undefined;
  let retryTimer: ReturnType<typeof setTimeout> | undefined;
  let attempt = 0;
  let closedByCaller = false;

  const connect = (): void => {
    socket = new WebSocket(url);

    socket.onopen = () => {
      attempt = 0;
      pingTimer = setInterval(() => socket?.send('ping'), PING_INTERVAL_MS);
    };

    socket.onmessage = (message: MessageEvent<string>) => {
      try {
        onEvent(JSON.parse(message.data) as ServerEvent);
      } catch {
        console.warn('[ws] dropped unparseable frame');
      }
    };

    socket.onclose = () => {
      clearInterval(pingTimer);
      if (closedByCaller) {
        return;
      }
      // Exponential backoff, capped: the hub restarts during development.
      attempt += 1;
      const delay = Math.min(1000 * 2 ** (attempt - 1), MAX_BACKOFF_MS);
      retryTimer = setTimeout(connect, delay);
    };

    socket.onerror = () => socket?.close();
  };

  connect();

  return () => {
    closedByCaller = true;
    clearInterval(pingTimer);
    clearTimeout(retryTimer);
    socket?.close();
  };
}
