use strict;
use warnings;
use File::Basename;
use lib dirname(__FILE__);
use PeqlNode;
use Test::More;

my $node = setup_peql_node();

$node->safe_psql('postgres', q{
CREATE TABLE planning_test AS SELECT generate_series(1, 1000) AS id;
ANALYZE planning_test;
});

# ── Test 1: track_planning = on with full verbosity ──
my $content = reset_and_get_log($node, query_sql => q{
SET peql.log_verbosity = 'full';
SET peql.track_planning = on;
SELECT count(*) FROM planning_test WHERE id > 500;
});

like($content, qr/Plan_time: [\d.]+/,
	"track_planning=on: Plan_time line present");

# ── Test 2: track_planning = off ──
$content = reset_and_get_log($node, query_sql => q{
SET peql.log_verbosity = 'full';
SET peql.track_planning = off;
SELECT count(*) FROM planning_test WHERE id > 500;
});

unlike($content, qr/Plan_time:/,
	"track_planning=off: no Plan_time line");

# ── Test 3: Plan_time not shown at minimal verbosity even if tracking is on ──
$content = reset_and_get_log($node, query_sql => q{
SET peql.log_verbosity = 'minimal';
SET peql.track_planning = on;
SELECT count(*) FROM planning_test WHERE id > 500;
});

unlike($content, qr/Plan_time:/,
	"minimal verbosity: no Plan_time line even with track_planning=on");

$node->safe_psql('postgres', 'DROP TABLE planning_test');
$node->stop;
done_testing();
