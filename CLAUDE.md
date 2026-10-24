# HomeStr - Claude Code Project Guidelines

## System Overview

HomeStr is a distributed IoT home automation system following a client-server
architecture:

- **Backend**: C++20 HTTP/WebSocket server handling hardware commands,
  discovery, and rule evaluation. Crow for routing and WebSockets,
  nlohmann/json for payloads, MariaDB Connector/C++ for persistence.
- **Frontend**: TypeScript + React WebUI (Vite) for real-time device control,
  dashboards, and household administration.
- **Persistence**: MariaDB relational database for user accounts, registered
  devices, and automation routines.
- **Methodology**: Spec-Driven Development (SDD) adhering to Scrum 3-week
  sprint deliverables.

---

## Picking Up Work

- `ROADMAP.md` is the backlog and the hand-off document. Read its "Current
  state" and "Backlog" sections before starting; its numbered steps give the
  order of work.
- When work lands, update the item's status in `ROADMAP.md`, and remove the
  entry from "Open items" in `specs/domain-model.md` if it closes one, in the
  same commit. A stale roadmap is a defect.
- Machine-specific setup (ports, SELinux labels) belongs in your untracked
  `.env`, never in a tracked file. `.env.example` documents every variable.
- Unit tests run on `InMemoryDatabase` and cannot catch SQL/schema drift. After
  touching SQL in `backend/src/services/database.cpp` or `database/init.sql`,
  run `docker compose restart backend` and confirm the log shows
  `[hub] household ...: N device(s) loaded` with no `[db]` lines, then log in
  once, since login is the only read path through `findUserByEmail`.

---

## Spec-Driven Development (SDD) Workflow

Before generating or modifying functional code:

1. **Check Specifications**: Inspect `/specs/` for architectural constraints,
   API contracts (`api-spec.yaml`), and data models (`domain-model.md`).
2. **Specs First**: If modifying an endpoint, table, or interface, update the
   specification file in `/specs/` before writing code.
3. **Tests Second**: Implement GoogleTest (`backend/tests/`) or Vitest
   (`frontend/tests/`) units confirming the spec behavior.
4. **Implementation Last**: Write minimal, clean C++ or TypeScript code that
   satisfies the tests and matches formatting rules.

A change to the domain model touches four files in this order:
`specs/domain-model.md` → `database/init.sql` →
`backend/include/homestr/...` → `frontend/src/types/index.ts`. Leaving any of
them behind is a defect, not a follow-up.

Anything specified but not yet built is listed under "Open items" in
`specs/domain-model.md`. Add to that list rather than silently widening scope.

---

## Development & Build Commands

### Full Stack via Docker

- Start all services: `docker compose up --build`
- Stop all services: `docker compose down -v` (also drops the DB volume, so
  `init.sql` re-seeds)
- Follow the hub: `docker compose logs -f backend`
- Rebuild after a C++ change: `docker compose restart backend`
- Database access: `docker compose exec db mariadb -u homestr_user -p homestr_db`

WebUI on <http://localhost:5173>, API on <http://localhost:8080/api/v1>,
MariaDB on `localhost:3306` (`DB_PUBLISHED_PORT` moves it if a local
MariaDB/MySQL already holds that port).

C++ has no hot reload: the dev entrypoint rebuilds from the bind-mounted
sources on container start, so a C++ change needs `docker compose restart
backend`. TypeScript hot-reloads in place.

`database/init.sql` runs only on an empty data directory. After editing the
schema: `docker compose down -v && docker compose up`.

### Backend (C++20 / CMake)

- Configure build: `cmake -B backend/build -S backend -DCMAKE_BUILD_TYPE=Debug`
- Compile: `cmake --build backend/build -j$(nproc)`
- Run unit tests: `ctest --test-dir backend/build --output-on-failure`
- Run one suite: `ctest --test-dir backend/build -R DeviceManager`
- Format code:
  `find backend/src backend/include -name '*.cpp' -o -name '*.hpp' | xargs clang-format -i`

Add `-G Ninja` to the configure step if Ninja is installed; the Docker dev
image uses it. Without MariaDB Connector/C++ installed, configure with
`-DHOMESTR_WITH_MARIADB=OFF`; the hub then runs against `InMemoryDatabase`.
Build with `-DHOMESTR_WARNINGS_AS_ERRORS=ON` before pushing - CI does.

### Frontend (TypeScript / Vite)

- Install dependencies: `cd frontend && npm install`
- Run dev server: `cd frontend && npm run dev` (proxies `/api` to the backend)
- Run linter: `cd frontend && npm run lint`
- Run unit tests: `cd frontend && npm test`
- Typecheck: `cd frontend && npm run typecheck`
- Format: `cd frontend && npm run format`
- Build production bundle: `cd frontend && npm run build`

---

## Coding Standards & Conventions

### C++ Conventions

- Standard: Modern C++20. Use RAII, smart pointers (`std::unique_ptr`,
  `std::shared_ptr`), and `std::optional`. Avoid raw pointers for ownership; a
  raw pointer is always borrowed and may be null, which the caller must check.
- Include style: Match `.clang-format` (Google style, 100 column limit, 2-space
  indentation). It is authoritative - never hand-format against it. Use
  **clang-format 16**: the dev image and CI both pin that major version, and
  other releases format the same file differently. The config lives at the repo
  root, so run the formatter from there.
- Headers: Use `#pragma once` for include guards.
- Layout: headers `.hpp` in `backend/include/homestr/`, sources `.cpp` in
  `backend/src/`, mirroring each other.
- Naming:
  - Classes/Structs: `PascalCase` (`SmartDevice`, `DeviceManager`)
  - Methods/Functions: `camelCase` (`sendCommand`, `turnOn`)
  - Member variables: `camelCase_` with a trailing underscore (`devices_`,
    `ipAddress_`); plain struct fields carry no underscore
  - Locals/parameters: `camelCase` (`deviceId`, `householdId`)
  - Constants/Enums: `UPPER_SNAKE_CASE` (`DEFAULT_PORT`) or
    `ScopedEnum::PascalCase` (`DeviceStatus::Online`)
- Everything lives in `namespace homestr`. File-local helpers go in an
  anonymous namespace, not `static`.
- `register` is still a reserved keyword, so the registration methods are
  `registerDevice` / `unregisterDevice`.
- Commands that can fail on the wire return `bool`/`CommandResult`, not
  exceptions. Exceptions from third-party code (`sql::SQLException`) are caught
  at the boundary where they occur.
- Pass `std::string_view` for read-only strings, `const std::string&` where an
  owned string is stored.

### TypeScript Conventions

- Strict typing: `noImplicitAny: true` enabled, plus `noUncheckedIndexedAccess`
  and `exactOptionalPropertyTypes` - indexed access yields `T | undefined`, so
  narrow it rather than asserting. Explicit return types on service and API
  functions.
- Naming:
  - Components/Interfaces/Types: `PascalCase` (`DeviceCard.tsx`, `SmartDevice`)
  - Functions/Variables: `camelCase` (`fetchDevices`, `isAuthenticated`)
- Imports: Use path aliases (`@/components`, `@/api`, `@/types`) configured in
  `tsconfig.app.json` (referenced by `tsconfig.json`) and mirrored in
  `vite.config.ts`. `import type` for type-only imports, enforced by ESLint.
- Prettier (100 columns, single quotes, trailing commas) plus type-aware
  ESLint; both run in CI.
- Components are named function declarations with an explicit props interface.
  No default exports.
- Transport types in `src/types/index.ts` mirror the backend JSON exactly,
  `camelCase` on the wire. Devices are discriminated on `type`.
- All network access goes through `src/api/client.ts`; components never call
  `fetch` directly.

### SQL Conventions

- 4-space indent, uppercase keywords, `snake_case` identifiers, plural table
  names. Named constraints (`fk_`, `uq_`, `ck_`, `idx_`).
- Every statement in `init.sql` stays idempotent - it is re-runnable by design.
- `trigger` is reserved in MariaDB; the rule columns are `trigger_type` and
  `trigger_payload`.

### Commits

Conventional commits, scoped by component:
`feat(backend): dispatch setBrightness through DeviceManager`.

---

## Directory Structure

```text
.
├── backend/
│   ├── CMakeLists.txt
│   ├── Dockerfile              # toolchain / dev / builder / runtime stages
│   ├── include/homestr/        # headers (.hpp)
│   │   ├── controllers/        # REST & WebSocket route handlers
│   │   ├── models/             # domain entities (User, SmartDevice, Rule)
│   │   └── services/           # DeviceManager, HomeStr hub, Database
│   ├── scripts/                # dev-entrypoint.sh
│   ├── src/                    # implementations (.cpp) mirroring include/
│   └── tests/                  # GoogleTest suites
├── frontend/
│   ├── Dockerfile              # dev / builder / runtime (nginx) stages
│   ├── package.json
│   ├── vite.config.ts          # dev server, /api proxy, Vitest config
│   ├── eslint.config.js
│   ├── src/
│   │   ├── api/                # REST client & WebSocket subscription
│   │   ├── components/         # Dashboard, DeviceCard, LoginForm
│   │   ├── types/              # interfaces matching backend models
│   │   └── App.tsx
│   └── tests/                  # Vitest unit tests
├── database/
│   └── init.sql                # MariaDB DDL and seed data
├── specs/                      # domain model and API contract
├── .github/workflows/          # CI: format, build, test, schema, images
├── docker-compose.yml
├── .clang-format
└── .editorconfig
```

---

## Domain Architecture Rules (from UML)

1. **Role-Based Access Control**:
   - `User`: Can authenticate, view dashboard (`getDashboard`), control assigned
     devices (`controlDevice`), and configure triggers (`createRule`).
   - `Admin`: Inherits from `User`. Exclusively authorized to register/remove
     devices and create/delete users.
   - In code the specialisation is the `role` discriminator, not a subclass:
     `User::isAdmin()` gates the admin-only endpoints, which `api-spec.yaml`
     marks `403`. Per-user device assignment is not built yet - see "Open
     items" in `specs/domain-model.md`.
2. **Device Specialization**:
   - `SmartDevice` is the abstract base class containing universal attributes:
     `id` (UUID), `name`, `ipAddress`, and `status`. `type()` is pure virtual,
     so only a concrete specialisation can be instantiated.
   - Concrete implementations (`Light`, `SmartSpeaker`, `GenericDevice`) must
     implement state serialization to JSON - extend the object returned by
     `SmartDevice::toJson()` rather than replacing it - and inherit base network
     control logic (`turnOn`/`turnOff`).
3. **Database Synchronization**:
   - All state updates to `DeviceManager` and `User` must be persisted to the
     MariaDB store using parameterized queries to prevent SQL injection. No
     string-concatenated SQL, ever.

### Component Boundaries

- `HomeStr` (`services/home_str.hpp`) is the only object the controllers hold.
  It owns the `Database` and the `DeviceManager` and coordinates the rules.
- `DeviceManager` is the only component that touches devices, and it is
  internally locked. Never reach around it to mutate a `SmartDevice`.
- Persistence is accessed through the abstract `Database`. Unit tests use
  `InMemoryDatabase`; no test requires a live MariaDB.
- The WebSocket channel is push-only. Clients send `ping` and nothing else;
  state changes arrive as `device_state` frames after a successful command.
- Device state changes are audited in `device_events` whether the command
  succeeded or failed - do not make the audit write conditional on success.

## Security Constraints

- `User::toJson()` never emits `password_hash`; keep it that way, and never log
  a password, a hash, or a bearer token.
- `password_hash` holds a bcrypt/argon2 digest. Plaintext passwords never reach
  the model layer or the database.
- Every MariaDB access uses a prepared statement (see rule 3 above).
- The seeded admin hash in `init.sql` is a placeholder and must be rotated
  before any non-local deployment.
- `.env` is git-ignored; `.env.example` carries placeholders only.
