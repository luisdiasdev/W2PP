#!/usr/bin/env bash
#
# Run a command against a throwaway PostgreSQL 19 container.
#
# Starts tests/db/docker-compose.yml under a per-run Compose project, waits for
# the container healthcheck, exports W2PP_TEST_DSN (and W2PP_PG_*) and runs the
# command supplied after `--`. The stack and its volumes are always removed,
# including on failure.
#
# Usage:
#   tests/db/run_db_tests.sh [--port N] [--command "cmd ..."]
#   tests/db/run_db_tests.sh -- ./w2pp_db_tests
#
set -u -o pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
COMPOSE_FILE="$SCRIPT_DIR/docker-compose.yml"

PORT="${W2PP_DB_PORT:-55432}"
COMMAND=""
TIMEOUT_SECONDS=90

while [[ $# -gt 0 ]]; do
    case "$1" in
        --port)
            PORT="$2"
            shift 2
            ;;
        --command)
            COMMAND="$2"
            shift 2
            ;;
        --timeout)
            TIMEOUT_SECONDS="$2"
            shift 2
            ;;
        --)
            shift
            COMMAND="$*"
            break
            ;;
        *)
            COMMAND="$*"
            break
            ;;
    esac
done

export W2PP_DB_PORT="$PORT"
PROJECT="w2pp-dbtest-$$"

compose() {
    docker compose -f "$COMPOSE_FILE" -p "$PROJECT" "$@"
}

cleanup() {
    compose down -v --remove-orphans >/dev/null 2>&1 || true

    local containers volumes
    containers="$(docker ps -aq --filter "label=com.docker.compose.project=$PROJECT")"
    if [[ -n "$containers" ]]; then
        echo "warning: containers still present for project $PROJECT: $containers" >&2
    fi

    volumes="$(docker volume ls -q --filter "label=com.docker.compose.project=$PROJECT")"
    if [[ -n "$volumes" ]]; then
        echo "warning: volumes still present for project $PROJECT: $volumes" >&2
    fi
}
trap cleanup EXIT

if ! command -v docker >/dev/null 2>&1; then
    echo "error: Docker is not available on PATH; install a container runtime or run with -DW2PP_DB_TESTS=OFF." >&2
    exit 1
fi

if ! docker info >/dev/null 2>&1; then
    echo "error: the Docker daemon is not reachable; start Docker Desktop (or the daemon) and retry." >&2
    exit 1
fi

echo "Starting PostgreSQL 19 test stack (project $PROJECT, port $PORT)..."
if ! compose up -d; then
    echo "error: docker compose up failed" >&2
    exit 1
fi

deadline=$((SECONDS + TIMEOUT_SECONDS))
while true; do
    container_id="$(compose ps -q postgres 2>/dev/null || true)"

    if [[ -n "$container_id" ]]; then
        status="$(docker inspect --format '{{if .State.Health}}{{.State.Health.Status}}{{else}}{{.State.Status}}{{end}}' "$container_id" 2>/dev/null || true)"

        if [[ "$status" == "healthy" ]]; then
            break
        fi
        if [[ "$status" == "exited" || "$status" == "dead" ]]; then
            echo "error: PostgreSQL container stopped before becoming healthy (status: $status)" >&2
            exit 1
        fi
    fi

    if (( SECONDS >= deadline )); then
        echo "error: timed out after $TIMEOUT_SECONDS seconds waiting for PostgreSQL" >&2
        exit 1
    fi

    sleep 1
done

export W2PP_TEST_DSN="host=127.0.0.1 port=$PORT dbname=w2pp user=w2pp password=w2pp_test sslmode=disable"
export W2PP_PG_HOST="127.0.0.1"
export W2PP_PG_PORT="$PORT"
export W2PP_PG_DATABASE="w2pp"
export W2PP_PG_USER="w2pp"
export W2PP_PG_PASSWORD="w2pp_test"
export W2PP_PG_SSLMODE="disable"

echo "PostgreSQL is healthy; W2PP_TEST_DSN=$W2PP_TEST_DSN"

if [[ -z "$COMMAND" ]]; then
    echo "No command supplied; the test stack started and will be removed."
    exit 0
fi

set +e
bash -c "$COMMAND"
status=$?
set -e
exit "$status"
