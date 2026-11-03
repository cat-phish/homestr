import { act, render, screen, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { beforeEach, describe, expect, it, vi } from 'vitest';

import { api, ApiError } from '@/api/client';
import { subscribe } from '@/api/websocket';
import { Dashboard } from '@/components/Dashboard';
import type { Dashboard as DashboardPayload } from '@/types';

vi.mock('@/api/websocket', () => ({ subscribe: vi.fn() }));

const initial: DashboardPayload = {
  householdId: 'home',
  devices: [
    {
      id: 'light-1',
      name: 'Kitchen',
      ipAddress: '127.0.0.1',
      type: 'light',
      status: 'ONLINE',
      powerOn: true,
      brightness: 80,
      color: '#ffffff',
    },
  ],
  rules: [
    {
      id: 'r1',
      name: 'Bedtime',
      enabled: true,
      triggerType: 'MANUAL',
      trigger: {},
      actions: [{ deviceId: 'light-1', command: 'turnOff', params: {} }],
    },
    { id: 'r2', name: 'Paused', enabled: false, triggerType: 'MANUAL', trigger: {}, actions: [] },
    {
      id: 'r3',
      name: 'Scheduled',
      enabled: true,
      triggerType: 'SCHEDULE',
      trigger: { cron: '0 22 * * *' },
      actions: [],
    },
  ],
};

beforeEach(() => {
  vi.mocked(subscribe).mockReturnValue(vi.fn());
  vi.spyOn(api, 'getDashboard').mockResolvedValue(initial);
});

describe('dashboard routines', () => {
  it('creates a routine and immediately shows the saved result', async () => {
    vi.spyOn(api, 'createRule').mockImplementation((rule) =>
      Promise.resolve({ ...rule, enabled: true }),
    );
    render(<Dashboard />);
    await screen.findByText('Bedtime');
    await userEvent.click(screen.getByRole('button', { name: 'New routine' }));
    await userEvent.type(screen.getByLabelText('Routine name'), 'Leaving home');
    await userEvent.selectOptions(screen.getByLabelText('Device'), 'light-1');
    await userEvent.click(screen.getByRole('button', { name: 'Save routine' }));
    expect(await screen.findByText('Leaving home')).toBeInTheDocument();
    expect(screen.queryByRole('form')).not.toBeInTheDocument();
    expect(screen.getByRole('status')).toHaveTextContent('Routine saved');
  });

  it('runs an enabled manual routine and refreshes the device state', async () => {
    vi.spyOn(api, 'triggerRule').mockResolvedValue({ fired: ['r1'] });
    vi.mocked(api.getDashboard)
      .mockResolvedValueOnce(initial)
      .mockResolvedValue({
        ...initial,
        devices: initial.devices.map((device) => ({ ...device, powerOn: false })),
      });
    render(<Dashboard />);
    await userEvent.click(await screen.findByRole('button', { name: 'Run Bedtime' }));
    expect(api.triggerRule).toHaveBeenCalledWith('r1');
    expect(await screen.findByRole('button', { name: 'Turn on' })).toBeInTheDocument();
    expect(screen.getByRole('status')).toHaveTextContent('Routine triggered');
    expect(screen.getByRole('button', { name: 'Run Paused' })).toBeDisabled();
    expect(screen.queryByRole('button', { name: 'Run Scheduled' })).not.toBeInTheDocument();
  });

  it('prevents overlapping runs while a trigger is pending', async () => {
    let resolve!: (value: { fired: string[] }) => void;
    const response = new Promise<{ fired: string[] }>((done) => {
      resolve = done;
    });
    vi.spyOn(api, 'triggerRule').mockReturnValue(response);
    render(<Dashboard />);
    await userEvent.click(await screen.findByRole('button', { name: 'Run Bedtime' }));
    expect(screen.getByRole('button', { name: 'Run Bedtime' })).toBeDisabled();
    await userEvent.click(screen.getByRole('button', { name: 'Run Bedtime' }));
    expect(api.triggerRule).toHaveBeenCalledOnce();
    await act(async () => {
      resolve({ fired: ['r1'] });
      await response;
    });
    expect(screen.getByRole('button', { name: 'Run Bedtime' })).toBeEnabled();
  });

  it('shows a trigger error without claiming that a routine ran', async () => {
    vi.spyOn(api, 'triggerRule').mockRejectedValue(new ApiError(404, 'rule is disabled'));
    render(<Dashboard />);
    await userEvent.click(await screen.findByRole('button', { name: 'Run Bedtime' }));
    expect(await screen.findByRole('alert')).toHaveTextContent('rule is disabled');
    expect(screen.queryByRole('status')).not.toBeInTheDocument();
  });

  it('distinguishes a refresh failure from a trigger failure', async () => {
    vi.spyOn(api, 'triggerRule').mockResolvedValue({ fired: ['r1'] });
    vi.mocked(api.getDashboard)
      .mockResolvedValueOnce(initial)
      .mockRejectedValue(new Error('offline'));
    render(<Dashboard />);
    await userEvent.click(await screen.findByRole('button', { name: 'Run Bedtime' }));
    expect(await screen.findByRole('alert')).toHaveTextContent(
      'Routine triggered, but device states could not be refreshed',
    );
  });

  it('explains why a routine cannot be created without devices', async () => {
    vi.mocked(api.getDashboard).mockResolvedValue({ ...initial, devices: [], rules: [] });
    render(<Dashboard />);
    expect(await screen.findByText('Add a device before creating a routine.')).toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'New routine' })).toBeDisabled();
    expect(within(screen.getByRole('main')).getByText('No routines yet.')).toBeInTheDocument();
  });
});

describe('automatic routine summaries', () => {
  it('shows the next scheduled run in the configured timezone', async () => {
    vi.mocked(api.getDashboard).mockResolvedValue({
      ...initial,
      rules: [
        {
          ...initial.rules[2]!,
          trigger: { cron: '0 22 * * *', timezone: 'America/Los_Angeles' },
          nextRunAt: '2026-11-04T06:00:00Z',
        },
      ],
    });
    render(<Dashboard />);
    expect(await screen.findByText(/Next run:/)).toHaveTextContent('America/Los_Angeles');
    expect(screen.queryByRole('button', { name: 'Run Scheduled' })).not.toBeInTheDocument();
  });
});
