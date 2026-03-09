# Using with pt-query-digest

[Back to README](../README.md)

## Basic Report

```bash
pt-query-digest --type slowlog /path/to/peql-slow.log
```

## Filter by Query Time

```bash
# Only show queries slower than 1 second
pt-query-digest --type slowlog --filter '$event->{Query_time} > 1' /path/to/peql-slow.log
```

## Filter by User

```bash
pt-query-digest --type slowlog --filter '$event->{user} eq "myapp"' /path/to/peql-slow.log
```

## Review Specific Time Window

```bash
pt-query-digest --type slowlog --since '2026-02-27 14:00:00' --until '2026-02-27 15:00:00' /path/to/peql-slow.log
```

## Show Sequential Scans Only

The extended `# Key: Value` attributes are available in the event hash:

```bash
pt-query-digest --type slowlog --filter '$event->{Full_scan} eq "Yes"' /path/to/peql-slow.log
```

## Output to File

```bash
pt-query-digest --type slowlog /path/to/peql-slow.log > report.txt
```

## Comparing Two Time Periods

```bash
pt-query-digest --type slowlog /path/to/before.log > before.txt
pt-query-digest --type slowlog /path/to/after.log  > after.txt
diff before.txt after.txt
```

## Sampled Data

When `peql.rate_limit > 1`, each log entry includes `Log_slow_rate_type`, `Log_slow_rate_limit`, and `Log_slow_rate_limit_always_log_duration` metadata. pt-query-digest can use this to extrapolate totals from sampled data.
