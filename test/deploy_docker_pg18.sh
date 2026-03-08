#!/usr/bin/env bash
#
# Deploy a PostgreSQL 18 Docker container, compile pg_enhanced_query_logging
# from the host source tree, install and load it into the running instance.
#
# Usage:
#   ./test/deploy_docker_pg18.sh          # build, start, compile, load
#   ./test/deploy_docker_pg18.sh teardown  # stop and remove the container
#
# Environment variables (all optional):
#   PEQL_PG_PORT        Host port to expose PostgreSQL on (default: 15432)
#   PEQL_PG_PASSWORD    Password for the postgres user    (default: peqltest)
#   PEQL_MEMORY_LIMIT   Container memory cap              (default: 50g)
#   PEQL_DISK_LIMIT     Container disk cap                (default: 200g)
#
set -euo pipefail

CONTAINER_NAME="peql-pg18-test"
PG_IMAGE="postgres:18.3"
PG_PORT="${PEQL_PG_PORT:-15432}"
PG_PASSWORD="${PEQL_PG_PASSWORD:-peqltest}"
MEMORY_LIMIT="${PEQL_MEMORY_LIMIT:-50g}"
DISK_LIMIT="${PEQL_DISK_LIMIT:-200g}"
EXTENSION_DIR="/tmp/pg_enhanced_query_logging"

# --- helpers ----------------------------------------------------------------

info()  { printf '\033[1;34m==> %s\033[0m\n' "$*"; }
ok()    { printf '\033[1;32m  ✓ %s\033[0m\n' "$*"; }
fail()  { printf '\033[1;31m  ✗ %s\033[0m\n' "$*"; exit 1; }

supports_storage_opt() {
    docker run --rm --storage-opt size=1g busybox true >/dev/null 2>&1
}

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

# --- start container --------------------------------------------------------

if docker ps -a --format '{{.Names}}' | grep -qx "$CONTAINER_NAME"; then
    info "Removing existing container $CONTAINER_NAME"
    docker rm -f "$CONTAINER_NAME" >/dev/null
fi

info "Pulling $PG_IMAGE (if not cached)"
docker pull "$PG_IMAGE"

STORAGE_OPT=()
if supports_storage_opt; then
    STORAGE_OPT=(--storage-opt "size=$DISK_LIMIT")
fi

info "Starting PostgreSQL 18 container ($CONTAINER_NAME) on port $PG_PORT"
docker run -d \
    --name "$CONTAINER_NAME" \
    --memory="$MEMORY_LIMIT" \
    "${STORAGE_OPT[@]}" \
    -e POSTGRES_PASSWORD="$PG_PASSWORD" \
    -p "${PG_PORT}:5432" \
    "$PG_IMAGE" \
    -c shared_preload_libraries="" \
    >/dev/null

wait_for_pg
ok "PostgreSQL is ready"

# Print server version
docker exec "$CONTAINER_NAME" psql -U postgres -tAc "SELECT version();"

# --- install build dependencies ---------------------------------------------

info "Installing build dependencies inside the container"
docker exec "$CONTAINER_NAME" bash -c '
    apt-get update -qq &&
    apt-get install -y -qq --no-install-recommends \
        build-essential \
        libkrb5-dev \
        postgresql-server-dev-$(pg_config --version | sed "s/PostgreSQL //" | cut -d. -f1) \
        > /dev/null 2>&1
'
ok "Build tools installed"

# --- copy extension source into the container --------------------------------

info "Copying extension source into the container"
docker cp "$PROJECT_ROOT/." "$CONTAINER_NAME:$EXTENSION_DIR"
ok "Source copied to $EXTENSION_DIR"

# --- compile and install the extension ---------------------------------------

info "Compiling the extension"
docker exec -w "$EXTENSION_DIR" "$CONTAINER_NAME" bash -c '
    make clean USE_PGXS=1 2>/dev/null || true
    make USE_PGXS=1 -j$(nproc)
'
ok "Extension compiled"

info "Installing the extension"
docker exec -w "$EXTENSION_DIR" "$CONTAINER_NAME" make install USE_PGXS=1
ok "Extension installed"

# --- load the extension into PostgreSQL --------------------------------------

info "Configuring shared_preload_libraries and restarting PostgreSQL"
docker exec "$CONTAINER_NAME" bash -c '
    PGCONF="$(psql -U postgres -tAc "SHOW config_file;")"
    echo "shared_preload_libraries = '"'"'pg_enhanced_query_logging'"'"'" >> "$PGCONF"
    echo "peql.log_min_duration = 0"   >> "$PGCONF"
    echo "peql.log_verbosity = '"'"'full'"'"'" >> "$PGCONF"
    echo "track_io_timing = on"        >> "$PGCONF"
'

docker restart "$CONTAINER_NAME"
wait_for_pg
ok "PostgreSQL restarted with pg_enhanced_query_logging preloaded"

# --- create the extension and verify ----------------------------------------

info "Creating the extension in the 'postgres' database"
docker exec "$CONTAINER_NAME" psql -U postgres -c \
    "CREATE EXTENSION pg_enhanced_query_logging;"
ok "Extension created"

docker exec "$CONTAINER_NAME" psql -U postgres -c \
    "SHOW shared_preload_libraries;"

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
info "Container $CONTAINER_NAME is running PostgreSQL 18 with pg_enhanced_query_logging loaded"
echo ""
echo "  Connect:   PGPASSWORD=$PG_PASSWORD psql -h localhost -p $PG_PORT -U postgres"
echo "  Teardown:  $0 teardown"
echo ""
echo "  View log:  docker exec $CONTAINER_NAME bash -c 'cat \$(psql -U postgres -tAc \"SHOW data_directory;\")/\$(psql -U postgres -tAc \"SHOW log_directory;\")/peql-slow.log'"
echo ""
