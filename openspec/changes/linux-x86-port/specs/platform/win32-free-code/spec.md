## MODIFIED Requirements

### Requirement: No direct Win32 or Winsock usage in project sources

Source files owned by the project SHALL NOT include Windows platform headers or invoke Windows/Winsock API functions directly, and shared sources SHALL NOT depend on MSVC-only CRT headers or functions. Any platform capability SHALL be reached through a project wrapper, and the dependency check SHALL fail the build when a direct or MSVC-only usage is reintroduced.

#### Scenario: Direct platform include is added

- **WHEN** a project source or header adds an include for Windows, Winsock, windowsx, mmsystem or the Win32 CRT I/O header
- **THEN** the dependency check fails and names the offending file

#### Scenario: Direct platform call is added

- **WHEN** a project source calls a Win32 file, window, dialog, timer, socket or path function directly
- **THEN** the dependency check fails and names the offending symbol

#### Scenario: MSVC-only CRT usage is added

- **WHEN** a shared source adds an MSVC-only CRT header or function outside a platform wrapper
- **THEN** the cross-platform dependency check or the Linux build fails and names the offending file

#### Scenario: Third-party internals are not counted

- **WHEN** a third-party header internally uses the platform API
- **THEN** the check does not flag it, because only project-owned code is in scope

### Requirement: Portable directory and file management

Directory enumeration, file existence, rename and delete operations SHALL expose the same observable outcomes as the previous platform calls on each supported platform, including the platform's path case rules (case-insensitive on Windows, case-sensitive on Linux) and the distinction between "not found" and "operation failed".

#### Scenario: Bulk import scans a directory

- **WHEN** the import directory contains files
- **THEN** enumeration returns the same set the legacy directory scan returned and processes them in the established order

#### Scenario: Rename into a missing directory

- **WHEN** a processed file cannot be moved to its archive location
- **THEN** the wrapper reports failure and the caller logs it while leaving the source file in place

#### Scenario: File is already absent

- **WHEN** deletion is requested for a file that no longer exists
- **THEN** the result distinguishes absence from an access failure

#### Scenario: Path case rules

- **WHEN** two file names differ only by letter case
- **THEN** Windows resolves them as the same path and Linux resolves them as distinct paths

## ADDED Requirements

### Requirement: Portable runtime paths

Runtime log, day-log, chat-log, item-log and dump paths SHALL use platform-neutral path separators and SHALL resolve to the existing logical directories on every supported target.

#### Scenario: Linux log creation

- **WHEN** a server starts and writes its first dated log on Linux
- **THEN** the file appears under `./Log`, `./DayLog`, `./chat_log` or `./item_log` and not as a file whose name contains literal backslashes

#### Scenario: Windows log path unchanged

- **WHEN** a server writes logs on Windows
- **THEN** the same logical directories and file names are used as before
