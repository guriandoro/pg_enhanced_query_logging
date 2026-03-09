# Contributing

[Back to README](../README.md)

Contributions are welcome. Here's how to get started:

1. Fork the repository and create a feature branch
2. Make your changes, following the existing code style (PostgreSQL C conventions)
3. Add or update tests as appropriate:
   - SQL regression tests in `sql/` with expected output in `expected/`
   - TAP tests in `t/` for log output verification (use `t/PeqlNode.pm` for common setup)
4. Ensure the extension compiles cleanly: `make USE_PGXS=1`
5. Run the full test suite: `make installcheck USE_PGXS=1 && make prove_installcheck USE_PGXS=1`
6. Submit a pull request -- CI will run both test suites automatically on Ubuntu and macOS

## Code Style

- Follow [PostgreSQL coding conventions](https://www.postgresql.org/docs/current/source-format.html): tabs for indentation, K&R brace style, descriptive variable names
- Use `ereport()` / `elog()` for error reporting
- Use `AllocateFile()` / `FreeFile()` instead of raw `fopen()` / `fclose()`
- Prefix all symbols with `peql_` to avoid namespace collisions
