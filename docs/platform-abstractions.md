# Platform abstractions

Project sources must not call Win32, Winsock or the Win32 CRT I/O API directly,
and shared sources must not depend on MSVC-only CRT headers or functions.
`Code/platform/` provides the small wrappers used by the servers. The wrappers
are covered by `tests/platform/PlatformTests.cpp`, and
`tests/tools/check_platform_dependencies.cmake` enforces the no-direct-use rule
on POSIX builds (the PowerShell equivalent covers Windows).

## Error model

Every wrapper distinguishes the cases callers need to report differently:

- **Not found**: the path does not exist.
- **Access/I/O failure**: the path exists but cannot be opened, read, written,
  renamed or removed.

No wrapper blocks in a dialog, and none terminates the process except
`ReportFatal`.

## `platform::File`

`platform::File` is a binary file handle with explicit size and seek.

| Operation | Contract |
|---|---|
| `OpenRead(path)` | Opens an existing file for reading. Missing file → `FileError::NotFound`. |
| `OpenReadWrite(path)` | Opens for reading and writing, creating an empty file when absent. **An existing file is not truncated.** |
| `Read(data, size)` | Reads exactly `size` bytes. A short read or I/O error returns `false` and sets `FileError::IoError`. |
| `Write(data, size)` | Writes exactly `size` bytes and flushes. A short write or I/O error returns `false`. |
| `Seek(offset, origin)` | `SEEK_SET`, `SEEK_CUR` or `SEEK_END`. |
| `Size()` | File size in bytes, or `-1`. The current position is preserved, matching `_filelength`. |
| `Tell()` | Current position, or `-1`. |
| `Close()` | Closes the stream; also called by the destructor. |

The tests `platform file round trip preserves bytes and offsets` and
`platform file reports missing files and short reads` cover the record
round-trip, seek/offset behavior, missing files and short reads.

The account, character and capsule records are written through this wrapper
with their existing `sizeof` values; the compile-time sizes are guarded by
`Code/AbiGuards.h`.

## `platform::RenameFile`, `platform::DeleteFile`

Both return `FsResult`:

| Result | Meaning |
|---|---|
| `Ok` | Operation completed. |
| `NotFound` | Source path is absent. |
| `Failed` | Operation failed for another reason. |

- `RenameFile` does **not** replace an existing destination, matching the
  legacy `MoveFile` behavior. A failed rename leaves the source in place.
- `DeleteFile` reports `NotFound` for an already absent file and `Failed` for
  an access error.

`<Windows.h>` maps `DeleteFile` to `DeleteFileA`/`DeleteFileW`;
`FileSystem.h` undefines that macro so the portable name is used.

Covered by `platform filesystem distinguishes not found from failure`.

## `platform::DirectoryIterator`

Enumerates a directory using a legacy-style pattern such as `"./account/*"`,
`"./Import/Item/*.txt"` or a plain directory path (treated as `*`).

- Leaf matching follows the platform path rules: case-insensitive on Windows
  and byte-exact (case-sensitive) on Linux. Both support `*` and `?`.
- `.` and `..` are never returned.
- Each `DirectoryEntry` carries the leaf `name`, a constructed `path`,
  `isDirectory`, and the local `lastWriteTime`.
- `Result()` reports `NotFound` when the directory does not exist and
  `Failed` when it is not a directory or cannot be read.
- Enumeration order is unspecified; callers must not depend on it. The legacy
  scans only compared file sets, and so do the tests.

Covered by `platform directory iterator matches a wildcard scan` and
`platform directory iterator follows platform path case rules`.

## `platform::MonotonicMs`, `platform::ElapsedMs`

- `MonotonicMs()` returns `std::uint32_t` milliseconds from
  `std::chrono::steady_clock`, truncated to 32 bits. The absolute epoch is
  opaque.
- `ElapsedMs(earlier, later)` returns `later - earlier` and is wrap-safe,
  exactly like the legacy `timeGetTime` difference arithmetic.

## `platform::LocalNow`, `platform::LocalTimeFromTimeT`

`LocalTime` carries `year`, `month` (1-12), `day`, `hour`, `minute`, `second`,
`millisecond`, `yearDay` (0-based, like `tm_yday`) and `weekday` (0 = Sunday).
`LocalNow()` and `LocalTimeFromTimeT()` use the local timezone.

Covered by `platform clock provides wrap-safe monotonic and local time`.

## `platform::ExecutablePath`, `platform::ExecutableDirectory`, `platform::SetWorkingDirectoryToExecutable`

The legacy host changed the working directory to the executable's directory so
paths such as `../../Common`, `./account` and `./char` resolved. The wrappers
preserve that:

- `ExecutablePath()` returns the running executable's absolute path, or an
  empty string if it cannot be determined. Windows reads `_pgmptr`; Linux reads
  the `/proc/self/exe` link, growing the buffer when the link is truncated.
- `SetWorkingDirectoryToExecutable()` returns `false` when the directory is
  unavailable instead of silently using an unexpected directory.

Covered by `platform path resolves siblings from a foreign working directory`.

## Paths, case and encoding

- Runtime log, day-log, chat-log, item-log and dump paths use `/` on both
  platforms. Windows accepts forward slashes, so one literal serves both.
- Windows path lookup is case-insensitive; Linux lookup and directory pattern
  matching are case-sensitive. Wrappers expose the platform rule rather than
  papering over it, so a scan never returns a name a later `fopen` cannot open.
- Project sources and content files retain the legacy CP949/ANSI byte encoding;
  they are not transcoded to UTF-8. Helpers that inspect multibyte text preserve
  the CP949 rule that `0x81`-`0xFE` starts a two-byte character, and the
  uppercase helper only folds bytes that the default "C" locale maps (ASCII),
  matching the legacy `_strupr` behavior. ASCII-only build tooling and docs are
  UTF-8.

## Diagnostics and status

| Function | Behavior |
|---|---|
| `ReportWarning(owner, message, caption, flags)` | Formats `caption: message`, writes it to standard error and the registered diagnostic sink, and returns `1` (IDOK). Execution continues. |
| `ReportFatal(owner, message, caption, flags)` | Writes the same line to standard error and the sink, then calls `std::exit(EXIT_FAILURE)`. |
| `PublishStatus(line)` | Writes status text to the status sink, or standard error when no sink is registered. |

`owner` and `flags` keep the legacy modal-dialog call shape; they are ignored.
The servers register a diagnostic sink that forwards to their existing `Log`
function after logging starts, so boot warnings and fatal errors keep their
file-log context. Fatal errors also produce a non-zero exit code.

The only remaining modal calls are the two legacy shutdown-confirmation
dialogs in TMSrv and DBSrv; they disappear with the window procedure in the
headless cutover.
