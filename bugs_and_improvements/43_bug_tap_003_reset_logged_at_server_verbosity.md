# Bug: TAP test 003 fails because reset() is logged at server-level verbosity

## Status: Fixed

## Priority: Low

## Effort: Easy

## Description

TAP test `t/003_extended_metrics.pl` test 3 ("minimal: no Thread_id line")
fails because the log file still contains a `# Thread_id:` line even though
the test sets `peql.log_verbosity = 'minimal'`.

The root cause is that `reset_and_get_log()` (and the test's own
`safe_psql("SELECT pg_enhanced_query_logging_reset()")`) runs the reset
call in a **separate psql session** whose verbosity is whatever the
server-level default is (`full`, as configured in `PeqlNode.pm`). That
reset call itself gets logged with `Thread_id` present, polluting the log
content that the test then checks.

### Test output

```
not ok 3 - minimal: no Thread_id line
# '# Time: ...
# # User@Host: agustin[agustin] @ [local] []
# # Thread_id: 62383  Schema: postgres.public  Last_errno: 0  Killed: 0
# # Query_time: 0.000040  ...
# SET timestamp=1772314695;
# SELECT pg_enhanced_query_logging_reset();
# # Time: ...
# # User@Host: agustin[agustin] @ [local] []
# # Query_time: 0.000004  ...
# SET timestamp=1772314695;
# SELECT 'minimal_test';
# '
#           matches '(?^m:^# Thread_id:)'
```

The first entry is the reset call logged at `full` verbosity (showing
`Thread_id`). The second entry is the actual `SELECT 'minimal_test'` logged
at `minimal` verbosity (no `Thread_id`). The `unlike` assertion sees the
first entry and fails.

## Location

`t/003_extended_metrics.pl` line 36

## Fix

The test should issue the verbosity change and the reset in the **same psql
session**, so the reset is also logged at `minimal` verbosity. Alternatively,
the test can grep only for entries that do NOT contain `pg_enhanced_query_logging_reset`,
or the reset function should be called first and then the verbosity set in a
separate session before running the actual test query:

```perl
$node->safe_psql('postgres', "SELECT pg_enhanced_query_logging_reset()");
sleep 1;
$node->safe_psql('postgres', qq{
SET peql.log_verbosity = 'minimal';
SELECT 'minimal_test';
});
sleep 1;

my $content = slurp_file($log_file);
# Filter out the reset entry which is logged at server-default verbosity
my @minimal_entries = grep { /minimal_test/ } split(/(?=^# Time:)/m, $content);
unlike(join('', @minimal_entries), qr/^# Thread_id:/m,
    "minimal: no Thread_id line");
```

Or more simply, move the verbosity SET into the same session as the reset:

```perl
$node->safe_psql('postgres', qq{
SET peql.log_verbosity = 'minimal';
SELECT pg_enhanced_query_logging_reset();
});
$node->safe_psql('postgres', qq{
SET peql.log_verbosity = 'minimal';
SELECT 'minimal_test';
});
```
