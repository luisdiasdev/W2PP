param(
    [string]$ExeDir = "",
    [string]$CommonDir = ""
)

# Headless TMSrv smoke test.
#
# 1. A missing localip.txt / serverlist.txt exits non-zero with a diagnostic.
# 2. With a fixture runtime tree the server connects to the DB port, listens on
#    the game port, accepts a loopback client with the init handshake and
#    answers the console status/quit commands.

$ErrorActionPreference = "Stop"

if ($ExeDir -eq "") { $ExeDir = Join-Path $PSScriptRoot "..\..\build\x86-debug\Debug" }
if ($CommonDir -eq "") { $CommonDir = Join-Path $PSScriptRoot "..\..\build\Common" }

$ExeDir = (Resolve-Path $ExeDir).Path
$CommonDir = [System.IO.Path]::GetFullPath($CommonDir)

$exe = Join-Path $ExeDir "tmsrv.exe"
if (-not (Test-Path $exe)) { throw "tmsrv.exe not found at $exe" }

$results = @()

function New-FixtureTree {
    New-Item -ItemType Directory -Force -Path `
        (Join-Path $ExeDir "Log"), `
        (Join-Path $ExeDir "BaseMob"), `
        (Join-Path $CommonDir "Settings") | Out-Null

    foreach ($name in @("LevelItem.txt", "NPCGener.txt", "MobName.txt", "Language.txt")) {
        Set-Content -Path (Join-Path $ExeDir $name) -Value "" -Encoding ascii
    }

    [System.IO.File]::WriteAllBytes((Join-Path $ExeDir "heightmap.dat"), (New-Object byte[] 16))
    [System.IO.File]::WriteAllBytes((Join-Path $ExeDir "AttributeMap.dat"), (New-Object byte[] (1024 * 1024)))

    foreach ($name in @("TK", "FM", "BM", "HT")) {
        $path = Join-Path $ExeDir "BaseMob\$name"
        if (-not (Test-Path $path)) {
            [System.IO.File]::WriteAllBytes($path, (New-Object byte[] 816))
        }
    }

    foreach ($name in @("SancRate.txt", "QuestsRate.txt", "CompRate.txt", "MobMerc.txt", "CastleQuest.txt")) {
        Set-Content -Path (Join-Path $CommonDir ("Settings\" + $name)) -Value "" -Encoding ascii
    }

    Copy-Item -Path (Join-Path $PSScriptRoot "..\..\Code\ItemEffect.h") -Destination (Join-Path $CommonDir "ItemEffect.h") -Force

    foreach ($name in @("itemlist.csv", "extraitem.csv", "InitItem.csv", "SkillData.csv", "Guilds.txt")) {
        Set-Content -Path (Join-Path $CommonDir $name) -Value "" -Encoding ascii
    }

    Set-Content -Path (Join-Path $ExeDir "serverlist.txt") -Value "0 0 127.0.0.1`n0 1 127.0.0.1" -Encoding ascii
    Set-Content -Path (Join-Path $ExeDir "localip.txt") -Value "127.0.0.1" -Encoding ascii
}

function Start-Captured($arguments, $stdout, $stderr) {
    return Start-Process -FilePath $exe -ArgumentList $arguments `
        -WorkingDirectory $ExeDir -PassThru -WindowStyle Hidden `
        -RedirectStandardOutput $stdout -RedirectStandardError $stderr
}

function Connect-Loopback([int]$Port, [int]$TimeoutMs = 8000) {
    $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
    while ([DateTime]::UtcNow -lt $deadline) {
        try {
            $client = [System.Net.Sockets.TcpClient]::new()
            $task = $client.ConnectAsync("127.0.0.1", $Port)
            if ($task.Wait(300) -and $client.Connected) { return $client }
            $client.Close()
        } catch { }
        Start-Sleep -Milliseconds 100
    }
    return $null
}

New-FixtureTree

# --- 1. Missing configuration files fail the boot -------------------------

$missingOut = Join-Path $ExeDir "missing.out"
$missingErr = Join-Path $ExeDir "missing.err"

$localIpPath = Join-Path $ExeDir "localip.txt"
Move-Item $localIpPath "$localIpPath.bak" -Force
try {
    $process = Start-Captured $null $missingOut $missingErr
    $process.WaitForExit(15000) | Out-Null
    if (-not $process.HasExited) { $process.Kill() }
    $text = (Get-Content $missingErr -Raw)
    $ok = ($process.ExitCode -ne 0) -and ($text -match "LocalIP")
    $results += [pscustomobject]@{ Test = "missing localip.txt fails boot"; Passed = $ok; Detail = "exit=$($process.ExitCode)" }
} finally {
    Move-Item "$localIpPath.bak" $localIpPath -Force
}

$serverListPath = Join-Path $ExeDir "serverlist.txt"
Move-Item $serverListPath "$serverListPath.bak" -Force
try {
    $process = Start-Captured $null $missingOut $missingErr
    $process.WaitForExit(15000) | Out-Null
    if (-not $process.HasExited) { $process.Kill() }
    $text = (Get-Content $missingErr -Raw)
    $ok = ($process.ExitCode -ne 0) -and ($text -match "server list")
    $results += [pscustomobject]@{ Test = "missing serverlist.txt fails boot"; Passed = $ok; Detail = "exit=$($process.ExitCode)" }
} finally {
    Move-Item "$serverListPath.bak" $serverListPath -Force
}

# --- 2. Headless run with a stub DB link ----------------------------------

$dbListener = $null
$bindDeadline = [DateTime]::UtcNow.AddSeconds(10)

while ($dbListener -eq $null -and [DateTime]::UtcNow -lt $bindDeadline) {
    try {
        $dbListener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 7514)
        $dbListener.Start()
    } catch {
        $dbListener = $null
        Start-Sleep -Milliseconds 300
    }
}

if ($dbListener -eq $null) {
    $results += [pscustomobject]@{ Test = "db link handshake"; Passed = $false; Detail = "port 7514 unavailable" }
}

$dbAccept = $null
if ($dbListener -ne $null) { $dbAccept = $dbListener.AcceptTcpClientAsync() }

$runOut = Join-Path $ExeDir "smoke.out"
$runErr = Join-Path $ExeDir "smoke.err"
$server = Start-Captured $null $runOut $runErr

try {
    if ($dbAccept -ne $null) {
        $dbLinked = $dbAccept.Wait(10000)
        $dbInitOk = $false

        if ($dbLinked) {
            $dbClient = $dbAccept.Result
            $dbClient.ReceiveTimeout = 3000
            $buffer = New-Object byte[] 4
            try {
                $read = $dbClient.GetStream().Read($buffer, 0, 4)
                $dbInitOk = ($read -eq 4 -and $buffer[0] -eq 0x11 -and $buffer[1] -eq 0xF3 -and
                             $buffer[2] -eq 0x11 -and $buffer[3] -eq 0x1F)
            } catch { }

            $results += [pscustomobject]@{ Test = "db link handshake"; Passed = $dbInitOk;
                Detail = ("init bytes: " + (($buffer | ForEach-Object { $_.ToString('X2') }) -join ' ')) }

            $game = Connect-Loopback 8281
            if ($game -eq $null) {
                $results += [pscustomobject]@{ Test = "game accept"; Passed = $false; Detail = "port 8281 did not accept" }
            } else {
                $game.GetStream().Write([byte[]](0x11, 0xF3, 0x11, 0x1F), 0, 4)
                Start-Sleep -Milliseconds 300
                $stillOpen = $game.Connected
                $game.Client.Poll(0, [System.Net.Sockets.SelectMode]::SelectRead) | Out-Null
                $results += [pscustomobject]@{ Test = "game accept + init handshake"; Passed = $stillOpen; Detail = "connected=$stillOpen" }
                $game.Close()
            }

            $dbClient.Close()
        } else {
            $results += [pscustomobject]@{ Test = "db link handshake"; Passed = $false; Detail = "no DB connection" }
        }
    }

    if (-not $server.HasExited) { $server.Kill(); $server.WaitForExit() }
    Start-Sleep -Milliseconds 500

    # --- 3. Console commands -------------------------------------------------

    $commands = Join-Path $ExeDir "tmsrv-console.txt"
    $consoleOut = Join-Path $ExeDir "tmsrv-console.out"
    $consoleErr = Join-Path $ExeDir "tmsrv-console.err"
    Set-Content -Path $commands -Value "status`nsaveall`nquit" -Encoding ascii
    Remove-Item $consoleOut, $consoleErr -ErrorAction SilentlyContinue

    $env:W2PP_FORCE_CONSOLE = "1"
    $batch = Join-Path $ExeDir "run-tmsrv-console.bat"
    Set-Content -Path $batch -Value "@echo off`r`n`"$exe`" < `"$commands`"" -Encoding ascii
    $console = Start-Process -FilePath $env:ComSpec -ArgumentList "/c", $batch `
        -WorkingDirectory $ExeDir -PassThru -WindowStyle Hidden `
        -RedirectStandardOutput $consoleOut -RedirectStandardError $consoleErr
    Remove-Item Env:\W2PP_FORCE_CONSOLE -ErrorAction SilentlyContinue

    $exited = $console.WaitForExit(20000)
    if (-not $exited) { $console.Kill() }
    $consoleOutput = if (Test-Path $consoleOut) { Get-Content $consoleOut -Raw } else { "" }
    $statusOk = $consoleOutput -match "server group 0 index 0"
    $consoleOk = $exited -and ($console.ExitCode -eq 0) -and $statusOk
    $results += [pscustomobject]@{ Test = "console status/saveall/quit"; Passed = $consoleOk;
        Detail = "exit=$($console.ExitCode) status=$statusOk" }
}
finally {
    if (-not $server.HasExited) { $server.Kill(); $server.WaitForExit() }
    if ($dbListener -ne $null -and $dbListener.Server.IsBound) { $dbListener.Stop() }
}

$results | Format-Table -AutoSize
$failed = @($results | Where-Object { -not $_.Passed })
if ($failed.Count -gt 0) { exit 1 }
exit 0
