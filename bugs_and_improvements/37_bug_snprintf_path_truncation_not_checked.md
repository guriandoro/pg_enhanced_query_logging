# Bug: `snprintf` path truncation not checked in path resolution

## Status: Open

## Priority: Low

## Effort: Easy

## Description

`peql_resolve_log_path` builds the log file path with `snprintf` into a
fixed-size buffer (`MAXPGPATH`, typically 1024 bytes):

```c
if (is_absolute_path(dir))
    snprintf(result, resultsize, "%s/%s", dir, fname);
else
    snprintf(result, resultsize, "%s/%s/%s", DataDir, dir, fname);
```

If the combined path exceeds `MAXPGPATH`, `snprintf` silently truncates.
The truncated path is then passed to `fopen`, which would either:

- Fail to open (safe but confusing -- no error message explains the
  truncation), or
- Open a different, shorter-named file (unlikely but would silently write
  logs to the wrong location).

The same issue exists in `pg_enhanced_query_logging_reset` when constructing
the `.old` rename path:

```c
snprintf(oldpath, sizeof(oldpath), "%s.old", logpath);
```

If `logpath` is close to `MAXPGPATH`, appending `.old` (4 characters) could
truncate.

In practice this is unlikely because real-world paths are well under 1024
bytes, but it is a robustness gap.

## Location

`pg_enhanced_query_logging.c`:
- Lines 865-868 (`peql_resolve_log_path`)
- Line 1659 (`pg_enhanced_query_logging_reset`)

## Fix

Check the `snprintf` return value (which reports the number of characters
that *would* have been written) against the buffer size:

```c
int len = snprintf(result, resultsize, "%s/%s", dir, fname);
if (len >= (int) resultsize)
    ereport(LOG,
            (errmsg("peql: log file path exceeds maximum length (%d bytes)",
                    (int) resultsize)));
```

Alternatively, use PostgreSQL's `strlcpy`-style pattern or `psprintf` (which
pallocs an appropriately sized buffer).
