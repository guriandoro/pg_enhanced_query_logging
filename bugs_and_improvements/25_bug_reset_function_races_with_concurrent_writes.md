# Bug: `pg_enhanced_query_logging_reset()` races with concurrent log writers

## Status: Fixed

## Priority: Medium

## Effort: Medium

## Description

The reset function truncates the log file by opening it with mode `"w"`:

```c
fp = AllocateFile(logpath, "w");
...
FreeFile(fp);
```

Meanwhile, other backends may be in the middle of `peql_flush_to_file`, which
opens the file with `"a"` (append). The following race exists:

1. Backend A opens the file with `"a"` (gets fd with O_APPEND).
2. Reset function opens the file with `"w"`, truncating it to zero bytes.
3. Reset function closes the file.
4. Backend A calls `fwrite()` on its still-open fd. Because the fd has
   O_APPEND, the kernel seeks to the current end of file (byte 0, since it was
   truncated) and writes. This is correct.
5. Backend B opens the file with `"a"` and writes. This is also correct.

In the happy path, truncation works correctly because O_APPEND handles the
interleaving. **However**, there is a window where:

1. Backend A opens with `"a"`, gets a file offset at position N.
2. Reset truncates the file to 0 bytes.
3. Backend A's buffered `fwrite` flushes. With stdio buffering, the file
   position in the `FILE*` struct may be stale. On some systems (notably
   macOS/BSD), `fwrite` to a `FILE*` opened with `"a"` does NOT use
   `O_APPEND` semantics -- the `"a"` mode only affects the *initial* position,
   and subsequent writes use the stdio buffer's tracked position.

This means on macOS, Backend A's write after truncation could land at byte
position N (its old position), creating a file with N bytes of null padding
followed by the log entry. The null bytes corrupt the log file.

## Location

`pg_enhanced_query_logging.c`, lines 1421-1454 (`pg_enhanced_query_logging_reset`)
and lines 1193-1232 (`peql_flush_to_file`)

## Fix

**Option A** (Simple): Since `AllocateFile` uses `fopen` (stdio), and each
backend's `peql_flush_to_file` opens, writes, and closes atomically within a
single function, the race window is actually very small (only between
`AllocateFile("a")` and `FreeFile`). Documenting that reset should be called
during a quiet period is the simplest approach.

**Option B** (Robust): Instead of truncating with `"w"`, rename the old file
and create a new one:

```c
char newpath[MAXPGPATH];
snprintf(newpath, sizeof(newpath), "%s.old", logpath);
rename(logpath, newpath);
/* New writes will create a fresh file. */
```

This is how PostgreSQL's own log rotation works.

**Option C**: Use an advisory lock or a flag file to signal backends to re-open
the log file, similar to `pg_rotate_logfile()`.
