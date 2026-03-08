#!/usr/bin/env bash
#
# Build and run a Rocky Linux 9 + PostgreSQL 18 container with
# pg_enhanced_query_logging compiled, installed, and preloaded.
#
# Usage:
#   ./test/deploy_docker_pg18_rhel.sh          # build, start, verify
#   ./test/deploy_docker_pg18_rhel.sh teardown  # stop and remove
#
set -euo pipefail

CONTAINER_NAME="peql-pg18-rhel-test"
IMAGE_NAME="peql-pg18-rhel"
PG_PORT="${PEQL_PG_PORT:-15433}"
PG_PASSWORD="${PEQL_PG_PASSWORD:-peqltest}"

# --- helpers ----------------------------------------------------------------

info()  { printf '\033[1;34m==> %s\033[0m\n' "$*"; }
ok()    { printf '\033[1;32m  ✓ %s\033[0m\n' "$*"; }
fail()  { printf '\033[1;31m  ✗ %s\033[0m\n' "$*"; exit 1; }

wait_for_pg() {
    local retries=30
    while ! docker exec "$CONTAINER_NAME" pg_isready -U postgres >/dev/null 2>&1; do
        retries=$((retries - 1))
        if [ "$retries" -le 0 ]; then
            fail "PostgreSQL did not become ready in time"
        fi
        sleep 1
    done
}

# --- teardown ---------------------------------------------------------------

if [ "${1:-}" = "teardown" ]; then
    info "Tearing down container $CONTAINER_NAME"
    docker rm -f "$CONTAINER_NAME" 2>/dev/null || true
    ok "Container removed"
    exit 0
fi

# --- pre-flight checks ------------------------------------------------------

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

if ! command -v docker >/dev/null 2>&1; then
    fail "docker is not installed or not in PATH"
fi

# --- build the image --------------------------------------------------------

if docker ps -a --format '{{.Names}}' | grep -qx "$CONTAINER_NAME"; then
    info "Removing existing container $CONTAINER_NAME"
    docker rm -f "$CONTAINER_NAME" >/dev/null
fi

info "Building $IMAGE_NAME image (Rocky Linux 9 + PostgreSQL 18)"
docker build -f "$SCRIPT_DIR/Dockerfile.pg18-rhel" -t "$IMAGE_NAME" "$PROJECT_ROOT"
ok "Image built"

# --- start container --------------------------------------------------------

info "Starting container ($CONTAINER_NAME) on port $PG_PORT"
docker run -d \
    --name "$CONTAINER_NAME" \
    -e POSTGRES_PASSWORD="$PG_PASSWORD" \
    -p "${PG_PORT}:5432" \
    "$IMAGE_NAME" \
    >/dev/null

wait_for_pg
ok "PostgreSQL is ready"

docker exec "$CONTAINER_NAME" psql -U postgres -tAc "SELECT version();"

# --- create the extension and set password -----------------------------------

info "Setting postgres password and creating the extension"
docker exec "$CONTAINER_NAME" psql -U postgres -c \
    "ALTER USER postgres PASSWORD '$PG_PASSWORD';"
docker exec "$CONTAINER_NAME" psql -U postgres -c \
    "CREATE EXTENSION pg_enhanced_query_logging;"
ok "Extension created"

docker exec "$CONTAINER_NAME" psql -U postgres -c \
    "SHOW shared_preload_libraries;"

# --- verify logging ---------------------------------------------------------

info "Running a test query to verify logging"
docker exec "$CONTAINER_NAME" psql -U postgres -c \
    "SELECT generate_series(1, 100);" >/dev/null

sleep 1

docker exec "$CONTAINER_NAME" bash -c '
    DATA_DIR=$(psql -U postgres -tAc "SHOW data_directory;")
    LOG_DIR=$(psql -U postgres -tAc "SHOW log_directory;")
    cat "${DATA_DIR}/${LOG_DIR}/peql-slow.log" 2>/dev/null | head -20
' || true

# --- summary ----------------------------------------------------------------

echo ""
info "Container $CONTAINER_NAME is running PostgreSQL 18 (Rocky Linux 9) with pg_enhanced_query_logging loaded"
echo ""
echo "  Connect:   PGPASSWORD=$PG_PASSWORD psql -h localhost -p $PG_PORT -U postgres"
echo "  Teardown:  $0 teardown"
echo ""
echo "  View log:  docker exec $CONTAINER_NAME bash -c 'cat \$(psql -U postgres -tAc \"SHOW data_directory;\")/\$(psql -U postgres -tAc \"SHOW log_directory;\")/peql-slow.log'"
echo ""
