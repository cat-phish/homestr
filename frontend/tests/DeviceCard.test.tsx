import { render, screen } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { describe, expect, it, vi } from 'vitest';

import { DeviceCard } from '@/components/DeviceCard';
import type { Light, SmartSpeaker } from '@/types';

const light: Light = {
  id: 'light-1',
  name: 'Kitchen Ceiling',
  ipAddress: '192.168.1.40',
  type: 'light',
  status: 'ONLINE',
  powerOn: false,
  brightness: 70,
  color: '#ffaa00',
};

const speaker: SmartSpeaker = {
  id: 'speaker-1',
  name: 'Living Room Speaker',
  ipAddress: '192.168.1.41',
  type: 'speaker',
  status: 'ONLINE',
  powerOn: true,
  connected: true,
  volume: 25,
  playbackState: 'PLAYING',
  currentTrack: 'Nothing',
};

describe('DeviceCard', () => {
  it('dispatches turnOn for a device that is currently off', async () => {
    const onCommand = vi.fn();
    render(<DeviceCard device={light} onCommand={onCommand} />);

    await userEvent.click(screen.getByRole('button', { name: 'Turn on' }));

    expect(onCommand).toHaveBeenCalledWith('light-1', 'turnOn');
  });

  it('dispatches turnOff for a device that is currently on', async () => {
    const onCommand = vi.fn();
    render(<DeviceCard device={{ ...light, powerOn: true }} onCommand={onCommand} />);

    await userEvent.click(screen.getByRole('button', { name: 'Turn off' }));

    expect(onCommand).toHaveBeenCalledWith('light-1', 'turnOff');
  });

  it('shows light-only controls for a light', () => {
    render(<DeviceCard device={light} onCommand={vi.fn()} />);

    expect(screen.getByLabelText('Brightness')).toHaveValue('70');
    expect(screen.getByLabelText('Colour')).toHaveValue('#ffaa00');
  });

  it('shows pause for a playing speaker and dispatches pause', async () => {
    const onCommand = vi.fn();
    render(<DeviceCard device={speaker} onCommand={onCommand} />);

    await userEvent.click(screen.getByRole('button', { name: 'Pause' }));

    expect(onCommand).toHaveBeenCalledWith('speaker-1', 'pause');
  });

  it('does not offer light controls on a speaker', () => {
    render(<DeviceCard device={speaker} onCommand={vi.fn()} />);

    expect(screen.queryByLabelText('Brightness')).not.toBeInTheDocument();
    expect(screen.getByLabelText('Volume')).toBeInTheDocument();
  });

  it('disables every control while the device is offline', () => {
    render(<DeviceCard device={{ ...light, status: 'OFFLINE' }} onCommand={vi.fn()} />);

    expect(screen.getByRole('button', { name: 'Turn on' })).toBeDisabled();
    expect(screen.getByLabelText('Brightness')).toBeDisabled();
  });
});
