# Proposal

## Why

Builds and tests are only verified on developer machines, so a change that breaks the supported Windows x86 or Linux i386 matrix is discovered during review or on the next machine that builds. The matrix has enough moving parts (Win32 x86 under MSVC, i386 multilib, the Linux ABI flags, the community `x86-linux` vcpkg triplet and the pinned manifest) that an automated check on every pull request and merge is the cheapest way to keep both hosts working.

## What Changes

- Add a GitHub Actions workflow that validates the project on the currently supported hosts and configurations:
  - **Windows x86** (`windows-2022`, Visual Studio 2022 generator) with the `x86-debug` and `x86-release` presets.
  - **Linux i386** (`ubuntu-24.04`, Ninja and 32-bit multilib) with the `linux-x86-debug` and `linux-x86-release` presets.
- Each job configures, builds, and runs CTest with the integration label excluded, so the unit tests and the static platform-dependency check run while the port- and timing-sensitive integration harnesses stay local.
- Trigger on pull requests, pushes to the default branch, and manual dispatch. Cancel superseded runs on the same ref.
- Use the runner-provided vcpkg checkout with the pinned manifest baseline, and cache the vcpkg binary artifacts keyed on the manifest and triplet to keep the four jobs practical.
- Keep CI hermetic: disable the legacy release-tree copy with `-DW2PP_RELEASE_ROOT=` instead of writing outside the checkout.
- Document the workflow, its matrix, and the local reproduction commands in the README.

## Capabilities

### New Capabilities

- `build/ci-validation`: hosted continuous-integration validation that every supported host and configuration can resolve the pinned dependencies, configure, build, and pass the non-integration test suite using the documented presets.

### Modified Capabilities

None — the build, host, platform and transport requirements do not change; CI consumes the existing presets and test labels.

## Impact

- **New files**: `.github/workflows/ci.yml`.
- **Documentation**: a continuous-integration section in `README.md`.
- **No product changes**: no changes to `Code/`, `CMakeLists.txt`, `CMakePresets.json`, `vcpkg.json`, or the tests; no runtime, wire-format or on-disk behavior changes.
- **Runner prerequisites**: the Visual Studio 2022 C++ workload on Windows; `gcc-multilib`, `g++-multilib`, `libc6-dev-i386` and a bootstrapped vcpkg on Linux.
- **Out of scope**: deployment, artifact publishing, signing, coverage reporting, the integration smoke tests, macOS, and the x86_64 target still described by the pending `linux-x64-port` change.
