#requires -Version 5.1
<#
.SYNOPSIS
    Live DBSrv + PostgreSQL login smoke test.

.DESCRIPTION
    Requires W2PP_PG_* (provided by tests/db/run_db_tests.ps1):
      1. writes Config.txt for the compose database,
      2. generates a known legacy account tree and imports it with dbimport,
      3. starts DBSrv and waits for the DB port,
      4. runs db_login_probe for a valid login, a wrong password and a missing
         account,
      5. stops DBSrv and removes the fixture.

    Exit 0 when the admitted/wrong/missing signals are the expected ones.
#>
param(
    [string]$ExeDir = "",
    [string]$CommonDir = ""
)

$ErrorActionPreference = "Stop"

if ($ExeDir -eq "") { $ExeDir = Join-Path $PSScriptRoot "..\..\build\x86-debug\Debug" }
if ($CommonDir -eq "") { $CommonDir = Join-Path $PSScriptRoot "..\..\build\Common" }

$ExeDir = (Resolve-Path $ExeDir).Path
$CommonDir = [System.IO.Path]::GetFullPath($CommonDir)

if (-not $env:W2PP_PG_HOST) {
    Write-Host "skip: PostgreSQL is not configured (run through tests/db/run_db_tests.ps1)"
    exit 77
}

$server = $null
$fixtureRoot = Join-Path $env:TEMP "w2pp_e2e_fixture_$PID"

try {
    $config = @(
        "Sapphire 8",
        "PgHost $($env:W2PP_PG_HOST)",
        "PgPort $($env:W2PP_PG_PORT)",
        "PgDatabase $($env:W2PP_PG_DATABASE)",
        "PgUser $($env:W2PP_PG_USER)",
        "PgSslMode $($env:W2PP_PG_SSLMODE)"
    ) -join "`n"
    Set-Content -Path (Join-Path $ExeDir "Config.txt") -Value $config -Encoding ascii

    if (Test-Path $fixtureRoot) { Remove-Item $fixtureRoot -Recurse -Force }

    & (Join-Path $ExeDir "legacy_fixture.exe") $fixtureRoot E2E hunter2 E2ECHAR | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "legacy_fixture failed" }

    & (Join-Path $ExeDir "dbimport.exe") --source $fixtureRoot --report (Join-Path $fixtureRoot "import.txt") | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "dbimport failed" }
    Get-Content (Join-Path $fixtureRoot "import.txt") | ForEach-Object { Write-Host "import: $_" }

    $server = Start-Process -FilePath (Join-Path $ExeDir "dbsrv.exe") `
        -WorkingDirectory $ExeDir -PassThru -WindowStyle Hidden

    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    $listening = $false
    while ([DateTime]::UtcNow -lt $deadline) {
        if ($server.HasExited) { throw "dbsrv exited with code $($server.ExitCode)" }
        try {
            $client = [System.Net.Sockets.TcpClient]::new()
            $task = $client.ConnectAsync("127.0.0.1", 7514)
            if ($task.Wait(300) -and $client.Connected) { $listening = $true; $client.Close(); break }
            $client.Close()
        } catch { }
        Start-Sleep -Milliseconds 250
    }
    if (-not $listening) { throw "DBSrv did not listen on 7514" }

    & (Join-Path $ExeDir "db_login_probe.exe") 127.0.0.1 7514 MISSING hunter2
    $missing = $LASTEXITCODE

    & (Join-Path $ExeDir "db_login_probe.exe") 127.0.0.1 7514 E2E wrong
    $wrong = $LASTEXITCODE

    & (Join-Path $ExeDir "db_login_probe.exe") 127.0.0.1 7514 E2E hunter2
    $admitted = $LASTEXITCODE

    Write-Host "probe results: admitted=$admitted wrong=$wrong missing=$missing"

    if ($admitted -ne 0) {
        if (Test-Path (Join-Path $ExeDir "Log")) {
            Get-ChildItem (Join-Path $ExeDir "Log") -File | ForEach-Object {
                Write-Host "log $($_.Name):"
                Get-Content $_.FullName -Tail 20 | ForEach-Object { Write-Host "  $_" }
            }
        }
        throw "valid login was not admitted"
    }
    if ($wrong -ne 2) { throw "wrong password did not report the failure signal" }
    if ($missing -ne 2) { throw "missing account did not report the failure signal" }
} finally {
    if ($server -ne $null -and -not $server.HasExited) {
        $server.Kill()
        $server.WaitForExit()
    }
    if (Test-Path $fixtureRoot) { Remove-Item $fixtureRoot -Recurse -Force }
}

exit 0
