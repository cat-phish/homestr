import { useEffect, useState } from 'react';
import { api, ApiError } from '@/api/client';
import type { Device, DeviceRegistration, DiscoveredDevice } from '@/types';

interface Props {
  householdId: string;
  registeredDeviceIds: Set<string>;
  onDeviceRegistered: (device: Device) => void;
  onClose: () => void;
}

export function DeviceDiscovery({
  householdId,
  registeredDeviceIds,
  onDeviceRegistered,
  onClose,
}: Props) {
  const [discovered, setDiscovered] = useState<DiscoveredDevice[]>([]);
  const [loading, setLoading] = useState(false);
  const [scanning, setScanning] = useState(false);
  const [registering, setRegistering] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [customNames, setCustomNames] = useState<Record<string, string>>({});

  const handleScan = async (): Promise<void> => {
    setScanning(true);
    setError(null);
    try {
      const results = await api.discover();
      const available = results.filter((device) => !registeredDeviceIds.has(device.id));
      setDiscovered(available);
      if (available.length === 0) {
        setError('No new devices found on your network.');
      }
    } catch (cause) {
      setError(cause instanceof ApiError ? cause.message : 'Could not scan for devices');
    } finally {
      setScanning(false);
    }
  };

  const handleRegister = async (device: DiscoveredDevice): Promise<void> => {
    setRegistering(device.id);
    setError(null);
    try {
      const name = customNames[device.id] || device.name;
      if (!name || name.trim().length === 0) {
        setError('Please enter a device name');
        setRegistering(null);
        return;
      }

      const registration: DeviceRegistration = {
        id: device.id,
        householdId,
        name: name.trim(),
        ipAddress: device.ipAddress,
        type: device.type,
        status: device.status || 'ONLINE',
        attributes: device.attributes || {},
        // Driver configuration (e.g. which Hue light) goes back unchanged.
        ...(device.metadata !== undefined && { metadata: device.metadata }),
      };

      const result = await api.registerDevice(registration);
      const registered = await api.getDevice(result.registered);
      onDeviceRegistered(registered);

      setDiscovered((current) => current.filter((d) => d.id !== device.id));
      setCustomNames((current) => {
        const updated = { ...current };
        delete updated[device.id];
        return updated;
      });
    } catch (cause) {
      setError(cause instanceof ApiError ? cause.message : `Could not register ${device.name}`);
    } finally {
      setRegistering(null);
    }
  };

  useEffect(() => {
    setLoading(true);
    void handleScan().finally(() => setLoading(false));
  }, []);

  return (
    <dialog open className="device-discovery">
      <div className="device-discovery__header">
        <h2>Add Device</h2>
        <button type="button" aria-label="Close" onClick={onClose}>
          ×
        </button>
      </div>

      {error && <p role="alert">{error}</p>}

      {discovered.length === 0 && !scanning && !loading && (
        <div className="device-discovery__empty">
          <p>No new devices found.</p>
          <button type="button" disabled={scanning} onClick={() => void handleScan()}>
            {scanning ? 'Scanning…' : 'Scan Again'}
          </button>
        </div>
      )}

      {discovered.length > 0 && (
        <ul className="device-discovery__list">
          {discovered.map((device) => (
            <li key={device.id} className="device-discovery__item">
              <div className="device-discovery__info">
                <label>
                  <small className="device-discovery__label">{device.name}</small>
                  <input
                    type="text"
                    placeholder="Device name"
                    defaultValue={device.name}
                    onChange={(e) =>
                      setCustomNames((current) => ({ ...current, [device.id]: e.target.value }))
                    }
                    disabled={registering === device.id}
                  />
                </label>
                <small>{device.ipAddress}</small>
              </div>
              <button
                type="button"
                disabled={registering !== null || scanning}
                onClick={() => void handleRegister(device)}
              >
                {registering === device.id ? 'Adding…' : 'Add'}
              </button>
            </li>
          ))}
        </ul>
      )}

      {(loading || scanning) && <p role="status">Scanning for devices…</p>}

      <div className="device-discovery__footer">
        <button type="button" onClick={onClose} disabled={scanning}>
          Done
        </button>
      </div>
    </dialog>
  );
}
