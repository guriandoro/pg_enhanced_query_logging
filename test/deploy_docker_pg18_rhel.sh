#!/usr/bin/env bash
#
# Build and run a Rocky Linux 9 + PostgreSQL 18 container with
# pg_enhanced_query_logging compiled, installed, and preloaded.
#
# Usage:
#   ./test/deploy_docker_pg18_rhel.sh          # build, start, verify
#   ./test/deploy_docker_pg18_rhel.sh teardown  # stop and remove
#
# Environment variables (all optional):
#   PEQL_PG_PORT        Host port to expose PostgreSQL on (default: 15433)
#   PEQL_PG_PASSWORD    Password for the postgres user    (default: peqltest)
#   PEQL_MEMORY_LIMIT   Container memory cap              (default: 50g)
#   PEQL_DISK_LIMIT     Container disk cap                (default: 200g)
#   PEQL_PMM_PORT       Host port for PMM UI              (default: 8444)
#   PEQL_PMM_PASSWORD   PMM admin password                (default: admin)
#   PEQL_PMM_QAN        Enable Query Analytics via pg_stat_statements
#                       (set to 1/true/yes to enable; disabled by default)
#
set -euo pipefail

CONTAINER_NAME="peql-pg18-rhel-test"
IMAGE_NAME="peql-pg18-rhel"
PG_PORT="${PEQL_PG_PORT:-15433}"
PG_PASSWORD="${PEQL_PG_PASSWORD:-peqltest}"
MEMORY_LIMIT="${PEQL_MEMORY_LIMIT:-50g}"
DISK_LIMIT="${PEQL_DISK_LIMIT:-200g}"

PMM_CONTAINER="peql-pmm-server"
PMM_IMAGE="percona/pmm-server:3"
PMM_PORT="${PEQL_PMM_PORT:-8444}"
PMM_PASSWORD="${PEQL_PMM_PASSWORD:-admin}"
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

    if ! docker network inspect "$DOCKER_NETWORK" --format '{{range .Containers}}{{.Name}} {{end}}' 2>/dev/null | grep -q '[a-z]'; then
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

# --- build the image --------------------------------------------------------

if docker ps -a --format '{{.Names}}' | grep -qx "$CONTAINER_NAME"; then
    info "Removing existing container $CONTAINER_NAME"
    docker rm -f "$CONTAINER_NAME" >/dev/null
fi

info "Building $IMAGE_NAME image (Rocky Linux 9 + PostgreSQL 18)"
docker build -f "$SCRIPT_DIR/Dockerfile.pg18-rhel" -t "$IMAGE_NAME" "$PROJECT_ROOT"
ok "Image built"

# --- start container --------------------------------------------------------

STORAGE_OPT=()
if supports_storage_opt; then
    STORAGE_OPT=(--storage-opt "size=$DISK_LIMIT")
fi

info "Starting container ($CONTAINER_NAME) on port $PG_PORT"
docker run -d \
    --name "$CONTAINER_NAME" \
    --network "$DOCKER_NETWORK" \
    --memory="$MEMORY_LIMIT" \
    "${STORAGE_OPT[@]}" \
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

if [ "$PMM_QAN" -eq 1 ]; then
    info "Enabling pg_stat_statements for Query Analytics"
    docker exec "$CONTAINER_NAME" bash -c '
        PGCONF="$(psql -U postgres -tAc "SHOW config_file;")"
        sed -i "s/shared_preload_libraries = '\''pg_enhanced_query_logging'\''/shared_preload_libraries = '\''pg_enhanced_query_logging,pg_stat_statements'\''/" "$PGCONF"
    '
    docker restart "$CONTAINER_NAME"
    wait_for_pg
    docker exec "$CONTAINER_NAME" psql -U postgres -c \
        "CREATE EXTENSION IF NOT EXISTS pg_stat_statements;"
    ok "pg_stat_statements enabled"
fi

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

# --- PMM client installation ------------------------------------------------

info "Installing percona-release package"
docker exec --user root "$CONTAINER_NAME" bash -c '
    yum install -y https://repo.percona.com/yum/percona-release-latest.noarch.rpm
' || fail "Failed to install percona-release package"
ok "percona-release installed"

info "Enabling pmm3-client repository"
docker exec --user root "$CONTAINER_NAME" bash -c '
    percona-release enable pmm3-client
' || fail "Failed to enable pmm3-client repository"
ok "pmm3-client repository enabled"

info "Installing pmm-client (this may take a while)"
docker exec --user root "$CONTAINER_NAME" bash -c '
    yum install -y pmm-client
' || fail "Failed to install pmm-client"

docker exec "$CONTAINER_NAME" pmm-admin --version ||
    fail "pmm-admin not found after installation"
ok "pmm-client installed"

# --- PMM client registration ------------------------------------------------

info "Creating pmm user"
docker exec "$CONTAINER_NAME" psql -U postgres -c \
    "CREATE USER pmm WITH SUPERUSER ENCRYPTED PASSWORD 'pmm';"

info "Configuring PMM client"
docker exec --user root "$CONTAINER_NAME" bash -c "
    pmm-admin config --server-insecure-tls \
        --server-url=https://admin:${PMM_PASSWORD}@${PMM_CONTAINER}:8443
" || fail "Failed to configure PMM client"
ok "PMM client configured"

PMM_ADD_FLAGS="--username=pmm --password=pmm"
if [ "$PMM_QAN" -eq 1 ]; then
    PMM_ADD_FLAGS="$PMM_ADD_FLAGS --query-source=pgstatstatements"
else
    PMM_ADD_FLAGS="$PMM_ADD_FLAGS --query-source=none"
fi

info "Adding PostgreSQL service to PMM"
docker exec --user root "$CONTAINER_NAME" bash -c "
    pmm-admin add postgresql $PMM_ADD_FLAGS
" || fail "Failed to add PostgreSQL service to PMM"
ok "PostgreSQL service added to PMM"

# --- summary ----------------------------------------------------------------

echo ""
info "Container $CONTAINER_NAME is running PostgreSQL 18 (Rocky Linux 9) with pg_enhanced_query_logging loaded"
echo ""
echo "  Connect:   PGPASSWORD=$PG_PASSWORD psql -h localhost -p $PG_PORT -U postgres"
echo "  PMM UI:    https://localhost:$PMM_PORT  (admin / $PMM_PASSWORD)"
echo "  Teardown:  $0 teardown"
echo ""
echo "  View log:  docker exec $CONTAINER_NAME bash -c 'cat \$(psql -U postgres -tAc \"SHOW data_directory;\")/\$(psql -U postgres -tAc \"SHOW log_directory;\")/peql-slow.log'"
echo ""
