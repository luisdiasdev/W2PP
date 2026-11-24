# build/ci-validation Specification

## Purpose

Keeps every supported build host and configuration continuously verified by hosted CI, so configure, build and the non-integration test suite cannot regress unnoticed between developer machines.

## Requirements

### Requirement: Supported matrix validation

Hosted CI SHALL validate the full supported build matrix: Windows x86 and Linux i386, each in Debug and Release, using the project's documented CMake presets. Every combination SHALL run as its own job so one failing host or configuration does not hide the result of another.

#### Scenario: Pull request runs every combination

- **WHEN** a pull request is opened or updated
- **THEN** a Windows x86 Debug job, a Windows x86 Release job, a Linux i386 Debug job and a Linux i386 Release job each configure, build and run their tests

#### Scenario: One configuration fails

- **WHEN** any single job fails to configure or build
- **THEN** the workflow concludes with a failure while the remaining jobs still run to completion, and the failed job's log identifies the host and configuration

#### Scenario: Default branch is validated

- **WHEN** a commit is pushed to the default branch
- **THEN** the same four combinations are validated

#### Scenario: Manual validation

- **WHEN** a maintainer triggers the workflow manually
- **THEN** the same four combinations are validated and reported

### Requirement: Pinned dependency resolution

CI SHALL resolve third-party dependencies through the vcpkg manifest and its pinned baseline, using the platform triplet selected by the documented presets. CI SHALL NOT rely on machine-global dependency installs or floating versions.

#### Scenario: Clean runner resolves the manifest

- **GIVEN** a runner with an empty vcpkg package cache
- **WHEN** a job configures the project
- **THEN** asio and Catch2 resolve from the pinned manifest and the configure step succeeds

#### Scenario: Triplet selection

- **WHEN** the Linux jobs configure
- **THEN** dependencies are built for the 32-bit Linux triplet and the project targets are 32-bit

#### Scenario: Manifest change

- **WHEN** the dependency manifest or its baseline changes
- **THEN** subsequent jobs resolve the new pinned versions instead of reusing incompatible cached binaries

### Requirement: Non-integration test gate

Each job SHALL run the CTest suite with the integration label excluded and SHALL fail the job when any selected test fails. The unit tests and the static platform-dependency check SHALL be part of the gate; the port- and timing-sensitive integration harnesses SHALL remain outside it.

#### Scenario: Unit tests pass

- **WHEN** a job runs the filtered test command after a successful build
- **THEN** the Catch2 unit tests execute and a zero exit status is required for the job to pass

#### Scenario: Static checks run

- **WHEN** a job runs the filtered test command
- **THEN** the platform-dependency check is selected on both hosts and its failure fails the job

#### Scenario: Integration harnesses are excluded

- **WHEN** a job runs the filtered test command
- **THEN** tests labelled `integration` are not executed, so no server processes or fixed ports are required

#### Scenario: A selected test fails

- **WHEN** any non-integration test returns a non-zero status
- **THEN** the job reports the failing test output and the workflow concludes with a failure

### Requirement: Linux 32-bit prerequisites

The Linux jobs SHALL install the 32-bit multilib C and C++ runtime and development packages before configuring, and SHALL surface the project's existing configure diagnostic when the toolchain cannot produce a 32-bit binary with 64-bit `time_t`.

#### Scenario: Clean Linux runner

- **GIVEN** a fresh Linux runner without 32-bit development headers
- **WHEN** the Linux job prepares the environment
- **THEN** the multilib packages are installed and the i386 configure completes

#### Scenario: Missing multilib support

- **WHEN** the 32-bit toolchain is unavailable despite the setup step
- **THEN** configure fails with the documented message naming the required multilib packages and 64-bit `time_t` requirement

### Requirement: Hermetic and reusable runs

CI SHALL keep each job inside the checked-out workspace, SHALL grant the workflow read-only repository access, and SHALL cache only resolved dependency build outputs keyed by the manifest and triplet. A cache miss SHALL still produce a passing run.

#### Scenario: Cold cache

- **GIVEN** no cached dependency artifacts
- **WHEN** a job runs
- **THEN** dependencies build from source and the job can still pass

#### Scenario: Warm cache

- **WHEN** a job restores a cache created from the same manifest and triplet
- **THEN** the configure step reuses the cached dependency binaries and the resolved versions match the pinned manifest

#### Scenario: No repository writes

- **WHEN** any job runs
- **THEN** the workflow uses read-only repository permissions and the build does not write outside the workspace

### Requirement: No deployment or publishing

The workflow SHALL validate builds and tests only. It SHALL NOT deploy, publish release artifacts, create releases, or require repository secrets.

#### Scenario: Successful validation

- **WHEN** every job passes
- **THEN** no artifacts are published and no deployment step executes

#### Scenario: Fork pull request

- **WHEN** a pull request from a fork runs the workflow
- **THEN** it completes using only read-only access and no secrets

### Requirement: Documented reproduction

The project documentation SHALL describe what CI validates, the host and configuration matrix, and the local commands that reproduce each job.

#### Scenario: Developer reproduces a CI job

- **WHEN** a developer follows the documented commands for one host and configuration
- **THEN** they run the same configure, build and filtered test commands as the corresponding CI job
