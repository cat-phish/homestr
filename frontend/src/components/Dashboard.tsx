import { useCallback, useEffect, useRef, useState } from 'react';

import { api, ApiError } from '@/api/client';
import { subscribe } from '@/api/websocket';
import { DeviceCard } from '@/components/DeviceCard';
import { RuleForm } from '@/components/RuleForm';
import type { AutomationRule, Device, ServerEvent } from '@/types';

export function Dashboard() {
  const [devices, setDevices] = useState<Device[]>([]);
  const [rules, setRules] = useState<AutomationRule[]>([]);
  const [error, setError] = useState<string | null>(null);

  const [message, setMessage] = useState<string | null>(null);
  const [creating, setCreating] = useState(false);
  const [loading, setLoading] = useState(true);
  const [pendingRule, setPendingRule] = useState<string | null>(null);
  const ruleBusy = useRef(false);

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
          setLoading(false);
        }
      })
      .catch(() => {
        if (!cancelled) {
          setError('Could not load the dashboard');
          setLoading(false);
        }
      });

    // Live updates: the hub pushes a frame whenever any device changes state,
    // including changes made by automation rules or another client.
    const unsubscribe = subscribe((event: ServerEvent) => {
      if (cancelled) {
        return;
      }
      if (event.type === 'snapshot') {
        setLoading(false);
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

  const handleToggleRule = async (rule: AutomationRule): Promise<void> => {
    if (ruleBusy.current) {
      return;
    }
    ruleBusy.current = true;
    setPendingRule(rule.id);
    setError(null);
    setMessage(null);
    try {
      const { enabled } = await api.setRuleEnabled(rule.id, !rule.enabled);
      setRules((current) =>
        current.map((entry) => (entry.id === rule.id ? { ...entry, enabled } : entry)),
      );
    } catch (cause) {
      setError(cause instanceof ApiError ? cause.message : `Could not toggle routine ${rule.name}`);
    } finally {
      ruleBusy.current = false;
      setPendingRule(null);
    }
  };

  const handleRunRule = async (rule: AutomationRule): Promise<void> => {
    if (ruleBusy.current || !rule.enabled) {
      return;
    }
    ruleBusy.current = true;
    setPendingRule(rule.id);
    setError(null);
    setMessage(null);
    try {
      await api.triggerRule(rule.id);
    } catch (cause) {
      setError(cause instanceof ApiError ? cause.message : `Could not run routine ${rule.name}`);
      ruleBusy.current = false;
      setPendingRule(null);
      return;
    }
    try {
      const dashboard = await api.getDashboard();
      setDevices(dashboard.devices);
      setRules(dashboard.rules);
      setMessage('Routine triggered. Review the device states above.');
    } catch {
      setError(
        'Routine triggered, but device states could not be refreshed. Refresh the dashboard to check them.',
      );
    } finally {
      ruleBusy.current = false;
      setPendingRule(null);
    }
  };

  const handleSavedRule = (rule: AutomationRule): void => {
    setRules((current) => [...current.filter((entry) => entry.id !== rule.id), rule]);
    setCreating(false);
    setError(null);
    setMessage('Routine saved. Use Run to trigger it.');
  };

  return (
    <main className="dashboard">
      <h2>Devices</h2>
      {error !== null && <p role="alert">{error}</p>}
      {message !== null && <p role="status">{message}</p>}
      {loading && <p role="status">Loading dashboard…</p>}
      {devices.length === 0 ? (
        <p>No devices registered yet.</p>
      ) : (
        <section className="dashboard__devices">
          {devices.map((device) => (
            <DeviceCard key={device.id} device={device} onCommand={handleCommand} />
          ))}
        </section>
      )}

      <section aria-labelledby="routines-title" className="dashboard__routines">
        <div className="dashboard__section-heading">
          <h2 id="routines-title">Automation routines</h2>
          {!creating && (
            <button
              type="button"
              disabled={loading || devices.length === 0 || pendingRule !== null}
              onClick={() => {
                setCreating(true);
                setMessage(null);
              }}
            >
              New routine
            </button>
          )}
        </div>
        {!loading && devices.length === 0 && <p>Add a device before creating a routine.</p>}
        {creating && (
          <RuleForm
            devices={devices}
            onSaved={handleSavedRule}
            onCancel={() => setCreating(false)}
          />
        )}
        {!loading && rules.length === 0 && <p>No routines yet.</p>}
        <ul className="dashboard__rules">
          {rules.map((rule) => (
            <li key={rule.id}>
              <div className="routine-summary">
                <strong>{rule.name}</strong>
                <small>
                  {rule.triggerType === 'MANUAL'
                    ? 'Manual'
                    : rule.triggerType === 'SCHEDULE'
                      ? 'Scheduled'
                      : 'Device state'}{' '}
                  · {rule.actions.length} {rule.actions.length === 1 ? 'action' : 'actions'} ·{' '}
                  {rule.enabled ? 'Enabled' : 'Disabled'}
                </small>
              </div>
              <div className="routine-buttons">
                {rule.triggerType === 'MANUAL' && (
                  <button
                    type="button"
                    aria-label={`Run ${rule.name}`}
                    disabled={!rule.enabled || pendingRule !== null}
                    onClick={() => void handleRunRule(rule)}
                  >
                    Run
                  </button>
                )}
                <button
                  type="button"
                  aria-label={`${rule.enabled ? 'Disable' : 'Enable'} ${rule.name}`}
                  disabled={pendingRule !== null}
                  onClick={() => void handleToggleRule(rule)}
                >
                  {pendingRule === rule.id ? 'Working…' : rule.enabled ? 'Disable' : 'Enable'}
                </button>
              </div>
            </li>
          ))}
        </ul>
      </section>
    </main>
  );
}
