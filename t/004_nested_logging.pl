use strict;
use warnings;
use File::Basename;
use lib dirname(__FILE__);
use PeqlNode;
use Test::More;

my $node = setup_peql_node(extra_conf => qq{
peql.log_utility = on
});

$node->safe_psql('postgres', q{
CREATE OR REPLACE FUNCTION nested_test() RETURNS void AS $$
BEGIN
  PERFORM 'inner_one';
  PERFORM 'inner_two';
END;
$$ LANGUAGE plpgsql;
});

# ── Test 1: log_nested = off -- only outer call is logged ──
my $content = reset_and_get_log($node,
	query_sql => q{SET peql.log_nested = off; SELECT nested_test()});

like($content, qr/SELECT nested_test\(\)/,
	"log_nested=off: outer call is logged");
unlike($content, qr/inner_one/,
	"log_nested=off: inner statement 'inner_one' is NOT logged");
unlike($content, qr/inner_two/,
	"log_nested=off: inner statement 'inner_two' is NOT logged");

# ── Test 2: log_nested = on -- inner statements also appear ──
$content = reset_and_get_log($node,
	query_sql => q{SET peql.log_nested = on; SELECT nested_test()});

like($content, qr/SELECT nested_test\(\)/,
	"log_nested=on: outer call is logged");
like($content, qr/inner_one/,
	"log_nested=on: inner statement 'inner_one' is logged");
like($content, qr/inner_two/,
	"log_nested=on: inner statement 'inner_two' is logged");

# ── Test 3: switching back to off hides inner statements again ──
$content = reset_and_get_log($node,
	query_sql => q{SET peql.log_nested = off; SELECT nested_test()});

unlike($content, qr/inner_one/,
	"log_nested toggled back to off: inner statements hidden");

$node->safe_psql('postgres', 'DROP FUNCTION nested_test()');
$node->stop;
done_testing();
