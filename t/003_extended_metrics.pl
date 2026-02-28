use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->append_conf('postgresql.conf', qq{
shared_preload_libraries = 'pg_enhanced_query_logging'
peql.enabled = on
peql.log_min_duration = 0
peql.log_filename = 'peql-slow.log'
logging_collector = on
log_directory = 'log'
});
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION pg_enhanced_query_logging');

my $data_dir = $node->data_dir;
my $log_file = "$data_dir/log/peql-slow.log";

# ── Test 1: minimal verbosity - no Thread_id or buffer metrics ──
# Reset and set verbosity in the same session so the reset entry itself
# is also logged at minimal verbosity (avoids Thread_id from a full entry).
$node->safe_psql('postgres', qq{
SET peql.log_verbosity = 'minimal';
SELECT pg_enhanced_query_logging_reset();
});
$node->safe_psql('postgres', qq{
SET peql.log_verbosity = 'minimal';
SELECT 'minimal_test';
});

my $content = slurp_file($log_file);
like($content, qr/^# Time:/m,
	"minimal: Time line present");
like($content, qr/^# Query_time:/m,
	"minimal: Query_time line present");
unlike($content, qr/^# Thread_id:/m,
	"minimal: no Thread_id line");
unlike($content, qr/^# Shared_blks_hit:/m,
	"minimal: no buffer metrics");

# ── Test 2: standard verbosity - Thread_id and Schema present ──
$node->safe_psql('postgres', qq{
SET peql.log_verbosity = 'standard';
SELECT pg_enhanced_query_logging_reset();
});
$node->safe_psql('postgres', qq{
SET peql.log_verbosity = 'standard';
SELECT 'standard_test';
});

$content = slurp_file($log_file);
like($content, qr/^# Thread_id: \d+\s+Schema: /m,
	"standard: Thread_id and Schema present");
unlike($content, qr/^# Shared_blks_hit:/m,
	"standard: no buffer metrics");

# ── Test 3: full verbosity - buffer and plan quality metrics ──
$node->safe_psql('postgres', "SELECT pg_enhanced_query_logging_reset()");
$node->safe_psql('postgres', qq{
SET peql.log_verbosity = 'full';
CREATE TABLE IF NOT EXISTS test_metrics (id int, val text);
INSERT INTO test_metrics SELECT g, 'row' || g FROM generate_series(1,100) g;
SELECT * FROM test_metrics WHERE id > 50;
});

$content = slurp_file($log_file);
like($content, qr/^# Shared_blks_hit: \d+\s+Shared_blks_read: \d+/m,
	"full: shared buffer metrics present");
like($content, qr/^# Local_blks_hit: \d+\s+Local_blks_read: \d+/m,
	"full: local buffer metrics present");
like($content, qr/^# Temp_blks_read: \d+\s+Temp_blks_written: \d+/m,
	"full: temp buffer metrics present");
like($content, qr/^# Full_scan: (?:Yes|No)\s+Temp_table: (?:Yes|No)/m,
	"full: plan quality booleans present");

# ── Test 4: WAL metrics appear when track_wal is on ──
like($content, qr/^# WAL_records: \d+\s+WAL_bytes: \d+\s+WAL_fpi: \d+/m,
	"full: WAL metrics present with track_wal=on");

# ── Test 5: WAL metrics disappear when track_wal is off ──
$node->safe_psql('postgres', qq{
SET peql.log_verbosity = 'full';
SET peql.track_wal = off;
SELECT pg_enhanced_query_logging_reset();
});
$node->safe_psql('postgres', qq{
SET peql.log_verbosity = 'full';
SET peql.track_wal = off;
SELECT 'no_wal_test';
});

$content = slurp_file($log_file);
unlike($content, qr/^# WAL_records:/m,
	"full: WAL metrics absent when track_wal=off");

# ── Test 6: utility logging when peql.log_utility is on ──
$node->safe_psql('postgres', "SELECT pg_enhanced_query_logging_reset()");
$node->safe_psql('postgres', qq{
SET peql.log_verbosity = 'standard';
SET peql.log_utility = on;
CREATE TABLE IF NOT EXISTS utility_test (id int);
});

$content = slurp_file($log_file);
like($content, qr/CREATE TABLE/,
	"utility: DDL statement appears when log_utility=on");

# ── Test 7: utility not logged when peql.log_utility is off ──
$node->safe_psql('postgres', qq{
SET peql.log_utility = off;
SELECT pg_enhanced_query_logging_reset();
});
$node->safe_psql('postgres', qq{
SET peql.log_utility = off;
CREATE TABLE IF NOT EXISTS utility_test2 (id int);
});

$content = slurp_file($log_file);
unlike($content, qr/utility_test2/,
	"utility: DDL statement absent when log_utility=off");

# ── Test 8: Rows_sent is populated for SELECT ──
$node->safe_psql('postgres', "SELECT pg_enhanced_query_logging_reset()");
$node->safe_psql('postgres', qq{
SET peql.log_verbosity = 'minimal';
SELECT * FROM test_metrics;
});

$content = slurp_file($log_file);
like($content, qr/Rows_sent: (?!0\b)\d+/,
	"Rows_sent is non-zero for SELECT returning rows");

# ── Test 9: Rows_affected appears for DML at standard verbosity ──
$node->safe_psql('postgres', "SELECT pg_enhanced_query_logging_reset()");
$node->safe_psql('postgres', qq{
SET peql.log_verbosity = 'standard';
DELETE FROM test_metrics WHERE id <= 10;
});

$content = slurp_file($log_file);
like($content, qr/^# Rows_affected: (?!0\b)\d+/m,
	"Rows_affected is non-zero for DELETE");

# Cleanup
$node->safe_psql('postgres', "DROP TABLE IF EXISTS test_metrics, utility_test, utility_test2");

$node->stop;
done_testing();
