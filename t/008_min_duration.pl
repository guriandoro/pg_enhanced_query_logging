use strict;
use warnings;
use File::Basename;
use lib dirname(__FILE__);
use PeqlNode;
use Test::More;

my $node = setup_peql_node();

# ── Test 1: log_min_duration = 500 filters fast queries ──
my $content = reset_and_get_log($node, query_sql => q{
SET peql.log_min_duration = 500;
SELECT 'fast_query';
SELECT pg_sleep(0.6);
});

unlike($content, qr/fast_query/,
	"min_duration=500: fast query not logged");
like($content, qr/pg_sleep/,
	"min_duration=500: slow query (pg_sleep 600ms) is logged");

# ── Test 2: log_min_duration = 0 logs everything ──
$content = reset_and_get_log($node, query_sql => q{
SET peql.log_min_duration = 0;
SELECT 'all_logged';
});

like($content, qr/all_logged/,
	"min_duration=0: all queries logged");

# ── Test 3: log_min_duration = -1 disables logging ──
$content = reset_and_get_log($node, query_sql => q{
SET peql.log_min_duration = -1;
SELECT 'none_logged';
});

# The marker query from reset_and_get_log runs before we SET -1,
# so 'none_logged' specifically should not appear.
unlike($content, qr/none_logged/,
	"min_duration=-1: query not logged");

# ── Test 4: threshold boundary -- query just under the limit ──
$content = reset_and_get_log($node, query_sql => q{
SET peql.log_min_duration = 1000;
SELECT pg_sleep(0.1);
});

unlike($content, qr/pg_sleep/,
	"min_duration=1000: 100ms query not logged (below threshold)");

$node->stop;
done_testing();
