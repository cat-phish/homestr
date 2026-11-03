# Product backlog

Proposed work for moving HomeStr from its current simulator into a dependable
home automation app. This document describes user outcomes and implementation
direction; [ROADMAP.md](../ROADMAP.md) remains the implementation sequence and
status reference. Update the specifications before implementing changes to
behavior, APIs, or data models.

## Ratings

Size is relative effort, including specifications, implementation, and relevant
verification. It is an estimate, not a delivery commitment:

- **S** — Focused change within an existing feature.
- **M** — Several related changes, potentially across frontend and backend.
- **L** — Substantial work across layers or a new subsystem.
- **XL** — Broad work with significant dependencies or unknowns; split before
  implementation.

Importance describes the value of completing the task:

- **Critical** — Required for trustworthy access and control.
- **High** — Core functionality or reliability needed for everyday use.
- **Medium** — Improves usability or extends an already useful core.

Items are proposed work unless explicitly marked complete.

## Core tasks

### 1. Secure sign-in and session handling

**Size: L · Importance: Critical · Status: Complete**

**Delivered:** Argon2id verification, persisted 24-hour sessions, REST and
WebSocket authorization, logout revocation, administrator device guards,
frontend session validation, user attribution, and interactive administrator
password setup/reset. Verified with unit tests and a disposable MariaDB stack.

**What:** Require valid credentials and an active session to access household
data and control devices. Signing out must end that session.

**How:** Implement password hashing and verification, persisted sessions,
expiration, and logout invalidation. Resolve the signed-in user for protected
REST requests and WebSocket connections. Enforce administrator permissions for
device registration and removal. Have the frontend validate its session rather
than treating a stored token as proof of authentication. Record the acting user
in command history. Verify incorrect passwords, expired and invalidated sessions,
and unauthorized requests.

**Why:** Previously, login accepted any non-empty password for a known account,
and bearer tokens were not enforced. Reliable identity is also a prerequisite
for household isolation, device permissions, and useful activity history.

**Dependencies:** First core task; aligns with roadmap Step 1.

### 2. Preserve automation rules across restarts

**Size: M · Importance: High · Status: Complete**

**Delivered:** Household-scoped persistence for rule definitions, ordered actions,
trigger payloads, and enabled state. Startup restores saved rules; failed writes
leave the previous state intact and return an error. Verified with unit tests
and a disposable MariaDB stack, including forced transaction failures. No schema
migration is required. CI integration remains roadmap Step 3.

**What:** Keep saved routines, their actions, and their enabled or disabled state
when the hub restarts.

**How:** Add rule read/write operations to the database interface and both
implementations. Persist rule creation and enable/disable changes, and restore
rules during startup. Cover deletion when that operation is exposed. Verify a
save-and-restart flow against MariaDB as well as behavior in the in-memory store.
Close the roadmap's SQL integration-test gap alongside this work.

**Why:** Losing routines during a normal restart undermined trust and made
automation difficult to use beyond a demo. Persistence provides the foundation
for the rule editor and scheduler.

**Dependencies:** Aligns with roadmap Steps 2–3.

### 3. Create and run scheduled routines from the dashboard

**Size: XL · Importance: High**

**Progress:** The manual routine form now supports device/action selection,
parameters, ordering, validation, saving, and enable/disable. Scheduling inputs,
timezone handling, next-run display, and the scheduler remain open.

**What:** Let a household member create a routine such as “turn off the kitchen
light every day at 22:00,” view its next run, and pause it.

**How:** Split this into scheduler support and a frontend rule editor. Add
validated schedule handling and a configured timezone, then dispatch due actions
through the existing command path. Define daylight-saving behavior, duplicate
execution prevention, and what happens when the hub misses a scheduled run.
Provide device and action selection, schedule entry, validation feedback, and
enable/disable controls. Show the next scheduled run and verify that disabled
rules do not execute.

**Why:** Scheduling turns the existing manually triggered rule engine into
useful everyday automation. A UI removes the need to construct API requests.

**Dependencies:** Secure access and rule persistence. Aligns with roadmap Steps
4 and 7. Start with one device action per routine to keep the first delivery
manageable.

### 4. Control a physical device with reliable feedback

**Size: XL · Importance: High**

**What:** Make a dashboard command affect a supported physical device, with clear
feedback when the device does not respond.

**How:** Introduce a driver interface behind the device manager and implement
one protocol first, such as Shelly or Tasmota over HTTP. Define what constitutes
confirmation for that protocol. Send commands through the driver, update state
from confirmed results, and report failures without displaying an unconfirmed
change as successful. Add health checks and last-seen tracking so availability
reflects device communication. Verify success, timeout, and recovery behavior
using a controlled device endpoint and physical hardware where available.

**Why:** Commands currently change stored model state without communicating with
hardware. Real device I/O is necessary for HomeStr to function as a home
automation hub, and honest feedback makes its controls trustworthy.

**Dependencies:** Secure access and integration coverage. Aligns with roadmap
Step 5; split driver infrastructure, one device integration, and health
monitoring into separate implementation tasks.

### 5. Discover and register devices from the app

**Size: L · Importance: High**

**What:** Let an administrator find a supported device on the local network,
name it, and add it to the dashboard without using the terminal.

**How:** Implement discovery for the first supported device family. Add an
administrator setup screen with scan progress, discovered candidates, naming,
and registration feedback. Identify devices consistently to prevent duplicate
registration. Persist registrations and provide useful guidance when a scan
finds no supported devices.

**Why:** Device setup is a basic onboarding step. A dashboard that requires
manual API calls or demo seeding is difficult for a household to adopt.

**Dependencies:** Administrator authorization and the first physical-device
integration. Aligns with roadmap Steps 5 and 7.

### 6. Show connection loss and refresh after recovery

**Size: M · Importance: High**

**What:** Tell the user when live dashboard information may be outdated, and
restore current state when the connection returns.

**How:** Expose connection status from the existing reconnecting WebSocket
client. Show a reconnecting indicator and mark displayed state as potentially
outdated. Refresh from an authoritative snapshot after reconnecting, clear the
indicator after synchronization, and handle session expiration through the
sign-in flow. Verify device changes made while the dashboard was disconnected.

**Why:** Automatic reconnection already exists, but users cannot see whether
their dashboard is receiving updates. Visible connection status prevents old
values from being mistaken for current device state.

**Dependencies:** Can begin independently; coordinate session-expiration
behavior with task 1. Fits the frontend work in roadmap Step 7.

## Further candidates

### 7. Household membership and per-device permissions

**Size: XL · Importance: Critical**

**What:** Restrict household data and device control to the appropriate members
and assigned devices.

**How:** Add household membership management and device assignments. Scope
queries, command authorization, automation actions, and WebSocket updates to
the resolved user and household. Verify that users cannot access another
household or bypass assignments through direct requests.

**Why:** The current hub uses a single hard-coded household and does not enforce
per-user device assignments. This becomes essential before supporting multiple
households or members with restricted access.

**Dependencies:** Secure sessions. Aligns with roadmap Step 6; split household
isolation and device assignment into separate tasks.

### 8. View device activity and automation outcomes

**Size: M · Importance: Medium**

**What:** Show recent device commands, who or what initiated them, and whether
they succeeded.

**How:** Build a household-scoped, paginated activity endpoint and a device
history view using existing command audit records. Preserve user attribution
and distinguish manual commands from automation actions. Add rule execution
records if the existing audit data cannot explain automation outcomes.

**Why:** History helps users answer “why did this light turn off?” and diagnose
failed commands without querying the database.

**Dependencies:** Secure access and actor attribution. Household scoping must
follow the permissions model as it develops.

### 9. Run named scenes with one click

**Size: M · Importance: Medium · Status: In progress**

**Delivered:** Create named manual routines in the WebUI, choose supported
commands and parameters, add/remove/reorder actions, save, enable/disable, and
Run. Failed saves retain the draft; pending requests prevent repeated clicks.
The initiating dashboard refreshes device states after running. Verified with
frontend tests and a local browser flow against a disposable backend.

**Remaining:** Per-device access follows task 7. The backend trigger response
currently confirms which rule fired; reporting per-action or partial failures
requires additional API support. The UI does not claim every action succeeded.

**What:** Run a saved group of actions such as “Leaving home” from the dashboard.

**How:** Build on persisted manual rules and their action lists. Add creation
and execution controls, check access to each target device, and report partial
failures when some actions succeed and others fail.

**Why:** Scenes provide useful automation without requiring schedules or
device-state conditions.

**Dependencies:** Rule persistence and a rule-editing UI. Physical-device
integration makes scenes useful beyond the simulator.

### 10. Trigger routines from device state changes

**Size: L · Importance: Medium**

**What:** Run an action when a device enters a selected state.

**How:** Feed confirmed device-state changes into rule evaluation. Add a UI for
selecting the source device, state condition, and target action. Define whether
a trigger fires only on entering the matching state. Prevent repeated execution
and circular rule chains, and dispatch actions outside device-manager locks.
Include state changes reported by hardware, not only commands issued by HomeStr.

**Why:** State-based routines support automation that responds to what happens
in the home. Loop protection is necessary to keep interacting rules reliable.

**Dependencies:** Rule persistence, reliable state events, and a rule editor.
Aligns with roadmap Step 4.

## Suggested delivery order

Secure access, rule persistence, and manual routine creation are complete. The
database integration checks run locally; add them to CI next. Connection
visibility is another useful task. Then add the scheduler and scheduling fields
to the existing form. Follow with one physical-device integration and onboarding
for that device family.

Complete household isolation and assignments before expanding to multiple
households or restricted members. Activity history, scenes, and state-based
triggers can follow as the core becomes dependable. This product grouping does
not replace the detailed engineering sequence in the roadmap.
