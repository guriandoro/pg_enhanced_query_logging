use strict;
use warnings FATAL => 'all';
use File::Basename;
use lib dirname(__FILE__);
use PeqlNode;
use Test::More;

my $node = setup_peql_node();

# ── Test 1: parameter values logged with log_parameter_values = on ──
my $content = reset_and_get_log($node, query_sql => q{
SET peql.log_parameter_values = on;
PREPARE param_test (int, text) AS SELECT $1, $2;
EXECUTE param_test(42, 'hello world');
DEALLOCATE param_test;
});

like($content, qr/Parameters:/,
	"log_parameter_values=on: Parameters line present");
like($content, qr/42/,
	"log_parameter_values=on: integer parameter value appears");
like($content, qr/hello world/,
	"log_parameter_values=on: text parameter value appears");

# ── Test 2: NULL parameter values shown as NULL ──
$content = reset_and_get_log($node, query_sql => q{
SET peql.log_parameter_values = on;
PREPARE null_test (int, text) AS SELECT $1, $2;
EXECUTE null_test(NULL, NULL);
DEALLOCATE null_test;
});

like($content, qr/Parameters:.*NULL/,
	"NULL parameter values shown as NULL");

# ── Test 3: parameters NOT logged when log_parameter_values = off ──
$content = reset_and_get_log($node, query_sql => q{
SET peql.log_parameter_values = off;
PREPARE noparams (int) AS SELECT $1;
EXECUTE noparams(99);
DEALLOCATE noparams;
});

unlike($content, qr/Parameters:/,
	"log_parameter_values=off: no Parameters line");

$node->stop;
done_testing();
