# HomeStr Roadmap

Where the project stands and what to build next. Keep this file current: it is
the entry point for anyone (or any agent) picking the project back up.

Companion documents: [README.md](README.md) for setup and testing,
[CLAUDE.md](CLAUDE.md) for conventions, [specs/](specs/) for the contracts that
changes must go through first.

---

## Current state

As of 2026-11-02, after authentication, rule persistence, and manual routine creation in the WebUI.

The architecture, schema, build, container stack, tests and CI are in place and
the stack runs end to end. It is best understood as **a working simulator of
the architecture, not a partially-working product**.

The gap that matters most, and which the stub list understates:
`DeviceManager::sendCommand` mutates an in-memory object and writes it to
MariaDB, but **no byte ever leaves the host**. There is no driver layer and no
socket. A device's `ONLINE` status is a value someone typed at registration
time. "Turn on the light" currently means "record that the light is on".

| Working | Stubbed |
| --- | --- |
| MariaDB schema, seed, first-boot init; Argon2id password verification | Frontend device onboarding and rule editor |
| Persisted sessions, REST/WebSocket authorization, logout, admin device guards | Household scoping and per-device permissions |
| Commands applied to the model and persisted to the subtype tables | No device I/O at all — no driver, no network |
| Audit trail in `device_events`, failures included | LAN discovery returns an empty list |
| Dashboard aggregate and WebSocket push | Cron scheduler never ticks; `evaluate()` is only reached via the manual trigger endpoint |
| Persisted rules; WebUI manual creation, action ordering, Run and enable/disable | Scheduled/device-state editors and rule editing/deletion endpoints |
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
- **Review** — GitHub Actions CI and spec documentation. Specs are written;
  `ci.yml` has never been confirmed green on Actions (Step 0).
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

- **To Do** — Network discovery and compatibility validation (Step 5).
- **In Progress** — Admin device registration, household association and
  decommissioning. Backend register/unregister and admin guards work; no UI,
  household hard-coded (Steps 6, 7).
- **To Do** — Device driver layer and protocol integration (Step 5).
- **To Do** — Device health and status monitoring (Step 5).

### 6. Dashboard & Real-Time Control

- **Done** — REST/WebSocket endpoints for device state and commands.
- **Done** — Dashboard grid and device control cards.

### 7. Automation Engine

- **In Progress** — `AutomationRule` model and execution. `evaluate()` works;
  no scheduler, no device-state triggers, no recursion guard (Step 4).
- **Done** — Rule persistence, startup restoration, and persisted enable/disable (Step 2).
- **Done** — WebUI manual routine creation with ordered actions, validation, Run,
  and enable/disable controls (Step 7).
- **To Do** — Scheduled/device-state editors and editing/deleting saved rules.

### 8. Testing & Quality Assurance

- **In Progress** — C++ GoogleTest units cover domain models, database fakes,
  authentication, and middleware. `scripts/test-auth.py` verifies auth SQL,
  device/rule reload, ordered actions, transaction rollback, and WebSockets
  against a disposable MariaDB stack. Integration coverage still needs to run
  in CI (Step 3).
- **In Progress** — Vitest covers device cards, API requests, frontend session
  handling, WebSocket authentication, manual routine forms, and dashboard Run
  behavior. A local Chromium check verifies creation, ordering, execution,
  restart/reload persistence, and mobile layout against a disposable stack.
  Automated browser coverage in CI remains open.

### 9. Households & Security

- **To Do** — Households and per-user device assignment (Step 6).
- **To Do** — Security hardening: CORS and login rate limiting ("Smaller items").
- **Done** — Interactive administrator password setup/reset with session revocation.

---

## Step 0 — Get CI green and merge

Two unknowns were never exercised, because the equivalent commands were only
run locally and never under GitHub Actions:

- The `backend` job builds with `-Werror` on Ubuntu 24.04's GCC 13; the local
  verification used the container's GCC 12, so a newer compiler may surface new
  warnings.
- The `format` job installs `clang-format-16`; if that package name is not in
  Ubuntu 24.04's repos the job fails on the install step, not on formatting.

Fix what those surface and merge, so everyone branches from a known-good main.

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

## Step 4 — Make triggers fire

`AutomationRule::evaluate` is written and tested but only ever reached from the
manual trigger endpoint.

- [ ] Schedule triggers: a background thread in `HomeStr` ticking once a
      minute and matching cron expressions. Use **croncpp** (header-only, via
      FetchContent, same pattern as the other dependencies) rather than
      hand-rolling cron parsing.
- [ ] Device-state triggers: emit a `TriggerEvent` after each successful
      command so `DEVICE_STATE` rules chain off real changes.
- [ ] Recursion guard. Rule A turning on a light that triggers rule B that
      turns it off is an infinite loop, and dispatching from inside
      `DeviceManager`'s mutex risks deadlock.

## Step 5 — Real device I/O

Where the project stops being a simulation. Write the spec before the code.

- [ ] Introduce a `DeviceDriver` interface behind `DeviceManager`, so
      `turnOn()` delegates to a transport instead of setting a bool. The
      current split (models own state, manager owns dispatch) makes this clean,
      but the seam does not exist yet.
- [ ] Choose one protocol and commit. **Tasmota or Shelly over plain HTTP** is
      the cheapest useful target — no broker, and works with hardware you can
      buy for the price of a lunch. MQTT scales better but adds a broker to
      compose.
- [ ] Status reality: poll or subscribe, write `last_seen_at`, flip devices to
      `OFFLINE`/`ERROR` on failure. Today a device stays `ONLINE` forever
      because someone said so once.
- [ ] Then discovery: mDNS (Shelly, Tasmota) and SSDP (Hue), replacing the
      empty-cache `discover()`.

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
- [ ] Admin screens: register from discovery results, user management.
- [x] Manual routine creation: names, supported device commands and parameters,
      ordered actions, validation, pending/error handling, and saved results.
- [x] Run enabled manual routines and refresh device states; retain enable/disable.
- [ ] Editing/deleting saved rules.
- [ ] Scheduled/device-state editors after their trigger engines are implemented.
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
