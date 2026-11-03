import { act, fireEvent, render, screen, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { describe, expect, it, vi } from 'vitest';

import { api, ApiError } from '@/api/client';
import { RuleForm } from '@/components/RuleForm';
import type { AutomationRule, Device } from '@/types';

const devices: Device[] = [
  {
    id: 'light-1',
    name: 'Kitchen light',
    ipAddress: '127.0.0.1',
    type: 'light',
    status: 'ONLINE',
    powerOn: true,
    brightness: 80,
    color: '#ffffff',
  },
  {
    id: 'speaker-1',
    name: 'Speaker',
    ipAddress: '127.0.0.2',
    type: 'speaker',
    status: 'ONLINE',
    powerOn: true,
    connected: true,
    volume: 30,
    playbackState: 'PLAYING',
    currentTrack: '',
  },
  {
    id: 'relay-1',
    name: 'Relay',
    ipAddress: '127.0.0.3',
    type: 'generic',
    status: 'OFFLINE',
    powerOn: false,
  },
];

const action = (number: number) => within(screen.getByRole('group', { name: `Action ${number}` }));

function setup() {
  const onSaved = vi.fn();
  const onCancel = vi.fn();
  const create = vi
    .spyOn(api, 'createRule')
    .mockImplementation((rule) => Promise.resolve({ ...rule, enabled: rule.enabled ?? true }));
  render(<RuleForm devices={devices} onSaved={onSaved} onCancel={onCancel} />);
  return { create, onSaved, onCancel };
}

describe('manual routine creation', () => {
  it('saves trimmed names, supported parameters, and the chosen action order', async () => {
    const { create, onSaved } = setup();
    await userEvent.type(screen.getByLabelText('Routine name'), '  Leaving home  ');
    await userEvent.selectOptions(action(1).getByLabelText('Device'), 'light-1');
    await userEvent.selectOptions(action(1).getByLabelText('Command'), 'setBrightness');
    await userEvent.clear(action(1).getByLabelText('Brightness'));
    await userEvent.type(action(1).getByLabelText('Brightness'), '15');
    await userEvent.click(screen.getByRole('button', { name: 'Add action' }));
    await userEvent.selectOptions(action(2).getByLabelText('Device'), 'speaker-1');
    await userEvent.selectOptions(action(2).getByLabelText('Command'), 'pause');
    await userEvent.click(screen.getByRole('button', { name: 'Move action 2 up' }));
    await userEvent.click(screen.getByRole('button', { name: 'Save routine' }));
    expect(create).toHaveBeenCalledWith({
      id: expect.stringMatching(/^[0-9a-f-]{36}$/) as string,
      name: 'Leaving home',
      enabled: true,
      triggerType: 'MANUAL',
      trigger: {},
      actions: [
        { deviceId: 'speaker-1', command: 'pause', params: {} },
        { deviceId: 'light-1', command: 'setBrightness', params: { level: 15 } },
      ],
    });
    expect(onSaved).toHaveBeenCalledOnce();
  });

  it('limits commands to the selected device and clears incompatible settings', async () => {
    setup();
    await userEvent.selectOptions(action(1).getByLabelText('Device'), 'light-1');
    await userEvent.selectOptions(action(1).getByLabelText('Command'), 'setColor');
    expect(action(1).getByLabelText('Color')).toBeInTheDocument();
    await userEvent.selectOptions(action(1).getByLabelText('Device'), 'relay-1');
    expect(action(1).queryByLabelText('Color')).not.toBeInTheDocument();
    expect(
      action(1)
        .getAllByRole('option')
        .filter((entry) => entry.closest('select')?.getAttribute('name') === 'command')
        .map((entry) => entry.textContent),
    ).toEqual(['Turn on', 'Turn off']);
    expect(screen.getByText(/Offline devices may not respond/)).toBeInTheDocument();
  });

  it('validates missing targets, whitespace names, and invalid numeric values', async () => {
    const { create } = setup();
    await userEvent.type(screen.getByLabelText('Routine name'), '   ');
    await userEvent.click(screen.getByRole('button', { name: 'Save routine' }));
    expect(screen.getByRole('alert')).toHaveTextContent('Enter a routine name');
    await userEvent.type(screen.getByLabelText('Routine name'), 'Bedtime');
    await userEvent.click(screen.getByRole('button', { name: 'Save routine' }));
    expect(screen.getByRole('alert')).toHaveTextContent('Action 1: select a device');
    await userEvent.selectOptions(action(1).getByLabelText('Device'), 'light-1');
    await userEvent.selectOptions(action(1).getByLabelText('Command'), 'setBrightness');
    await userEvent.clear(action(1).getByLabelText('Brightness'));
    await userEvent.type(action(1).getByLabelText('Brightness'), '101');
    await userEvent.click(screen.getByRole('button', { name: 'Save routine' }));
    expect(screen.getByRole('alert')).toHaveTextContent('whole number from 0 to 100');
    expect(create).not.toHaveBeenCalled();
  });

  it('removes an action without losing the remaining configuration', async () => {
    setup();
    await userEvent.selectOptions(action(1).getByLabelText('Device'), 'light-1');
    await userEvent.click(screen.getByRole('button', { name: 'Add action' }));
    await userEvent.selectOptions(action(2).getByLabelText('Device'), 'speaker-1');
    await userEvent.click(screen.getByRole('button', { name: 'Remove action 1' }));
    expect(action(1).getByLabelText('Device')).toHaveValue('speaker-1');
    expect(screen.queryByRole('group', { name: 'Action 2' })).not.toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'Remove action 1' })).toBeDisabled();
  });

  it('retains the draft after a failed save and allows retrying', async () => {
    const { create, onSaved } = setup();
    create.mockRejectedValueOnce(new ApiError(503, 'could not save rule'));
    await userEvent.type(screen.getByLabelText('Routine name'), 'Bedtime');
    await userEvent.selectOptions(action(1).getByLabelText('Device'), 'light-1');
    await userEvent.click(screen.getByRole('button', { name: 'Save routine' }));
    expect(await screen.findByRole('alert')).toHaveTextContent('could not save rule');
    expect(screen.getByLabelText('Routine name')).toHaveValue('Bedtime');
    expect(onSaved).not.toHaveBeenCalled();
    await userEvent.click(screen.getByRole('button', { name: 'Save routine' }));
    expect(onSaved).toHaveBeenCalledOnce();
  });

  it.each([
    {
      device: 'light-1',
      command: 'setColor',
      label: 'Color',
      value: '#123abc',
      params: { color: '#123abc' },
    },
    {
      device: 'speaker-1',
      command: 'setVolume',
      label: 'Volume',
      value: '0',
      params: { level: 0 },
    },
    {
      device: 'speaker-1',
      command: 'play',
      label: 'Track (optional)',
      value: '  Evening music  ',
      params: { track: 'Evening music' },
    },
  ])(
    'saves $command with its command settings',
    async ({ device, command, label, value, params }) => {
      const { create } = setup();
      await userEvent.type(screen.getByLabelText('Routine name'), 'Settings');
      await userEvent.selectOptions(action(1).getByLabelText('Device'), device);
      await userEvent.selectOptions(action(1).getByLabelText('Command'), command);
      fireEvent.change(action(1).getByLabelText(label), { target: { value } });
      await userEvent.click(screen.getByRole('button', { name: 'Save routine' }));
      expect(create.mock.calls[0]?.[0].actions).toEqual([{ deviceId: device, command, params }]);
    },
  );

  it('prevents repeat submissions and changes while a save is pending', async () => {
    const { create, onSaved } = setup();
    let resolve!: (rule: AutomationRule) => void;
    const response = new Promise<AutomationRule>((done) => {
      resolve = done;
    });
    create.mockReturnValue(response);
    await userEvent.type(screen.getByLabelText('Routine name'), 'Bedtime');
    await userEvent.selectOptions(action(1).getByLabelText('Device'), 'light-1');
    await userEvent.click(screen.getByRole('button', { name: 'Save routine' }));
    expect(screen.getByLabelText('Routine name')).toBeDisabled();
    expect(screen.getByRole('button', { name: 'Cancel' })).toBeDisabled();
    await userEvent.click(screen.getByRole('button', { name: 'Saving…' }));
    expect(create).toHaveBeenCalledOnce();
    const submitted = create.mock.calls[0]?.[0];
    if (submitted === undefined) {
      throw new Error('No request submitted');
    }
    await act(async () => {
      resolve({ ...submitted, enabled: true });
      await response;
    });
    expect(onSaved).toHaveBeenCalledOnce();
    expect(screen.getByRole('button', { name: 'Save routine' })).toBeEnabled();
  });

  it('cancels without sending a request', async () => {
    const { create, onCancel } = setup();
    await userEvent.click(screen.getByRole('button', { name: 'Cancel' }));
    expect(onCancel).toHaveBeenCalledOnce();
    expect(create).not.toHaveBeenCalled();
  });
});
