import { api, getToken, invalidateSession } from '@/api/client';
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

export interface SubscribeOptions {
  url?: string;
  /** Called each time the live connection is lost and a retry will follow. */
  onReconnecting?: () => void;
}

/**
 * Auto-reconnecting subscription to the hub's push channel. Returns an
 * unsubscribe function that closes the socket and stops reconnecting. The hub
 * sends a `snapshot` frame on every connect, which callers use to resynchronize
 * after an `onReconnecting` notification.
 */
export function subscribe(
  onEvent: (event: ServerEvent) => void,
  { url = defaultUrl(), onReconnecting }: SubscribeOptions = {},
): () => void {
  let socket: WebSocket | null = null;
  let pingTimer: ReturnType<typeof setInterval> | undefined;
  let retryTimer: ReturnType<typeof setTimeout> | undefined;
  let attempt = 0;
  let closedByCaller = false;

  const connect = (): void => {
    const token = getToken();
    if (token === null) {
      return;
    }
    socket = new WebSocket(url, ['homestr', `bearer.${token}`]);

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

    socket.onclose = (event: CloseEvent) => {
      clearInterval(pingTimer);
      if (event.code === 4001) {
        invalidateSession(token);
        return;
      }
      if (closedByCaller) {
        return;
      }
      onReconnecting?.();
      // Browsers hide HTTP handshake errors. Check the session over REST so a
      // rejected upgrade can return the user to sign-in instead of retrying forever.
      const scheduleRetry = (): void => {
        if (closedByCaller || getToken() !== token) {
          return;
        }
        attempt += 1;
        const delay = Math.min(1000 * 2 ** (attempt - 1), MAX_BACKOFF_MS);
        retryTimer = setTimeout(connect, delay);
      };
      void api.me().then(scheduleRetry, scheduleRetry);
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
