#!/usr/bin/env bash
#
# Run pgbench benchmarks against PostgreSQL containers deployed by the
# deploy_docker_pg18*.sh scripts.  Designed to measure the overhead of
# pg_enhanced_query_logging under configurable concurrency and workload
# scenarios.
#
# The script performs a five-phase comparison:
#   Phase 1 -- PEQL ON:       peql.enabled = on  (measures extension overhead)
#   Phase 2 -- PEQL OFF:      peql.enabled = off (extension loaded but disabled)
#   Phase 3 -- PEQL ON (1%):  peql.enabled = on, peql.rate_limit = 100
#                             (measures overhead with 1% query sampling)
#   Phase 4 -- PG logging:    PEQL removed from shared_preload_libraries,
#                             native log_min_duration_statement = 0
#                             (measures native PostgreSQL logging overhead)
#   Phase 5 -- No logging:    PEQL removed, no query logging at all
#                             (true baseline with zero logging overhead)
# A comparison summary with deltas is printed at the end.
#
# Usage:
#   ./test/run_pgbench.sh              # run with defaults
#   ./test/run_pgbench.sh init         # (re-)initialize pgbench data only
#   ./test/run_pgbench.sh --help       # show help
#
# All settings are controlled via environment variables (see below).
#
# -- Core pgbench settings ----------------------------------------------
#   PEQL_BENCH_CLIENTS      Number of concurrent connections      (default: 10)
#   PEQL_BENCH_DURATION     Runtime in seconds                    (default: 600)
#   PEQL_BENCH_MODE         Workload type:
#                             read-only   -- SELECT-only (pgbench -S)
#                             read-write  -- default TPC-B mix
#                             write-heavy -- custom INSERT/UPDATE-heavy script
#                           (default: read-write)
#
# -- Additional pgbench settings ----------------------------------------
#   PEQL_BENCH_THREADS      pgbench threads (default: min(clients, nproc))
#   PEQL_BENCH_SCALE        Scale factor for pgbench -i           (default: 100)
#   PEQL_BENCH_RATE          Target TPS rate limit; 0 = unlimited (default: 0)
#   PEQL_BENCH_PROTOCOL     Query protocol: simple | extended | prepared
#                           (default: prepared)
#   PEQL_BENCH_CUSTOM_SCRIPT  Path to a custom pgbench script     (optional)
#   PEQL_BENCH_INIT         Force (re-)initialization of pgbench
#                           tables: yes | no | auto
#                           (default: auto -- init if tables are missing)
#
# -- PEQL extension settings (applied during the PEQL ON phase) --------
#   PEQL_BENCH_VERBOSITY         minimal | standard | full
#                                (default: current server setting)
#   PEQL_BENCH_LOG_MIN_DURATION  peql.log_min_duration value
#                                (default: current server setting)
#
# -- Connection settings ------------------------------------------------
#   PEQL_PG_HOST            PostgreSQL host     (default: localhost)
#   PEQL_PG_PORT            PostgreSQL port     (default: auto-detected
#                           from running container, or 15432)
#   PEQL_PG_PASSWORD        Password            (default: peqltest)
#   PEQL_PG_USER            User                (default: postgres)
#   PEQL_PG_DATABASE        Database            (default: postgres)
#
set -euo pipefail

# -- help ----------------------------------------------------------------

if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
    sed -n '2,/^[^#]/{ /^#/s/^# \?//p }' "$0"
    exit 0
fi

# -- helpers -------------------------------------------------------------

info()  { printf '\033[1;34m==> %s\033[0m\n' "$*"; }
ok()    { printf '\033[1;32m  + %s\033[0m\n' "$*"; }
warn()  { printf '\033[1;33m  ! %s\033[0m\n' "$*"; }
fail()  { printf '\033[1;31m  x %s\033[0m\n' "$*"; exit 1; }

# -- PMM annotation helper ------------------------------------------------

detect_pmm_client() {
    for name in peql-pmm-client peql-pmm-client-rhel; do
        if docker ps --format '{{.Names}}' 2>/dev/null | grep -qx "$name"; then
            echo "$name"
            return
        fi
    done
    echo ""
}

PMM_CLIENT_CONTAINER=$(detect_pmm_client)

pmm_annotate() {
    local text="$1"
    if [[ -n "$PMM_CLIENT_CONTAINER" ]]; then
        info "PMM annotation: $text"
        docker exec "$PMM_CLIENT_CONTAINER" pmm-admin annotate "$text" 2>/dev/null \
            && ok "Annotation created" \
            || warn "Failed to create PMM annotation (non-fatal)"
    else
        warn "No PMM client container running -- skipping annotation"
    fi
}

# -- detect container and auto-resolve port ------------------------------

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

# -- configuration -------------------------------------------------------

PG_HOST="${PEQL_PG_HOST:-localhost}"
PG_PORT="${PEQL_PG_PORT:-${AUTO_PORT:-15432}}"
PG_PASSWORD="${PEQL_PG_PASSWORD:-peqltest}"
PG_USER="${PEQL_PG_USER:-postgres}"
PG_DATABASE="${PEQL_PG_DATABASE:-postgres}"

CLIENTS="${PEQL_BENCH_CLIENTS:-10}"
DURATION="${PEQL_BENCH_DURATION:-600}"
MODE="${PEQL_BENCH_MODE:-read-write}"
SCALE="${PEQL_BENCH_SCALE:-100}"
RATE="${PEQL_BENCH_RATE:-0}"
PROTOCOL="${PEQL_BENCH_PROTOCOL:-prepared}"
CUSTOM_SCRIPT="${PEQL_BENCH_CUSTOM_SCRIPT:-}"
INIT_MODE="${PEQL_BENCH_INIT:-auto}"

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

# -- validate ------------------------------------------------------------

info "Validating connection to PostgreSQL at $PG_HOST:$PG_PORT"
if ! psql_cmd -c "SELECT 1;" >/dev/null 2>&1; then
    fail "Cannot connect to PostgreSQL at $PG_HOST:$PG_PORT (user=$PG_USER, db=$PG_DATABASE)"
fi
ok "PostgreSQL is reachable"

PG_VERSION=$(psql_cmd -c "SHOW server_version;")
info "Server version: $PG_VERSION"

# -- validate settings ---------------------------------------------------

case "$MODE" in
    read-only|read-write|write-heavy) ;;
    *) fail "Invalid PEQL_BENCH_MODE '$MODE'. Must be: read-only, read-write, write-heavy" ;;
esac

case "$PROTOCOL" in
    simple|extended|prepared) ;;
    *) fail "Invalid PEQL_BENCH_PROTOCOL '$PROTOCOL'. Must be: simple, extended, prepared" ;;
esac

if [[ -n "$PEQL_VERBOSITY" ]]; then
    case "$PEQL_VERBOSITY" in
        minimal|standard|full) ;;
        *) fail "Invalid PEQL_BENCH_VERBOSITY '$PEQL_VERBOSITY'. Must be: minimal, standard, full" ;;
    esac
fi

# -- pgbench initialization ----------------------------------------------

needs_init() {
    local count
    count=$(psql_cmd -c "SELECT count(*) FROM information_schema.tables WHERE table_name = 'pgbench_accounts';" 2>/dev/null || echo "0")
    [[ "$count" -eq 0 ]]
}

do_init() {
    info "Initializing pgbench data (scale=$SCALE) -- this may take a while"
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

# -- write-heavy custom script ------------------------------------------

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

# -- build pgbench args -------------------------------------------------

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

# -- results file -------------------------------------------------------

TIMESTAMP=$(date +%Y%m%d_%H%M%S)
mkdir -p "$RESULTS_DIR"
RESULTS_FILE="$RESULTS_DIR/bench_${MODE}_c${CLIENTS}_t${DURATION}s_${TIMESTAMP}.txt"

# -- helper: format bytes ----------------------------------------------

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

# -- helper: extract value from pgbench output -------------------------

extract() {
    local pattern="$1" file="$2"
    local val
    val=$(grep "$pattern" "$file" 2>/dev/null | head -1) || true
    echo "$val"
}

# -- run_benchmark: configure, run pgbench, collect metrics ------------
#
# Arguments:
#   $1 -- run label (e.g. "PEQL ON", "PEQL OFF", "PEQL ON (1%)")
#   $2 -- peql.enabled value ("on" or "off")
#   $3 -- variable prefix for storing results (e.g. "ON", "OFF", "RATE")
#   $4 -- (optional) extra SQL to run after enabling PEQL (e.g. rate limit GUCs)
#
# Sets result variables with the given prefix for later comparison.

run_benchmark() {
    local label="$1" peql_setting="$2" prefix="$3"
    local extra_sql="${4:-}"

    echo ""
    info "================================================================"
    info "  Phase: $label (peql.enabled = $peql_setting)"
    info "================================================================"

    # -- configure PEQL
    info "Configuring PEQL extension (peql.enabled = $peql_setting)"
    psql_cmd -c "ALTER SYSTEM SET peql.enabled = '$peql_setting';"
    psql_cmd -c "ALTER SYSTEM SET peql.rate_limit = 1;"
    psql_cmd -c "ALTER SYSTEM SET peql.rate_limit_type = 'query';"
    if [[ -n "$PEQL_VERBOSITY" ]]; then
        psql_cmd -c "ALTER SYSTEM SET peql.log_verbosity = '$PEQL_VERBOSITY';"
    fi
    if [[ -n "$PEQL_LOG_MIN_DURATION" ]]; then
        psql_cmd -c "ALTER SYSTEM SET peql.log_min_duration = $PEQL_LOG_MIN_DURATION;"
    fi
    if [[ -n "$extra_sql" ]]; then
        local -a psql_args=()
        local stmt
        while IFS= read -r stmt; do
            stmt="${stmt#"${stmt%%[![:space:]]*}"}"
            stmt="${stmt%"${stmt##*[![:space:]]}"}"
            [[ -n "$stmt" ]] && psql_args+=(-c "$stmt;")
        done < <(tr ';' '\n' <<< "$extra_sql")
        [[ ${#psql_args[@]} -gt 0 ]] && psql_cmd "${psql_args[@]}"
    fi
    psql_cmd -c "SELECT pg_reload_conf();" >/dev/null

    local actual_enabled actual_verbosity actual_min_duration actual_rate_limit actual_rate_limit_type
    actual_enabled=$(psql_cmd -c "SHOW peql.enabled;")
    actual_verbosity=$(psql_cmd -c "SHOW peql.log_verbosity;")
    actual_min_duration=$(psql_cmd -c "SHOW peql.log_min_duration;")
    actual_rate_limit=$(psql_cmd -c "SHOW peql.rate_limit;")
    actual_rate_limit_type=$(psql_cmd -c "SHOW peql.rate_limit_type;")

    ok "peql.enabled = $actual_enabled"
    ok "peql.log_verbosity = $actual_verbosity"
    ok "peql.log_min_duration = $actual_min_duration"
    ok "peql.rate_limit = $actual_rate_limit"
    ok "peql.rate_limit_type = $actual_rate_limit_type"

    # -- reset PEQL log
    info "Resetting PEQL slow log"
    psql_cmd -c "SELECT pg_enhanced_query_logging_reset();" >/dev/null 2>&1 \
        || warn "Could not reset log (extension may not be installed in this database)"

    # -- print configuration for this phase
    {
        cat <<EOF

========================================================================
  pgbench benchmark -- $label
========================================================================

  PostgreSQL .............. $PG_VERSION ($PG_HOST:$PG_PORT)
  Container .............. ${CONTAINER:-"(direct connection)"}

  Workload mode .......... $MODE
  Clients ................ $CLIENTS
  Threads ................ $THREADS
  Duration ............... ${DURATION}s
  Scale factor ........... $SCALE
  Protocol ............... $PROTOCOL
  Rate limit ............. $([ "$RATE" -gt 0 ] && echo "${RATE} TPS" || echo "unlimited")

  peql.enabled ........... $actual_enabled
  peql.log_verbosity ..... $actual_verbosity
  peql.log_min_duration .. $actual_min_duration
  peql.rate_limit ........ $actual_rate_limit
  peql.rate_limit_type ... $actual_rate_limit_type

------------------------------------------------------------------------
EOF
    } | tee -a "$RESULTS_FILE"

    # -- force checkpoint and drop OS page cache to avoid background I/O
    info "Running CHECKPOINT"
    psql_cmd -c "CHECKPOINT;" >/dev/null
    if [[ -n "$CONTAINER" ]]; then
        info "Syncing and dropping OS page cache inside container"
        docker exec "$CONTAINER" bash -c 'sync && echo 3 > /proc/sys/vm/drop_caches' 2>/dev/null \
            && ok "Page cache cleared" \
            || warn "Could not drop page cache (non-fatal, may lack privileges)"
    fi

    # -- PMM annotation
    pmm_annotate "pgbench START -- $label | ${MODE} ${CLIENTS}c ${DURATION}s"

    # -- run pgbench
    info "Running pgbench ($MODE, ${CLIENTS}c x ${THREADS}j x ${DURATION}s, protocol=$PROTOCOL)"

    local output_file
    output_file=$(mktemp /tmp/peql_bench_output.XXXXXX)
    TMPFILES+=("$output_file")

    set +e
    pgbench_cmd "${PGBENCH_ARGS[@]}" 2>&1 | tee "$output_file"
    local pgbench_exit=$?
    set -e

    if [[ "$pgbench_exit" -ne 0 ]]; then
        warn "pgbench exited with code $pgbench_exit"
    fi

    pmm_annotate "pgbench END -- $label | exit=$pgbench_exit"

    echo "" >> "$RESULTS_FILE"
    echo "-- pgbench output ($label) --------------------------------------" >> "$RESULTS_FILE"
    cat "$output_file" >> "$RESULTS_FILE"

    # -- collect PEQL metrics
    echo ""
    info "Collecting PEQL metrics"

    local peql_stats queries_logged queries_skipped
    peql_stats=$(psql_cmd -c "SELECT queries_logged, queries_skipped FROM pg_enhanced_query_logging_stats();" 2>/dev/null || echo "|")
    queries_logged=$(echo "$peql_stats" | cut -d'|' -f1)
    queries_skipped=$(echo "$peql_stats" | cut -d'|' -f2)

    local log_size="(unknown)" log_entries="(unknown)"
    if [[ -n "$CONTAINER" ]]; then
        log_size=$(docker exec "$CONTAINER" bash -c '
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

        log_entries=$(docker exec "$CONTAINER" bash -c '
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

    # -- extract TPS / latency from pgbench output
    local tps_incl tps_excl avg_latency latency_stddev txns_processed
    tps_incl=$(sed -n 's/.*tps = \([0-9.]*\).*including.*/\1/p' "$output_file" | head -1)
    tps_excl=$(sed -n 's/.*tps = \([0-9.]*\).*excluding.*/\1/p' "$output_file" | head -1)
    # PG 17+ only reports one TPS line: "tps = ... (without initial connection time)"
    if [[ -z "$tps_excl" ]]; then
        tps_excl=$(sed -n 's/.*tps = \([0-9.]*\).*(without initial connection time).*/\1/p' "$output_file" | head -1)
        [[ -z "$tps_incl" && -n "$tps_excl" ]] && tps_incl="$tps_excl"
    fi
    avg_latency=$(extract "latency average" "$output_file" | sed -n 's/.*latency average = \([0-9.]*\).*/\1/p')
    latency_stddev=$(extract "latency stddev" "$output_file" | sed -n 's/.*latency stddev = \([0-9.]*\).*/\1/p')
    txns_processed=$(extract "number of transactions actually processed" "$output_file" | sed -n 's/.*processed: \([0-9]*\).*/\1/p')

    : "${tps_incl:=n/a}"
    : "${tps_excl:=n/a}"
    : "${avg_latency:=n/a}"
    : "${latency_stddev:=n/a}"
    : "${txns_processed:=n/a}"

    # -- print phase summary
    {
        cat <<EOF

========================================================================
  Results -- $label
========================================================================

  TPS (incl. conn) ....... $tps_incl
  TPS (excl. conn) ....... $tps_excl
  Avg latency ............ ${avg_latency} ms
  Latency stddev ......... ${latency_stddev} ms
  Transactions ........... $txns_processed

  -- PEQL metrics --
  Queries logged ......... ${queries_logged:-n/a}
  Queries skipped ........ ${queries_skipped:-n/a}
  Log file size .......... $(format_bytes "$log_size")
  Log entries ............ $log_entries

========================================================================
EOF
    } | tee -a "$RESULTS_FILE"

    # -- store results in global variables for comparison
    eval "${prefix}_TPS_INCL='$tps_incl'"
    eval "${prefix}_TPS_EXCL='$tps_excl'"
    eval "${prefix}_AVG_LATENCY='$avg_latency'"
    eval "${prefix}_LATENCY_STDDEV='$latency_stddev'"
    eval "${prefix}_TXNS='$txns_processed'"
    eval "${prefix}_QUERIES_LOGGED='${queries_logged:-n/a}'"
    eval "${prefix}_QUERIES_SKIPPED='${queries_skipped:-n/a}'"
    eval "${prefix}_LOG_SIZE='$log_size'"
    eval "${prefix}_LOG_ENTRIES='$log_entries'"
}

# -- temp file cleanup -------------------------------------------------

TMPFILES=()
cleanup_all() {
    cleanup_write_heavy_script
    for f in "${TMPFILES[@]}"; do
        [[ -f "$f" ]] && rm -f "$f"
    done
}
trap cleanup_all EXIT

# -- Phase 1: PEQL ON --------------------------------------------------

run_benchmark "PEQL ON" "on" "ON"

# -- Phase 2: PEQL OFF -------------------------------------------------

run_benchmark "PEQL OFF" "off" "OFF"

# -- Phase 3: PEQL ON with 1% rate limit ------------------------------

run_benchmark "PEQL ON (1% rate limit)" "on" "RATE" \
    "ALTER SYSTEM SET peql.rate_limit = 100; ALTER SYSTEM SET peql.rate_limit_type = 'query'; ALTER SYSTEM SET peql.log_min_duration = 0;"

# -- comparison summary ------------------------------------------------

pct_diff() {
    local on_val="$1" off_val="$2"
    if [[ "$on_val" == "n/a" || "$off_val" == "n/a" ]]; then
        echo "n/a"
        return
    fi
    if [[ $(echo "$off_val == 0" | bc -l) -eq 1 ]]; then
        echo "n/a"
        return
    fi
    printf "%+.2f%%" "$(echo "scale=4; ($on_val - $off_val) / $off_val * 100" | bc -l)"
}

print_comparison() {
    local on_off_tps_diff on_off_lat_diff rate_off_tps_diff rate_off_lat_diff
    on_off_tps_diff=$(pct_diff "$ON_TPS_EXCL" "$OFF_TPS_EXCL")
    on_off_lat_diff=$(pct_diff "$ON_AVG_LATENCY" "$OFF_AVG_LATENCY")
    rate_off_tps_diff=$(pct_diff "$RATE_TPS_EXCL" "$OFF_TPS_EXCL")
    rate_off_lat_diff=$(pct_diff "$RATE_AVG_LATENCY" "$OFF_AVG_LATENCY")

    local C=15  # column width for data values
    local L=24  # label column width

    local hdr_fmt="  %-${L}s %${C}s %${C}s %${C}s %${C}s %${C}s\n"
    local row_fmt="  %-${L}s %${C}s %${C}s %${C}s %${C}s %${C}s\n"
    local row3_fmt="  %-${L}s %${C}s %${C}s %${C}s\n"

    local total_w=$(( L + 2 + (C + 1) * 5 ))
    local sep
    sep="  $(printf '%*s' "$total_w" '' | tr ' ' '-')"

    cat <<EOF

============================================================================================
  A/B/C Comparison -- PEQL ON vs PEQL OFF vs PEQL ON (1% rate limit)
============================================================================================

EOF
    printf "$hdr_fmt" "" "PEQL ON" "PEQL OFF" "PEQL ON 1%" "ON vs OFF" "1% vs OFF"
    echo "$sep"
    printf "$row_fmt" "TPS (excl. conn)" "$ON_TPS_EXCL" "$OFF_TPS_EXCL" "$RATE_TPS_EXCL" "$on_off_tps_diff" "$rate_off_tps_diff"
    printf "$row_fmt" "Avg latency (ms)" "$ON_AVG_LATENCY" "$OFF_AVG_LATENCY" "$RATE_AVG_LATENCY" "$on_off_lat_diff" "$rate_off_lat_diff"
    printf "$row3_fmt" "Latency stddev (ms)" "$ON_LATENCY_STDDEV" "$OFF_LATENCY_STDDEV" "$RATE_LATENCY_STDDEV"
    printf "$row3_fmt" "Transactions" "$ON_TXNS" "$OFF_TXNS" "$RATE_TXNS"

    cat <<EOF

  -- PEQL ON metrics --
  Queries logged ......... ${ON_QUERIES_LOGGED}
  Queries skipped ........ ${ON_QUERIES_SKIPPED}
  Log file size .......... $(format_bytes "$ON_LOG_SIZE")
  Log entries ............ $ON_LOG_ENTRIES

  -- PEQL ON (1% rate limit) metrics --
  Queries logged ......... ${RATE_QUERIES_LOGGED}
  Queries skipped ........ ${RATE_QUERIES_SKIPPED}
  Log file size .......... $(format_bytes "$RATE_LOG_SIZE")
  Log entries ............ $RATE_LOG_ENTRIES

  Results saved to: $RESULTS_FILE
============================================================================================
EOF
}

print_comparison | tee -a "$RESULTS_FILE"

ok "Benchmark complete (results saved to $RESULTS_FILE)"
