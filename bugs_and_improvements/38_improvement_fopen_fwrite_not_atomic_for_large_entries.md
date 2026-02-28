# Improvement: `fopen("a")` + `fwrite` is not atomic for large log entries

## Status: Fixed

## Priority: Low

## Effort: Medium

## Description

The log writer opens the file with `fopen(logpath, "a")` and writes via
`fwrite`:

```c
fp = fopen(logpath, "a");
...
fwrite(data, 1, len, fp);
...
fclose(fp);
```

The code comment (lines 1365-1368) notes that `O_APPEND` ensures atomic
writes on POSIX. However, POSIX atomicity guarantees apply to the `write(2)`
syscall, not to `fwrite(3)`. The `fwrite` function uses stdio buffering and
may split a single logical write into multiple `write()` syscalls if the
data exceeds the stdio buffer size (typically 4-8 KB).

When multiple backends write concurrently and a log entry is large (e.g.,
when `peql.log_query_plan = on` generates a multi-kilobyte EXPLAIN plan),
portions of different backends' entries could interleave in the file. This
would produce malformed log entries that pt-query-digest cannot parse.

For typical short entries (a few hundred bytes), this is not a problem
because the data fits in a single `write()` call.

## Location

`pg_enhanced_query_logging.c`, lines 1380-1417 (`peql_flush_to_file`)

## Fix

Replace stdio `fopen`/`fwrite`/`fclose` with POSIX `open`/`write`/`close`
to guarantee a single atomic `write()` call per entry:

```c
int fd = open(logpath, O_WRONLY | O_APPEND | O_CREAT, pg_file_create_mode);
if (fd < 0)
{
    ereport(LOG, (errcode_for_file_access(),
                  errmsg("peql: could not open \"%s\": %m", logpath)));
    return;
}
{
    ssize_t written = write(fd, data, len);
    if (written != (ssize_t) len)
        ereport(LOG, (errcode_for_file_access(),
                      errmsg("peql: short write to \"%s\"", logpath)));
}
close(fd);
```

Note: POSIX only guarantees atomicity for writes up to `PIPE_BUF` (512
bytes minimum, typically 4096). For entries larger than `PIPE_BUF`,
interleaving is possible even with raw `write()`, but in practice most OSes
handle `O_APPEND` writes atomically for sizes well beyond `PIPE_BUF` on
local filesystems.
