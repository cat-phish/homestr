# HomeStr Domain Model Specification

Source of truth for the class model. The MariaDB schema
(`database/init.sql`), the C++ headers (`backend/include/homestr/`) and the
TypeScript transport types (`frontend/src/types/index.ts`) all derive from this
document; change it first.

---

## 1. Entities & Inheritance

### User & Admin

- **User**:
  - Attributes: `id: UUID`, `name: string`, `email: string`,
    `password_hash: string`, `role: enum('user', 'admin')`
  - Operations: `authenticate()`, `login()`, `logout()`, `getDashboard()`,
    `controlDevice(deviceId, command)`, `createRule(ruleConfig)`
- **Admin** (Specialization of User):
  - Operations: `addDevice(deviceConfig)`, `removeDevice(deviceId)`,
    `addUser(email, password, role)`, `removeUser(userId)`

> **Implementation note.** The UML specialisation is collapsed into the `role`
> discriminator in both the schema (`users.role`) and C++ (`User::isAdmin()`).
> The two classes differ only in which operations are permitted, so a subclass
> would add a hierarchy without adding state. Authorisation is enforced in the
> controllers; the admin-only endpoints are marked `403` in
> `specs/api-spec.yaml`.

### Device Hierarchy

- **SmartDevice** (abstract base):
  - Attributes: `id: UUID`, `name: string`, `ipAddress: string`,
    `status: enum('ONLINE', 'OFFLINE', 'ERROR')`, `powerOn: bool`
  - Operations: `turnOn()`, `turnOff()`, `getStatus()`, `getIp()`, and the pure
    virtual `type()` discriminator, so only a concrete subclass can be
    instantiated.
  - Invariant: every command fails while `status != ONLINE`. Commands return
    `false` rather than throwing, so the caller can record the failed attempt.
  - Every concrete subclass serialises its own state by extending the object
    returned by `SmartDevice::toJson()`.
- **Light** (Extends SmartDevice):
  - Attributes: `brightness: int (0-100)`, `color: string (#RRGGBB)`
  - Operations: `setBrightness(level: int)` (clamps to range),
    `setColor(hex: string)` (rejects a malformed value, leaving the colour
    unchanged)
- **SmartSpeaker** (Extends SmartDevice):
  - Attributes: `connected: bool`, `volume: int (0-100)`,
    `playbackState: enum('PLAYING', 'PAUSED', 'STOPPED')`, `currentTrack: string`
  - Operations: `connect()`, `play(track?)`, `pause()`, `setVolume(level: int)`
  - Invariant: transport commands require `connected == true`; `turnOff()`
    drops the connection and stops playback.
- **GenericDevice** (Extends SmartDevice):
  - The concrete stand-in for `device_type = 'generic'`: power on/off only, no
    added attributes. It exists because `SmartDevice` is abstract.

Persistence uses class-table inheritance: `devices` holds the base attributes,
`device_light` and `device_speaker` hold the subclass attributes keyed 1:1 on
`devices.id`. `devices.device_type` is the discriminator used to rebuild the
right C++ subclass (`DeviceManager::materialise`).

### Households

- **Household**:
  - Attributes: `id: UUID`, `name: string`
  - Relations: many-to-many with `User` through `household_members`
    (`member_role: enum('owner','member')`); one-to-many with `SmartDevice`
    (`devices.household_id`, `NULL` while a device is discovered but not yet
    registered)

### Aggregates & System Control

- **HomeStr** (central controller):
  - Owns the `Database` and the `DeviceManager`, holds the loaded
    `AutomationRule` list
  - Operations: `start(householdId)`, `handleEvent(event)`, `dashboard()`,
    `addRule(rule)`, `setRuleEnabled(ruleId, enabled)`
- **DeviceManager**:
  - Maintains `devices: List<SmartDevice>`
  - Operations: `discover()`, `registerDevice(SmartDevice)`,
    `unregisterDevice(UUID)`, `findById(UUID)`, `loadHousehold(UUID)`,
    `sendCommand(deviceId, command, params, actorId)`, `snapshot()`
  - Thread-safe: the HTTP handlers, the WebSocket broadcaster and the automation
    engine all call it concurrently.
- **Database**:
  - Abstract persistence boundary. `MariaDbDatabase` uses MariaDB
    Connector/C++; `InMemoryDatabase` backs the unit tests.
  - Operations: `connect()`, `findUserByEmail(email)`,
    `listDevices(householdId)`, `findDevice(id)`, `saveDevice(record)`,
    `deleteDevice(id)`, `recordEvent(...)`
- **AutomationRule**:
  - Attributes: `id: UUID`, `name: string`, `enabled: bool`,
    `triggerType: enum('SCHEDULE','DEVICE_STATE','MANUAL')`,
    `trigger: json`, `actions: List<RuleAction>`
  - Operations: `evaluate(event)`, `execute()`, `enable()`, `disable()`
  - Invariant: a disabled rule never matches and `execute()` yields no actions.
- **RuleAction**: `deviceId: UUID`, `command: string`, `params: json`,
  ordered by `automation_actions.sequence_no`.

---

## 2. Workflows

### Device discovery

1. An admin calls `GET /devices/discover`.
2. `DeviceManager::discover()` sweeps the LAN (mDNS + SSDP) and returns the
   candidates that are not yet in `devices`.
3. Candidates are not persisted: they live only in the discovery cache until
   registration.

### Device registration

1. An admin posts the candidate to `POST /devices` with a `householdId`.
2. `DeviceManager::registerDevice()` rejects an id that is already registered (`409`),
   otherwise writes `devices` plus the matching subtype row and materialises the
   C++ object.
3. The device appears in the next dashboard payload and WebSocket snapshot.

### Command dispatch

1. A user posts to `POST /devices/{id}/commands`.
2. `DeviceManager::sendCommand()` resolves the device, applies the command to
   the live object, then — outside the lock — persists the new state and appends
   a `device_events` row. The audit row is written whether or not the command
   succeeded.
3. On success the controller broadcasts a `device_state` WebSocket frame, so
   every open WebUI reflects the change without polling.

### Automation

1. The scheduler (`SCHEDULE`) or a device state change (`DEVICE_STATE`) produces
   a `TriggerEvent`.
2. `HomeStr::handleEvent()` offers the event to every rule; each rule's
   `evaluate()` decides for itself (disabled rules decline).
3. Matching rules' actions are dispatched through `DeviceManager::sendCommand`
   with an empty actor id, so the audit trail attributes them to the rule.

---

## 3. Open items

Tracked here so the specs stay ahead of the code:

- Password hashing and bearer-token verification (`sessions` table exists;
  `POST /auth/login` currently only proves the user exists).
- Real mDNS/SSDP discovery; `discover()` returns the cache today.
- Cron evaluation for `SCHEDULE` triggers: the rule matches a tick that the
  scheduler has already resolved, and the scheduler itself is not implemented.
- Per-household scoping from the session instead of the single default
  household in `main.cpp`.
- Automation rule persistence: `automation_rules` and `automation_actions` exist
  in the schema, but `HomeStr` keeps rules in memory only, so they are lost on
  restart. Needs `Database` read/write methods plus a load in `start()`.
- Per-user device assignment: the RBAC rule says a `User` controls *assigned*
  devices, but today any authenticated caller may command any device in the
  loaded household. Needs an assignment table plus a check in the controller.
