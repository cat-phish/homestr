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

### Authentication and sessions

- Passwords are hashed with libsodium Argon2id; hashes never appear in responses.
- A session contains a SHA-256 token hash, user ID, and absolute expiry. Tokens
  contain 32 random bytes encoded as hex and expire after 24 hours; only their
  hashes are persisted. Expired or revoked tokens cannot authenticate.
- All `/api/v1` routes except `/auth/login` require a session. `/healthz` and
  CORS preflight remain public. Device registration, removal, and discovery
  require the administrator role. Direct commands record the resolved user ID.
- `GET /auth/me` validates the session and returns the current user. Logout
  deletes the current session and closes its WebSocket connections.
- Browser WebSockets offer `homestr` and `bearer.<token>` subprotocols. The server
  selects only `homestr`; credentials never enter URLs. Non-browser clients may
  use the Authorization header. Validate before sending any snapshot, before
  each push/message, and every second while idle; close invalid sessions with
  code 4001. Tokens are never echoed or logged.
- The frontend validates a stored token before showing the dashboard. A protected
  REST 401 or a WebSocket 4001 clears authentication and returns to sign-in.
- `homestr_server --set-admin-password` interactively sets the seeded admin's
  password (8–1024 bytes), also on existing databases, and revokes that user's
  sessions. There is no shipped default password; unconfigured accounts cannot
  sign in. The command does not start the HTTP server.
- Database operations serialize access to each shared connection. Password
  updates and session revocation are atomic. Session issuance checks that the
  stored password hash still matches the verified hash, so an in-flight login
  cannot bypass a password reset.

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
    `deleteDevice(id)`, `listRules(householdId)`, `saveRule(record)`,
    `deleteRule(householdId, id)`, `recordEvent(...)`, `updatePasswordHash(email, hash)`,
    `saveSession(record)`, `findSessionUser(tokenHash, now)`, `deleteSession(tokenHash)`
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

### Manual routines in the WebUI

- The dashboard offers a form for creating manual routines with a trimmed name
  (1–100 characters), a client-generated UUID, and at least one ordered action.
  New routines are enabled, use `MANUAL`, and have an empty trigger payload.
- Each action selects a registered device and a command supported by its type.
  Brightness and volume require an integer from 0–100; color requires `#RRGGBB`;
  playback may include an optional track. Users can add, remove, and reorder
  actions before saving. Unavailable selections and invalid values block saving.
- Save uses the existing rule endpoint and publishes the returned rule only
  after success. Failed saves retain the draft and show a useful error. Cancel
  discards the draft. Pending saves disable repeat submissions and draft changes.
- Without registered devices the dashboard explains why creation is unavailable.
  Offline devices can be configured, with a note that they may not respond.
- Enabled manual routines have a Run button, guarded against repeated clicks
  while pending. Disabled routines cannot run. Other trigger types retain their
  enable/disable controls but are not offered manual Run buttons.
- After a manual trigger the initiating dashboard refreshes device state from
  the hub. The current endpoint confirms a rule was triggered, not that every
  action succeeded; feedback must not claim all actions completed successfully.
  Trigger and refresh failures are shown separately. Creation and execution use
  the existing authenticated API and introduce no transport or schema changes.
- Editing/deleting saved rules and automatic schedule/device-state execution
  remain separate backlog work.

### Rule persistence

- `RuleRecord` stores the rule ID, household ID, name, enabled state, trigger
  type/payload, and ordered action list. The public rule JSON stays unchanged.
- `Database::listRules(householdId)` returns only that household's complete
  records, ordered by rule ID and action sequence. An empty list is successful;
  a read failure is distinct and prevents hub startup with an incomplete list.
- `saveRule(record)` atomically saves the rule and replaces its ordered actions.
  A failed write rolls back the entire change. A rule ID belonging to a different
  household cannot be overwritten. `deleteRule(householdId, id)` deletes only a
  matching household's rule, with action deletion cascading from the schema.
- Startup loads persisted rules as well as devices. Repeated startup replaces
  the rule snapshot rather than appending duplicates. Enabled and disabled rules,
  empty action lists, trigger payloads, and action parameters survive restart.
- Creation and enable/disable changes are persisted before the in-memory
  snapshot changes. Duplicate creation returns 409, an unknown rule returns 404,
  and persistence failures return 503 without reporting a successful change.
- Rule reads return a snapshot by value; mutations are synchronized so concurrent
  creates/toggles cannot race with dashboard reads or execution.

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

- Real mDNS/SSDP discovery; `discover()` returns the cache today.
- Cron evaluation for `SCHEDULE` triggers: the rule matches a tick that the
  scheduler has already resolved, and the scheduler itself is not implemented.
- Per-household scoping from the session instead of the single default
  household in `main.cpp`.
- Per-user device assignment: the RBAC rule says a `User` controls *assigned*
  devices, but today any authenticated caller may command any device in the
  loaded household. Needs an assignment table plus a check in the controller.
