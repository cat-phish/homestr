import type { AutomationRule, Device } from '@/types';

export interface TriggerProperty {
  value: string;
  label: string;
  kind: 'boolean' | 'number' | 'color' | 'playback';
}

export function triggerProperties(device: Device | undefined): TriggerProperty[] {
  const power: TriggerProperty = { value: 'powerOn', label: 'Power', kind: 'boolean' };
  if (device?.type === 'light') {
    return [
      power,
      { value: 'brightness', label: 'Brightness', kind: 'number' },
      { value: 'color', label: 'Color', kind: 'color' },
    ];
  }
  if (device?.type === 'speaker') {
    return [
      power,
      { value: 'connected', label: 'Connection', kind: 'boolean' },
      { value: 'volume', label: 'Volume', kind: 'number' },
      { value: 'playbackState', label: 'Playback', kind: 'playback' },
    ];
  }
  return [power];
}

export function nextRunLabel(rule: AutomationRule): string {
  if (!rule.enabled) {
    return 'Schedule paused';
  }
  if (!rule.nextRunAt) {
    return 'No next run available';
  }
  const timezone = typeof rule.trigger.timezone === 'string' ? rule.trigger.timezone : 'UTC';
  try {
    const formatted = new Intl.DateTimeFormat(undefined, {
      timeZone: timezone,
      month: 'short',
      day: 'numeric',
      hour: 'numeric',
      minute: '2-digit',
    }).format(new Date(rule.nextRunAt));
    return `Next run: ${formatted} (${timezone})`;
  } catch {
    return 'No next run available';
  }
}

export function stateTriggerLabel(rule: AutomationRule, devices: Device[]): string {
  const source = devices.find((device) => device.id === rule.trigger.deviceId);
  const property = triggerProperties(source).find(
    (entry) => entry.value === rule.trigger.attribute,
  );
  const expected = rule.trigger.equals;
  let value = typeof expected === 'string' || typeof expected === 'number' ? String(expected) : '';
  if (typeof expected === 'boolean') {
    value =
      rule.trigger.attribute === 'connected'
        ? expected
          ? 'connected'
          : 'disconnected'
        : expected
          ? 'on'
          : 'off';
  }
  return `When ${source?.name ?? 'unavailable device'} ${property?.label.toLowerCase() ?? 'state'} ${value === '' ? 'changes' : `becomes ${value}`}`;
}
