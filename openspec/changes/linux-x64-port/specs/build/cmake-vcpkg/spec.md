# Spec Delta

## MODIFIED Requirements

### Requirement: CMake build targets

The build SHALL define a shared core library plus separate game-server, DB-server and test executables. It SHALL support Windows x86 and Linux x86_64 targets, each with Debug and Release configurations. Unsupported platforms and architectures SHALL fail configure with a clear message.

#### Scenario: Build both servers

- **WHEN** the build is run for a supported target and configuration
- **THEN** the game server and DB server executables are produced with the shared core linked

#### Scenario: Build tests

- **WHEN** the test target is built
- **THEN** a test executable is produced and registered with CTest

#### Scenario: Unsupported architecture

- **WHEN** a configuration other than a supported target architecture is requested
- **THEN** configure fails with a clear message rather than silently producing a mismatched binary

#### Scenario: Unsupported Windows architecture

- **WHEN** a Windows configuration other than Win32 x86 is requested
- **THEN** configure fails with a clear message rather than silently producing a mismatched binary

#### Scenario: Unsupported Linux architecture

- **WHEN** a Linux configuration other than x86_64 is requested
- **THEN** configure fails with a clear message

#### Scenario: Unsupported platform

- **WHEN** configure runs on a platform other than Windows or Linux
- **THEN** configure fails with a clear message naming the supported targets

### Requirement: Consistent runtime library linkage

On Windows, Debug builds SHALL link the debug CRT and Release builds SHALL link the release CRT, and no Release artifact SHALL import debug runtime libraries. On Linux, each configuration SHALL use the toolchain's system C++ runtime consistently across all targets.

#### Scenario: Release binary inspection

- **WHEN** a Windows Release executable's imports are inspected
- **THEN** only release runtime libraries appear and no debug runtime DLL is required

#### Scenario: Mixed configuration link

- **WHEN** targets are built in the same configuration
- **THEN** all targets in that configuration use the matching runtime library

#### Scenario: Linux runtime selection

- **WHEN** Linux targets are linked in Debug or Release
- **THEN** every target uses the same system C++ runtime and no debug CRT is required

### Requirement: Preserved source and ABI assumptions

The build SHALL preserve the 12-byte wire header and the existing account, character, capsule and selected-character record sizes on every supported target. Layout-sensitive records SHALL use fixed-width integer fields and SHALL NOT depend on pointer width or the platform width of `long`. The C++ standard and default struct packing SHALL be selected explicitly.

#### Scenario: Structure size check

- **WHEN** the build compiles the protocol and persistence structures on Windows x86 or Linux x86_64
- **THEN** a compile-time size check confirms the expected header and record sizes

#### Scenario: LP64 toolset

- **WHEN** the Linux x86_64 toolchain compiles a guarded record
- **THEN** its size and on-disk byte layout match the Windows x86 build

#### Scenario: Toolset change

- **WHEN** a newer compiler is installed on a supported build machine
- **THEN** the project still selects the intended language standard and packing rather than silently changing layout

### Requirement: Automated test execution

The project SHALL expose a single test command per supported target that runs the codec, transport, platform and host tests without live game clients, external servers or content files. Windows SHALL retain its integration harnesses and Linux SHALL provide an equivalent POSIX smoke test.

#### Scenario: Codec regression

- **WHEN** the test suite runs
- **THEN** handshake, framing, obfuscation round-trip, checksum and size-window cases are exercised and any mismatch fails the run

#### Scenario: Loopback transport

- **WHEN** the integration test runs
- **THEN** a listener and client exchange a valid frame over the loopback interface and the decoded payload matches

#### Scenario: Linux test command

- **WHEN** the Linux test preset runs
- **THEN** the unit tests and the Linux server smoke test execute and any failure fails the run

#### Scenario: Missing test dependency

- **WHEN** the test target is configured but the test framework is unavailable
- **THEN** configure fails clearly instead of producing a build that silently omits tests

## ADDED Requirements

### Requirement: Native Linux x86_64 configuration

The build SHALL provide Linux x86_64 configure, build and test presets using the vcpkg Linux triplet. Configuring on Linux SHALL NOT require a Windows toolchain, Wine or a Visual Studio generator. The existing Windows presets SHALL remain available.

#### Scenario: Clean Linux configure

- **GIVEN** a Linux x86_64 host with CMake, Ninja and vcpkg available
- **WHEN** the documented Linux configure preset runs on a clean checkout
- **THEN** dependencies resolve from the pinned manifest and build files are generated without a Windows toolchain

#### Scenario: Build Linux servers

- **WHEN** the Linux Release preset is built
- **THEN** the game server, DB server and test executables are produced

#### Scenario: Windows presets remain available

- **WHEN** a developer configures the existing Windows x86 preset
- **THEN** the previous Windows workflow still succeeds
