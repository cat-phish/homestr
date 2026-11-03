# HomeStr

A self hosted home automation application.

HomeStr is an IoT home automation hub: a **C++20** backend serving REST and
WebSocket, a **TypeScript/React** WebUI, and **MariaDB** for users, households,
devices and automation rules. The whole stack runs under Docker Compose.

The project follows **Spec-Driven Development**: `specs/` is the source of truth
and changes before the code does. See [CLAUDE.md](CLAUDE.md) for conventions and
the full command reference, and [ROADMAP.md](ROADMAP.md) for what to build next.

> **Status: scaffolding.** The architecture, schema, build, tests and CI are in
> place and the stack runs end to end, but several pieces are deliberately
> stubbed. Read [What works today](#what-works-today) before filing a bug.

---

## Prerequisites

- Docker with Compose v2 (`docker compose version`)
- Node 20.19+ and npm, only if you want to run the frontend outside Docker
- A C++20 toolchain and CMake 3.22+, only if you want to build the backend on
  the host. You do not need either: the Docker image carries them.

Nothing else. In particular you do **not** need MariaDB or the MariaDB
Connector/C++ installed locally.

## Quick start

```bash
git clone <this repo> && cd homestr
cp .env.example .env
```

Now open `.env` and check three things:

1. `VOLUME_SUFFIX` — set it to `,z` if you are on Fedora, RHEL or CentOS
   (SELinux). Leave it empty on macOS, Windows, or Linux without SELinux.
   Without it, SELinux hosts fail with `permission denied` on the mounted
   sources.
2. `DB_PUBLISHED_PORT` — if you already run MariaDB or MySQL locally, port 3306
   is taken and the `db` container will fail to start. Set it to `3307` or
   anything free. Same for `HOMESTR_PUBLISHED_PORT` and
   `FRONTEND_PUBLISHED_PORT` if 8080 or 5173 are busy.
3. The passwords are placeholders. Fine for local work, never for anything
   reachable.

See [Working across machines](#working-across-machines-and-alongside-other-developers)
for running more than one stack at once.

Then:

```bash
docker compose up --build
```

First run takes a few minutes: the backend image compiles MariaDB
Connector/C++ from source (it is not packaged for Debian), and the first
container start fetches and builds Crow, nlohmann/json and GoogleTest into a
named volume. Later starts are seconds. Wait for:

```
homestr-backend  | [hub] listening on 0.0.0.0:8080
```

At the default ports — substitute your own if you changed them in `.env`. The
`curl` examples further down assume the defaults too.

| Service | URL | `.env` variable |
| --- | --- | --- |
| WebUI | <http://localhost:5173> | `FRONTEND_PUBLISHED_PORT` |
| API | <http://localhost:8080/api/v1> | `HOMESTR_PUBLISHED_PORT` |
| Health probe | <http://localhost:8080/healthz> | `HOMESTR_PUBLISHED_PORT` |
| MariaDB | `localhost:3306` | `DB_PUBLISHED_PORT` |

### Log in

Set the administrator password once, then sign in as `admin@homestr.local`:

```bash
docker compose exec backend /app/build/homestr_server --set-admin-password
```

The command prompts twice without echoing the password. Use 8–1024 bytes. It
also works with existing database volumes and revokes existing administrator
sessions. No default password is shipped; old placeholder hashes cannot sign in.
You can run the same command again to reset the password.

Sessions expire after 24 hours. Sign out revokes the current session on the hub.
The dashboard validates stored sessions and returns to sign-in on expiration.

### Get some devices on the dashboard

A fresh database has no devices, so the dashboard starts empty. Seed four demo
devices and one automation rule:

```bash
./scripts/seed-demo.sh
```

The script prompts for the administrator password and revokes its temporary
session when it finishes. Alternatively, provide `HOMESTR_TOKEN` with an existing
admin bearer token. Re-running it is safe — existing devices report `exists`
rather than erroring.

---

## What works today

| Working | Stubbed / not built |
| --- | --- |
| Argon2id password verification, persisted 24-hour sessions, logout revocation | Multi-household support — the hub loads one hard-coded household at boot |
| Bearer-token enforcement on REST and WebSocket; admin-only device setup | Device health monitoring and physical device communication |
| Device commands: `turnOn`, `turnOff`, `setBrightness`, `setColor`, `play`, `pause`, `setVolume`, `connect` | LAN discovery — `GET /devices/discover` returns an empty list, no mDNS/SSDP sweep |
| Scheduled routines with timezones, next-run display, and durable run claims | Physical-device state subscriptions |
| Audit trail in `device_events`, including failed commands | Rule editing and deletion endpoints are not built |
| Dashboard aggregate, live state/rule updates, bounded device-state trigger chains | Per-user device assignment — any authenticated caller can command any device in the household |
| Persisted rules; WebUI manual/scheduled/device-state creation and ordered actions | Device setup screens |

Core backend gaps above are tracked under "Open items" in
[`specs/domain-model.md`](specs/domain-model.md), and sequenced into work items
in [ROADMAP.md](ROADMAP.md). Add to those lists rather than widening scope
silently.

Because there is no real hardware, a device only accepts commands while its
`status` is `ONLINE`. Registering one as `OFFLINE` is how you exercise the
unreachable path — the WebUI greys it out and the API answers `409`.

---

## Testing

### Automated tests

**Backend — GoogleTest suites, no database needed.** They run against
`InMemoryDatabase`, so they are fast and hermetic:

```bash
docker run --rm -v "$(pwd)/backend:/app:z" -v homestr-test-build:/build \
  --entrypoint bash homestr-backend -c \
  'cmake -B /build -S /app -G Ninja && cmake --build /build && ctest --test-dir /build --output-on-failure'
```

The named `/build` volume keeps the fetched dependencies between runs. On a
host with CMake and a C++20 compiler you can skip Docker entirely:

```bash
# Install libsodium development headers, pkg-config, and tzdata first.
cmake -B backend/build -S backend -DCMAKE_BUILD_TYPE=Debug -DHOMESTR_WITH_MARIADB=OFF
cmake --build backend/build -j"$(nproc)"
ctest --test-dir backend/build --output-on-failure
```

**Frontend — Vitest suites:**

```bash
cd frontend
npm ci
npm test          # single run
npm run lint      # type-aware ESLint
npm run typecheck
```

### Manual verification

With the stack up and `./scripts/seed-demo.sh` run, the WebUI should show four
device cards and one rule:

| Card | What to exercise |
| --- | --- |
| Kitchen Ceiling (light) | On/off, brightness slider, colour picker |
| Living Room Speaker | Play/Pause, volume slider |
| Garage Relay (generic) | On/off only — no extra controls, which is the device-subclass split working |
| Porch Light (offline) | Greyed out, every control disabled |
| Rule "Lights out at 22:00" | Enable/Disable toggle |

**Create a routine.** Under **Automation routines**, choose **New routine**.
Name it and choose **When to run**:

- **Manually:** use **Run** on the saved routine.
- **On a schedule:** choose a time, weekdays, and IANA timezone, such as
  `America/Los_Angeles`. The form defaults to the browser timezone. The dashboard
  shows the next run in that timezone. Legacy API schedules without a timezone
  use UTC.
- **When a device changes:** choose a source device, property, and the value it
  must become. Power conditions work for every device; lights add brightness and
  color, and speakers add connection, volume, and playback state.

Select a device and command for each action and enter any brightness, color,
volume, or optional track settings. Use **Add action**, **Move up**, **Move down**,
and **Remove** to arrange actions, then choose **Save routine**. At least one
action is required. Failed saves retain your draft for retrying.

New routines are enabled. **Disable** pauses any trigger type; **Enable** resumes
it. Rules, trigger configuration, and enabled state survive backend restarts.
Manual **Run** refreshes device states; automatic actions push live updates to
connected dashboards. Trigger confirmation does not report individual action
outcomes, so check device states to confirm the result.

**Timing and chains.** Missed schedule occurrences are skipped; there is no
catch-up after downtime. Nonexistent spring-forward times are skipped, and a
repeated fall-back local minute runs only on its first occurrence. A scheduled
minute is claimed before dispatch to prevent replay: a crash after claiming may
leave actions incomplete, with no automatic retry.

State conditions fire when the watched value changes into the selected value,
not when an unrelated property changes or a command leaves the value unchanged.
Automation actions can trigger other routines. Each rule runs at most once per
root event chain, with limits of 64 rules and 256 actions to stop circular or
excessive chains. Device changes currently come from this app's simulated
commands and routines; physical-device monitoring remains a separate task.

Editing/deleting saved routines and per-action execution results remain future
work.

**Authenticated API examples.** Set `HOMESTR_TOKEN` to a token returned by
`POST /api/v1/auth/login` with your email and password. Every API request below
requires that token; `/healthz` remains public. Do not commit tokens or credentials.

**Live push.** Leave the dashboard open in a browser and run:

```bash
curl -s -X POST localhost:8080/api/v1/devices/11111111-0000-4000-8000-000000000001/commands \
  -H "Authorization: Bearer ${HOMESTR_TOKEN}" \
  -H 'Content-Type: application/json' -d '{"command":"setBrightness","params":{"level":15}}'
```

The slider moves on its own. That is the WebSocket `device_state` frame, not
polling.

**Manual rule execution** is available through **Run** in the dashboard. For an
API call, set `RULE_ID` to the ID of a saved `MANUAL` routine from `GET /rules`:

```bash
curl -s -X POST "localhost:8080/api/v1/rules/${RULE_ID}/trigger" \
  -H "Authorization: Bearer ${HOMESTR_TOKEN}"
```

Returns the fired rule ID. The dashboard refreshes device states after its Run
action; the API response confirms the trigger rather than each action outcome.

**Error paths** are real, not swallowed:

```bash
# 409: command not supported by that device type
curl -s -X POST localhost:8080/api/v1/devices/33333333-0000-4000-8000-000000000003/commands \
  -H "Authorization: Bearer ${HOMESTR_TOKEN}" \
  -H 'Content-Type: application/json' -d '{"command":"setBrightness","params":{"level":50}}'

# 409: device unreachable
curl -s -X POST localhost:8080/api/v1/devices/44444444-0000-4000-8000-000000000004/commands \
  -H "Authorization: Bearer ${HOMESTR_TOKEN}" \
  -H 'Content-Type: application/json' -d '{"command":"turnOn"}'
```

**Persistence.** The surest check that the C++ queries match the schema is to
restart the backend and confirm state comes back out of the database:

```bash
docker compose restart backend
docker compose logs backend | grep '\[hub\] household'
# => [hub] household 0000...00a1: 4 device(s) loaded
```

With demo data seeded, the log should show `4 device(s) loaded; 1 rule(s) loaded`.
Device state, rule definitions, ordered actions, and enabled/disabled settings
are restored from MariaDB. A rule read failure prevents startup rather than
silently presenting an empty rule list. Unexpected counts or `[db]` errors need
investigation.

**Audit trail**, including the failures from above:

```bash
docker compose exec db mariadb -u homestr_user -p homestr_db \
  -e "SELECT d.name, e.command, e.succeeded, e.error
      FROM device_events e JOIN devices d ON d.id = e.device_id;"
```

---

## Working across machines, and alongside other developers

Nothing machine-specific is tracked in git. `docker-compose.yml` reads every
environment-dependent value from a variable with a working default, so the only
file you own is your untracked `.env` — `.env.example` documents each knob.
Moving to another machine means `git clone`, `cp .env.example .env`, and
adjusting the handful of lines that machine forces on you.

| Variable | When you change it |
| --- | --- |
| `VOLUME_SUFFIX` | `,z` on SELinux hosts (Fedora, RHEL, CentOS), empty on macOS, Windows and non-SELinux Linux. It is a comma option because every mount already declares `:rw` or `:ro`. |
| `DB_PUBLISHED_PORT`, `HOMESTR_PUBLISHED_PORT`, `FRONTEND_PUBLISHED_PORT` | Something already holds the default. Only the host side moves; the ports inside the containers stay fixed, so the Vite proxy and nginx can always reach `backend:8080`. |
| `COMPOSE_PROJECT_NAME` | You want a second, fully independent stack — another checkout, another branch, a git worktree, or a second developer on a shared host. |

Container names are intentionally **not** pinned with `container_name`. Compose
derives them from the project name, so two stacks coexist:

```bash
# terminal 1 — the default stack
docker compose up

# terminal 2 — a second, independent stack with its own database
COMPOSE_PROJECT_NAME=homestr-wip \
  DB_PUBLISHED_PORT=3407 HOMESTR_PUBLISHED_PORT=8180 FRONTEND_PUBLISHED_PORT=5273 \
  docker compose up
```

Those get their own containers, network and volumes — `homestr-wip_db-data` is a
different database from `homestr_db-data`, so one developer's schema experiment
cannot touch another's data. Put the overrides in `.env` to make them your
default. Tear a stack down with the same variable set, or its volumes linger:
`COMPOSE_PROJECT_NAME=homestr-wip docker compose down -v`.

Two things that are **not** portable and should never be committed:

- `.env` — git-ignored, and deliberately so. Each machine has its own.
- `frontend/node_modules` — also ignored, and the container keeps its own copy
  in a named volume, so Linux-built binaries never leak onto a macOS host.

## Development loop

| Change | What to do |
| --- | --- |
| TypeScript under `frontend/src/` | Nothing — Vite hot-reloads in the browser |
| C++ under `backend/` | `docker compose restart backend` (it recompiles from the bind mount on start; there is no C++ hot reload) |
| `database/init.sql` | `docker compose down -v && docker compose up` — the seed only runs on an empty data directory |
| `specs/` | Update the spec first, then schema, then backend, then frontend types |

Useful:

```bash
docker compose logs -f backend            # hub log
docker compose exec db mariadb -u homestr_user -p homestr_db
docker compose down                       # stop, keep the database
docker compose down -v                    # stop and wipe the database
```

Formatting, both enforced in CI:

```bash
# C++ — run from the repo root so clang-format finds .clang-format
find backend/src backend/include -name '*.cpp' -o -name '*.hpp' | xargs clang-format -i

cd frontend && npm run format
```

Use **clang-format 16**. The dev image and CI pin that major version, and other
releases format the same file differently — mixing them produces a diff of pure
noise.

---

## Troubleshooting

**`address already in use ... 0.0.0.0:3306`**
A local MariaDB/MySQL holds the port. Set `DB_PUBLISHED_PORT=3307` in `.env`.

**`permission denied` reading mounted sources (Fedora, RHEL, CentOS)**
SELinux. Set `VOLUME_SUFFIX=,z` in `.env`. Ad-hoc `docker run -v` commands need
`:z` on the mount themselves. Note that relabelling is persistent, so a
directory mounted once with `z` keeps working afterwards — which can mask a
missing label until someone clones fresh.

**A mount appears empty, and the backend logs `The source directory "/app" does
not appear to contain CMakeLists.txt`**
`VOLUME_SUFFIX` is malformed. It must start with a comma (`,z`), not a colon:
every mount already declares `:rw` or `:ro`, so a `:z` makes Docker read the
whole thing as a target path.

**`SELinux relabeling of / is not allowed`**
Your `$PWD` was `/`. Use an absolute path for the mount source.

**`mariadbd: io_uring_queue_init() failed with EPERM`**
A warning, not an error. MariaDB falls back to normal AIO and works fine.

**Login says `invalid credentials` for a user that exists**
Check `docker compose logs backend` for a `[db]` line. A `No such column`
there means a query in `backend/src/services/database.cpp` disagrees with
`database/init.sql`.

**Dashboard is empty after a restart**
Run `./scripts/seed-demo.sh`. If devices were there before the restart and are
gone now, see the persistence check above. Both devices and saved automation
rules should survive backend restarts.

**The backend container restarts in a loop at first boot**
It retries the database for 60s while MariaDB initialises, then exits. Give it
another `docker compose up`; if it persists, check `docker compose logs db`.

---

## Layout

```text
backend/            C++20 hub: Crow, nlohmann/json, MariaDB Connector/C++
  include/homestr/  headers — models/, services/, controllers/
  src/              implementations, mirroring include/
  tests/            GoogleTest suites
frontend/           Vite + React + TypeScript WebUI, Vitest
  src/api/          REST client and WebSocket subscription
  src/components/   Dashboard, DeviceCard, LoginForm
  src/types/        transport types mirroring the backend JSON
database/init.sql   MariaDB DDL and development seed data
specs/              domain model and OpenAPI contract — source of truth
scripts/            seed-demo.sh
.github/workflows/  CI: formatting, backend, frontend, schema, images
```

## CI

`.github/workflows/ci.yml` runs on every push and pull request:

| Job | Checks |
| --- | --- |
| `format` | `clang-format --dry-run --Werror` over all C++ |
| `backend` | Builds with `-Werror` and runs the GoogleTest suites |
| `frontend` | ESLint, Prettier, `tsc`, Vitest, production build |
| `schema` | Applies `init.sql` to a clean MariaDB 11.4 |
| `docker` | Builds both release images (which run the tests inside) |

CI does not yet run an integration test across the three containers. The auth
smoke test has a separate **disposable** Compose stack with its own database
and build volumes. It binds the API only to loopback on port 48080 (override with
`HOMESTR_AUTH_TEST_PORT` if needed):

```bash
docker compose -f docker-compose.auth-test.yml up -d --build --wait --wait-timeout 300
python3 scripts/test-auth.py docker-compose.auth-test.yml http://localhost:48080
python3 scripts/test-triggers.py docker-compose.auth-test.yml http://localhost:48080
docker compose -f docker-compose.auth-test.yml down -v
```

It sets the test administrator password and inserts test records, then checks
route permissions, audit attribution, WebSocket authentication and revocation,
expiry, and session/device/rule persistence across backend restarts. It also
verifies action order, disabled-state recovery, and transaction rollback after
forced write failures. Do not point it
at a database you use. The trigger smoke test additionally verifies typed
conditions, no-op suppression, circular-chain protection, RULE audit attribution,
a real scheduler tick (up to one minute of waiting), and durable claims after
restart. The manual persistence check above remains useful for other SQL changes.
