use strict;
use warnings FATAL => 'all';
use File::Basename;
use lib dirname(__FILE__);
use PeqlNode;
use Test::More;

my $node = setup_peql_node();

$node->safe_psql('postgres', q{
CREATE TABLE plan_test AS SELECT generate_series(1, 1000) AS id;
ANALYZE plan_test;
});

# ── Test 1: text-format plan with log_query_plan = on ──
my $content = reset_and_get_log($node, query_sql => q{
SET peql.log_query_plan = on;
SET peql.log_query_plan_format = 'text';
SELECT count(*) FROM plan_test WHERE id > 500;
});

like($content, qr/Plan:/,
	"log_query_plan=on (text): Plan line present");
like($content, qr/Seq Scan|Index Scan|Bitmap/i,
	"log_query_plan=on (text): EXPLAIN node type present");

# ── Test 2: JSON-format plan ──
$content = reset_and_get_log($node, query_sql => q{
SET peql.log_query_plan = on;
SET peql.log_query_plan_format = 'json';
SELECT count(*) FROM plan_test WHERE id > 500;
});

like($content, qr/Plan:/,
	"log_query_plan=on (json): Plan line present");
like($content, qr/"Node Type"/,
	"log_query_plan=on (json): JSON plan contains Node Type key");

# ── Test 3: plan NOT logged when log_query_plan = off ──
$content = reset_and_get_log($node, query_sql => q{
SET peql.log_query_plan = off;
SELECT count(*) FROM plan_test WHERE id > 500;
});

unlike($content, qr/Plan:/,
	"log_query_plan=off: no Plan line");

$node->safe_psql('postgres', 'DROP TABLE plan_test');
$node->stop;
done_testing();
