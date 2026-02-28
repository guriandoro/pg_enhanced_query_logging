# Improvement: Detect and warn on fwrite short writes

## Status: Not fixed

## Priority: Low

## Effort: Trivial

## Description

The file writer discards the `fwrite` return value:

```c
(void) fwrite(data, 1, len, fp);
```

If the disk is full or another I/O error occurs, the write silently fails. The
operator has no indication that log entries are being lost, which defeats the
purpose of a logging extension.

## Location

`pg_enhanced_query_logging.c`, line 1230 (`peql_flush_to_file`)

## Fix

Check the return value of `fwrite` and log a warning on short writes:

```c
size_t written = fwrite(data, 1, len, fp);
if (written != (size_t) len)
{
    ereport(LOG,
            (errcode_for_file_access(),
             errmsg("peql: short write to log file \"%s\": wrote %zu of %d bytes",
                    logpath, written, len)));
}
```

Use a static throttle or rate limit on the warning to avoid flooding the PG log
if the disk stays full.
