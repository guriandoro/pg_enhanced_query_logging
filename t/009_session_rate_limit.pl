use strict;
use warnings;
use File::Basename;
use lib dirname(__FILE__);
use PeqlNode;
use Test::More;

my $node = setup_peql_node(extra_conf => qq{
peql.rate_limit = 2
peql.rate_limit_type = 'session'
});

# Session-mode rate limiting is all-or-nothing: within a single session,
# either every query is logged or none are. With rate_limit=2, roughly
# half the sessions should be sampled.

# Run multiple independent sessions and check for the all-or-nothing
# property within each one.
my $mixed_count = 0;
my $sessions = 4;

for my $i (1 .. $sessions) {
	$node->safe_psql('postgres', "SELECT pg_enhanced_query_logging_reset()");

	$node->safe_psql('postgres', qq{
SELECT 'session_${i}_a';
SELECT 'session_${i}_b';
SELECT 'session_${i}_c';
});

	my $log_file = peql_log_path($node);
	my $content = -f $log_file ? slurp_file($log_file) : '';

	my $has_a = ($content =~ /session_${i}_a/) ? 1 : 0;
	my $has_b = ($content =~ /session_${i}_b/) ? 1 : 0;
	my $has_c = ($content =~ /session_${i}_c/) ? 1 : 0;

	my $sum = $has_a + $has_b + $has_c;
	if ($sum != 0 && $sum != 3) {
		$mixed_count++;
	}
}

is($mixed_count, 0,
	"session rate limiting is all-or-nothing (no mixed sessions in $sessions trials)");

# ── Test 2: rate_limit=1 with session mode logs everything ──
my $content = reset_and_get_log($node, setup_sql => q{
ALTER SYSTEM SET peql.rate_limit = 1;
SELECT pg_reload_conf();
}, query_sql => q{
SELECT 'session_all_a';
SELECT 'session_all_b';
});

like($content, qr/session_all_a/,
	"session rate_limit=1: first query logged");
like($content, qr/session_all_b/,
	"session rate_limit=1: second query logged");

# Restore original setting
$node->safe_psql('postgres', q{
ALTER SYSTEM SET peql.rate_limit = 2;
SELECT pg_reload_conf();
});

$node->stop;
done_testing();
