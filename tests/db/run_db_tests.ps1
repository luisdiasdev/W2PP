#requires -Version 5.1
<#
.SYNOPSIS
    Run a command against a throwaway PostgreSQL 19 container.

.DESCRIPTION
    Starts tests/db/docker-compose.yml under a per-run Compose project, waits
    until the container reports healthy, exports W2PP_TEST_DSN (and the
    W2PP_PG_* variables) and runs -Command. The stack, its containers and any
    volumes are always removed afterwards, including on failure.

    With no -Command the runner only proves the stack can start and stops
    again.

.PARAMETER Command
    Command line to execute while the database is up. It runs in PowerShell.

.PARAMETER Port
    Host port for PostgreSQL. Defaults to $env:W2PP_DB_PORT or 55432.

.PARAMETER TimeoutSeconds
    How long to wait for the healthcheck.
#>
[CmdletBinding()]
param(
    [string]$Command = "",
    [int]$Port = 0,
    [int]$TimeoutSeconds = 90
)

$ErrorActionPreference = "Stop"

$ComposeFile = Join-Path $PSScriptRoot "docker-compose.yml"

if ($Port -le 0) {
    if ($env:W2PP_DB_PORT) {
        $Port = [int]$env:W2PP_DB_PORT
    } else {
        $Port = 55432
    }
}

$env:W2PP_DB_PORT = "$Port"
$Project = "w2pp-dbtest-$PID"

function Remove-Stack {
    try {
        & docker compose -f $ComposeFile -p $Project down --volumes --remove-orphans | Out-Null
    } catch {
        Write-Warning "compose down failed: $_"
    }

    $containers = & docker ps -aq --filter "label=com.docker.compose.project=$Project"
    if ($containers) {
        Write-Warning "containers still present for project ${Project}: $containers"
    }

    $volumes = & docker volume ls -q --filter "label=com.docker.compose.project=$Project"
    if ($volumes) {
        Write-Warning "volumes still present for project ${Project}: $volumes"
    }
}

$exitCode = 0

try {
    if (-not (Get-Command docker -ErrorAction SilentlyContinue)) {
        throw "Docker is not available on PATH; install a container runtime or run with -DW2PP_DB_TESTS=OFF."
    }

    & docker info --format '{{.ServerVersion}}' | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "the Docker daemon is not reachable; start Docker Desktop (or the daemon) and retry."
    }

    Write-Host "Starting PostgreSQL 19 test stack (project $Project, port $Port)..."
    & docker compose -f $ComposeFile -p $Project up --detach
    if ($LASTEXITCODE -ne 0) {
        throw "docker compose up failed"
    }

    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)

    while ($true) {
        $containerId = (& docker compose -f $ComposeFile -p $Project ps -q postgres | Out-String).Trim()

        if ($containerId) {
            $status = (& docker inspect --format '{{if .State.Health}}{{.State.Health.Status}}{{else}}{{.State.Status}}{{end}}' $containerId).Trim()

            if ($status -eq "healthy") {
                break
            }
            if ($status -eq "exited" -or $status -eq "dead") {
                throw "PostgreSQL container stopped before becoming healthy (status: $status)"
            }
        }

        if ((Get-Date) -gt $deadline) {
            throw "timed out after $TimeoutSeconds seconds waiting for PostgreSQL to become healthy"
        }

        Start-Sleep -Milliseconds 1000
    }

    $env:W2PP_TEST_DSN = "host=127.0.0.1 port=$Port dbname=w2pp user=w2pp password=w2pp_test sslmode=disable"
    $env:W2PP_PG_HOST = "127.0.0.1"
    $env:W2PP_PG_PORT = "$Port"
    $env:W2PP_PG_DATABASE = "w2pp"
    $env:W2PP_PG_USER = "w2pp"
    $env:W2PP_PG_PASSWORD = "w2pp_test"
    $env:W2PP_PG_SSLMODE = "disable"

    Write-Host "PostgreSQL is healthy; W2PP_TEST_DSN=$env:W2PP_TEST_DSN"

    if ($Command) {
        & ([scriptblock]::Create($Command))
        $exitCode = if ($null -ne $LASTEXITCODE) { $LASTEXITCODE } else { 0 }
    } else {
        Write-Host "No -Command supplied; the test stack started and will be removed."
    }
} catch {
    Write-Error $_.Exception.Message
    $exitCode = 1
} finally {
    Remove-Stack
}

exit $exitCode
