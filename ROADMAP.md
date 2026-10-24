# HomeStr Roadmap

Where the project stands and what to build next. Keep this file current: it is
the entry point for anyone (or any agent) picking the project back up.

Companion documents: [README.md](README.md) for setup and testing,
[CLAUDE.md](CLAUDE.md) for conventions, [specs/](specs/) for the contracts that
changes must go through first.

---

## Current state

As of 2026-10-21 (`5f4fb69`, after the portable dev stack landed).

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
| MariaDB schema, seed, first-boot init | Password verification — login accepts any non-empty password for a known email |
| Device registration/unregistration, persisted and reloaded on restart | Bearer tokens issued but **not enforced on any route**, including the WebSocket |
| Commands applied to the model and persisted to the subtype tables | No device I/O at all — no driver, no network |
| Audit trail in `device_events`, failures included | LAN discovery returns an empty list |
| Dashboard aggregate and WebSocket push | Cron scheduler never ticks; `evaluate()` is only reached via the manual trigger endpoint |
| Rule create / enable / disable / manual trigger | Rules are in memory only and vanish on restart |
| 113 GoogleTest + 13 Vitest cases, green | Per-user device assignment; single hard-coded household |

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
- **In Progress** — C++ database wrapper. Device and user queries use prepared
  statements; rule read/write methods are missing (Step 2).

### 3. Authentication & Access Control (RBAC)

- **In Progress** — User and Admin domain models and permissions. `isAdmin()`
  exists; no route enforces it (Step 1).
- **In Progress** — Login/authentication endpoints. Login only checks the user
  exists; tokens are not validated; logout is not written (Step 1).
- **In Progress** — Frontend login and admin user management. Login form done;
  no token-validating auth guard, no admin screens (Step 7).

### 4. Core Device Management

- **Done** — `SmartDevice` base class and `Light`, `SmartSpeaker`,
  `GenericDevice`.
- **Done** — `DeviceManager` registry and command routing (in-memory state
  only; real I/O is under 5).

### 5. Device Discovery & Commissioning

- **To Do** — Network discovery and compatibility validation (Step 5).
- **In Progress** — Admin device registration, household association and
  decommissioning. Backend register/unregister work; no admin guard, no UI,
  household hard-coded (Steps 1, 6, 7).
- **To Do** — Device driver layer and protocol integration (Step 5).
- **To Do** — Device health and status monitoring (Step 5).

### 6. Dashboard & Real-Time Control

- **Done** — REST/WebSocket endpoints for device state and commands.
- **Done** — Dashboard grid and device control cards.

### 7. Automation Engine

- **In Progress** — `AutomationRule` model and execution. `evaluate()` works;
  no scheduler, no device-state triggers, no recursion guard (Step 4).
- **To Do** — Rule persistence and frontend rule configurator (Steps 2, 7).

### 8. Testing & Quality Assurance

- **In Progress** — C++ GoogleTest units. 113 cases, all on `InMemoryDatabase`;
  the device hierarchy, `User`, `AutomationRule` and the `InMemoryDatabase` /
  `DatabaseConfig` / `UserRecord` layer are covered. `MariaDbDatabase` SQL is
  not (Step 3).
- **In Progress** — Vitest units and end-to-end flows. 13 Vitest cases; no
  end-to-end tests.

### 9. Households & Security

- **To Do** — Households and per-user device assignment (Step 6).
- **To Do** — Security hardening: CORS, login rate limiting, admin hash
  rotation (Step 1 and "Smaller items").

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

## Step 1 — Authentication

First, because it is the only item that is a *defect* rather than a missing
feature, and because several later steps depend on having a resolved user.

- [ ] Choose a hashing library. **libsodium** (`libsodium-dev`, argon2id) is
      preferred over bcrypt: apt-available, so unlike the MariaDB connector it
      needs no source build.
- [ ] `services/auth_service.{hpp,cpp}`: `hashPassword`, `verifyPassword`,
      `issueSession`, `validateSession`. The `sessions` table already exists
      and expects a SHA-256 of the bearer token.
- [ ] Crow middleware resolving `Authorization: Bearer` to a user, 401 on
      failure. Cover the WebSocket route too.
- [ ] Thread the resolved user id into `sendCommand`'s `actorId`, which is
      always empty today, so every audit row reads `SYSTEM` instead of naming
      the user who acted.
- [ ] Admin guard (403) on device register/unregister. `api-spec.yaml`
      documents these as admin-only; nothing enforces it.
- [ ] Implement `POST /auth/logout` — specced, never written.
- [ ] Rotate the placeholder admin hash in `init.sql` and provide a way to set
      a real password.
- [ ] Tests: hash round-trip, expired token, 401 on missing token, 403 on
      non-admin.

`api-spec.yaml` already describes these responses, so this step is mostly
making reality match the spec.

## Step 2 — Rule persistence

Smallest real gap. `automation_rules` and `automation_actions` are in the
schema and entirely unused.

- [ ] Add `listRules` / `saveRule` / `deleteRule` to the `Database` interface
      and both implementations, following the `saveDevice` / `saveSubtype`
      pattern.
- [ ] Load rules in `HomeStr::start`; persist on create and on enable/disable.
- [ ] Tests against `InMemoryDatabase`.

Removes the most confusing thing a newcomer hits: rules silently disappearing
on `docker compose restart backend`.

## Step 3 — Close the integration test gap

Worth doing before the driver layer, because that step multiplies the surface
area for this class of failure.

Nothing in the suite or CI compares the SQL string literals in
`backend/src/services/database.cpp` against `database/init.sql`. Two bugs
shipped past a green suite because of it: a column-name mismatch that broke
login, and a broken `DEVICE_SELECT` that made every device read fail silently.
All unit tests use `InMemoryDatabase`, so none of them can catch it.

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

- [ ] Router and an auth guard that *validates* the token, rather than checking
      that something is present in `localStorage`.
- [ ] Admin screens: register from discovery results, user management.
- [ ] Rule editor.
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
