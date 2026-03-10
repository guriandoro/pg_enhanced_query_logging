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
#   PEQL_PMM_PORT       Host port for PMM UI              (default: 8443)
#   PEQL_PMM_PASSWORD   PMM admin password                (default: admin)
#   PEQL_PMM_QAN        Enable Query Analytics via pg_stat_statements
#                       (set to 1/true/yes to enable; disabled by default)
#
set -euo pipefail

CONTAINER_NAME="peql-pg18-test"
PG_IMAGE="postgres:18.3"
PG_PORT="${PEQL_PG_PORT:-15432}"
PG_PASSWORD="${PEQL_PG_PASSWORD:-peqltest}"
MEMORY_LIMIT="${PEQL_MEMORY_LIMIT:-50g}"
DISK_LIMIT="${PEQL_DISK_LIMIT:-200g}"
EXTENSION_DIR="/tmp/pg_enhanced_query_logging"

PMM_CONTAINER="peql-pmm-server"
PMM_IMAGE="percona/pmm-server:3"
PMM_PORT="${PEQL_PMM_PORT:-8443}"
PMM_PASSWORD="${PEQL_PMM_PASSWORD:-admin}"
PMM_CLIENT_CONTAINER="peql-pmm-client"
PMM_CLIENT_IMAGE="percona/pmm-client:3"
DOCKER_NETWORK="peql-pmm-net"

case "${PEQL_PMM_QAN:-}" in
    1|true|yes) PMM_QAN=1 ;;
    *)          PMM_QAN=0 ;;
esac

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

wait_for_pmm() {
    local retries=60
    info "Waiting for PMM server to become healthy"
    while true; do
        local status
        status=$(docker inspect --format '{{.State.Health.Status}}' "$PMM_CONTAINER" 2>/dev/null || echo "unknown")
        if [ "$status" = "healthy" ]; then
            break
        fi
        retries=$((retries - 1))
        if [ "$retries" -le 0 ]; then
            fail "PMM server did not become healthy in time"
        fi
        sleep 5
    done
}

# --- teardown ---------------------------------------------------------------

if [ "${1:-}" = "teardown" ]; then
    info "Tearing down container $CONTAINER_NAME"
    docker rm -f "$CONTAINER_NAME" 2>/dev/null || true
    ok "Container removed"

    info "Removing PMM client container $PMM_CLIENT_CONTAINER"
    docker rm -f "$PMM_CLIENT_CONTAINER" 2>/dev/null || true
    ok "PMM client removed"

    if ! docker network inspect "$DOCKER_NETWORK" --format '{{range .Containers}}{{.Name}} {{end}}' 2>/dev/null | grep -v "$PMM_CONTAINER" | grep -q '[a-z]'; then
        info "Removing PMM server container $PMM_CONTAINER"
        docker rm -f "$PMM_CONTAINER" 2>/dev/null || true
        ok "PMM server removed"

        info "Removing Docker network $DOCKER_NETWORK"
        docker network rm "$DOCKER_NETWORK" 2>/dev/null || true
        ok "Network removed"
    else
        info "Other containers still on $DOCKER_NETWORK — keeping PMM server and network"
    fi
    exit 0
fi

# --- pre-flight checks ------------------------------------------------------

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

if ! command -v docker >/dev/null 2>&1; then
    fail "docker is not installed or not in PATH"
fi

# --- Docker network ---------------------------------------------------------

info "Ensuring Docker network $DOCKER_NETWORK exists"
docker network inspect "$DOCKER_NETWORK" >/dev/null 2>&1 ||
    docker network create "$DOCKER_NETWORK"
ok "Network ready"

# --- PMM server -------------------------------------------------------------

if docker ps --format '{{.Names}}' | grep -qx "$PMM_CONTAINER"; then
    info "PMM server container $PMM_CONTAINER is already running — reusing"
else
    if docker ps -a --format '{{.Names}}' | grep -qx "$PMM_CONTAINER"; then
        docker rm -f "$PMM_CONTAINER" >/dev/null
    fi

    info "Pulling $PMM_IMAGE (if not cached)"
    docker pull "$PMM_IMAGE"

    info "Starting PMM server ($PMM_CONTAINER) on port $PMM_PORT"
    docker run -d \
        --name "$PMM_CONTAINER" \
        --network "$DOCKER_NETWORK" \
        -e PMM_ADMIN_PASSWORD="$PMM_PASSWORD" \
        -p "${PMM_PORT}:8443" \
        "$PMM_IMAGE" \
        >/dev/null

    wait_for_pmm
    ok "PMM server is healthy"
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
    --network "$DOCKER_NETWORK" \
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

# --- install pt-query-digest ------------------------------------------------

info "Installing pt-query-digest"
docker exec "$CONTAINER_NAME" bash -c '
    if ! command -v pt-query-digest >/dev/null 2>&1; then
        apt-get install -y -qq --no-install-recommends curl perl >/dev/null 2>&1
        curl -sSL -o /usr/local/bin/pt-query-digest https://percona.com/get/pt-query-digest
        chmod +x /usr/local/bin/pt-query-digest
        chown root:root /usr/local/bin/pt-query-digest
    fi
'
ok "pt-query-digest installed"

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

PRELOAD_LIBS="pg_enhanced_query_logging"
if [ "$PMM_QAN" -eq 1 ]; then
    PRELOAD_LIBS="pg_enhanced_query_logging,pg_stat_statements"
fi

info "Configuring shared_preload_libraries, tuning, and restarting PostgreSQL"
docker exec "$CONTAINER_NAME" bash -c '
    PGCONF="$(psql -U postgres -tAc "SHOW config_file;")"
    echo "shared_preload_libraries = '"'"''"$PRELOAD_LIBS"''"'"'" >> "$PGCONF"
    echo "peql.log_min_duration = 0"   >> "$PGCONF"
    echo "peql.log_verbosity = '"'"'full'"'"'" >> "$PGCONF"
    echo "track_io_timing = on"        >> "$PGCONF"

    # --- performance tuning (50 GB container) ---
    echo "shared_buffers = '"'"'12GB'"'"'"              >> "$PGCONF"
    echo "effective_cache_size = '"'"'36GB'"'"'"         >> "$PGCONF"
    echo "work_mem = '"'"'128MB'"'"'"                   >> "$PGCONF"
    echo "maintenance_work_mem = '"'"'2GB'"'"'"          >> "$PGCONF"
    echo "wal_buffers = '"'"'64MB'"'"'"                 >> "$PGCONF"
    echo "checkpoint_timeout = '"'"'30min'"'"'"          >> "$PGCONF"
    echo "checkpoint_completion_target = 0.9"           >> "$PGCONF"
    echo "max_wal_size = '"'"'10GB'"'"'"                >> "$PGCONF"
    echo "min_wal_size = '"'"'1GB'"'"'"                 >> "$PGCONF"
    echo "random_page_cost = 1.1"                      >> "$PGCONF"
    echo "effective_io_concurrency = 200"               >> "$PGCONF"
    echo "max_parallel_workers_per_gather = 4"          >> "$PGCONF"
    echo "max_parallel_workers = 8"                     >> "$PGCONF"
    echo "max_worker_processes = 16"                    >> "$PGCONF"
    echo "max_connections = 200"                        >> "$PGCONF"
    echo "bgwriter_lru_maxpages = 400"                  >> "$PGCONF"
    echo "bgwriter_lru_multiplier = 4.0"                >> "$PGCONF"
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

# --- PMM client setup -------------------------------------------------------

info "Creating pmm user in PostgreSQL"
docker exec "$CONTAINER_NAME" psql -U postgres -c \
    "CREATE USER pmm WITH SUPERUSER ENCRYPTED PASSWORD 'pmm';"

if [ "$PMM_QAN" -eq 1 ]; then
    info "Creating pg_stat_statements extension (QAN enabled)"
    docker exec "$CONTAINER_NAME" psql -U postgres -c \
        "CREATE EXTENSION IF NOT EXISTS pg_stat_statements;"
fi

if docker ps -a --format '{{.Names}}' | grep -qx "$PMM_CLIENT_CONTAINER"; then
    info "Removing existing PMM client container $PMM_CLIENT_CONTAINER"
    docker rm -f "$PMM_CLIENT_CONTAINER" >/dev/null
fi

info "Pulling $PMM_CLIENT_IMAGE (if not cached)"
docker pull "$PMM_CLIENT_IMAGE"

info "Starting PMM client container ($PMM_CLIENT_CONTAINER)"
docker run -d \
    --name "$PMM_CLIENT_CONTAINER" \
    --network "$DOCKER_NETWORK" \
    -e PMM_AGENT_SERVER_ADDRESS="${PMM_CONTAINER}:8443" \
    -e PMM_AGENT_SERVER_USERNAME=admin \
    -e PMM_AGENT_SERVER_PASSWORD="${PMM_PASSWORD}" \
    -e PMM_AGENT_SERVER_INSECURE_TLS=1 \
    -e PMM_AGENT_SETUP=1 \
    -e PMM_AGENT_CONFIG_FILE=config/pmm-agent.yaml \
    -e PMM_AGENT_PRERUN_SCRIPT="pmm-admin status --wait=10s" \
    "$PMM_CLIENT_IMAGE" \
    >/dev/null

info "Waiting for PMM client to register"
local_retries=30
while ! docker exec "$PMM_CLIENT_CONTAINER" pmm-admin status >/dev/null 2>&1; do
    local_retries=$((local_retries - 1))
    if [ "$local_retries" -le 0 ]; then
        fail "PMM client did not become ready in time"
    fi
    sleep 2
done
ok "PMM client is ready"

PMM_ADD_FLAGS="--username=pmm --password=pmm --host=$CONTAINER_NAME --port=5432"
if [ "$PMM_QAN" -eq 1 ]; then
    PMM_ADD_FLAGS="$PMM_ADD_FLAGS --query-source=pgstatstatements"
else
    PMM_ADD_FLAGS="$PMM_ADD_FLAGS --query-source=none"
fi

info "Adding PostgreSQL service to PMM"
docker exec "$PMM_CLIENT_CONTAINER" pmm-admin add postgresql $PMM_ADD_FLAGS ||
    fail "Failed to add PostgreSQL service to PMM"
ok "PostgreSQL service added to PMM"

# --- summary ----------------------------------------------------------------

echo ""
info "Container $CONTAINER_NAME is running PostgreSQL 18 with pg_enhanced_query_logging loaded"
echo ""
echo "  Connect:      PGPASSWORD=$PG_PASSWORD psql -h localhost -p $PG_PORT -U postgres"
echo "  PMM UI:       https://localhost:$PMM_PORT  (admin / $PMM_PASSWORD)"
echo "  PMM client:   docker exec $PMM_CLIENT_CONTAINER pmm-admin status"
echo "  Teardown:     $0 teardown"
echo ""
echo "  View log:     docker exec $CONTAINER_NAME bash -c 'cat \$(psql -U postgres -tAc \"SHOW data_directory;\")/\$(psql -U postgres -tAc \"SHOW log_directory;\")/peql-slow.log'"
echo "  Digest:       docker exec $CONTAINER_NAME bash -c 'pt-query-digest --type slowlog \$(psql -U postgres -tAc \"SHOW data_directory;\")/\$(psql -U postgres -tAc \"SHOW log_directory;\")/peql-slow.log'"
echo ""
