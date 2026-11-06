# HomeStr Roadmap

Where the project stands and what to build next. Keep this file current: it is
the entry point for anyone (or any agent) picking the project back up.

Companion documents: [README.md](README.md) for setup and testing,
[CLAUDE.md](CLAUDE.md) for conventions, [specs/](specs/) for the contracts that
changes must go through first.

---

## Current state

As of 2026-11-05, after authentication, persisted routines, automatic triggers,
and the first device driver (Hue, proven on a physical bridge).

The architecture, schema, build, container stack, tests and CI are in place and
the stack runs end to end. It is best understood as **a working simulator of
the architecture, not a partially-working product**.

The gap that matters most, and which the stub list understates: for every
device except a Hue light registered with `{"driver":"hue"}` metadata,
`DeviceManager::sendCommand` mutates an in-memory object and writes it to
MariaDB, but **no byte leaves the host**. Hue lights are real: added from the
WebUI and controlled through a physical Hue bridge (Step 5). For simulated devices, `ONLINE` is still a value someone typed
at registration time, and "turn on the light" means "record that the light is on".

| Working | Stubbed |
| --- | --- |
| MariaDB schema, seed, first-boot init; Argon2id password verification | Frontend device onboarding and rule editor |
| Persisted sessions, REST/WebSocket authorization, logout, admin device guards | Household scoping and per-device permissions |
| Commands applied to the model and persisted to the subtype tables | Device I/O: Hue lights only |
| Audit trail in `device_events`, failures included | Discovery: Hue bridge lights only; no LAN scan |
| Dashboard push; scheduled and device-state execution with chain limits | Physical device state monitoring |
| WebUI manual/scheduled/device-state creation, action ordering, next run, Run and toggles | Rule editing/deletion endpoints |
| Backend and frontend unit suites; disposable-stack auth smoke test | Single hard-coded household |

Every gap is also tracked under "Open items" in
[specs/domain-model.md](specs/domain-model.md).

---

## Backlog

The project board (To Do / In Progress / Review / Done) tracks the items below.
This list is the record of their status: when a card moves, update its status
here in the same commit as the work. The numbered steps further down give the
order in which to tackle what is still open.

Status meanings: **Done** is built and verified; **Review** is built but not
yet proven; **In Progress** is partly built, with the remainder noted; **To Do**
has nothing built yet.

### 1. Infrastructure & DevOps

- **Done** — Multi-container Docker Compose environment and networking.
- **Done** — C++ (CMake) and TypeScript (Vite/lint) build pipelines.
- **Done** — GitHub Actions CI and spec documentation. All five `ci.yml` jobs
  pass on Actions (Step 0).
- **To Do** — Database integration test in CI (Step 3).

### 2. Database & Persistence

- **Done** — MariaDB relational schema (`init.sql`).
- **Done** — C++ database wrapper for devices, users, sessions, and household-scoped
  rule records. Rule/action updates are transactional.

### 3. Authentication & Access Control (RBAC)

- **Done** — Password verification, persisted expiring sessions, REST/WebSocket
  session enforcement, logout revocation, and administrator device guards (Step 1).
- **Done** — Frontend session validation and return to sign-in on expiration.
- **To Do** — Admin user management screens (Step 7); household and per-device
  permissions (Step 6).

### 4. Core Device Management

- **Done** — `SmartDevice` base class and `Light`, `SmartSpeaker`,
  `GenericDevice`.
- **Done** — `DeviceManager` registry and command routing (in-memory state
  only; real I/O is under 5).

### 5. Device Discovery & Commissioning

- **Done** — Admin "Add Device" dialog (`DeviceDiscovery`) backed by Hue bridge
  discovery: `discover()` lists the configured bridge's dimmable lights with
  driver metadata, and registration sends it back, so added lights are
  controllable. The hard-coded fake devices are gone; with no bridge configured
  the list is empty. Unit-tested (backend and Vitest), lint/format clean; the
  middleware treats `/devices/discover` and `POST /devices` as admin-only.
  Verified by hand: a light added from the WebUI against the physical bridge is
  controllable. Device reload after a restart is covered by the disposable-stack
  smoke test.
- **Done** — Admin rename and remove from the device card. Rename is
  `PATCH /devices/{id}` (HomeStr owns names; vendor-app renames are not
  synced). Removal cascades routine actions in the store and the live hub and
  pushes a snapshot. Unit-tested and checked end to end on a disposable MariaDB
  stack (persistence across restarts, rule trimming).
- **To Do** — Bridge discovery and pairing from the WebUI; mDNS/SSDP for other
  device families (Step 5).
- **To Do** — Rooms. HomeStr has no room concept; Hue rooms (bridge groups) are
  not imported and the driver controls single lights only.
- **Done** — Device driver layer and protocol integration (Step 5). The
  `DeviceDriver` seam and a Hue API v1 driver are built, unit-tested, and
  verified by hand controlling a physical bulb through a real Hue bridge.
- **To Do** — Device health and status monitoring (Step 5).

### 6. Dashboard & Real-Time Control

- **Done** — REST/WebSocket endpoints for device state and commands.
- **Done** — Dashboard grid and device control cards.
- **Done** — Connection status: a reconnecting indicator and "may be outdated"
  marks on device cards after the live connection drops; the hub's reconnect
  snapshot refreshes the dashboard and clears them (Step 7).

### 7. Automation Engine

- **Done** — Scheduled execution, timezone/DST policies, persisted run claims,
  device-state transitions, queued chains, and execution limits (Step 4).
- **Done** — Rule persistence, startup restoration, and persisted enable/disable (Step 2).
- **Done** — WebUI manual routine creation with ordered actions, validation, Run,
  and enable/disable controls (Step 7).
- **Done** — Scheduled/device-state creation forms and next-run display.
- **To Do** — Editing/deleting saved rules.

### 8. Testing & Quality Assurance

- **In Progress** — C++ GoogleTest units cover domain models, database fakes,
  authentication, and middleware. `scripts/test-auth.py` verifies auth SQL,
  device/rule reload, ordered actions, transaction rollback, and WebSockets
  against a disposable MariaDB stack. Integration coverage still needs to run
  in CI (Step 3).
- **In Progress** — Vitest covers device cards, API requests, frontend session
  handling, WebSocket authentication and loss reporting, connection-status
  display, manual routine forms, and dashboard Run behavior. A local Chromium check verifies creation, ordering, execution,
  restart/reload persistence, and mobile layout against a disposable stack.
  Automated browser coverage in CI remains open.

### 9. Households & Security

- **To Do** — Households and per-user device assignment (Step 6).
- **To Do** — Security hardening: CORS and login rate limiting ("Smaller items").
- **Done** — Interactive administrator password setup/reset with session revocation.

---

## Step 0 — Get CI green — Done

- [x] `backend` builds with `-Werror` under Ubuntu 24.04's GCC 13.
- [x] `format` installs `clang-format-16` from Ubuntu 24.04's repositories.
- [x] `schema` had failed on every run since CI was added: the runner image has
      no `mariadb` client, so `init.sql` was never applied. It now runs the
      client inside the MariaDB service container, applies `init.sql` twice to
      enforce idempotency, and fails unless the tables in `homestr_db` exactly
      match the `CREATE TABLE` statements in `init.sql`.

The `backend` job builds without MariaDB Connector/C++, so CI compiles the SQL
in `database.cpp` only inside the Docker image and never runs it against a
database. Step 3 closes that gap.

## Step 1 — Authentication — Done

- [x] libsodium Argon2id password hashing and verification; no default password.
- [x] `AuthService`: login, random token issuance, SHA-256 token hashes, persisted
      24-hour sessions, validation, and logout revocation.
- [x] Crow middleware resolving bearer tokens to users; protected REST routes
      return 401, administrator device operations return 403 for non-admins.
- [x] Browser WebSocket authentication through subprotocols, with no tokens in
      URLs or echoed protocol headers. Connections close on expiry/revocation.
- [x] Resolved user IDs recorded for direct device commands.
- [x] `POST /auth/logout` and `GET /auth/me`; frontend session validation,
      expiration handling, and server-side logout.
- [x] Interactive `homestr_server --set-admin-password` command for new and
      existing databases. Resetting a password revokes all that user's sessions
      and prevents in-flight logins from issuing sessions with the old hash.
- [x] Unit coverage for credentials, expiry, revocation, middleware, roles, and
      frontend session handling; disposable-stack smoke coverage for MariaDB,
      restart persistence, audit attribution, and WebSocket lifecycle.

Setup and reset instructions are in [README.md](README.md#log-in). Household
isolation and per-device permissions remain Step 6; login rate limiting and CORS
restrictions remain under Smaller items.

## Step 2 — Rule persistence — Done

- [x] `RuleRecord` and household-scoped `listRules` / `saveRule` / `deleteRule`
      operations in both database implementations.
- [x] Atomic rule/action writes, preserving action order and rolling back on
      failure. No schema migration is required; existing tables are used.
- [x] Restore rules during `HomeStr::start`; persist creation and enable/disable
      before changing the in-memory snapshot.
- [x] Distinguish duplicate IDs (409), unknown rules (404), and storage failures
      (503). Rule-load failures prevent an incomplete startup.
- [x] Synchronized rule mutation and snapshot reads.
- [x] Unit coverage for configuration round-trips, household isolation, startup
      restoration, duplicate handling, and failed writes/reads.
- [x] Disposable MariaDB smoke coverage for multiple trigger types, empty and
      ordered action lists, disable/restart/re-enable/restart/execute, dashboard
      and WebSocket snapshots, and forced transaction rollback.

Rule editing/deletion endpoints and a frontend editor remain future work.
Database integration checks still need CI wiring in Step 3.

## Step 3 — Close the integration test gap

Worth doing before the driver layer, because that step multiplies the surface
area for this class of failure.

Unit tests use `InMemoryDatabase` and cannot detect SQL/schema drift. Two bugs
previously shipped past a green suite: a column-name mismatch that broke login,
and a broken `DEVICE_SELECT` that made every device read fail silently.
`scripts/test-auth.py` now exercises authentication, device reload, and rule
persistence against MariaDB locally. The remaining gap is running that smoke
test in CI.

- [ ] CI job: bring the compose stack up, run `scripts/seed-demo.sh`, restart
      the backend, assert `4 device(s) loaded` in the log.

## Step 4 — Make triggers fire — Done

- [x] A server timer checks due schedules every second through `HomeStr`.
- [x] Pinned croncpp parsing and date-tz IANA conversion, using system tzdata.
      Five numeric cron fields; explicit per-rule timezone, legacy default UTC.
- [x] Next-run metadata and WebUI local time/weekday/timezone configuration.
      Skip missed/nonexistent times; run repeated local minutes only once.
- [x] Durable at-most-once scheduled-minute claims using existing
      `automation_rules.last_fired_at`; no schema migration is required.
- [x] Before/after state events outside device locks; equality conditions fire
      only on entering a match. No-op/failed commands and hydration do not fire.
- [x] Serialized, queued descendant events; each rule runs once per chain,
      capped at 64 rules and 256 actions. RULE actor attribution in audit rows.
- [x] Automatic state/rule WebSocket updates and locked device-state snapshots.
- [x] Device/property/typed-value trigger configuration and validation in WebUI/API.
- [x] Unit, disposable MariaDB, and local browser checks, including actual timer
      dispatch, restart claims, circular chains, and DST behavior.

Physical-device change ingestion follows the driver/monitoring work in Step 5.
Per-action results, retry/replay policies, and browser/integration CI coverage
remain separate work.

## Step 5 — Real device I/O

Where the project stops being a simulation. Write the spec before the code.

- [x] `DeviceDriver` interface behind `DeviceManager`, selected per device from
      `devices.metadata`. The driver is called before the model changes; a
      transport failure sets `ERROR`. Devices without a driver stay simulated.
- [x] Protocol: **Philips Hue API v1 over plain HTTP** (cpp-httplib client),
      chosen because the hardware is on hand and diyHue gives a risk-free
      target with the same API. Lights only; see "Device drivers" in
      `specs/domain-model.md`.
- [x] Prove it: a physical bulb on the real bridge, toggled from the WebUI.
- [ ] Next drivers, easiest first: Gosund WP3 plug (Tuya local protocol needs a
      key from the Tuya cloud, or a Tasmota flash if the chip allows), then
      Nest speakers (Cast v2, protobuf over TLS).
- [ ] Status reality: poll or subscribe, write `last_seen_at`, flip devices to
      `OFFLINE`/`ERROR` on failure. Today a device stays `ONLINE` forever
      because someone said so once.
- [x] Hue light discovery from the configured bridge, feeding the WebUI
      "Add Device" dialog with driver metadata.
- [ ] Bridge discovery (SSDP / N-UPnP) and link-button pairing from the WebUI;
      mDNS for other device families.

## Step 6 — Households and RBAC completeness

- [ ] Household CRUD and membership endpoints.
- [ ] Per-user device assignment. The RBAC rule says a user controls *assigned*
      devices; no such table exists, so any authenticated user can command any
      device.
- [ ] Scope every query from the session instead of the single hard-coded
      household id in `main.cpp`.

## Step 7 — Frontend buildout

- [x] Auth guard validating the stored token and handling expired sessions.
- [ ] Router for additional screens.
- [x] Admin device screens: register from discovery results, rename, remove.
- [ ] Admin user management screen.
- [x] Manual routine creation: names, supported device commands and parameters,
      ordered actions, validation, pending/error handling, and saved results.
- [x] Run enabled manual routines and refresh device states; retain enable/disable.
- [ ] Editing/deleting saved rules.
- [x] Scheduled/device-state creation forms and next-run display.
- [x] Dashboard connection status: reconnecting indicator, outdated device marks,
      refresh from the hub's snapshot on reconnect.
- [ ] A real error surface; errors currently land in a single `role="alert"`
      string.
- [ ] Optimistic updates with rollback on a failed command.

---

## Smaller items

Not worth their own step, easy to pick up alongside one:

- CORS is wide open (`CORSHandler` defaults) — tighten to the known origins.
- No rate limiting on `POST /auth/login`.
- `devices.metadata` is written but never read.
- `GET /devices` has no pagination or filtering.
- `Database::listDevices` is only called at startup; the HTTP reads all serve
  from `DeviceManager`'s in-memory snapshot. Fine today, worth revisiting once
  more than one process can write.
