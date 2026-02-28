use strict;
use warnings FATAL => 'all';
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->append_conf('postgresql.conf', qq{
shared_preload_libraries = 'pg_enhanced_query_logging'
peql.enabled = on
peql.log_min_duration = 0
peql.log_verbosity = 'standard'
peql.log_filename = 'peql-slow.log'
logging_collector = on
log_directory = 'log'
});
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION pg_enhanced_query_logging');

my $data_dir = $node->data_dir;
my $log_file = "$data_dir/log/peql-slow.log";

# ── Test 1: log file is created after a query ──
$node->safe_psql('postgres', "SELECT 'hello_peql'");

sleep 1;

ok(-f $log_file, "log file exists after running a query");

# ── Test 2: log entry contains required pt-query-digest fields ──
my $content = slurp_file($log_file);

like($content, qr/^# Time: \d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d+$/m,
	"log entry contains # Time: line in ISO-8601 format");

like($content, qr/^# User@Host: \S+\[\S+\] @ \S* \[\]$/m,
	"log entry contains # User@Host: line");

like($content, qr/^# Query_time: [\d.]+\s+Lock_time: [\d.]+\s+Rows_sent: \d+\s+Rows_examined: \d+$/m,
	"log entry contains # Query_time/Lock_time/Rows_sent/Rows_examined line");

like($content, qr/^SET timestamp=\d+;$/m,
	"log entry contains SET timestamp line");

# ── Test 3: standard verbosity includes Thread_id and Schema ──
like($content, qr/^# Thread_id: \d+\s+Schema: /m,
	"standard verbosity includes Thread_id and Schema");

# ── Test 4: the actual query text appears in the log ──
like($content, qr/SELECT 'hello_peql'/,
	"query text appears in the log");

# ── Test 5: reset function truncates the log ──
$node->safe_psql('postgres', "SELECT pg_enhanced_query_logging_reset()");

# After reset the old file is renamed; issue a query so a new log file
# is created, then read it.
$node->safe_psql('postgres', "SELECT 'after_reset_marker'");
sleep 1;

my $after_reset = slurp_file($log_file);
unlike($after_reset, qr/hello_peql/,
	"log file no longer contains previous query after reset");

# ── Test 6: logging disabled when peql.enabled = off ──
$node->safe_psql('postgres', "SELECT pg_enhanced_query_logging_reset()");
$node->safe_psql('postgres', "SELECT 'post_reset_6'");
sleep 1;
$node->safe_psql('postgres', "SET peql.enabled = off; SELECT 'should_not_appear'");
sleep 1;

my $disabled_content = slurp_file($log_file);
unlike($disabled_content, qr/should_not_appear/,
	"query not logged when peql.enabled = off");

# ── Test 7: logging disabled when log_min_duration = -1 ──
$node->safe_psql('postgres', "SELECT pg_enhanced_query_logging_reset()");
$node->safe_psql('postgres', "SELECT 'post_reset_7'");
sleep 1;
$node->safe_psql('postgres', "SET peql.log_min_duration = '-1'; SELECT 'also_not_logged'");
sleep 1;

my $min_dur_content = slurp_file($log_file);
unlike($min_dur_content, qr/also_not_logged/,
	"query not logged when peql.log_min_duration = -1");

# ── Test 8: multiple entries are separated properly ──
$node->safe_psql('postgres', "SELECT pg_enhanced_query_logging_reset()");
$node->safe_psql('postgres', "SELECT 'entry_one'");
$node->safe_psql('postgres', "SELECT 'entry_two'");

sleep 1;
my $multi = slurp_file($log_file);
my @time_lines = ($multi =~ /^# Time:/mg);
cmp_ok(scalar @time_lines, '>=', 2,
	"multiple log entries each start with # Time:");

$node->stop;
done_testing();
