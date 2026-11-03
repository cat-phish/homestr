import { useRef, useState } from 'react';

import { api, ApiError } from '@/api/client';
import type { AutomationRule, Device, RuleAction } from '@/types';
import { createId } from '@/utils/id';

interface RuleFormProps {
  devices: Device[];
  onSaved: (rule: AutomationRule) => void;
  onCancel: () => void;
}

interface DraftAction {
  key: string;
  deviceId: string;
  command: string;
  level: string;
  color: string;
  track: string;
}

function newAction(): DraftAction {
  return {
    key: createId(),
    deviceId: '',
    command: 'turnOn',
    level: '50',
    color: '#ffffff',
    track: '',
  };
}

function commands(device: Device | undefined): { value: string; label: string }[] {
  const common = [
    { value: 'turnOn', label: 'Turn on' },
    { value: 'turnOff', label: 'Turn off' },
  ];
  if (device?.type === 'light') {
    return [
      ...common,
      { value: 'setBrightness', label: 'Set brightness' },
      { value: 'setColor', label: 'Set color' },
    ];
  }
  if (device?.type === 'speaker') {
    return [
      ...common,
      { value: 'connect', label: 'Connect' },
      { value: 'play', label: 'Play' },
      { value: 'pause', label: 'Pause' },
      { value: 'setVolume', label: 'Set volume' },
    ];
  }
  return common;
}

export function RuleForm({ devices, onSaved, onCancel }: RuleFormProps) {
  const [name, setName] = useState('');
  const [actions, setActions] = useState<DraftAction[]>(() => [newAction()]);
  const [pending, setPending] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const saving = useRef(false);
  const ruleId = useRef<string | null>(null);

  const updateAction = (key: string, changes: Partial<DraftAction>): void => {
    setActions((current) =>
      current.map((action) => (action.key === key ? { ...action, ...changes } : action)),
    );
  };

  const moveAction = (index: number, direction: number): void => {
    setActions((current) => {
      const destination = index + direction;
      const moved = current[index];
      if (moved === undefined || destination < 0 || destination >= current.length) {
        return current;
      }
      const next = [...current];
      next.splice(index, 1);
      next.splice(destination, 0, moved);
      return next;
    });
  };

  const save = async (event: React.FormEvent<HTMLFormElement>): Promise<void> => {
    event.preventDefault();
    if (saving.current) {
      return;
    }
    setError(null);
    const trimmedName = name.trim();
    if (trimmedName.length === 0 || trimmedName.length > 100) {
      setError('Enter a routine name of 1 to 100 characters.');
      return;
    }
    const validated: RuleAction[] = [];
    for (const [index, action] of actions.entries()) {
      const device = devices.find((entry) => entry.id === action.deviceId);
      if (device === undefined) {
        setError(`Action ${index + 1}: select a device that is still registered.`);
        return;
      }
      if (!commands(device).some((command) => command.value === action.command)) {
        setError(`Action ${index + 1}: select a supported command.`);
        return;
      }
      const params: Record<string, unknown> = {};
      if (action.command === 'setBrightness' || action.command === 'setVolume') {
        const level = Number(action.level);
        if (action.level.trim() === '' || !Number.isInteger(level) || level < 0 || level > 100) {
          setError(`Action ${index + 1}: enter a whole number from 0 to 100.`);
          return;
        }
        params.level = level;
      } else if (action.command === 'setColor') {
        if (!/^#[0-9a-f]{6}$/i.test(action.color)) {
          setError(`Action ${index + 1}: choose a valid color.`);
          return;
        }
        params.color = action.color;
      } else if (action.command === 'play' && action.track.trim() !== '') {
        params.track = action.track.trim();
      }
      validated.push({ deviceId: device.id, command: action.command, params });
    }
    if (validated.length === 0) {
      setError('Add at least one action.');
      return;
    }
    saving.current = true;
    setPending(true);
    try {
      ruleId.current ??= createId();
      const saved = await api.createRule({
        id: ruleId.current,
        name: trimmedName,
        enabled: true,
        triggerType: 'MANUAL',
        trigger: {},
        actions: validated,
      });
      onSaved(saved);
    } catch (cause) {
      setError(
        cause instanceof ApiError
          ? cause.message
          : 'Could not save the routine. Check your connection and retry.',
      );
    } finally {
      saving.current = false;
      setPending(false);
    }
  };

  return (
    <form
      className="rule-form"
      aria-labelledby="routine-editor-title"
      noValidate
      onSubmit={(event) => void save(event)}
    >
      <h3 id="routine-editor-title">New routine</h3>
      <p>Choose actions to run together when you press Run. Actions run in the order shown.</p>
      <fieldset className="rule-form__fields" disabled={pending}>
        <label htmlFor="routine-name">Routine name</label>
        <input
          id="routine-name"
          autoFocus
          maxLength={100}
          required
          value={name}
          placeholder="e.g. Leaving home"
          onChange={(event) => setName(event.target.value)}
        />
        <ol className="rule-form__actions">
          {actions.map((action, index) => {
            const device = devices.find((entry) => entry.id === action.deviceId);
            return (
              <li key={action.key}>
                <fieldset className="rule-action">
                  <legend>Action {index + 1}</legend>
                  <div className="rule-action__inputs">
                    <div>
                      <label htmlFor={`device-${action.key}`}>Device</label>
                      <select
                        id={`device-${action.key}`}
                        required
                        value={action.deviceId}
                        onChange={(event) => {
                          const selected = devices.find((entry) => entry.id === event.target.value);
                          updateAction(action.key, {
                            deviceId: event.target.value,
                            command: 'turnOn',
                            track: '',
                            level: String(
                              selected?.type === 'light'
                                ? selected.brightness
                                : selected?.type === 'speaker'
                                  ? selected.volume
                                  : 50,
                            ),
                            color: selected?.type === 'light' ? selected.color : '#ffffff',
                          });
                        }}
                      >
                        <option value="">Select a device</option>
                        {devices.map((entry) => (
                          <option key={entry.id} value={entry.id}>
                            {entry.name}
                            {entry.status === 'ONLINE' ? '' : ' (offline)'}
                          </option>
                        ))}
                      </select>
                    </div>
                    <div>
                      <label htmlFor={`command-${action.key}`}>Command</label>
                      <select
                        id={`command-${action.key}`}
                        name="command"
                        disabled={device === undefined}
                        value={action.command}
                        onChange={(event) =>
                          updateAction(action.key, { command: event.target.value })
                        }
                      >
                        {commands(device).map((command) => (
                          <option key={command.value} value={command.value}>
                            {command.label}
                          </option>
                        ))}
                      </select>
                    </div>
                    {(action.command === 'setBrightness' || action.command === 'setVolume') && (
                      <div>
                        <label htmlFor={`level-${action.key}`}>
                          {action.command === 'setBrightness' ? 'Brightness' : 'Volume'}
                        </label>
                        <input
                          id={`level-${action.key}`}
                          type="number"
                          min={0}
                          max={100}
                          step={1}
                          required
                          value={action.level}
                          onChange={(event) =>
                            updateAction(action.key, { level: event.target.value })
                          }
                        />
                        <small>0–100</small>
                      </div>
                    )}
                    {action.command === 'setColor' && (
                      <div>
                        <label htmlFor={`color-${action.key}`}>Color</label>
                        <input
                          id={`color-${action.key}`}
                          type="color"
                          value={action.color}
                          onChange={(event) =>
                            updateAction(action.key, { color: event.target.value })
                          }
                        />
                      </div>
                    )}
                    {action.command === 'play' && (
                      <div>
                        <label htmlFor={`track-${action.key}`}>Track (optional)</label>
                        <input
                          id={`track-${action.key}`}
                          value={action.track}
                          onChange={(event) =>
                            updateAction(action.key, { track: event.target.value })
                          }
                        />
                      </div>
                    )}
                  </div>
                  <div className="rule-action__buttons">
                    <button
                      type="button"
                      aria-label={`Move action ${index + 1} up`}
                      disabled={index === 0}
                      onClick={() => moveAction(index, -1)}
                    >
                      Move up
                    </button>
                    <button
                      type="button"
                      aria-label={`Move action ${index + 1} down`}
                      disabled={index === actions.length - 1}
                      onClick={() => moveAction(index, 1)}
                    >
                      Move down
                    </button>
                    <button
                      type="button"
                      aria-label={`Remove action ${index + 1}`}
                      disabled={actions.length === 1}
                      onClick={() =>
                        setActions((current) => current.filter((entry) => entry.key !== action.key))
                      }
                    >
                      Remove
                    </button>
                  </div>
                </fieldset>
              </li>
            );
          })}
        </ol>
        <button type="button" onClick={() => setActions((current) => [...current, newAction()])}>
          Add action
        </button>
        {actions.some((action) =>
          devices.some((device) => device.id === action.deviceId && device.status !== 'ONLINE'),
        ) && <p>Offline devices may not respond when the routine runs.</p>}
        <div className="rule-form__buttons">
          <button type="submit">{pending ? 'Saving…' : 'Save routine'}</button>
          <button type="button" onClick={onCancel}>
            Cancel
          </button>
        </div>
      </fieldset>
      {error !== null && <p role="alert">{error}</p>}
    </form>
  );
}
