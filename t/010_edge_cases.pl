use strict;
use warnings FATAL => 'all';
use File::Basename;
use lib dirname(__FILE__);
use PeqlNode;
use Test::More;

my $node = setup_peql_node(extra_conf => qq{
peql.track_memory = on
});

# ── Test 1: Filesort_on_disk with low work_mem ──
my $content = reset_and_get_log($node, query_sql => q{
SET peql.log_verbosity = 'full';
SET work_mem = '64kB';
SELECT * FROM generate_series(1, 10000) AS n ORDER BY n DESC;
});

like($content, qr/Filesort: Yes/,
	"large ORDER BY triggers Filesort: Yes");

# ── Test 2: Temp_table with CTE ──
$content = reset_and_get_log($node, query_sql => q{
SET peql.log_verbosity = 'full';
SET work_mem = '64kB';
WITH big AS MATERIALIZED (SELECT generate_series(1, 10000) AS n) SELECT count(*) FROM big;
});

like($content, qr/Temp_table: Yes/,
	"materialized CTE triggers Temp_table: Yes");

# ── Test 3: Mem_allocated present when track_memory = on ──
$content = reset_and_get_log($node, query_sql => q{
SET peql.log_verbosity = 'full';
SELECT generate_series(1, 100);
});

like($content, qr/Mem_allocated: \d+/,
	"track_memory=on: Mem_allocated field present");

# ── Test 4: Mem_allocated absent when track_memory = off ──
# Reset with track_memory=off in the same session so the reset entry
# itself also omits Mem_allocated (avoids polluting the unlike check).
$node->safe_psql('postgres', q{
SET peql.track_memory = off;
SELECT pg_enhanced_query_logging_reset();
});
$node->safe_psql('postgres', q{
SET peql.log_verbosity = 'full';
SET peql.track_memory = off;
SELECT generate_series(1, 100);
});
$content = slurp_file(peql_log_path($node));

unlike($content, qr/Mem_allocated:/,
	"track_memory=off: Mem_allocated field absent");

# ── Test 5: Schema field changes with search_path ──
$content = reset_and_get_log($node, query_sql => q{
SET peql.log_verbosity = 'standard';
SET search_path = pg_catalog;
SELECT 'schema_test_catalog';
});

like($content, qr/Schema: postgres\.pg_catalog/,
	"Schema reflects pg_catalog search_path");

$content = reset_and_get_log($node, query_sql => q{
SET peql.log_verbosity = 'standard';
SET search_path = public;
SELECT 'schema_test_public';
});

like($content, qr/Schema: postgres\.public/,
	"Schema reflects public search_path");

# ── Test 6: Full_scan on sequential scan ──
$node->safe_psql('postgres', q{
CREATE TABLE fullscan_test AS SELECT generate_series(1, 1000) AS id;
ANALYZE fullscan_test;
});

$content = reset_and_get_log($node, query_sql => q{
SET peql.log_verbosity = 'full';
SELECT * FROM fullscan_test;
});

like($content, qr/Full_scan: Yes/,
	"sequential scan triggers Full_scan: Yes");

$node->safe_psql('postgres', 'DROP TABLE fullscan_test');
$node->stop;
done_testing();
