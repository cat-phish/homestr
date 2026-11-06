import { useState } from 'react';

import { type Device, isLight, isSpeaker } from '@/types';

interface DeviceCardProps {
  device: Device;
  /** True while the live connection is down, so the shown state may be out of date. */
  stale?: boolean;
  /** Dispatches a command; the parent owns the optimistic/refresh behaviour. */
  onCommand: (deviceId: string, command: string, params?: Record<string, unknown>) => void;
  /** Admin only: omit to hide the Rename action. */
  onRename?: (deviceId: string, name: string) => void;
  /** Admin only: omit to hide the Remove action. Called after the user confirms. */
  onRemove?: (deviceId: string) => void;
}

export function DeviceCard({
  device,
  stale = false,
  onCommand,
  onRename,
  onRemove,
}: DeviceCardProps) {
  const offline = device.status !== 'ONLINE';
  const [renaming, setRenaming] = useState(false);
  const [draft, setDraft] = useState(device.name);

  const saveName = (): void => {
    const name = draft.trim();
    if (name.length > 0 && onRename) {
      if (name !== device.name) {
        onRename(device.id, name);
      }
      setRenaming(false);
    }
  };

  const confirmRemove = (): void => {
    if (
      onRemove &&
      window.confirm(`Remove "${device.name}"? Routines that use it lose those actions.`)
    ) {
      onRemove(device.id);
    }
  };

  return (
    <article
      className="device-card"
      aria-label={device.name}
      data-status={device.status}
      data-stale={stale ? 'true' : undefined}
    >
      <header>
        <h3>{device.name}</h3>
        <span className="device-card__status">{device.status}</span>
      </header>
      {stale && <small className="device-card__stale">May be outdated</small>}

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

      {(onRename !== undefined || onRemove !== undefined) && (
        <div className="device-card__admin">
          {renaming ? (
            <form
              className="device-card__rename"
              onSubmit={(event) => {
                event.preventDefault();
                saveName();
              }}
            >
              <label htmlFor={`name-${device.id}`}>Name</label>
              <input
                id={`name-${device.id}`}
                type="text"
                value={draft}
                maxLength={100}
                autoFocus
                onChange={(event) => setDraft(event.target.value)}
              />
              <button type="submit" disabled={draft.trim().length === 0}>
                Save
              </button>
              <button type="button" onClick={() => setRenaming(false)}>
                Cancel
              </button>
            </form>
          ) : (
            <>
              {onRename !== undefined && (
                <button
                  type="button"
                  aria-label={`Rename ${device.name}`}
                  onClick={() => {
                    setDraft(device.name);
                    setRenaming(true);
                  }}
                >
                  Rename
                </button>
              )}
              {onRemove !== undefined && (
                <button
                  type="button"
                  className="device-card__remove"
                  aria-label={`Remove ${device.name}`}
                  onClick={confirmRemove}
                >
                  Remove
                </button>
              )}
            </>
          )}
        </div>
      )}
    </article>
  );
}
