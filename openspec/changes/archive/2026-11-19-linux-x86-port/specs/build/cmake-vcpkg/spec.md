## MODIFIED Requirements

### Requirement: CMake build targets

The build SHALL define a shared core library plus separate game-server, DB-server and test executables. All targets SHALL build for Windows x86 and Linux i386 (32-bit), and SHALL be configurable in Debug and Release. Unsupported platforms and architectures SHALL fail configure with a clear message.

#### Scenario: Build both servers

- **WHEN** the build is run for a supported target and configuration
- **THEN** the game server and DB server executables are produced with the shared core linked

#### Scenario: Build tests

- **WHEN** the test target is built
- **THEN** a test executable is produced and registered with CTest

#### Scenario: Unsupported architecture

- **WHEN** a configuration other than the supported x86 targets is requested
- **THEN** configure fails with a clear message rather than silently producing a mismatched binary

#### Scenario: Unsupported Windows architecture

- **WHEN** a Windows configuration other than Win32 x86 is requested
- **THEN** configure fails with a clear message rather than silently producing a mismatched binary

#### Scenario: Unsupported Linux architecture

- **WHEN** a Linux configuration other than i386/32-bit is requested
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

The build SHALL preserve the 32-bit layout, default struct packing, message-structure sizes and C++ standard needed for wire and on-disk compatibility on every supported target. On Linux i386, the build SHALL explicitly select the `long long`/`double` alignment and `time_t` width that match the Win32 x86 ABI instead of inheriting the platform default. Build settings that affect structure layout SHALL be explicit rather than inherited from a machine-specific toolset default.

#### Scenario: Structure size check

- **WHEN** the build compiles the protocol and persistence structures
- **THEN** a compile-time size check confirms the expected header size and record sizes

#### Scenario: Win32-compatible Linux layout

- **WHEN** the Linux i386 target compiles the guarded structures
- **THEN** the header and account, character, capsule and selected-character record sizes and field offsets match the Win32 x86 values

#### Scenario: Toolset change

- **WHEN** a newer compiler is installed on the build machine
- **THEN** the project still selects the intended language standard, alignment and packing rather than silently changing layout

### Requirement: Automated test execution

The project SHALL expose a single test command per supported target that runs the transport codec, platform and host tests without requiring live game clients, external servers or content files. Windows SHALL retain its integration harnesses and Linux SHALL provide an equivalent POSIX smoke test.

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

### Requirement: Native Linux i386 configuration

The build SHALL provide Linux i386 configure, build and test presets using the vcpkg `x86-linux` triplet and a 32-bit toolchain. Configuring on Linux SHALL NOT require a Windows toolchain, Wine, or a Visual Studio generator. The existing Windows presets SHALL remain available.

#### Scenario: Clean Linux configure

- **GIVEN** a Linux host with CMake, Ninja or Make, 32-bit multilib support and a bootstrapped vcpkg checkout
- **WHEN** the documented Linux configure preset runs on a clean checkout
- **THEN** dependencies resolve from the pinned manifest and the generated build targets i386 without a Windows toolchain

#### Scenario: Build Linux servers

- **WHEN** the Linux Release preset is built
- **THEN** the game server, DB server and test executables are produced

#### Scenario: Windows presets remain available

- **WHEN** a developer configures an existing Windows x86 preset
- **THEN** the previous Windows workflow still succeeds
