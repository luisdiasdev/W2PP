# Dependency check: project-owned sources must not use Win32/Winsock directly.
#
# Scans the project's own .h/.cpp files (everything under -CodeDir except the
# platform wrapper directory) and fails when one of them includes a Windows
# platform header or calls a Win32/Winsock API. Third-party headers that use the
# platform API internally are out of scope because only project sources are
# scanned.
#
# Comments and string/char literals are masked before matching, so documentation
# that merely mentions a legacy API name is not reported.

[CmdletBinding()]
param(
    [string]$CodeDir = (Join-Path (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)) 'Code')
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $CodeDir)) {
    Write-Error "code directory not found: $CodeDir"
    exit 2
}

# Platform wrappers are allowed to use the platform API; they are the seam.
$excludedDir = [System.IO.Path]::GetFullPath((Join-Path $CodeDir 'platform'))

$forbiddenHeaders = @(
    'windows\.h', 'winsock\.h', 'winsock2\.h', 'ws2tcpip\.h', 'windowsx\.h',
    'mmsystem\.h', 'afxwin\.h', 'afx\.h', 'windef\.h', 'winbase\.h',
    'winuser\.h', 'wingdi\.h', 'winsvc\.h', 'io\.h'
)

$forbiddenSymbols = @(
    # Window, GDI, menu and dialog surface
    'HWND', 'HDC', 'HFONT', 'HMENU', 'HINSTANCE', 'HBRUSH', 'HICON', 'HCURSOR',
    'HGDIOBJ', 'HMODULE', 'FARPROC', 'WPARAM', 'LPARAM', 'LRESULT',
    'MessageBoxA?', 'CreateWindow[A-Za-z]*', 'RegisterClass[A-Za-z]*',
    'ShowWindow', 'UpdateWindow', 'InvalidateRect', 'BeginPaint', 'EndPaint',
    'TextOutA?', 'SetWindowTextA?', 'GetMessageA?', 'DispatchMessageA?',
    'SetTimer', 'KillTimer', 'MB_[A-Z_]+', 'WndProc',
    # Winsock and socket API
    'WSA[A-Za-z]+', 'closesocket', 'ioctlsocket', 'gethostby[A-Za-z]*',
    'gethostname', 'getaddrinfo', 'inet_ntop', 'htonl', 'htons', 'ntohl', 'ntohs',
    # Windows types and macros
    'BOOL', 'TRUE', 'FALSE', 'UINT', 'DWORD', 'WORD', 'BYTE', 'ULONG', 'USHORT',
    'LPSTR', 'LPCSTR', 'LPVOID', 'TCHAR', 'LPCTSTR',
    'Sleep', 'HIBYTE', 'LOBYTE', 'MAKEWORD', 'MAKELONG', 'HIWORD', 'LOWORD',
    'ZeroMemory', 'CopyMemory', 'FillMemory', 'RtlZeroMemory',
    # Win32 file, path, process and time API
    '_open', '_read', '_write', '_lseek', '_close', '_filelength', '_stat',
    'MoveFileA?', 'DeleteFileA?', 'FindFirstFileA?', 'FindNextFileA?', 'FindClose',
    'GetModuleFileNameA?', 'SetCurrentDirectoryA?', 'GetCurrentDirectoryA?',
    'GetLocalTime', 'GetSystemTime', 'GetTickCount', 'GetTickCount64',
    'timeGetTime', 'GetLastError', 'SetLastError', 'CharNextA?', 'CharPrevA?',
    'CreateThread', '_beginthread', '_beginthreadex', 'WaitForSingleObject',
    'TerminateThread', 'GetStdHandle', 'WriteConsoleA?', 'OutputDebugStringA?',
    'LoadLibraryA?', 'GetProcAddress', 'FreeLibrary'
)

$headerPattern = '^\s*#\s*include\s*[<"](' + ($forbiddenHeaders -join '|') + ')[>"]'
$symbolPattern = '\b(' + ($forbiddenSymbols -join '|') + ')\b'

# Blanks comments and literals while preserving offsets and line breaks, so the
# reported line numbers still refer to the original file.
function Get-MaskedSource([string]$text) {
    $pattern = '(?s)/\*.*?\*/|//[^\r\n]*|"(?:\\.|[^"\\])*"|''(?:\\.|[^''\\])*'''
    return [regex]::Replace($text, $pattern, {
        param($m)
        $value = $m.Value
        if ($value.StartsWith('//') -or $value.StartsWith('/*')) {
            return [regex]::Replace($value, '[^\r\n]', ' ')
        }
        # Literals keep their quotes so the surrounding code still parses
        # visually, but their contents cannot look like API calls.
        return $value.Substring(0, 1) + ([regex]::Replace($value.Substring(1, $value.Length - 2), '[^\r\n]', ' ')) + $value.Substring($value.Length - 1)
    })
}

$problems = New-Object System.Collections.Generic.List[string]

# Calls into the platform wrapper namespace are the sanctioned replacement for
# the raw API, so they must not be flagged (for example platform::DeleteFile).
$wrapperPattern = '\bplatform\s*::\s*[A-Za-z_][A-Za-z0-9_]*'
$blankEvaluator = { param($m) [regex]::Replace($m.Value, '[^\r\n]', ' ') }

$files = Get-ChildItem -LiteralPath $CodeDir -Recurse -File -Include *.h,*.cpp |
    Where-Object { -not $_.FullName.StartsWith($excludedDir, [System.StringComparison]::OrdinalIgnoreCase) }

foreach ($file in $files) {
    $relative = $file.FullName.Substring($CodeDir.Length).TrimStart('\', '/')
    $masked = Get-MaskedSource ([System.IO.File]::ReadAllText($file.FullName))
    $masked = [regex]::Replace($masked, $wrapperPattern, $blankEvaluator)

    $line = 0
    foreach ($sourceLine in $masked -split "`r?`n") {
        $line++

        $headerMatch = [regex]::Match($sourceLine, $headerPattern)
        if ($headerMatch.Success) {
            $problems.Add("${relative}:${line}: forbidden platform include for '$($headerMatch.Groups[1].Value)'")
            continue
        }

        $symbolMatch = [regex]::Match($sourceLine, $symbolPattern)
        if ($symbolMatch.Success) {
            $problems.Add("${relative}:${line}: forbidden platform symbol '$($symbolMatch.Groups[1].Value)'")
        }
    }
}

if ($problems.Count -gt 0) {
    Write-Output "platform dependency check failed: $($problems.Count) offense(s) in project sources"
    foreach ($problem in $problems) {
        Write-Output "  $problem"
    }
    exit 1
}

Write-Output "platform dependency check passed: $($files.Count) project source files are free of Win32/Winsock usage"
exit 0
