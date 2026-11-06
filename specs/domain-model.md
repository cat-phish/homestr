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

### Device drivers

A `DeviceDriver` is the transport to one physical device. `devices.metadata`
names it: `{"driver": "hue", "lightId": "3"}`. A device with no driver (or one
that cannot be configured) stays simulated, as every device was before drivers
existed. `metadata` never holds credentials; those come from the environment.

- **Ordering**: the driver is called before the model changes, so the model
  only records state the device confirmed. A command the driver rejects
  (invalid params, unsupported) changes nothing. A transport or device-side
  failure sets `status = ERROR` and persists it; a later successful send sets
  `ONLINE`. For a driver-backed device the stored status therefore reflects the
  last contact, not a value typed at registration.
- **Audit**: the `device_events` row is written either way, as for simulated
  devices.
- **`hue`** (Philips Hue API v1 over plain HTTP; a real bridge or diyHue):
  - Config: `HUE_BRIDGE_URL`, `HUE_USERNAME` (the bridge whitelist key, a
    credential — never logged). Applies to `light` devices only.
  - `PUT /api/<username>/lights/<lightId>/state`, 2 s timeouts.
  - `turnOn` sends the full model state `{"on":true,"bri","xy"}`; `turnOff`
    sends `{"on":false}`.
  - `setBrightness` maps 0-100 onto `bri` 1-254; `setColor` converts `#RRGGBB`
    to CIE `xy`. With `metadata.color == false` (a dimmable white bulb) `xy`
    is never sent and `setColor` is rejected. Missing `color` means true. Hue rejects both while a light is off, so while off they are
    recorded in the model only and delivered by the next `turnOn`.
  - The bridge reports errors as HTTP 200 with `[{"error":{...}}]`; any error
    element is a failure.

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

1. An admin calls `GET /devices/discover` (the WebUI "Add Device" dialog).
2. `DeviceManager::discover()` asks each configured discovery source for
   candidates, outside the manager lock, then drops any that are already
   registered: same `id`, or same `metadata.driver` and `metadata.lightId` (so a
   light registered by hand under another id is not offered twice).
3. Candidates are not persisted. Registration posts a candidate back unchanged
   apart from its name, including its `metadata`, so the registered device gets
   its driver.
4. With no source configured, discovery returns an empty list. A source that
   fails (bridge unreachable, bad username) is logged and contributes nothing.

Sources:

- **Hue bridge** (when `HUE_BRIDGE_URL` and `HUE_USERNAME` are set):
  `GET /api/<username>/lights`. Each light with a brightness (`state.bri`)
  becomes a `light` candidate; on/off-only units such as Hue smart plugs are
  skipped until a driver supports them.
  - `id`: a deterministic UUID-shaped value from the light's `uniqueid`, so a
    rescan offers the same id.
  - `ipAddress`: the bridge host. `status`: `ONLINE` if `state.reachable`,
    otherwise `OFFLINE`. `attributes.brightness`: `state.bri` scaled to 0-100.
  - `metadata`: `{"driver":"hue","lightId":"<bridge id>","color":<bool>}`;
    `color` is true when the light reports `state.xy`.

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
   For a driver-backed device the driver is called first (see "Device
   drivers").
3. On success the controller broadcasts a `device_state` WebSocket frame, so
   every open WebUI reflects the change without polling.

### Routines in the WebUI

- Create a routine with a trimmed name (1–100 characters), client UUID, and at
  least one ordered action. Supported triggers are manual, scheduled, and device
  state. Existing command selection, parameters, action ordering, validation,
  pending-request protection, and draft retention on save errors apply to all.
- Scheduled routines select a local time, one or more weekdays, and an IANA
  timezone (initially the browser timezone). The form emits a five-field cron
  expression and `timezone`. The dashboard shows the next run in that timezone.
- Device-state routines select a registered source device, a supported property,
  and the value it must become. Supported UI properties are power, light
  brightness/color, speaker connection/volume/playback state. Values retain their
  JSON types; invalid/missing selections block saving.
- Manual routines retain Run controls. All types can be enabled/disabled. Save,
  toggle, and automatic execution refresh rule metadata for connected dashboards.
- Trigger confirmation does not assert that all actions succeeded. Refresh and
  command errors retain the existing feedback. Editing/deletion remain future work.

### Dashboard connection status

- The WebSocket client reconnects with backoff and reports each loss of the live
  connection to its caller. Closing on purpose (unmount) and session rejection
  (code 4001, REST 401) are not reported; those follow the sign-in flow.
- While the first connection is being established the dashboard shows the REST
  payload as current. After a loss, the dashboard shows a `role="status"`
  reconnecting indicator and marks every device card as potentially outdated
  (`data-stale`, visible "May be outdated" text). Controls stay available.
- Recovery is the `snapshot` frame the hub sends on every connect. It replaces the
  displayed devices and routines, so changes made while disconnected appear, and it
  clears the indicator and the outdated marks. An open socket without a snapshot
  does not count as recovered.

### Automatic trigger execution

- Schedules use five numeric cron fields: minute, hour, day-of-month, month,
  weekday (Sunday=0 through Saturday=6), with wildcards, lists, ranges and steps.
  Both day fields must match. Validate with croncpp. Timezone conversion uses the
  system IANA timezone database through date-tz, without changing process `TZ`.
  Legacy schedules without `timezone` use UTC.
- A server timer calls `HomeStr::tickSchedules(now)` every second. It executes
  only due occurrences in the current minute, never catches up missed runs, and
  recomputes future occurrences after a clock jump. New/enabled/restored rules
  are planned strictly after the current instant. Disabled rules do not run.
- Skip nonexistent spring-forward local times. At fall-back, run only the first
  occurrence of a repeated local minute. `nextRunAt` is an optional, nullable
  ISO-8601 UTC timestamp in schedule rule JSON; disabled schedules have null.
- Before dispatch, atomically claim the scheduled UTC minute in existing
  `automation_rules.last_fired_at`, conditioned on enabled state and prior claim.
  Restore this watermark at startup. This is at-most-once dispatch: a crash after
  claiming may leave actions incomplete; retries and replay are not automatic.
- A successful command emits before/after state only if a value actually changed,
  after releasing device/database locks. The hub broadcasts the new state and
  offers a DEVICE_STATE event to the engine. Failed/no-op commands and startup
  hydration do not fire state rules. Automation actions also emit changes.
- Equality conditions fire when the watched value enters the matching state,
  not on unrelated updates while it already matches. An API condition with no
  `equals` watches changes to its attribute (or any change if no attribute given).
  Validate source devices, supported properties, and comparison types on creation.
- Serialize command/trigger dispatch in the hub. Queue descendant state events
  instead of recursively executing them. Each rule fires at most once per root
  event chain, capped at 64 rules and 256 actions; remaining work is dropped with
  a diagnostic when capped. Independent root changes start fresh chains.
- Attribute automation commands to actor type RULE and its rule ID. Device
  commands/automatic updates use locked state snapshots instead of borrowed
  device pointers. Future hardware drivers must use this same change path;
  physical-device monitoring is still a separate integration task.

### Rule persistence

- `RuleRecord` stores the rule ID, household ID, name, enabled state, trigger
  type/payload, and ordered action list. Scheduled rule JSON additionally includes next-run metadata.
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
3. Matching actions dispatch through the hub and device manager with RULE actor
   attribution; resulting state events are queued within the same bounded chain.

---

## 3. Open items

Tracked here so the specs stay ahead of the code:

- LAN discovery of bridges and other device families (mDNS/SSDP). Hue lights
  are discovered from a bridge configured by hand; finding the bridge (SSDP or
  N-UPnP) and link-button pairing (`POST /api`) from the WebUI are not built.
- A failed discovery source is only logged; the WebUI cannot tell "nothing
  found" from "bridge unreachable".
- Registration does not carry the discovered power state, so a light that is on
  at registration shows as off until the first command.
- Driver calls run under the `DeviceManager` lock, so a slow device (up to the
  2 s timeouts) stalls other commands. Move I/O outside the lock.
- Driver-backed devices are not polled: a light switched at the wall, or a
  bridge that goes away between commands, is noticed only on the next command.
  `last_seen_at` is not written yet.
- A failed command that sets `ERROR` is not broadcast over the WebSocket.
- Per-household scoping from the session instead of the single default
  household in `main.cpp`.
- Per-user device assignment: the RBAC rule says a `User` controls *assigned*
  devices, but today any authenticated caller may command any device in the
  loaded household. Needs an assignment table plus a check in the controller.
