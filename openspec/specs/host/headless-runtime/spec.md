# host/headless-runtime Specification

## Purpose

Defines how both servers start, run their periodic work and shut down as headless processes, replacing the Win32 window procedure and menu with an event loop, console controls and signals.

## Requirements

### Requirement: Headless process lifecycle

Both servers SHALL start from a console entry point with no window, message pump, GDI surface or desktop dependency. A normal start SHALL return only when the server has shut down; startup failures SHALL produce a diagnostic and a non-zero exit code.

#### Scenario: Start on a host with no interactive desktop

- **WHEN** the game server or DB server is launched in a non-interactive session
- **THEN** it starts, initializes configuration and begins listening without requiring or creating a window

#### Scenario: Boot configuration is missing

- **WHEN** a required startup file or listener cannot be initialized
- **THEN** the failure is written to the log and standard error, resources acquired so far are released, and the process exits non-zero

#### Scenario: Normal shutdown exit code

- **WHEN** the server completes a requested graceful shutdown
- **THEN** it flushes the shutdown work and exits with success

### Requirement: Periodic tick scheduling

The runtime SHALL invoke each server's existing periodic handlers at its legacy cadence: TMSrv's second-tick every 500 ms and minute-tick every 12,000 ms, and DBSrv's second-tick every 1,000 ms. Ticks SHALL be scheduled from a monotonic clock and SHALL NOT overlap: a new tick is scheduled only after the previous one returns.

#### Scenario: Tick cadence

- **WHEN** the server has been running for ten seconds
- **THEN** the second-tick handler has been invoked about twenty times and the minute-tick handler about once

#### Scenario: Handler exceeds its interval

- **WHEN** a tick handler takes longer than its interval
- **THEN** the next tick is scheduled after it completes and no concurrent invocation occurs

#### Scenario: Existing timing semantics are preserved

- **WHEN** gameplay logic compares counters or calendar fields
- **THEN** it observes the same 500 ms/12 s tick behavior as the previous host, including the existing unit conventions

### Requirement: Graceful shutdown

A shutdown request — console quit, interrupt signal or termination signal — SHALL stop accepting new connections, close client/DB/admin/billing links, persist the state the legacy shutdown path persisted (including guild data and logs), flush diagnostics and exit cleanly.

#### Scenario: Operator requests quit

- **WHEN** the operator issues the quit command or sends an interrupt
- **THEN** the server stops accepting, performs the legacy shutdown writes, closes links and exits without losing buffered log lines

#### Scenario: Shutdown during active traffic

- **WHEN** shutdown is requested while clients are connected
- **THEN** no new sessions are admitted, existing work is allowed to finish its current callback, and the process terminates without deadlock

### Requirement: Operator console replaces the window menu

The runtime SHALL expose the former menu actions as console commands: reload mobs, reboot, save all, read guild, read guild names, read game configuration and exit. Commands SHALL be applied on the event-loop thread in submission order.

#### Scenario: Save-all command

- **WHEN** the operator enters the save-all command
- **THEN** the server performs the same save-all operation previously triggered by the menu and reports completion

#### Scenario: Command ordering

- **WHEN** the operator submits several commands in quick succession
- **THEN** they are applied in the order submitted and cannot interleave with a partially executing command

### Requirement: Single-threaded world mutation

Gameplay state, timers and session state SHALL be mutated only on the event-loop thread. Any input acquisition that needs another thread SHALL hand work to the event loop as queued tasks rather than touching world state directly.

#### Scenario: Console input arrives during a tick

- **WHEN** a console command is read while a tick handler is running
- **THEN** the command waits and executes only after the current handler returns

#### Scenario: No unsynchronized shared state

- **WHEN** the process runs under normal traffic and console use
- **THEN** no world-state field is read or written concurrently from two threads

### Requirement: Runtime status and diagnostics sinks

The runtime SHALL provide a status snapshot equivalent to the former painted window — server group/index, connection counts, uptime, billing and event state — available on demand and periodically in the log. Existing log, chat-log and item-log files SHALL keep their names, formats and rotation behavior.

#### Scenario: Status on demand

- **WHEN** the operator requests status
- **THEN** the current server identity, uptime, connected-user count and link states are printed without disturbing gameplay

#### Scenario: Log content remains usable

- **WHEN** the server writes a log entry
- **THEN** the entry still contains the message plus account/IP context and is written to the same dated log file
