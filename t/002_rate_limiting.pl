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
peql.log_verbosity = 'standard'
peql.log_filename = 'peql-slow.log'
logging_collector = on
log_directory = 'log'
});
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION pg_enhanced_query_logging');

my $data_dir = $node->data_dir;
my $log_file = "$data_dir/log/peql-slow.log";

# ── Test 1: rate_limit = 1 logs everything ──
$node->safe_psql('postgres', "SELECT pg_enhanced_query_logging_reset()");
$node->safe_psql('postgres', qq{
SET peql.rate_limit = 1;
SELECT 'rate_one_a';
SELECT 'rate_one_b';
SELECT 'rate_one_c';
});

my $content = slurp_file($log_file);
my @entries = ($content =~ /^# Time:/mg);
cmp_ok(scalar @entries, '>=', 3,
	"rate_limit=1 logs all queries");

# ── Test 2: rate_limit > 1 produces fewer log entries (query mode) ──
# With rate_limit=1000, we expect roughly 1 in 1000 queries to be logged.
# Run 200 queries -- statistically almost none should be logged.
$node->safe_psql('postgres', "SELECT pg_enhanced_query_logging_reset()");

my $batch = "SET peql.rate_limit = 1000; SET peql.rate_limit_type = 'query';\n";
for my $i (1..200) {
	$batch .= "SELECT $i;\n";
}
$node->safe_psql('postgres', $batch);

$content = slurp_file($log_file);
@entries = ($content =~ /^# Time:/mg);
cmp_ok(scalar @entries, '<', 10,
	"rate_limit=1000 logs very few out of 200 queries");

# ── Test 3: Log_slow_rate_type and Log_slow_rate_limit appear in output ──
$node->safe_psql('postgres', "SELECT pg_enhanced_query_logging_reset()");

# Set rate_limit=2 and run enough queries that at least one gets logged
$batch = "SET peql.rate_limit = 2; SET peql.rate_limit_type = 'query';\n";
for my $i (1..50) {
	$batch .= "SELECT $i;\n";
}
$node->safe_psql('postgres', $batch);

$content = slurp_file($log_file);

SKIP: {
	skip "no entries logged (unlikely but possible with sampling)", 1
		unless $content =~ /^# Time:/m;

	like($content, qr/^# Log_slow_rate_type: query\s+Log_slow_rate_limit: 2\s+Log_slow_rate_limit_always_log_duration: \d+$/m,
		"rate limit metadata appears in log output");
}

# ── Test 4: rate_limit metadata not present when rate_limit=1 ──
$node->safe_psql('postgres', "SELECT pg_enhanced_query_logging_reset()");
$node->safe_psql('postgres', qq{
SET peql.rate_limit = 1;
SELECT 'no_rate_meta';
});

$content = slurp_file($log_file);
unlike($content, qr/Log_slow_rate_type/,
	"no rate limit metadata when rate_limit=1");

# ── Test 5: always-log-duration bypasses rate limiter ──
# Set a very high rate_limit so normal queries are almost never logged,
# but set always_log_duration to 0 so everything bypasses the limiter.
$node->safe_psql('postgres', "SELECT pg_enhanced_query_logging_reset()");
$node->safe_psql('postgres', qq{
SET peql.rate_limit = 1000000;
SET peql.rate_limit_always_log_duration = 0;
SELECT 'always_logged';
});

$content = slurp_file($log_file);
like($content, qr/always_logged/,
	"always_log_duration=0 bypasses rate limiter");

$node->stop;
done_testing();
