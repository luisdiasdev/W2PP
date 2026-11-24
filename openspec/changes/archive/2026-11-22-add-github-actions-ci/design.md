# Design

## Context

See `proposal.md` — Why. The repository has no `.github/` directory, so this is the first hosted pipeline. The pieces it must drive already exist:

- `CMakePresets.json` defines self-contained configure/build/test presets: `x86-debug`/`x86-release` (Visual Studio 17 2022, Win32) and `linux-x86-debug`/`linux-x86-release` (Ninja, `-m32`, vcpkg `x86-linux` with the community triplet overlay).
- Presets read `VCPKG_ROOT` and enable vcpkg manifest mode, so the pinned `vcpkg.json` baseline controls dependency versions.
- CTest labels the PowerShell integration harnesses and the POSIX smoke test `integration`; the platform-dependency checks are labelled `static`; the Catch2 unit tests are unlabelled. This makes an exclusion filter (`-LE integration`) sufficient to run the gate the proposal describes.
- The supported platform matrix is Windows x86 and Linux i386 only; the pending `linux-x64-port` change is not applied and is not part of this matrix.

## Goals / Non-Goals

**Goals:**

- One workflow definition that yields four independent jobs: Windows x86 and Linux i386, each in Debug and Release.
- Reuse the existing presets verbatim so CI validates the same commands developers run.
- Deterministic dependency resolution from the pinned manifest, with enough caching that a warm run does not rebuild asio and Catch2.
- A pipeline that needs no repository secrets, publishes nothing, and stays inside the workspace.

**Non-Goals:**

- Running the integration smoke tests in CI, or changing their labels or timeouts.
- Deployment, artifact upload, release creation, signing, coverage, or static analysis beyond the existing platform check.
- Self-hosted runners, containers, cross-compilation, or a second Linux architecture.
- Branch protection or required-check configuration (a repository setting, not a code artifact).

## Decisions

### D1 — Single matrix job, four instances

Use one job with a matrix `include` list rather than separate Windows and Linux jobs:

| `os` | `preset` | `triplet` |
|---|---|---|
| `windows-2022` | `x86-debug` | `x86-windows` |
| `windows-2022` | `x86-release` | `x86-windows` |
| `ubuntu-24.04` | `linux-x86-debug` | `x86-linux` |
| `ubuntu-24.04` | `linux-x86-release` | `x86-linux` |

The same preset name drives configure, build and test because each preset pair already maps to the same binary directory and configuration. Platform-specific setup steps are gated with `if: runner.os == '...'`. `fail-fast: false` keeps the other three results visible when one fails, and the job names (`Windows x86 Debug`, …) identify the failing combination.

Alternatives considered:

- **Separate Windows and Linux jobs with a two-value config matrix**: also four jobs, but duplicates checkout/configure/test steps or forces composite actions; rejected for maintenance cost.
- **One job running all configurations sequentially**: loses parallelism and hides which host failed first; rejected.
- **`windows-latest`/`ubuntu-latest`**: simplest, but a runner-image roll could remove the VS 2022 generator or change the glibc baseline without a repository change. Pinning `windows-2022` (VS 2022 / v143) and `ubuntu-24.04` (glibc 2.39, so `_TIME_BITS=64` is available) keeps the pipeline aligned with the documented prerequisites.

### D2 — Use the presets unchanged, with one CI-only cache override

Each job runs:

```
cmake --preset <preset> -DW2PP_RELEASE_ROOT=
cmake --build --preset <preset>
ctest --preset <preset> -LE integration --output-on-failure
```

`-DW2PP_RELEASE_ROOT=` disables the Windows post-build copy into `../release`, keeping the job inside the workspace; on Linux the default is already empty, so the same command is harmless. Everything else (generator, architecture, triplet, `-m32`, ABI flags, test configuration) comes from the existing presets, so the CI commands and the documented developer commands cannot drift.

Alternatives considered:

- **Adding CI-only presets**: duplicates the preset matrix and risks validating a configuration developers never use; rejected.
- **Inline CMake flags**: bypasses the presets and would not catch broken presets; rejected.

### D3 — Prefer the runner-provided vcpkg, guarantee the baseline commit

Set `VCPKG_ROOT` from the runner's `VCPKG_INSTALLATION_ROOT` (present on both hosted images), and verify the pinned baseline commit exists in that checkout. If it is missing (for example a shallow clone), fetch that commit into the checkout. If the runner has no vcpkg at all, fall back to cloning and bootstrapping a fresh checkout.

The manifest baseline, not the vcpkg tool revision, pins the ports. Using the image's vcpkg avoids cloning ~200 MB per job and avoids depending on a third-party action.

Alternatives considered:

- **`lukka/run-vcpkg` or another third-party action**: convenient, but adds a supply-chain dependency and its own version to keep current; the plain checkout is sufficient.
- **Always clone and bootstrap**: fully controlled but pays the clone/bootstrap cost on every cold job and duplicates what the image already provides.
- **Restore the vcpkg binary cache only and let vcpkg self-provision**: vcpkg still needs a tool checkout to resolve ports; not sufficient alone.

### D4 — Install the Linux i386 prerequisites explicitly

The Linux jobs run an `apt-get` step for `gcc-multilib`, `g++-multilib`, `libc6-dev-i386` and `ninja-build` before configure. These are exactly the prerequisites documented in the README; `ubuntu-24.04` supplies glibc 2.39, so the project's `_TIME_BITS=64` check passes. The Windows image already has the Desktop C++ workload and the VS 2022 generator.

If a future image loses multilib, the project's own configure check fails with the documented message naming the missing packages instead of producing a 32-bit/64-bit mismatch.

### D5 — Gate on `-LE integration`

`ctest --preset <preset> -LE integration` runs the Catch2 unit tests plus the `platform-dependency-check` (`static` label) and skips the integration harnesses. This matches the chosen scope and avoids fixed-port (`7514`, `8281`, `8895`) and process-timing dependencies on shared runners.

A label allow-list (`-L`) was rejected because the unit tests carry no labels today; adding labels to them would be a test change outside this proposal. Running the full suite and retrying integration failures was rejected as slower and flakier.

### D6 — Cache vcpkg binary archives, keyed by manifest and triplet

Use `actions/cache` on the vcpkg binary-archive directory (`%LOCALAPPDATA%\vcpkg\archives` on Windows, `~/.cache/vcpkg/archives` on Linux) with key `vcpkg-<os>-<triplet>-<hash of vcpkg.json>`. Restore keys allow a partial hit when only the baseline hash changes. Debug and Release share a key because the archives are content-addressed and the triplet/compiler/flags are part of vcpkg's internal ABI hash; if two parallel jobs save the same key, the first writer wins and both archives remain valid.

A cache miss simply builds the two dependencies from source. Caching `build/` or CMake state was rejected: it obscures whether a clean configure works, which is the point of the pipeline.

### D7 — Triggers, concurrency and permissions

```yaml
on:
  pull_request:
  push:
    branches: [master]
  workflow_dispatch:

permissions:
  contents: read

concurrency:
  group: ci-${{ github.workflow }}-${{ github.ref }}
  cancel-in-progress: ${{ github.event_name == 'pull_request' }}
```

`master` is the repository's current default branch; if it is renamed, the trigger list changes with it. Read-only `contents` keeps fork PRs working and enforces the no-write requirement. Cancelling superseded PR runs saves runner minutes without cancelling main-branch validation.

### D8 — Documentation lives in the README

Add a short "Continuous integration" section after the build instructions: the four combinations, that the workflow runs on PRs, pushes to `master` and manual dispatch, that integration tests are excluded, and the three local commands per preset. No badge is added because it would encode the repository owner in the README.

## Risks / Trade-offs

- **Cold vcpkg builds are slow (32-bit asio/Catch2 from source)** → binary archive cache, per-job `timeout-minutes` with headroom, and `fail-fast: false` so other results still report. A cold run is expected to be the slowest.
- **Runner image drift removes VS 2022 or changes glibc** → pin `windows-2022`/`ubuntu-24.04`; when an image is retired, update the matrix and re-verify the documented prerequisites in the same PR.
- **Preinstalled vcpkg is stale or shallow and lacks the baseline commit** → fetch the baseline commit before configure, with a clone/bootstrap fallback.
- **Integration tests are not continuously verified** → they remain a documented local/release check; the proposal explicitly scopes them out and the README says so.
- **Linux 32-bit packages unavailable or mirror outage** → the job fails at the documented `apt-get` step, which is visible and actionable, rather than with an opaque compile error.
- **Parallel Debug/Release jobs both save the same cache key** → harmless first-writer-wins; the archives are content-addressed and any stale entry is ignored by vcpkg.
- **`master` is renamed later** → the push trigger silently stops matching; the README and workflow carry the branch name in one place each, and a default-branch rename should update both.

## Migration Plan

1. Add `.github/workflows/ci.yml` with the matrix, setup steps, cache, and preset commands.
2. Add the README section describing the matrix and local reproduction commands.
3. Open a pull request and confirm all four jobs pass on a cold cache, then confirm a warm run reuses the cache without changing resolved versions.
4. Optionally mark the workflow as a required check in repository settings (outside this change).

**Rollback:** delete the workflow file (or revert the commit). No product code, build definition, dependency manifest, or runtime behavior changes, so all local workflows remain intact.

## Open Questions

None. The runner images, preset reuse, vcpkg source, cache scope and test filter are decided above and do not change the spec-level contract.
