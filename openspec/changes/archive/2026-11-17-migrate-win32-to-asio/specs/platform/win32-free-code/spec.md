# Spec Delta

## Purpose

Ensures project-owned source code no longer calls Win32 or Winsock directly, and that filesystem, clock, path and diagnostic behavior rely on portable wrappers with defined outcomes.

## ADDED Requirements

### Requirement: No direct Win32 or Winsock usage in project sources

Source files owned by the project SHALL NOT include Windows platform headers or invoke Windows/Winsock API functions directly. Any platform capability SHALL be reached through a project wrapper, and the dependency check SHALL fail the build when a direct usage is reintroduced.

#### Scenario: Direct platform include is added

- **WHEN** a project source or header adds an include for Windows, Winsock, windowsx, mmsystem or the Win32 CRT I/O header
- **THEN** the dependency check fails and names the offending file

#### Scenario: Direct platform call is added

- **WHEN** a project source calls a Win32 file, window, dialog, timer, socket or path function directly
- **THEN** the dependency check fails and names the offending symbol

#### Scenario: Third-party internals are not counted

- **WHEN** a third-party header internally uses the platform API
- **THEN** the check does not flag it, because only project-owned code is in scope

### Requirement: Portable binary file access

Reading and writing fixed-size binary records SHALL preserve existing byte semantics: binary mode, exact record sizes, explicit seek and size queries, and detection of failed reads or writes. Failures SHALL be reported to the caller instead of being ignored.

#### Scenario: Short write

- **WHEN** a record write completes with fewer bytes than requested
- **THEN** the wrapper reports failure and the caller can log or abort the save

#### Scenario: Missing file

- **WHEN** a read is requested for a path that does not exist
- **THEN** the wrapper reports a not-found result without terminating the process

#### Scenario: Byte-identical persistence

- **WHEN** an account, character or capsule record is written and read back through the wrapper
- **THEN** the bytes and record size match the legacy on-disk format exactly

### Requirement: Portable directory and file management

Directory enumeration, file existence, rename and delete operations SHALL expose the same observable outcomes as the previous platform calls, including case-insensitive Windows path behavior and the distinction between "not found" and "operation failed".

#### Scenario: Bulk import scans a directory

- **WHEN** the import directory contains files
- **THEN** enumeration returns the same set the legacy directory scan returned and processes them in the established order

#### Scenario: Rename into a missing directory

- **WHEN** a processed file cannot be moved to its archive location
- **THEN** the wrapper reports failure and the caller logs it while leaving the source file in place

#### Scenario: File is already absent

- **WHEN** deletion is requested for a file that no longer exists
- **THEN** the result distinguishes absence from an access failure

### Requirement: Executable-relative working directory

On startup the process SHALL set its working directory to the executable's directory, as the legacy host did, so existing relative data paths (`../../Common`, `./account`, `./char`, `./Log`, configuration files) resolve unchanged.

#### Scenario: Launch from a different directory

- **WHEN** the executable is launched with a working directory other than its own folder
- **THEN** data files are still located relative to the executable directory

#### Scenario: Executable path unavailable

- **WHEN** the executable directory cannot be determined or entered
- **THEN** startup reports the failure and exits non-zero rather than silently using an unexpected directory

### Requirement: Monotonic and calendar time

The runtime SHALL provide a monotonic millisecond clock for elapsed-time logic and a local calendar time for log naming and scheduled events. Elapsed-time values SHALL preserve the width and wrap behavior expected by existing comparisons.

#### Scenario: Elapsed-time comparison

- **WHEN** a handler compares a stored timestamp with the current monotonic value
- **THEN** the comparison uses the same millisecond unit and unsigned width as before

#### Scenario: Log file naming

- **WHEN** a new daily log is started
- **THEN** its name contains the local date fields in the existing format

### Requirement: Non-blocking delays

Waiting SHALL be implemented as scheduled work on the event loop, not as a process-wide sleep, in any path that runs on the event-loop thread. Consecutive attempts that previously used a fixed delay SHALL preserve the same spacing.

#### Scenario: Retry spacing

- **WHEN** a reconnection path schedules a retry after a delay
- **THEN** the retry occurs after the same interval without stalling other sessions

### Requirement: Diagnostics replace modal dialogs

Every former modal-dialog path SHALL become a non-blocking diagnostic. Fatal initialization failures SHALL log and terminate with a non-zero status; non-fatal conditions SHALL log and continue. Shutdown confirmation SHALL become an explicit operator command or signal.

#### Scenario: Missing content file at boot

- **WHEN** a required content file is missing during initialization
- **THEN** the server writes a diagnostic naming the file and exits non-zero instead of waiting for a user to dismiss a dialog

#### Scenario: Non-fatal warning

- **WHEN** an optional integration or file is unavailable
- **THEN** the server logs the condition and continues running

### Requirement: Status text without a window

Status text previously pushed to a window caption SHALL be written to the operator console and/or the active log stream, retaining the same content and ordering.

#### Scenario: Status update

- **WHEN** a log or status update occurs
- **THEN** the text appears in the console/log sink and no window handle is required
