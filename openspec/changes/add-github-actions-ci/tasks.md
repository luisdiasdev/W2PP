# Tasks

## 1. Workflow scaffolding

- [x] 1.1 Create `.github/workflows/ci.yml` with the workflow name, `pull_request`, `push` to `master` and `workflow_dispatch` triggers, `permissions: contents: read`, and the PR-scoped concurrency group; verify the file parses as YAML and `actionlint` (when available) reports no errors.
- [x] 1.2 Add a single `fail-fast: false` matrix job with four explicit entries (`windows-2022` + `x86-debug`, `windows-2022` + `x86-release`, `ubuntu-24.04` + `linux-x86-debug`, `ubuntu-24.04` + `linux-x86-release`) and a job name derived from the host and configuration; verify the workflow expands to exactly four uniquely named jobs after pushing the branch.
- [x] 1.3 Add a per-job `timeout-minutes` with headroom for a cold vcpkg dependency build; verify the value is present in the job definition and the four jobs are independent (one failure does not cancel the others).

## 2. Runner preparation and dependency resolution

- [x] 2.1 Add a step that sets `VCPKG_ROOT` from the runner's `VCPKG_INSTALLATION_ROOT`, verifies the `vcpkg.json` baseline commit exists in that checkout, fetches the commit when it is missing, and falls back to cloning/bootstrapping vcpkg when the runner provides none; verify the step log shows the resolved vcpkg root and confirms the baseline commit.
- [x] 2.2 Add the Linux-only setup step installing `gcc-multilib`, `g++-multilib`, `libc6-dev-i386` and `ninja-build`; verify the step succeeds on `ubuntu-24.04` and a 32-bit compile probe (`echo 'int main(){}' | gcc -m32 -x c - -o /tmp/w2pp-m32-probe`) succeeds before configure.
- [x] 2.3 Add `actions/cache` steps for the vcpkg binary-archive directory (`%LOCALAPPDATA%\vcpkg\archives` on Windows, `~/.cache/vcpkg/archives` on Linux) keyed by OS, triplet and the `vcpkg.json` hash, with a matching restore-key prefix; verify a cold run still passes and a subsequent run reports a cache hit without changing the resolved dependency versions.

## 3. Configure, build and test

- [x] 3.1 Add the configure step running `cmake --preset <matrix preset> -DW2PP_RELEASE_ROOT=`; verify the log shows the expected generator/architecture on Windows, the i386 target on Linux, the correct vcpkg triplet, and that no files are written outside the workspace.
- [x] 3.2 Add the build step running `cmake --build --preset <matrix preset>`; verify each job produces `tmsrv`/`dbsrv`/`w2pp_tests` (with `.exe` on Windows) under the preset's binary directory and that no release-tree copy occurs.
- [x] 3.3 Add the test step running `ctest --preset <matrix preset> -LE integration --output-on-failure`; verify the log lists the Catch2 unit tests and the `platform-dependency-check`, lists no `integration`-labelled test, and that a non-zero test result fails the job.

## 4. Documentation

- [x] 4.1 Add a "Continuous integration" section to `README.md` documenting the four validated combinations, the triggers, the exclusion of integration tests, and the local configure/build/test commands for each preset; verify every documented command matches the workflow and runs as written locally on the matching host.

## 5. Pipeline verification

- [x] 5.1 Push the change to a pull request and verify the workflow runs four uniquely named jobs, all four configure and build, and all four pass the filtered test step on a cold cache.
- [x] 5.2 Re-run the workflow (new commit or re-run) and verify the vcpkg binary cache is restored in every job and the jobs still pass with the same dependency versions.
- [x] 5.3 Verify the pipeline publishes nothing and needs no secrets: inspect the workflow run and confirm there are no upload/deploy/release steps, the run uses read-only repository permissions, and a fork-style pull request would not require credentials.
- [x] 5.4 Confirm the integration harnesses remain untested by CI as scoped: verify `ctest --preset <preset> -LE integration -N` omits `dbsrv-loopback`/`tmsrv-smoke`/`posix-smoke`, and that the README states where those tests are run instead.

## Workflow follow-up

- Archive the change after the pipeline is green and the README section is merged.
- Optionally mark the CI workflow as a required check in repository settings after merge (outside this change).
