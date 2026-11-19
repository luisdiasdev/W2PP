# build/cmake-vcpkg Specification

## Purpose

Makes both server binaries and their tests build reproducibly from a single CMake project with vcpkg-managed, pinned dependencies on Win32 x86 and Linux i386.

## Requirements

### Requirement: Pinned dependency manifest

Third-party dependencies SHALL be declared in a vcpkg manifest with a pinned baseline. A clean checkout SHALL resolve the same dependency versions without relying on machine-global installs or floating SDK selections.

#### Scenario: Clean machine build

- **GIVEN** a checkout with no prior package cache
- **WHEN** the documented configure command runs with vcpkg available
- **THEN** dependencies resolve from the manifest at the pinned versions and the build proceeds

#### Scenario: Dependency version drift

- **WHEN** the manifest baseline is unchanged
- **THEN** two machines configure the same third-party versions instead of picking up whatever is installed globally

#### Scenario: No Boost dependency

- **WHEN** the transport dependency is configured
- **THEN** it builds in standalone mode and the build does not require or link Boost

### Requirement: CMake build targets

The build SHALL define a shared core library plus separate game-server, DB-server and test executables. All targets SHALL build for Win32 x86 and SHALL be configurable in Debug and Release.

#### Scenario: Build both servers

- **WHEN** the build is run for a supported configuration
- **THEN** the game server and DB server executables are produced with the shared core linked

#### Scenario: Build tests

- **WHEN** the test target is built
- **THEN** a test executable is produced and registered with CTest

#### Scenario: Unsupported architecture

- **WHEN** a configuration other than Win32 x86 is requested
- **THEN** configure fails with a clear message rather than silently producing a mismatched binary

### Requirement: Consistent runtime library linkage

Debug builds SHALL link the debug CRT and Release builds SHALL link the release CRT. No Release artifact SHALL import debug runtime libraries.

#### Scenario: Release binary inspection

- **WHEN** a Release executable's imports are inspected
- **THEN** only release runtime libraries appear and no debug runtime DLL is required

#### Scenario: Mixed configuration link

- **WHEN** targets are built in the same configuration
- **THEN** all targets in that configuration use the matching runtime library

### Requirement: Preserved source and ABI assumptions

The build SHALL preserve the 32-bit layout, default struct packing, message-structure sizes and C++ standard needed for wire and on-disk compatibility. Build settings that affect structure layout SHALL be explicit rather than inherited from a machine-specific toolset default.

#### Scenario: Structure size check

- **WHEN** the build compiles the protocol and persistence structures
- **THEN** a compile-time size check confirms the expected header size and record sizes

#### Scenario: Toolset change

- **WHEN** a newer compiler is installed on the build machine
- **THEN** the project still selects the intended language standard and packing rather than silently changing layout

### Requirement: Automated test execution

The project SHALL expose a single test command that runs the transport codec tests and the loopback integration test without requiring live game clients, external servers or content files.

#### Scenario: Codec regression

- **WHEN** the test suite runs
- **THEN** handshake, framing, obfuscation round-trip, checksum and size-window cases are exercised and any mismatch fails the run

#### Scenario: Loopback transport

- **WHEN** the integration test runs
- **THEN** a listener and client exchange a valid frame over the loopback interface and the decoded payload matches

#### Scenario: Missing test dependency

- **WHEN** the test target is configured but the test framework is unavailable
- **THEN** configure fails clearly instead of producing a build that silently omits tests

### Requirement: Legacy build artifacts removed

The MSBuild solution and project files, Win32 resource scripts and generated resource caches SHALL no longer define the build. Developer documentation SHALL describe the CMake/vcpkg workflow.

#### Scenario: Opening the old solution

- **WHEN** a developer looks for the previous solution or project files
- **THEN** they are absent and the documentation points to the CMake presets and build commands

#### Scenario: Documented workflow

- **WHEN** a new developer follows the README build section on a clean Windows machine
- **THEN** they can configure, build and run tests using the documented commands
