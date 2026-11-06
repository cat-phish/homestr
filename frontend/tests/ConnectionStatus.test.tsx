import { act, render, screen } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

import { api, clearToken, getToken, setToken } from '@/api/client';
import { Dashboard } from '@/components/Dashboard';
import type { Dashboard as DashboardPayload, Light, User } from '@/types';

// Unlike Dashboard.test.tsx, the real reconnecting WebSocket client is used here,
// driven through a fake socket, so the two are verified working together.
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

const kitchen: Light = {
  id: 'light-1',
  name: 'Kitchen',
  ipAddress: '127.0.0.1',
  type: 'light',
  status: 'ONLINE',
  powerOn: true,
  brightness: 80,
  color: '#ffffff',
};

const mockAdmin: User = {
  id: 'user-1',
  name: 'Admin',
  email: 'admin@example.com',
  role: 'admin',
};

const dashboardWith = (device: Light): DashboardPayload => ({
  householdId: 'home',
  devices: [device],
  rules: [],
});

const socketAt = (index: number): FakeSocket => {
  const socket = FakeSocket.instances[index];
  if (socket === undefined) {
    throw new Error(`socket ${index} was never opened`);
  }
  return socket;
};

const open = (socket: FakeSocket): void => {
  act(() => socket.onopen?.());
};

const deliverSnapshot = (socket: FakeSocket, payload: DashboardPayload): void => {
  act(() =>
    socket.onmessage?.({
      data: JSON.stringify({ type: 'snapshot', payload }),
    } as MessageEvent<string>),
  );
};

const drop = (socket: FakeSocket): void => {
  act(() => socket.onclose?.({ code: 1006 } as CloseEvent));
};

const waitForReconnectAttempt = async (ms: number): Promise<void> => {
  await act(async () => {
    await vi.advanceTimersByTimeAsync(ms);
  });
};

const indicator = (): HTMLElement | null => screen.queryByText(/Reconnecting to the hub/);

const renderConnectedDashboard = async (): Promise<void> => {
  render(<Dashboard user={mockAdmin} />);
  await act(async () => {
    await vi.advanceTimersByTimeAsync(0);
  });
  open(socketAt(0));
  deliverSnapshot(socketAt(0), dashboardWith(kitchen));
};

beforeEach(() => {
  vi.useFakeTimers();
  vi.stubGlobal('WebSocket', FakeSocket);
  FakeSocket.instances = [];
  setToken('session-token');
  vi.spyOn(api, 'getDashboard').mockResolvedValue(dashboardWith(kitchen));
  vi.spyOn(api, 'me').mockResolvedValue({
    id: 'u1',
    name: 'Ada',
    email: 'ada@example.com',
    role: 'user',
  });
});

afterEach(() => {
  clearToken();
  vi.restoreAllMocks();
  vi.unstubAllGlobals();
  vi.useRealTimers();
});

describe('Feature: Dashboard connection status', () => {
  describe('Scenario: Lose the live connection', () => {
    it('shows a reconnecting indicator and marks device states as potentially outdated', async () => {
      await renderConnectedDashboard();
      expect(indicator()).not.toBeInTheDocument();
      expect(screen.getByRole('article', { name: 'Kitchen' })).not.toHaveAttribute('data-stale');

      drop(socketAt(0));

      expect(indicator()).toBeInTheDocument();
      expect(screen.getByRole('article', { name: 'Kitchen' })).toHaveAttribute(
        'data-stale',
        'true',
      );
      expect(screen.getByText('May be outdated')).toBeInTheDocument();
    });

    it('keeps the last known state visible while disconnected', async () => {
      await renderConnectedDashboard();
      drop(socketAt(0));
      expect(screen.getByRole('button', { name: 'Turn off' })).toBeInTheDocument();
    });

    it('keeps showing the indicator across repeated failed reconnect attempts', async () => {
      await renderConnectedDashboard();
      drop(socketAt(0));
      await waitForReconnectAttempt(1000);
      expect(FakeSocket.instances).toHaveLength(2);

      drop(socketAt(1));
      await waitForReconnectAttempt(2000);
      expect(FakeSocket.instances).toHaveLength(3);
      expect(indicator()).toBeInTheDocument();
      expect(screen.getByRole('article', { name: 'Kitchen' })).toHaveAttribute(
        'data-stale',
        'true',
      );
    });

    it('also flags a hub that is unreachable on the very first connection', async () => {
      render(<Dashboard user={mockAdmin} />);
      await act(async () => {
        await vi.advanceTimersByTimeAsync(0);
      });
      drop(socketAt(0));
      expect(indicator()).toBeInTheDocument();
      expect(screen.getByRole('article', { name: 'Kitchen' })).toHaveAttribute(
        'data-stale',
        'true',
      );
    });
  });

  describe('Scenario: Recover the live connection', () => {
    it('refreshes to the current device state and removes the indicator', async () => {
      await renderConnectedDashboard();
      drop(socketAt(0));
      // The device turns off while the dashboard cannot hear about it.
      await waitForReconnectAttempt(1000);
      expect(screen.getByRole('button', { name: 'Turn off' })).toBeInTheDocument();

      open(socketAt(1));
      deliverSnapshot(socketAt(1), dashboardWith({ ...kitchen, powerOn: false }));

      expect(screen.getByRole('button', { name: 'Turn on' })).toBeInTheDocument();
      expect(indicator()).not.toBeInTheDocument();
      expect(screen.queryByText('May be outdated')).not.toBeInTheDocument();
      expect(screen.getByRole('article', { name: 'Kitchen' })).not.toHaveAttribute('data-stale');
    });

    it('stays flagged after the socket reopens until the snapshot has arrived', async () => {
      await renderConnectedDashboard();
      drop(socketAt(0));
      await waitForReconnectAttempt(1000);

      open(socketAt(1));
      expect(indicator()).toBeInTheDocument();
      expect(screen.getByText('May be outdated')).toBeInTheDocument();

      deliverSnapshot(socketAt(1), dashboardWith(kitchen));
      expect(indicator()).not.toBeInTheDocument();
    });

    it('can lose and recover the connection more than once', async () => {
      await renderConnectedDashboard();

      drop(socketAt(0));
      await waitForReconnectAttempt(1000);
      open(socketAt(1));
      deliverSnapshot(socketAt(1), dashboardWith(kitchen));
      expect(indicator()).not.toBeInTheDocument();

      drop(socketAt(1));
      expect(indicator()).toBeInTheDocument();
      await waitForReconnectAttempt(1000);
      open(socketAt(2));
      deliverSnapshot(socketAt(2), dashboardWith({ ...kitchen, brightness: 20 }));
      expect(indicator()).not.toBeInTheDocument();
      expect(screen.getByLabelText('Brightness')).toHaveValue('20');
    });
  });

  describe('Session expiration while disconnected', () => {
    it('returns to sign-in instead of reconnecting when the session was rejected', async () => {
      vi.spyOn(api, 'me').mockImplementation(() => {
        clearToken();
        return Promise.reject(new Error('401'));
      });
      await renderConnectedDashboard();

      drop(socketAt(0));
      await waitForReconnectAttempt(5000);

      expect(getToken()).toBeNull();
      expect(FakeSocket.instances).toHaveLength(1);
    });

    it('does not flag a connection the server closed for an expired session', async () => {
      await renderConnectedDashboard();
      act(() => socketAt(0).onclose?.({ code: 4001 } as CloseEvent));
      expect(indicator()).not.toBeInTheDocument();
      expect(getToken()).toBeNull();
    });
  });
});
