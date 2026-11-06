import { render, screen, waitFor } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { describe, expect, it, vi, beforeEach } from 'vitest';

import { api } from '@/api/client';
import { DeviceDiscovery } from '@/components/DeviceDiscovery';
import type { DeviceRegistration, Light } from '@/types';

vi.mock('@/api/client');

const mockLight: DeviceRegistration = {
  id: 'mock-light-001',
  householdId: '',
  name: 'Living Room Light',
  ipAddress: '192.168.1.100',
  type: 'light',
  status: 'ONLINE',
  attributes: { brightness: 100, color: '#FFFFFF' },
};

const mockSpeaker: DeviceRegistration = {
  id: 'mock-speaker-001',
  householdId: '',
  name: 'Kitchen Speaker',
  ipAddress: '192.168.1.101',
  type: 'speaker',
  status: 'ONLINE',
  attributes: { connected: false, volume: 50, playbackState: 'STOPPED' },
};

const registeredLight: Light = {
  id: 'mock-light-001',
  name: 'Living Room Light',
  ipAddress: '192.168.1.100',
  type: 'light',
  status: 'ONLINE',
  powerOn: false,
  brightness: 100,
  color: '#FFFFFF',
};

describe('DeviceDiscovery', () => {
  beforeEach(() => {
    vi.clearAllMocks();
  });

  it('scans for devices on mount', async () => {
    vi.mocked(api.discover).mockResolvedValue([mockLight, mockSpeaker]);

    render(
      <DeviceDiscovery
        householdId="home-1"
        registeredDeviceIds={new Set()}
        onDeviceRegistered={() => {}}
        onClose={() => {}}
      />,
    );

    await waitFor(() => {
      expect(screen.getByText('Living Room Light')).toBeInTheDocument();
    });
  });

  it('filters out already registered devices', async () => {
    const registered = new Set([mockLight.id]);
    vi.mocked(api.discover).mockResolvedValue([mockLight, mockSpeaker]);

    render(
      <DeviceDiscovery
        householdId="home-1"
        registeredDeviceIds={registered}
        onDeviceRegistered={() => {}}
        onClose={() => {}}
      />,
    );

    await waitFor(() => {
      expect(screen.queryByText('Living Room Light')).not.toBeInTheDocument();
      expect(screen.getByText('Kitchen Speaker')).toBeInTheDocument();
    });
  });

  it('allows user to customize device name before registration', async () => {
    const user = userEvent.setup();
    vi.mocked(api.discover).mockResolvedValue([mockLight]);
    vi.mocked(api.registerDevice).mockResolvedValue({ registered: mockLight.id });
    vi.mocked(api.getDevice).mockResolvedValue(registeredLight);

    const onRegistered = vi.fn();

    render(
      <DeviceDiscovery
        householdId="home-1"
        registeredDeviceIds={new Set()}
        onDeviceRegistered={onRegistered}
        onClose={() => {}}
      />,
    );

    await waitFor(() => {
      expect(screen.getByText('Living Room Light')).toBeInTheDocument();
    });

    const nameInput = screen.getByDisplayValue('Living Room Light') as HTMLInputElement;
    await user.clear(nameInput);
    await user.type(nameInput, 'Kitchen Ceiling');

    await user.click(screen.getByRole('button', { name: 'Add' }));

    await waitFor(() => {
      expect(api.registerDevice).toHaveBeenCalledWith(
        expect.objectContaining({
          name: 'Kitchen Ceiling',
          id: mockLight.id,
          householdId: 'home-1',
        }),
      );
    });
  });

  it('calls onDeviceRegistered with the registered device', async () => {
    const user = userEvent.setup();
    vi.mocked(api.discover).mockResolvedValue([mockLight]);
    vi.mocked(api.registerDevice).mockResolvedValue({ registered: mockLight.id });
    vi.mocked(api.getDevice).mockResolvedValue(registeredLight);

    const onRegistered = vi.fn();

    render(
      <DeviceDiscovery
        householdId="home-1"
        registeredDeviceIds={new Set()}
        onDeviceRegistered={onRegistered}
        onClose={() => {}}
      />,
    );

    await waitFor(() => {
      expect(screen.getByText('Living Room Light')).toBeInTheDocument();
    });

    await user.click(screen.getByRole('button', { name: 'Add' }));

    await waitFor(() => {
      expect(onRegistered).toHaveBeenCalledWith(registeredLight);
    });
  });

  it('removes a device from the list after successful registration', async () => {
    const user = userEvent.setup();
    vi.mocked(api.discover).mockResolvedValue([mockLight, mockSpeaker]);
    vi.mocked(api.registerDevice).mockResolvedValue({ registered: mockLight.id });
    vi.mocked(api.getDevice).mockResolvedValue(registeredLight);

    render(
      <DeviceDiscovery
        householdId="home-1"
        registeredDeviceIds={new Set()}
        onDeviceRegistered={() => {}}
        onClose={() => {}}
      />,
    );

    await waitFor(() => {
      expect(screen.getByText('Living Room Light')).toBeInTheDocument();
      expect(screen.getByText('Kitchen Speaker')).toBeInTheDocument();
    });

    const addButtons = screen.getAllByRole('button', { name: 'Add' });
    await user.click(addButtons[0]!);

    await waitFor(() => {
      expect(screen.queryByText('Living Room Light')).not.toBeInTheDocument();
      expect(screen.getByText('Kitchen Speaker')).toBeInTheDocument();
    });
  });

  it('shows error when scan fails', async () => {
    vi.mocked(api.discover).mockRejectedValue(new Error('Network error'));

    render(
      <DeviceDiscovery
        householdId="home-1"
        registeredDeviceIds={new Set()}
        onDeviceRegistered={() => {}}
        onClose={() => {}}
      />,
    );

    await waitFor(() => {
      expect(screen.getByRole('alert')).toHaveTextContent('Could not scan for devices');
    });
  });

  it('closes dialog when close button is clicked', async () => {
    const user = userEvent.setup();
    vi.mocked(api.discover).mockResolvedValue([]);

    const onClose = vi.fn();

    render(
      <DeviceDiscovery
        householdId="home-1"
        registeredDeviceIds={new Set()}
        onDeviceRegistered={() => {}}
        onClose={onClose}
      />,
    );

    await user.click(screen.getByRole('button', { name: 'Done' }));

    expect(onClose).toHaveBeenCalled();
  });
});
