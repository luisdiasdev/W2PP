# W2PP

### About the Project:

The W2PP is a project based on a decompilation from a Polly's Server Release for the game W2. The project is entirely developed in C/C++ and is built with CMake and vcpkg (see [Building from source](#building-from-source)).

Both servers are headless console applications: they open no window, need no desktop session and are operated with console commands or `Ctrl+C`/`SIGTERM` (see [Running the servers](#running-the-servers)).

We use the latest client build to connect this release.

We do not claim any copyrights, this project was created just for study purposes. All rights reserved to Hanbitsoft.

The initial code was created by Klafke and TheHouse and are maintened by a active community at current moment.

### Building from source

The servers build with CMake and vcpkg on Windows x86 (Win32). Prerequisites:

* Visual Studio 2022 with the "Desktop development with C++" workload (v143 toolset and a Windows 10/11 SDK)
* CMake 3.25 or newer
* vcpkg, with the `VCPKG_ROOT` environment variable pointing at the vcpkg checkout (for example `C:\dev\vcpkg`)

Third-party dependencies (`asio`, `catch2`) are declared in `vcpkg.json` and pinned with `builtin-baseline` (`ec48c11eae9448c8c6e7739d93e06fa2433aa95f`), so no machine-global installs or floating SDK selections are used. `asio` is the standalone (no Boost) header-only build. The project targets Win32 x86 only; configuring any other architecture fails with a clear message.

```powershell
$env:VCPKG_ROOT = "C:\dev\vcpkg"

# Debug
cmake --preset x86-debug
cmake --build --preset x86-debug
ctest --preset x86-debug

# Release
cmake --preset x86-release
cmake --build --preset x86-release
ctest --preset x86-release
```

Configure and build into `build/x86-<config>`. The executables are written to `build/x86-<config>/<Config>/` (`tmsrv.exe`, `dbsrv.exe`, `w2pp_tests.exe`). Run a subset of the tests with `ctest --preset x86-debug -R <regex>`. Configure fails with a clear message if a non-Win32 architecture is requested.

The post-build step also copies `tmsrv.exe` and `dbsrv.exe` into the legacy release tree (by default `../release/TMSRV/run` and `../release/DBSRV/run`, i.e. `E:\wyd\test-setup\release` for this checkout), so they can be started next to the existing runtime data. Debug builds also copy the companion `.pdb` so a debugger can resolve symbols for the deployed binary. Override the destination with `-DW2PP_RELEASE_ROOT=<path>`, or set it to an empty value to disable the copy.

### Running the servers

Both executables are console applications. They open no window, need no desktop session, and change their working directory to the executable's folder, preserving the legacy layout (`../../Common`, `./account`, `./char`, `./Log`). The full runtime trees and startup requirements are documented in [docs/operations.md](docs/operations.md).

Ports are unchanged: TMSrv listens on game port `8281`, DBSrv on game-server `7514` and NP admin `8895`, the TMSrv -> DBSrv link uses `7514`, and the optional billing link uses the address in `biserver.txt`.

```powershell
Set-Location <exe dir>
.\tmsrv.exe
.\dbsrv.exe
```

Boot failures (missing base set, missing `localip.txt`/`serverlist.txt`, listener bind failure, both DB connect attempts failed) are written to the log and standard error, and the process exits non-zero. No modal dialog is shown.

Console commands are read only when stdin is a terminal; set `W2PP_FORCE_CONSOLE=1` to feed commands through a pipe in scripted runs.

| Server | Commands |
|---|---|
| TMSrv | `saveall`, `reboot`, `readguild`, `readguildname`, `readgameconfig`, `reloadmob`, `status`, `quit` |
| DBSrv | `readmobbase`, `status`, `quit` |

`quit`, `Ctrl+C` and `SIGTERM` share one graceful shutdown path: listeners stop, links close, pending data is flushed/closed, logs are flushed and the process exits successfully.

### Debugging with VS Code

`.vscode/` ships launch configurations for both servers. They run the copies in the release tree, which the `x86-debug` build refreshes together with the matching PDB:

1. Build with `Ctrl+Shift+B` (`cmake: build (x86-debug)`).
2. Pick `DBSrv (release tree)`, `TMSrv (release tree)`, or the `Servers: DBSrv + TMSrv` compound in Run and Debug, then press `F5`. Start DBSrv before TMSrv: if both DB connect attempts fail during boot, TMSrv exits.
3. `Attach to DBSrv` / `Attach to TMSrv` attach to an instance you started yourself from the release tree.

The launch configurations use the integrated terminal, so the console commands (`status`, `quit`, ...) keep working while debugging. If `W2PP_RELEASE_ROOT` was overridden, update the `program`/`cwd` paths in `.vscode/launch.json` accordingly.

### Behavior differences from the legacy Win32 servers

The migration preserves the wire format and on-disk formats but intentionally changes the host behavior. These deviations are recorded for reviewers and operators in the [change release notes](openspec/changes/migrate-win32-to-asio/release-notes.md) and summarized here:

1. **No modal dialogs** — fatal boot errors log and exit non-zero; warnings log and continue.
2. **No window or menu** — operator actions are console commands or signals; status is text.
3. **Non-blocking reconnects** — the 200 ms DB retry spacing is preserved, but the event loop keeps running instead of stalling.
4. **Full receive buffer is a protocol violation** — the connection is closed instead of retrying a desynchronized stream.
5. **Checksum mismatch is rejected** — the frame is dropped and the connection closed instead of being delivered with an error flag.
6. **Send errors are observable** — queue-full and write failures are reported instead of silently returning success.
7. **Final frames flush before close** — queued frames are written for up to one second before the socket closes.
8. **`CurrentTime` is monotonic since boot** rather than exactly `timeGetTime`; comparisons are difference-based and `ClientTick` is opaque.
9. **Lower idle memory** — receive buffers are allocated on accept and the per-session send buffer is replaced by a frame queue.

### Content of W2PP Project:

Here's a list with avaliable content in actual project and what means:

* AttributeMap_Editor: Developed to make changes in attributes of Map.
* BISrv: Application deprecated destined to control GameServers and CPU usage.
* ClientPatch_v7662: DLL to unlock Client.
* DBSrv: Application to control accounts, chars and others.
* DropTool: Application to change drop.
* EDITAPMOB: Application to edit summons. (Envocations and Mounts)
* EDITAPNPC: Application to edit npcs.
* ExpTool: Application to change exp rate in mobs.
* NPTool: Application to manage accounts. (Administrative tool)
* SearchPass
* TMSrv: Application to control connections.
* ZerarSkill

### Thigs to do:
- [X] Decompile the release from Polly's Server.
- [X] Stable release.
- [X] Remove all compile warnings.
- [ ] Fix all bugs.
- [ ] Create an documentation.
- [ ] Re-create the project, to make more organized.
- [ ] ???

### General Public License:

The code is under the [GNU GPL v3](https://www.gnu.org/licenses/gpl-3.0.html) license, so you can be feel free to study and contribute.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

### Helpful Links:    
* [Visual Studio 2015](https://www.visualstudio.com/pt-br/downloads/download-visual-studio-vs.aspx)
* [Learn Git/Github](https://help.github.com/articles/good-resources-for-learning-git-and-github/)
* [Learn C/C++](http://www.learn-c.org/)


