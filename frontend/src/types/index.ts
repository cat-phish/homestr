/**
 * Transport types mirroring the backend payloads in specs/api-spec.yaml.
 * Change the spec first, then these types, then the components (SDD).
 */

export type Role = 'user' | 'admin';

export type DeviceStatus = 'ONLINE' | 'OFFLINE' | 'ERROR';

export type DeviceType = 'light' | 'speaker' | 'generic';

export type PlaybackState = 'PLAYING' | 'PAUSED' | 'STOPPED';

export type TriggerType = 'SCHEDULE' | 'DEVICE_STATE' | 'MANUAL';

export interface User {
  id: string;
  name: string;
  email: string;
  role: Role;
}

export interface LoginResponse {
  token: string;
  user: User;
}

interface DeviceBase {
  id: string;
  name: string;
  ipAddress: string;
  status: DeviceStatus;
  powerOn: boolean;
}

export interface Light extends DeviceBase {
  type: 'light';
  brightness: number;
  color: string;
}

export interface SmartSpeaker extends DeviceBase {
  type: 'speaker';
  connected: boolean;
  volume: number;
  playbackState: PlaybackState;
  currentTrack: string;
}

export interface GenericDevice extends DeviceBase {
  type: 'generic';
}

/** Discriminated on `type`, matching SmartDevice::type() in the backend. */
export type Device = Light | SmartSpeaker | GenericDevice;

export interface RuleAction {
  deviceId: string;
  command: string;
  params: Record<string, unknown>;
}

export interface AutomationRule {
  id: string;
  name: string;
  enabled: boolean;
  triggerType: TriggerType;
  trigger: Record<string, unknown>;
  actions: RuleAction[];
}

export interface Dashboard {
  householdId: string;
  devices: Device[];
  rules: AutomationRule[];
}

export interface DeviceRegistration {
  id: string;
  householdId: string;
  name: string;
  ipAddress: string;
  type: DeviceType;
  status?: DeviceStatus;
  attributes?: Record<string, unknown>;
}

/** Frames pushed over /api/v1/ws. */
export type ServerEvent =
  | { type: 'snapshot'; payload: Dashboard }
  | { type: 'device_state'; device: Device }
  | { type: 'pong' };

export const isLight = (device: Device): device is Light => device.type === 'light';

export const isSpeaker = (device: Device): device is SmartSpeaker => device.type === 'speaker';
