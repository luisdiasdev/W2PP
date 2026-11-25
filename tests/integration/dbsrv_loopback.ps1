param(
    [string]$ExeDir = "",
    [string]$CommonDir = ""
)

$ErrorActionPreference = "Stop"

if ($ExeDir -eq "") { $ExeDir = Join-Path $PSScriptRoot "..\..\build\x86-debug\Debug" }
if ($CommonDir -eq "") { $CommonDir = Join-Path $PSScriptRoot "..\..\build\Common" }

$ExeDir = (Resolve-Path $ExeDir).Path
$CommonDir = [System.IO.Path]::GetFullPath($CommonDir)

# DBSrv now needs PostgreSQL and a completed import. CTest skips this test when
# no database is provisioned; run it through tests/db/run_db_tests.ps1 locally.
if (-not $env:W2PP_PG_HOST) {
    Write-Host "skip: PostgreSQL is not configured (run through tests/db/run_db_tests.ps1)"
    exit 77
}

$exe = Join-Path $ExeDir "dbsrv.exe"
if (-not (Test-Path $exe)) { throw "dbsrv.exe not found at $exe" }

New-Item -ItemType Directory -Force -Path `
    (Join-Path $ExeDir "BaseMob"), `
    (Join-Path $ExeDir "Log"), `
    (Join-Path $ExeDir "Import\Item"), `
    (Join-Path $ExeDir "Import\User"), `
    (Join-Path $ExeDir "Import\Donate"), `
    $CommonDir | Out-Null

$config = @(
    "Sapphire 8",
    "PgHost $($env:W2PP_PG_HOST)",
    "PgPort $($env:W2PP_PG_PORT)",
    "PgDatabase $($env:W2PP_PG_DATABASE)",
    "PgUser $($env:W2PP_PG_USER)",
    "PgSslMode $($env:W2PP_PG_SSLMODE)"
) -join "`n"

Set-Content -Path (Join-Path $ExeDir "Config.txt") -Value $config -Encoding ascii

$dbimport = Join-Path $ExeDir "dbimport.exe"
if (-not (Test-Path $dbimport)) { throw "dbimport.exe not found at $dbimport" }

& $dbimport --empty | Out-Null
if ($LASTEXITCODE -ne 0) { throw "dbimport --empty failed with exit code $LASTEXITCODE" }

foreach ($name in @("TK", "FM", "BM", "HT")) {
    [System.IO.File]::WriteAllBytes((Join-Path $ExeDir "BaseMob\$name"), (New-Object byte[] 816))
}

Copy-Item -Path (Join-Path $PSScriptRoot "..\..\Code\ItemEffect.h") -Destination (Join-Path $CommonDir "ItemEffect.h") -Force

foreach ($name in @("itemlist.csv", "extraitem.csv", "InitItem.csv", "SkillData.csv", "Guilds.txt")) {
    Set-Content -Path (Join-Path $CommonDir $name) -Value "" -Encoding ascii
}

[System.IO.File]::WriteAllBytes((Join-Path $ExeDir "AttributeMap.dat"), (New-Object byte[] (1024 * 1024)))

Set-Content -Path (Join-Path $ExeDir "serverlist.txt") -Value "0 0 127.0.0.1`n0 1 127.0.0.1" -Encoding ascii
Set-Content -Path (Join-Path $ExeDir "Admin.txt") -Value "0 127 0 0 1" -Encoding ascii

$redirectListener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 0)
$redirectListener.Start()
$redirectPort = $redirectListener.LocalEndpoint.Port
$redirectAccept = $redirectListener.AcceptTcpClientAsync()

Set-Content -Path (Join-Path $ExeDir "redirect.txt") -Value "127.0.0.1 $redirectPort test test" -Encoding ascii

$server = Start-Process -FilePath $exe -WorkingDirectory $ExeDir -PassThru -WindowStyle Hidden

$results = @()

function Connect-Loopback([int]$Port, [int]$TimeoutMs = 3000) {
    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    while ([DateTime]::UtcNow -lt $deadline) {
        try {
            $client = [System.Net.Sockets.TcpClient]::new()
            $task = $client.ConnectAsync("127.0.0.1", $Port)
            if ($task.Wait(500) -and $client.Connected) { return $client }
            $client.Close()
        } catch { }
        Start-Sleep -Milliseconds 100
    }
    return $null
}

function Send-InitCode([System.Net.Sockets.TcpClient]$Client) {
    $bytes = [byte[]](0x11, 0xF3, 0x11, 0x1F)
    $Client.GetStream().Write($bytes, 0, $bytes.Length)
}

function Read-WithTimeout([System.Net.Sockets.TcpClient]$Client, [int]$TimeoutMs = 1500) {
    $Client.ReceiveTimeout = $TimeoutMs
    $buffer = New-Object byte[] 4096
    try { return $Client.GetStream().Read($buffer, 0, $buffer.Length) } catch { return 0 }
}

try {
    $game = Connect-Loopback 7514
    if ($game -eq $null) { throw "DB port 7514 did not accept a loopback connection" }
    Send-InitCode $game
    Start-Sleep -Milliseconds 300
    $gameRead = Read-WithTimeout $game
    $results += [pscustomobject]@{ Test = "game accept"; Passed = ($gameRead -gt 0); Detail = "$gameRead bytes" }
    $game.Close()

    $admin = Connect-Loopback 8895
    if ($admin -eq $null) { throw "admin port 8895 did not accept a loopback connection" }
    Send-InitCode $admin
    Start-Sleep -Milliseconds 300
    $adminRead = Read-WithTimeout $admin
    $results += [pscustomobject]@{ Test = "admin accept"; Passed = ($adminRead -gt 0); Detail = "$adminRead bytes" }
    $admin.Close()

    if ($redirectAccept.Wait(5000)) {
        $redirectClient = $redirectAccept.Result
        $stream = $redirectClient.GetStream()
        $stream.ReadTimeout = 2000
        $buffer = New-Object byte[] 4
        $read = $stream.Read($buffer, 0, 4)
        $ok = ($read -eq 4 -and $buffer[0] -eq 0x11 -and $buffer[1] -eq 0xF3 -and $buffer[2] -eq 0x11 -and $buffer[3] -eq 0x1F)
        $results += [pscustomobject]@{ Test = "redirect connect"; Passed = $ok; Detail = ("init bytes: " + (($buffer | ForEach-Object { $_.ToString('X2') }) -join ' ')) }
        $redirectClient.Close()
    } else {
        $results += [pscustomobject]@{ Test = "redirect connect"; Passed = $false; Detail = "no connection" }
    }

    $lan = [System.Net.Dns]::GetHostAddresses([System.Net.Dns]::GetHostName()) |
        Where-Object { $_.AddressFamily -eq "InterNetwork" -and $_.ToString() -ne "127.0.0.1" } |
        Select-Object -First 1
    if ($lan -ne $null) {
        $outer = $null
        try {
            $outer = [System.Net.Sockets.TcpClient]::new()
            $outer.Connect($lan.ToString(), 7514)
            Send-InitCode $outer
            Start-Sleep -Milliseconds 300
            $outerRead = Read-WithTimeout $outer 1000
            $results += [pscustomobject]@{ Test = "unknown IP rejection"; Passed = ($outerRead -eq 0); Detail = "$outerRead bytes from $($lan.ToString())" }
        } catch {
            $results += [pscustomobject]@{ Test = "unknown IP rejection"; Passed = $true; Detail = "connection refused" }
        } finally {
            if ($outer -ne $null) { $outer.Close() }
        }
    } else {
        $results += [pscustomobject]@{ Test = "unknown IP rejection"; Passed = $true; Detail = "skipped (no non-loopback IPv4)" }
    }

    if (-not $server.HasExited) { $server.Kill(); $server.WaitForExit() }
    Start-Sleep -Milliseconds 500

    $commands = Join-Path $ExeDir "console-commands.txt"
    $consoleOut = Join-Path $ExeDir "console.out"
    $consoleErr = Join-Path $ExeDir "console.err"
    Set-Content -Path $commands -Value "status`nreadmobbase`nquit" -Encoding ascii
    Remove-Item $consoleOut, $consoleErr -ErrorAction SilentlyContinue

    $env:W2PP_FORCE_CONSOLE = "1"
    $batch = Join-Path $ExeDir "run-console.bat"
    Set-Content -Path $batch -Value "@echo off`r`n`"$exe`" < `"$commands`"" -Encoding ascii
    $console = Start-Process -FilePath $env:ComSpec -ArgumentList "/c", $batch `
        -WorkingDirectory $ExeDir -PassThru -WindowStyle Hidden `
        -RedirectStandardOutput $consoleOut -RedirectStandardError $consoleErr
    Remove-Item Env:\W2PP_FORCE_CONSOLE -ErrorAction SilentlyContinue

    $exited = $console.WaitForExit(10000)
    if (-not $exited) { $console.Kill() }
    $consoleOutput = if (Test-Path $consoleOut) { Get-Content $consoleOut -Raw } else { "" }
    $consoleOk = $exited -and ($console.ExitCode -eq 0) -and ($consoleOutput -match "server group")
    $results += [pscustomobject]@{ Test = "console commands"; Passed = $consoleOk; Detail = "exit=$($console.ExitCode) status=$($consoleOutput -match 'server group')" }
}
finally {
    if (-not $server.HasExited) { $server.Kill() }
    $redirectListener.Stop()
}

$results | Format-Table -AutoSize
$failed = @($results | Where-Object { -not $_.Passed })
if ($failed.Count -gt 0) { exit 1 }
exit 0
