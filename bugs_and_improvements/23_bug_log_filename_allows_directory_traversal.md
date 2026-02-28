# Bug: `peql.log_filename` allows directory traversal and arbitrary file writes

## Status: Fixed

## Priority: High

## Effort: Easy

## Description

The `peql.log_filename` and `peql.log_directory` GUC strings are used without
any path sanitization in `peql_resolve_log_path`:

```c
fname = (peql_log_filename && peql_log_filename[0] != '\0')
    ? peql_log_filename : "peql-slow.log";

if (is_absolute_path(dir))
    snprintf(result, resultsize, "%s/%s", dir, fname);
else
    snprintf(result, resultsize, "%s/%s/%s", DataDir, dir, fname);
```

A superuser (PGC_SIGHUP context) could set:

```sql
ALTER SYSTEM SET peql.log_filename = '../../pg_hba.conf';
```

After reload, the extension would append slow query log entries to `pg_hba.conf`,
corrupting the authentication configuration.

Similarly:

```sql
ALTER SYSTEM SET peql.log_directory = '/etc';
ALTER SYSTEM SET peql.log_filename = 'crontab';
```

While these require superuser privileges (PGC_SIGHUP), the principle of defense
in depth means the extension should validate that:

1. `peql.log_filename` contains no path separators (`/`, `\`)
2. `peql.log_filename` does not start with `.`
3. `peql.log_directory` does not contain `..` segments

PostgreSQL's own `log_filename` GUC also lacks strict validation, but it is
handled by the syslogger which runs in a dedicated process with specific
directory expectations. This extension writes from every backend, so the
blast radius is larger.

## Location

`pg_enhanced_query_logging.c`, lines 762-782 (`peql_resolve_log_path`) and
lines 284-302 (`_PG_init`, GUC definitions for log_directory and log_filename)

## Fix

Add a `check_hook` for both string GUCs that rejects values with path separators
or `..` components:

```c
static bool
peql_check_log_filename(char **newval, void **extra, GucSource source)
{
    if (*newval && (strchr(*newval, '/') || strchr(*newval, '\\') ||
                    strstr(*newval, "..")))
    {
        GUC_check_errdetail("peql.log_filename must not contain path separators "
                            "or \"..\" components.");
        return false;
    }
    return true;
}
```
