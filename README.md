# W2PP

### About the Project:

The W2PP is a project based on a decompilation from a Polly's Server Release for the game W2. The project is entirely developed in C/C++ and is built with CMake and vcpkg (see [Building from source](#building-from-source)).

We use the latest client build to connect this release.

We do not claim any copyrights, this project was created just for study purposes. All rights reserved to Hanbitsoft.

The initial code was created by Klafke and TheHouse and are maintened by a active community at current moment.

### Building from source

The servers build with CMake and vcpkg on Windows x86 (Win32). Prerequisites:

* Visual Studio 2022 with the "Desktop development with C++" workload (v143 toolset and a Windows 10/11 SDK)
* CMake 3.25 or newer
* vcpkg, with the `VCPKG_ROOT` environment variable pointing at the vcpkg checkout (for example `C:\dev\vcpkg`)

Third-party dependencies (`asio`, `catch2`) are declared in `vcpkg.json` and pinned with `builtin-baseline`, so no machine-global installs are required.

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

The post-build step also copies `tmsrv.exe` and `dbsrv.exe` into the legacy release tree (by default `../release/TMSRV/run` and `../release/DBSRV/run`, i.e. `E:\wyd\test-setup\release` for this checkout), so they can be started next to the existing runtime data. Override the destination with `-DW2PP_RELEASE_ROOT=<path>`, or set it to an empty value to disable the copy.

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


