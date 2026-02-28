# Improvement: TAP test rate-limit assertion is too weak to catch regressions

## Status: Fixed

## Priority: Low

## Effort: Trivial

## Description

In `t/002_rate_limiting.pl`, Test 2 validates that `rate_limit=1000` with
200 queries produces fewer log entries:

```perl
my $batch = "SET peql.rate_limit = 1000; SET peql.rate_limit_type = 'query';\n";
for my $i (1..200) {
    $batch .= "SELECT $i;\n";
}
$node->safe_psql('postgres', $batch);

$content = slurp_file($log_file);
@entries = ($content =~ /^# Time:/mg);
cmp_ok(scalar @entries, '<', 200,
    "rate_limit=1000 logs fewer than 200 out of 200 queries");
```

With `rate_limit=1000`, the expected number of logged queries out of 200 is
`200/1000 = 0.2` -- statistically almost none will be logged. The assertion
`< 200` would only fail if **every single query** were logged, which is
nearly impossible even with `rate_limit=2`.

This assertion is so weak it cannot detect most regressions. For example, if
rate limiting were completely broken and logged every other query (100 out of
200), the test would still pass.

## Location

`t/002_rate_limiting.pl`, line 52

## Fix

Use a tighter bound that still accounts for sampling variance:

```perl
cmp_ok(scalar @entries, '<', 20,
    "rate_limit=1000 logs far fewer than 200 out of 200 queries");
```

Or increase the query count and use a proportional bound:

```perl
for my $i (1..1000) {
    $batch .= "SELECT $i;\n";
}
# With rate_limit=1000, expect ~1 log entry out of 1000 queries.
# Allow generous headroom but still meaningful.
cmp_ok(scalar @entries, '<', 50,
    "rate_limit=1000 produces very few log entries");
```
