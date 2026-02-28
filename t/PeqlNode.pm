package PeqlNode;

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Exporter 'import';

our @EXPORT = qw(setup_peql_node reset_and_get_log peql_log_path _ignore_siginfo);

# On macOS, pg_ctl sends SIGINFO (signal 29) to its process group when
# starting/stopping PostgreSQL.  The default Perl action terminates the
# process.  Install the handler and re-arm it after every IPC::Run call
# since subprocesses may reset signal dispositions.
sub _ignore_siginfo {
	$SIG{INFO} = 'IGNORE' if exists $SIG{INFO};
}
_ignore_siginfo();

sub setup_peql_node {
	my (%opts) = @_;
	my $name = $opts{name} // 'main';
	my $extra_conf = $opts{extra_conf} // '';

	my $node = PostgreSQL::Test::Cluster->new($name);
	$node->init;
	$node->append_conf('postgresql.conf', qq{
shared_preload_libraries = 'pg_enhanced_query_logging'
peql.enabled = on
peql.log_min_duration = 0
peql.log_verbosity = 'full'
peql.log_filename = 'peql-slow.log'
logging_collector = on
log_directory = 'log'
$extra_conf});
	$node->start;
	$node->safe_psql('postgres', 'CREATE EXTENSION pg_enhanced_query_logging');
	return $node;
}

sub peql_log_path {
	my ($node) = @_;
	return $node->data_dir . "/log/peql-slow.log";
}

# Reset the log, run queries, then return the log contents.
# No sleep needed: the extension writes directly via open/write/close
# (not through the logging collector), so data is on disk when
# safe_psql returns.
sub reset_and_get_log {
	my ($node, %opts) = @_;
	my $setup_sql = $opts{setup_sql} // '';
	my $query_sql = $opts{query_sql} // "SELECT 'peql_marker'";

	$node->safe_psql('postgres', "SELECT pg_enhanced_query_logging_reset()");
	_ignore_siginfo();

	if ($setup_sql ne '') {
		$node->safe_psql('postgres', $setup_sql);
		_ignore_siginfo();
	}

	$node->safe_psql('postgres', $query_sql);
	_ignore_siginfo();

	my $log_file = peql_log_path($node);
	return slurp_file($log_file);
}

1;
