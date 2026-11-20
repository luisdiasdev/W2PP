# POSIX equivalent of check_platform_dependencies.ps1.
#
# Run with:
#   cmake -DCODE_DIR=<repo>/Code -P tests/tools/check_platform_dependencies.cmake
#
# Project-owned sources must not include Windows platform headers or MSVC-only
# CRT headers, and must not call the corresponding functions directly. The
# platform wrapper directory is the sanctioned seam. Comments and string/char
# literals are masked before matching, so documentation that mentions a legacy
# name is not reported.

cmake_minimum_required(VERSION 3.25)

if(NOT DEFINED CODE_DIR)
  message(FATAL_ERROR "CODE_DIR must be set to the project Code directory")
endif()

if(NOT IS_DIRECTORY "${CODE_DIR}")
  message(FATAL_ERROR "code directory not found: ${CODE_DIR}")
endif()

set(forbidden_headers
  "windows\\.h"
  "winsock\\.h"
  "winsock2\\.h"
  "ws2tcpip\\.h"
  "windowsx\\.h"
  "mmsystem\\.h"
  "afxwin\\.h"
  "afx\\.h"
  "windef\\.h"
  "winbase\\.h"
  "winuser\\.h"
  "wingdi\\.h"
  "winsvc\\.h"
  "io\\.h"
  "mbstring\\.h"
  "tchar\\.h"
  "conio\\.h"
)
string(JOIN "|" forbidden_header_pattern ${forbidden_headers})

set(forbidden_symbols
  # MSVC-only CRT helpers and intrinsics
  "_mbsinc" "_mbschr" "_mbsncpy" "_mbslen" "_strupr" "_strlwr"
  "fopen_s" "sprintf_s" "strcpy_s" "strncpy_s" "_snprintf" "_sntprintf"
  "_open" "_read" "_write" "_lseek" "_close" "_filelength" "_stat" "_pgmptr"
  "__int8" "__int16" "__int32" "__int64" "FLOAT"
  # Win32 types and macros
  "HWND" "HDC" "HFONT" "HMENU" "HINSTANCE" "HBRUSH" "HICON" "HCURSOR"
  "HGDIOBJ" "HMODULE" "FARPROC" "WPARAM" "LPARAM" "LRESULT"
  "BOOL" "TRUE" "FALSE" "UINT" "DWORD" "WORD" "BYTE" "ULONG" "USHORT"
  "LPSTR" "LPCSTR" "LPVOID" "TCHAR" "LPCTSTR"
  "Sleep" "HIBYTE" "LOBYTE" "MAKEWORD" "MAKELONG" "HIWORD" "LOWORD"
  "ZeroMemory" "CopyMemory" "FillMemory" "RtlZeroMemory"
  # Win32 window, dialog, file, process and time API
  "MessageBoxA?" "CreateWindow[A-Za-z]*" "RegisterClass[A-Za-z]*"
  "ShowWindow" "UpdateWindow" "InvalidateRect" "BeginPaint" "EndPaint"
  "TextOutA?" "SetWindowTextA?" "GetMessageA?" "DispatchMessageA?"
  "SetTimer" "KillTimer" "MB_[A-Z_]+" "WndProc"
  "MoveFileA?" "DeleteFileA?" "FindFirstFileA?" "FindNextFileA?" "FindClose"
  "GetModuleFileNameA?" "SetCurrentDirectoryA?" "GetCurrentDirectoryA?"
  "GetLocalTime" "GetSystemTime" "GetTickCount" "GetTickCount64"
  "timeGetTime" "GetLastError" "SetLastError" "CharNextA?" "CharPrevA?"
  "CreateThread" "_beginthread" "_beginthreadex" "WaitForSingleObject"
  "TerminateThread" "GetStdHandle" "WriteConsoleA?" "OutputDebugStringA?"
  "LoadLibraryA?" "GetProcAddress" "FreeLibrary"
  "WSA[A-Za-z]+" "closesocket" "ioctlsocket" "gethostby[A-Za-z]*"
  "gethostname" "getaddrinfo" "inet_ntop" "htonl" "htons" "ntohl" "ntohs"
)
string(JOIN "|" forbidden_symbol_pattern ${forbidden_symbols})

file(GLOB_RECURSE sources "${CODE_DIR}/*.h" "${CODE_DIR}/*.cpp")
list(SORT sources)

set(problems "")
set(scanned 0)

foreach(file ${sources})
  file(RELATIVE_PATH relative "${CODE_DIR}" "${file}")

  if(relative MATCHES "^platform[/\\\\]")
    continue()
  endif()

  file(READ "${file}" content)
  string(REPLACE "\r\n" "\n" content "${content}")
  string(REPLACE "\r" "\n" content "${content}")

  math(EXPR scanned "${scanned}+1")

  set(offset 0)
  set(line_number 0)
  set(in_block_comment FALSE)
  string(LENGTH "${content}" content_length)

  while(offset LESS content_length)
    string(SUBSTRING "${content}" ${offset} -1 rest)
    string(FIND "${rest}" "\n" newline)

    if(newline EQUAL -1)
      set(line "${rest}")
      set(offset ${content_length})
    else()
      string(SUBSTRING "${rest}" 0 ${newline} line)
      math(EXPR offset "${offset}+${newline}+1")
    endif()

    math(EXPR line_number "${line_number}+1")

    # Includes are checked on the line itself: a real directive starts with
    # optional whitespace followed by '#', so a commented-out include is not
    # matched. Skip lines that continue a block comment.
    if(NOT in_block_comment AND line MATCHES "^[ \t]*#[ \t]*include[ \t]*[<\"]([^>\"]+)[>\"]")
      set(included_header "${CMAKE_MATCH_1}")
      if(included_header MATCHES "^(${forbidden_header_pattern})$")
        list(APPEND problems "${relative}:${line_number}: forbidden platform include for '${included_header}'")
      endif()
    endif()

    # Mask string and char literal contents, then strip comments. This keeps
    # names that appear only in documentation out of the symbol scan.
    string(REGEX REPLACE "\"([^\"\\\\]|\\\\.)*\"" "\"\"" masked "${line}")
    string(REGEX REPLACE "'([^'\\\\]|\\\\.)*'" "''" masked "${masked}")

    while(TRUE)
      if(in_block_comment)
        string(FIND "${masked}" "*/" close)
        if(close EQUAL -1)
          set(masked "")
          break()
        endif()
        math(EXPR after_close "${close}+2")
        string(SUBSTRING "${masked}" ${after_close} -1 masked)
        set(in_block_comment FALSE)
      else()
        string(FIND "${masked}" "//" line_comment)
        string(FIND "${masked}" "/*" block_comment)

        if(line_comment EQUAL -1 AND block_comment EQUAL -1)
          break()
        endif()

        if(line_comment GREATER -1 AND block_comment EQUAL -1)
          string(SUBSTRING "${masked}" 0 ${line_comment} masked)
          break()
        endif()

        if(line_comment GREATER -1 AND line_comment LESS block_comment)
          string(SUBSTRING "${masked}" 0 ${line_comment} masked)
          break()
        endif()

        # Only a block comment remains (possibly with a later line comment,
        # which the next loop iteration will handle).
        if(TRUE)
          string(SUBSTRING "${masked}" 0 ${block_comment} prefix)
          math(EXPR after_open "${block_comment}+2")
          string(SUBSTRING "${masked}" ${after_open} -1 rest)
          string(FIND "${rest}" "*/" close)

          if(close EQUAL -1)
            set(masked "${prefix}")
            set(in_block_comment TRUE)
            break()
          endif()

          math(EXPR after_close "${close}+2")
          string(SUBSTRING "${rest}" ${after_close} -1 tail)
          set(masked "${prefix} ${tail}")
        endif()
      endif()
    endwhile()

    # platform::Foo calls are the sanctioned replacement for the raw API.
    string(REGEX REPLACE "platform[ \t]*::[ \t]*[A-Za-z_][A-Za-z0-9_]*" " " masked "${masked}")

    if(masked MATCHES "(^|[^A-Za-z0-9_])(${forbidden_symbol_pattern})([^A-Za-z0-9_]|$)")
      set(offense "${CMAKE_MATCH_2}")
      list(APPEND problems "${relative}:${line_number}: forbidden platform symbol '${offense}'")
    endif()
  endwhile()
endforeach()

list(LENGTH problems problem_count)

if(problem_count GREATER 0)
  message("platform dependency check failed: ${problem_count} offense(s) in project sources")
  foreach(problem ${problems})
    message("  ${problem}")
  endforeach()
  message(FATAL_ERROR "platform dependency check failed")
endif()

message("platform dependency check passed: ${scanned} project source files are free of Win32/MSVC-only usage")
