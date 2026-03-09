#!/usr/bin/env bash
#
# Run pgbench benchmarks against PostgreSQL containers deployed by the
# deploy_docker_pg18*.sh scripts.  Designed to measure the overhead of
# pg_enhanced_query_logging under configurable concurrency and workload
# scenarios.
#
# Usage:
#   ./test/run_pgbench.sh              # run with defaults
#   ./test/run_pgbench.sh init         # (re-)initialize pgbench data only
#   ./test/run_pgbench.sh --help       # show help
#
# All settings are controlled via environment variables (see below).
#
# ── Core pgbench settings ──────────────────────────────────────────────
#   PEQL_BENCH_CLIENTS      Number of concurrent connections      (default: 10)
#   PEQL_BENCH_DURATION     Runtime in seconds                    (default: 60)
#   PEQL_BENCH_MODE         Workload type:
#                             read-only   — SELECT-only (pgbench -S)
#                             read-write  — default TPC-B mix
#                             write-heavy — custom INSERT/UPDATE-heavy script
#                           (default: read-write)
#
# ── Additional pgbench settings ────────────────────────────────────────
#   PEQL_BENCH_THREADS      pgbench threads (default: min(clients, nproc))
#   PEQL_BENCH_SCALE        Scale factor for pgbench -i           (default: 100)
#   PEQL_BENCH_RATE          Target TPS rate limit; 0 = unlimited (default: 0)
#   PEQL_BENCH_PROTOCOL     Query protocol: simple | extended | prepared
#                           (default: prepared)
#   PEQL_BENCH_CUSTOM_SCRIPT  Path to a custom pgbench script     (optional)
#   PEQL_BENCH_INIT         Force (re-)initialization of pgbench
#                           tables: yes | no | auto
#                           (default: auto — init if tables are missing)
#
# ── PEQL extension toggle (A/B comparison) ─────────────────────────────
#   PEQL_BENCH_PEQL_ENABLED      on | off  (default: on)
#   PEQL_BENCH_VERBOSITY         minimal | standard | full
#                                (default: current server setting)
#   PEQL_BENCH_LOG_MIN_DURATION  peql.log_min_duration value
#                                (default: current server setting)
#
# ── Connection settings ────────────────────────────────────────────────
#   PEQL_PG_HOST            PostgreSQL host     (default: localhost)
#   PEQL_PG_PORT            PostgreSQL port     (default: auto-detected
#                           from running container, or 15432)
#   PEQL_PG_PASSWORD        Password            (default: peqltest)
#   PEQL_PG_USER            User                (default: postgres)
#   PEQL_PG_DATABASE        Database            (default: postgres)
#
set -euo pipefail

# ── help ────────────────────────────────────────────────────────────────

if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
    sed -n '2,/^[^#]/{ /^#/s/^# \?//p }' "$0"
    exit 0
fi

# ── helpers ─────────────────────────────────────────────────────────────

info()  { printf '\033[1;34m==> %s\033[0m\n' "$*"; }
ok()    { printf '\033[1;32m  ✓ %s\033[0m\n' "$*"; }
warn()  { printf '\033[1;33m  ! %s\033[0m\n' "$*"; }
fail()  { printf '\033[1;31m  ✗ %s\033[0m\n' "$*"; exit 1; }

# ── detect container and auto-resolve port ──────────────────────────────

detect_container() {
    for name in peql-pg18-test peql-pg18-rhel-test; do
        if docker ps --format '{{.Names}}' 2>/dev/null | grep -qx "$name"; then
            echo "$name"
            return
        fi
    done
    echo ""
}

port_for_container() {
    local name="$1"
    docker port "$name" 5432 2>/dev/null | head -1 | sed 's/.*://'
}

CONTAINER=$(detect_container)

AUTO_PORT=""
if [[ -z "${PEQL_PG_PORT:-}" && -n "$CONTAINER" ]]; then
    AUTO_PORT=$(port_for_container "$CONTAINER")
    if [[ -n "$AUTO_PORT" ]]; then
        info "Auto-detected container $CONTAINER on port $AUTO_PORT"
    fi
fi

# ── configuration ───────────────────────────────────────────────────────

PG_HOST="${PEQL_PG_HOST:-localhost}"
PG_PORT="${PEQL_PG_PORT:-${AUTO_PORT:-15432}}"
PG_PASSWORD="${PEQL_PG_PASSWORD:-peqltest}"
PG_USER="${PEQL_PG_USER:-postgres}"
PG_DATABASE="${PEQL_PG_DATABASE:-postgres}"

CLIENTS="${PEQL_BENCH_CLIENTS:-10}"
DURATION="${PEQL_BENCH_DURATION:-60}"
MODE="${PEQL_BENCH_MODE:-read-write}"
SCALE="${PEQL_BENCH_SCALE:-100}"
RATE="${PEQL_BENCH_RATE:-0}"
PROTOCOL="${PEQL_BENCH_PROTOCOL:-prepared}"
CUSTOM_SCRIPT="${PEQL_BENCH_CUSTOM_SCRIPT:-}"
INIT_MODE="${PEQL_BENCH_INIT:-auto}"

PEQL_ENABLED="${PEQL_BENCH_PEQL_ENABLED:-on}"
PEQL_VERBOSITY="${PEQL_BENCH_VERBOSITY:-}"
PEQL_LOG_MIN_DURATION="${PEQL_BENCH_LOG_MIN_DURATION:-}"

NPROC=$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)
THREADS="${PEQL_BENCH_THREADS:-$(( CLIENTS < NPROC ? CLIENTS : NPROC ))}"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
RESULTS_DIR="$SCRIPT_DIR/bench_results"

export PGPASSWORD="$PG_PASSWORD"

psql_cmd() {
    psql -h "$PG_HOST" -p "$PG_PORT" -U "$PG_USER" -d "$PG_DATABASE" \
         -tAX "$@"
}

pgbench_cmd() {
    pgbench -h "$PG_HOST" -p "$PG_PORT" -U "$PG_USER" "$PG_DATABASE" "$@"
}

# ── validate ────────────────────────────────────────────────────────────

info "Validating connection to PostgreSQL at $PG_HOST:$PG_PORT"
if ! psql_cmd -c "SELECT 1;" >/dev/null 2>&1; then
    fail "Cannot connect to PostgreSQL at $PG_HOST:$PG_PORT (user=$PG_USER, db=$PG_DATABASE)"
fi
ok "PostgreSQL is reachable"

PG_VERSION=$(psql_cmd -c "SHOW server_version;")
info "Server version: $PG_VERSION"

# ── validate settings ───────────────────────────────────────────────────

case "$MODE" in
    read-only|read-write|write-heavy) ;;
    *) fail "Invalid PEQL_BENCH_MODE '$MODE'. Must be: read-only, read-write, write-heavy" ;;
esac

case "$PROTOCOL" in
    simple|extended|prepared) ;;
    *) fail "Invalid PEQL_BENCH_PROTOCOL '$PROTOCOL'. Must be: simple, extended, prepared" ;;
esac

case "$PEQL_ENABLED" in
    on|off) ;;
    *) fail "Invalid PEQL_BENCH_PEQL_ENABLED '$PEQL_ENABLED'. Must be: on, off" ;;
esac

if [[ -n "$PEQL_VERBOSITY" ]]; then
    case "$PEQL_VERBOSITY" in
        minimal|standard|full) ;;
        *) fail "Invalid PEQL_BENCH_VERBOSITY '$PEQL_VERBOSITY'. Must be: minimal, standard, full" ;;
    esac
fi

# ── pgbench initialization ──────────────────────────────────────────────

needs_init() {
    local count
    count=$(psql_cmd -c "SELECT count(*) FROM information_schema.tables WHERE table_name = 'pgbench_accounts';" 2>/dev/null || echo "0")
    [[ "$count" -eq 0 ]]
}

do_init() {
    info "Initializing pgbench data (scale=$SCALE) — this may take a while"
    pgbench_cmd -i -s "$SCALE" --no-vacuum 2>&1
    ok "pgbench data initialized (scale=$SCALE)"
}

if [[ "${1:-}" == "init" ]]; then
    do_init
    exit 0
fi

case "$INIT_MODE" in
    yes)  do_init ;;
    no)   info "Skipping pgbench initialization (PEQL_BENCH_INIT=no)" ;;
    auto)
        if needs_init; then
            do_init
        else
            ok "pgbench tables already exist (use PEQL_BENCH_INIT=yes to reinitialize)"
        fi
        ;;
esac

# ── write-heavy custom script ──────────────────────────────────────────

WRITE_HEAVY_SCRIPT=""

create_write_heavy_script() {
    WRITE_HEAVY_SCRIPT=$(mktemp /tmp/peql_bench_write_heavy.XXXXXX)
    cat > "$WRITE_HEAVY_SCRIPT" <<'PGBENCH_SCRIPT'
\set aid random(1, 100000 * :scale)
\set bid random(1, 1 * :scale)
\set tid random(1, 10 * :scale)
\set delta1 random(-5000, 5000)
\set delta2 random(-5000, 5000)
\set delta3 random(-5000, 5000)
BEGIN;
UPDATE pgbench_accounts SET abalance = abalance + :delta1 WHERE aid = :aid;
UPDATE pgbench_tellers SET tbalance = tbalance + :delta2 WHERE tid = :tid;
UPDATE pgbench_branches SET bbalance = bbalance + :delta3 WHERE bid = :bid;
INSERT INTO pgbench_history (tid, bid, aid, delta, mtime) VALUES (:tid, :bid, :aid, :delta1, CURRENT_TIMESTAMP);
INSERT INTO pgbench_history (tid, bid, aid, delta, mtime) VALUES (:tid, :bid, :aid, :delta2, CURRENT_TIMESTAMP);
INSERT INTO pgbench_history (tid, bid, aid, delta, mtime) VALUES (:tid, :bid, :aid, :delta3, CURRENT_TIMESTAMP);
UPDATE pgbench_accounts SET abalance = abalance + :delta2 WHERE aid = :aid;
SELECT abalance FROM pgbench_accounts WHERE aid = :aid;
END;
PGBENCH_SCRIPT
}

cleanup_write_heavy_script() {
    [[ -n "$WRITE_HEAVY_SCRIPT" && -f "$WRITE_HEAVY_SCRIPT" ]] && rm -f "$WRITE_HEAVY_SCRIPT"
}
trap cleanup_write_heavy_script EXIT

# ── configure PEQL extension ───────────────────────────────────────────

info "Configuring PEQL extension settings"

psql_cmd -c "ALTER SYSTEM SET peql.enabled = '$PEQL_ENABLED';"
if [[ -n "$PEQL_VERBOSITY" ]]; then
    psql_cmd -c "ALTER SYSTEM SET peql.log_verbosity = '$PEQL_VERBOSITY';"
fi
if [[ -n "$PEQL_LOG_MIN_DURATION" ]]; then
    psql_cmd -c "ALTER SYSTEM SET peql.log_min_duration = $PEQL_LOG_MIN_DURATION;"
fi

psql_cmd -c "SELECT pg_reload_conf();" >/dev/null

ACTUAL_ENABLED=$(psql_cmd -c "SHOW peql.enabled;")
ACTUAL_VERBOSITY=$(psql_cmd -c "SHOW peql.log_verbosity;")
ACTUAL_MIN_DURATION=$(psql_cmd -c "SHOW peql.log_min_duration;")

ok "peql.enabled = $ACTUAL_ENABLED"
ok "peql.log_verbosity = $ACTUAL_VERBOSITY"
ok "peql.log_min_duration = $ACTUAL_MIN_DURATION"

# ── reset peql log ─────────────────────────────────────────────────────

info "Resetting PEQL slow log"
psql_cmd -c "SELECT pg_enhanced_query_logging_reset();" >/dev/null 2>&1 || warn "Could not reset log (extension may not be installed in this database)"

# ── build pgbench command ──────────────────────────────────────────────

PGBENCH_ARGS=(
    -c "$CLIENTS"
    -j "$THREADS"
    -T "$DURATION"
    -M "$PROTOCOL"
    --progress=5
)

if [[ "$RATE" -gt 0 ]]; then
    PGBENCH_ARGS+=(-R "$RATE")
fi

if [[ -n "$CUSTOM_SCRIPT" ]]; then
    if [[ ! -f "$CUSTOM_SCRIPT" ]]; then
        fail "Custom script not found: $CUSTOM_SCRIPT"
    fi
    PGBENCH_ARGS+=(-f "$CUSTOM_SCRIPT")
elif [[ "$MODE" == "read-only" ]]; then
    PGBENCH_ARGS+=(-S)
elif [[ "$MODE" == "write-heavy" ]]; then
    create_write_heavy_script
    PGBENCH_ARGS+=(-f "$WRITE_HEAVY_SCRIPT")
fi
# read-write uses pgbench's built-in TPC-B (no extra flag needed)

# ── print configuration ────────────────────────────────────────────────

TIMESTAMP=$(date +%Y%m%d_%H%M%S)
mkdir -p "$RESULTS_DIR"
RESULTS_FILE="$RESULTS_DIR/bench_${MODE}_c${CLIENTS}_t${DURATION}s_${TIMESTAMP}.txt"

print_config() {
    cat <<EOF
════════════════════════════════════════════════════════════════════════
  pgbench benchmark — pg_enhanced_query_logging overhead test
════════════════════════════════════════════════════════════════════════

  PostgreSQL .............. $PG_VERSION ($PG_HOST:$PG_PORT)
  Container .............. ${CONTAINER:-"(direct connection)"}

  Workload mode .......... $MODE
  Clients ................ $CLIENTS
  Threads ................ $THREADS
  Duration ............... ${DURATION}s
  Scale factor ........... $SCALE
  Protocol ............... $PROTOCOL
  Rate limit ............. $([ "$RATE" -gt 0 ] && echo "${RATE} TPS" || echo "unlimited")

  peql.enabled ........... $ACTUAL_ENABLED
  peql.log_verbosity ..... $ACTUAL_VERBOSITY
  peql.log_min_duration .. $ACTUAL_MIN_DURATION

────────────────────────────────────────────────────────────────────────
EOF
}

print_config
print_config > "$RESULTS_FILE"

# ── run pgbench ─────────────────────────────────────────────────────────

info "Running pgbench ($MODE, ${CLIENTS}c × ${THREADS}j × ${DURATION}s, protocol=$PROTOCOL)"

PGBENCH_OUTPUT=$(mktemp /tmp/peql_bench_output.XXXXXX)
trap 'cleanup_write_heavy_script; rm -f "$PGBENCH_OUTPUT"' EXIT

set +e
pgbench_cmd "${PGBENCH_ARGS[@]}" 2>&1 | tee "$PGBENCH_OUTPUT"
PGBENCH_EXIT=$?
set -e

if [[ "$PGBENCH_EXIT" -ne 0 ]]; then
    warn "pgbench exited with code $PGBENCH_EXIT"
fi

echo "" >> "$RESULTS_FILE"
echo "── pgbench output ──────────────────────────────────────────────────" >> "$RESULTS_FILE"
cat "$PGBENCH_OUTPUT" >> "$RESULTS_FILE"

# ── collect PEQL metrics ───────────────────────────────────────────────

echo ""
info "Collecting PEQL metrics"

PEQL_STATS=$(psql_cmd -c "SELECT queries_logged, queries_skipped, bytes_written FROM pg_enhanced_query_logging_stats();" 2>/dev/null || echo "||")
QUERIES_LOGGED=$(echo "$PEQL_STATS" | cut -d'|' -f1)
QUERIES_SKIPPED=$(echo "$PEQL_STATS" | cut -d'|' -f2)
BYTES_WRITTEN=$(echo "$PEQL_STATS" | cut -d'|' -f3)

LOG_SIZE="(unknown)"
LOG_ENTRIES="(unknown)"
if [[ -n "$CONTAINER" ]]; then
    LOG_SIZE=$(docker exec "$CONTAINER" bash -c '
        DATA_DIR=$(psql -U postgres -tAc "SHOW data_directory;")
        LOG_DIR=$(psql -U postgres -tAc "SHOW log_directory;")
        LOG_FILE=$(psql -U postgres -tAc "SHOW peql.log_filename;")
        FILE="${DATA_DIR}/${LOG_DIR}/${LOG_FILE}"
        if [ -f "$FILE" ]; then
            stat -c %s "$FILE" 2>/dev/null || stat -f %z "$FILE" 2>/dev/null || echo 0
        else
            echo 0
        fi
    ' 2>/dev/null || echo "(unknown)")

    LOG_ENTRIES=$(docker exec "$CONTAINER" bash -c '
        DATA_DIR=$(psql -U postgres -tAc "SHOW data_directory;")
        LOG_DIR=$(psql -U postgres -tAc "SHOW log_directory;")
        LOG_FILE=$(psql -U postgres -tAc "SHOW peql.log_filename;")
        FILE="${DATA_DIR}/${LOG_DIR}/${LOG_FILE}"
        if [ -f "$FILE" ]; then
            grep -c "^# Time:" "$FILE" 2>/dev/null || echo 0
        else
            echo 0
        fi
    ' 2>/dev/null || echo "(unknown)")
fi

format_bytes() {
    local bytes=$1
    if [[ "$bytes" == "(unknown)" || -z "$bytes" ]]; then
        echo "(unknown)"
        return
    fi
    if (( bytes >= 1073741824 )); then
        printf "%.2f GB" "$(echo "scale=2; $bytes / 1073741824" | bc)"
    elif (( bytes >= 1048576 )); then
        printf "%.2f MB" "$(echo "scale=2; $bytes / 1048576" | bc)"
    elif (( bytes >= 1024 )); then
        printf "%.2f KB" "$(echo "scale=2; $bytes / 1024" | bc)"
    else
        echo "${bytes} B"
    fi
}

# ── extract TPS from pgbench output ────────────────────────────────────

extract() {
    local pattern="$1" file="$2"
    local val
    val=$(grep "$pattern" "$file" 2>/dev/null | head -1) || true
    echo "$val"
}

TPS_INCL=$(extract "including" "$PGBENCH_OUTPUT" | sed -n 's/.*tps = \([0-9.]*\).*including.*/\1/p')
TPS_EXCL=$(extract "excluding" "$PGBENCH_OUTPUT" | sed -n 's/.*tps = \([0-9.]*\).*excluding.*/\1/p')
AVG_LATENCY=$(extract "latency average" "$PGBENCH_OUTPUT" | sed -n 's/.*latency average = \([0-9.]*\).*/\1/p')
LATENCY_STDDEV=$(extract "latency stddev" "$PGBENCH_OUTPUT" | sed -n 's/.*latency stddev = \([0-9.]*\).*/\1/p')
TXNS_PROCESSED=$(extract "number of transactions actually processed" "$PGBENCH_OUTPUT" | sed -n 's/.*processed: \([0-9]*\).*/\1/p')

: "${TPS_INCL:=n/a}"
: "${TPS_EXCL:=n/a}"
: "${AVG_LATENCY:=n/a}"
: "${LATENCY_STDDEV:=n/a}"
: "${TXNS_PROCESSED:=n/a}"

# ── summary ─────────────────────────────────────────────────────────────

print_summary() {
    cat <<EOF

════════════════════════════════════════════════════════════════════════
  Results
════════════════════════════════════════════════════════════════════════

  TPS (incl. conn) ....... $TPS_INCL
  TPS (excl. conn) ....... $TPS_EXCL
  Avg latency ............ ${AVG_LATENCY} ms
  Latency stddev ......... ${LATENCY_STDDEV} ms
  Transactions ........... $TXNS_PROCESSED

  ── PEQL metrics ──
  Queries logged ......... ${QUERIES_LOGGED:-n/a}
  Queries skipped ........ ${QUERIES_SKIPPED:-n/a}
  Bytes written .......... $(format_bytes "${BYTES_WRITTEN:-0}")
  Log file size .......... $(format_bytes "$LOG_SIZE")
  Log entries ............ $LOG_ENTRIES

  Results saved to: $RESULTS_FILE
════════════════════════════════════════════════════════════════════════
EOF
}

print_summary
print_summary >> "$RESULTS_FILE"

ok "Benchmark complete"
