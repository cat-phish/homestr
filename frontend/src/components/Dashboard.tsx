import { useCallback, useEffect, useState } from 'react';

import { api } from '@/api/client';
import { subscribe } from '@/api/websocket';
import { DeviceCard } from '@/components/DeviceCard';
import type { AutomationRule, Device, ServerEvent } from '@/types';

export function Dashboard() {
  const [devices, setDevices] = useState<Device[]>([]);
  const [rules, setRules] = useState<AutomationRule[]>([]);
  const [error, setError] = useState<string | null>(null);

  const replaceDevice = useCallback((updated: Device): void => {
    setDevices((current) => current.map((device) => (device.id === updated.id ? updated : device)));
  }, []);

  useEffect(() => {
    let cancelled = false;

    api
      .getDashboard()
      .then((dashboard) => {
        if (!cancelled) {
          setDevices(dashboard.devices);
          setRules(dashboard.rules);
        }
      })
      .catch(() => {
        if (!cancelled) {
          setError('Could not load the dashboard');
        }
      });

    // Live updates: the hub pushes a frame whenever any device changes state,
    // including changes made by automation rules or another client.
    const unsubscribe = subscribe((event: ServerEvent) => {
      if (event.type === 'snapshot') {
        setDevices(event.payload.devices);
        setRules(event.payload.rules);
      } else if (event.type === 'device_state') {
        replaceDevice(event.device);
      }
    });

    return () => {
      cancelled = true;
      unsubscribe();
    };
  }, [replaceDevice]);

  const handleCommand = useCallback(
    (deviceId: string, command: string, params: Record<string, unknown> = {}): void => {
      api
        .sendCommand(deviceId, command, params)
        .then(replaceDevice)
        .catch(() => setError(`Command ${command} failed`));
    },
    [replaceDevice],
  );

  const handleToggleRule = useCallback((rule: AutomationRule): void => {
    api
      .setRuleEnabled(rule.id, !rule.enabled)
      .then(({ enabled }) =>
        setRules((current) =>
          current.map((entry) => (entry.id === rule.id ? { ...entry, enabled } : entry)),
        ),
      )
      .catch(() => setError(`Could not toggle rule ${rule.name}`));
  }, []);

  return (
    <main className="dashboard">
      <h2>Devices</h2>
      {error !== null && <p role="alert">{error}</p>}
      {devices.length === 0 ? (
        <p>No devices registered yet.</p>
      ) : (
        <section className="dashboard__devices">
          {devices.map((device) => (
            <DeviceCard key={device.id} device={device} onCommand={handleCommand} />
          ))}
        </section>
      )}

      <h2>Automation rules</h2>
      <ul className="dashboard__rules">
        {rules.map((rule) => (
          <li key={rule.id}>
            <span>{rule.name}</span>
            <button type="button" onClick={() => handleToggleRule(rule)}>
              {rule.enabled ? 'Disable' : 'Enable'}
            </button>
          </li>
        ))}
      </ul>
    </main>
  );
}
