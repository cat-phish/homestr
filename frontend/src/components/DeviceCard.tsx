import { type Device, isLight, isSpeaker } from '@/types';

interface DeviceCardProps {
  device: Device;
  /** Dispatches a command; the parent owns the optimistic/refresh behaviour. */
  onCommand: (deviceId: string, command: string, params?: Record<string, unknown>) => void;
}

export function DeviceCard({ device, onCommand }: DeviceCardProps) {
  const offline = device.status !== 'ONLINE';

  return (
    <article className="device-card" aria-label={device.name} data-status={device.status}>
      <header>
        <h3>{device.name}</h3>
        <span className="device-card__status">{device.status}</span>
      </header>

      <dl>
        <dt>Address</dt>
        <dd>{device.ipAddress}</dd>
        <dt>Type</dt>
        <dd>{device.type}</dd>
      </dl>

      <div className="device-card__controls">
        <button
          type="button"
          disabled={offline}
          onClick={() => onCommand(device.id, device.powerOn ? 'turnOff' : 'turnOn')}
        >
          {device.powerOn ? 'Turn off' : 'Turn on'}
        </button>

        {isLight(device) && (
          <>
            <label htmlFor={`brightness-${device.id}`}>Brightness</label>
            <input
              id={`brightness-${device.id}`}
              type="range"
              min={0}
              max={100}
              step={1}
              value={device.brightness}
              disabled={offline}
              onChange={(event) =>
                onCommand(device.id, 'setBrightness', { level: Number(event.target.value) })
              }
            />
            <label htmlFor={`color-${device.id}`}>Colour</label>
            <input
              id={`color-${device.id}`}
              type="color"
              value={device.color}
              disabled={offline}
              onChange={(event) => onCommand(device.id, 'setColor', { color: event.target.value })}
            />
          </>
        )}

        {isSpeaker(device) && (
          <>
            <button
              type="button"
              disabled={offline || !device.connected}
              onClick={() =>
                onCommand(device.id, device.playbackState === 'PLAYING' ? 'pause' : 'play')
              }
            >
              {device.playbackState === 'PLAYING' ? 'Pause' : 'Play'}
            </button>
            <label htmlFor={`volume-${device.id}`}>Volume</label>
            <input
              id={`volume-${device.id}`}
              type="range"
              min={0}
              max={100}
              step={1}
              value={device.volume}
              disabled={offline || !device.connected}
              onChange={(event) =>
                onCommand(device.id, 'setVolume', { level: Number(event.target.value) })
              }
            />
          </>
        )}
      </div>
    </article>
  );
}
