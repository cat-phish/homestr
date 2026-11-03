import { useRef, useState } from 'react';

import { api, ApiError } from '@/api/client';
import type { AutomationRule, Device, RuleAction, TriggerType } from '@/types';
import { createId } from '@/utils/id';
import { triggerProperties } from '@/utils/triggers';

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
  const [triggerType, setTriggerType] = useState<TriggerType>('MANUAL');
  const [time, setTime] = useState('22:00');
  const [timezone, setTimezone] = useState(
    () => Intl.DateTimeFormat().resolvedOptions().timeZone || 'UTC',
  );
  const [days, setDays] = useState([0, 1, 2, 3, 4, 5, 6]);
  const [sourceId, setSourceId] = useState('');
  const [attribute, setAttribute] = useState('powerOn');
  const [condition, setCondition] = useState('true');
  const source = devices.find((device) => device.id === sourceId);
  const properties = triggerProperties(source);
  const property = properties.find((entry) => entry.value === attribute);
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
    let trigger: Record<string, unknown> = {};
    if (triggerType === 'SCHEDULE') {
      if (!/^([01]\d|2[0-3]):[0-5]\d$/.test(time)) {
        setError('Choose a valid time.');
        return;
      }
      try {
        new Intl.DateTimeFormat(undefined, { timeZone: timezone.trim() }).format();
      } catch {
        setError('Enter a valid timezone, such as America/Los_Angeles or UTC.');
        return;
      }
      if (timezone.trim() === '') {
        setError('Enter a valid timezone.');
        return;
      }
      if (days.length === 0) {
        setError('Select at least one day.');
        return;
      }
      const [hour, minute] = time.split(':');
      trigger = {
        cron: `${Number(minute)} ${Number(hour)} * * ${days.length === 7 ? '*' : [...days].sort().join(',')}`,
        timezone: timezone.trim(),
      };
    } else if (triggerType === 'DEVICE_STATE') {
      if (source === undefined || property === undefined) {
        setError('Select a trigger device and property.');
        return;
      }
      let equals: string | number | boolean = condition;
      if (property.kind === 'boolean') {
        equals = condition === 'true';
      }
      if (property.kind === 'number') {
        const number = Number(condition);
        if (condition.trim() === '' || !Number.isInteger(number) || number < 0 || number > 100) {
          setError('The condition must be a whole number from 0 to 100.');
          return;
        }
        equals = number;
      }
      if (property.kind === 'color' && !/^#[0-9a-f]{6}$/i.test(condition)) {
        setError('Choose a valid condition color.');
        return;
      }
      trigger = { deviceId: sourceId, attribute, equals };
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
        triggerType,
        trigger,
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
      <p>
        Choose when this routine runs and which actions it performs. Actions run in the order shown.
      </p>
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
        <div className="rule-form__trigger">
          <label htmlFor="trigger-type">When to run</label>
          <select
            id="trigger-type"
            value={triggerType}
            onChange={(event) => setTriggerType(event.target.value as TriggerType)}
          >
            <option value="MANUAL">Manually</option>
            <option value="SCHEDULE">On a schedule</option>
            <option value="DEVICE_STATE">When a device changes</option>
          </select>
          {triggerType === 'SCHEDULE' && (
            <div className="trigger-settings">
              <div>
                <label htmlFor="schedule-time">Time</label>
                <input
                  id="schedule-time"
                  type="time"
                  required
                  value={time}
                  onChange={(event) => setTime(event.target.value)}
                />
              </div>
              <div>
                <label htmlFor="schedule-timezone">Timezone</label>
                <input
                  id="schedule-timezone"
                  required
                  value={timezone}
                  onChange={(event) => setTimezone(event.target.value)}
                  placeholder="America/Los_Angeles"
                />
                <small>Uses this timezone. Missed runs are skipped.</small>
              </div>
              <fieldset className="schedule-days">
                <legend>Days</legend>
                {['Sunday', 'Monday', 'Tuesday', 'Wednesday', 'Thursday', 'Friday', 'Saturday'].map(
                  (day, index) => (
                    <label key={day}>
                      <input
                        type="checkbox"
                        checked={days.includes(index)}
                        onChange={(event) =>
                          setDays((current) =>
                            event.target.checked
                              ? [...current, index]
                              : current.filter((entry) => entry !== index),
                          )
                        }
                      />
                      {day}
                    </label>
                  ),
                )}
              </fieldset>
            </div>
          )}
          {triggerType === 'DEVICE_STATE' && (
            <div className="trigger-settings">
              <div>
                <label htmlFor="trigger-device">Trigger device</label>
                <select
                  id="trigger-device"
                  value={sourceId}
                  required
                  onChange={(event) => {
                    setSourceId(event.target.value);
                    setAttribute('powerOn');
                    setCondition('true');
                  }}
                >
                  <option value="">Select a device</option>
                  {devices.map((device) => (
                    <option key={device.id} value={device.id}>
                      {device.name}
                    </option>
                  ))}
                </select>
              </div>
              <div>
                <label htmlFor="trigger-property">Property</label>
                <select
                  id="trigger-property"
                  disabled={source === undefined}
                  value={attribute}
                  onChange={(event) => {
                    const selected = properties.find((entry) => entry.value === event.target.value);
                    setAttribute(event.target.value);
                    setCondition(
                      selected?.kind === 'number'
                        ? '50'
                        : selected?.kind === 'color'
                          ? '#ffffff'
                          : selected?.kind === 'playback'
                            ? 'PLAYING'
                            : 'true',
                    );
                  }}
                >
                  {properties.map((entry) => (
                    <option key={entry.value} value={entry.value}>
                      {entry.label}
                    </option>
                  ))}
                </select>
              </div>
              <div>
                <label htmlFor="trigger-condition">Becomes</label>
                {property?.kind === 'boolean' ? (
                  <select
                    id="trigger-condition"
                    value={condition}
                    onChange={(event) => setCondition(event.target.value)}
                  >
                    <option value="true">{attribute === 'connected' ? 'Connected' : 'On'}</option>
                    <option value="false">
                      {attribute === 'connected' ? 'Disconnected' : 'Off'}
                    </option>
                  </select>
                ) : property?.kind === 'playback' ? (
                  <select
                    id="trigger-condition"
                    value={condition}
                    onChange={(event) => setCondition(event.target.value)}
                  >
                    <option value="PLAYING">Playing</option>
                    <option value="PAUSED">Paused</option>
                    <option value="STOPPED">Stopped</option>
                  </select>
                ) : (
                  <input
                    id="trigger-condition"
                    type={property?.kind === 'color' ? 'color' : 'number'}
                    min={0}
                    max={100}
                    step={1}
                    required
                    value={condition}
                    onChange={(event) => setCondition(event.target.value)}
                  />
                )}
              </div>
              <p>Runs when this property changes into the selected value.</p>
            </div>
          )}
        </div>
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
