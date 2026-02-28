use strict;
use warnings;
use File::Basename;
use lib dirname(__FILE__);
use PeqlNode;
use Test::More;

my $node = setup_peql_node();

# Use psql's \bind + \g to send queries through the extended query protocol
# so that queryDesc->params is populated and the extension can log them.
# Plain EXECUTE inlines parameter values via the simple protocol.

# ── Test 1: parameter values logged with log_parameter_values = on ──
my $content = reset_and_get_log($node, query_sql => q{
SET peql.log_parameter_values = on;
SELECT $1::int, $2::text \bind 42 'hello world' \g
});

like($content, qr/Parameters:/,
	"log_parameter_values=on: Parameters line present");
like($content, qr/42/,
	"log_parameter_values=on: integer parameter value appears");
like($content, qr/hello world/,
	"log_parameter_values=on: text parameter value appears");

# ── Test 2: multiple parameter types ──
$content = reset_and_get_log($node, query_sql => q{
SET peql.log_parameter_values = on;
SELECT $1::int, $2::text, $3::bool \bind 7 test true \g
});

like($content, qr/Parameters:.*7/,
	"multiple params: integer value appears");
like($content, qr/Parameters:.*test/,
	"multiple params: text value appears");

# ── Test 3: parameters NOT logged when log_parameter_values = off ──
$content = reset_and_get_log($node, query_sql => q{
SET peql.log_parameter_values = off;
SELECT $1::int \bind 99 \g
});

unlike($content, qr/Parameters:/,
	"log_parameter_values=off: no Parameters line");

$node->stop;
done_testing();
