/* pg_enhanced_query_logging--1.2.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_enhanced_query_logging" to load this extension. \quit

/*
 * pg_enhanced_query_logging_reset()
 *
 * Rotates the slow query log file and resets per-backend counters.
 * Returns true on success.  Requires superuser privileges.
 */
CREATE FUNCTION pg_enhanced_query_logging_reset()
RETURNS bool
AS 'MODULE_PATHNAME', 'pg_enhanced_query_logging_reset'
LANGUAGE C STRICT VOLATILE;

REVOKE ALL ON FUNCTION pg_enhanced_query_logging_reset() FROM PUBLIC;

/*
 * pg_enhanced_query_logging_stats()
 *
 * Returns per-backend logging counters.  The counters are local to the
 * calling backend and reset on log rotation or server restart.
 */
CREATE FUNCTION pg_enhanced_query_logging_stats(
    OUT queries_logged  bigint,
    OUT queries_skipped bigint,
    OUT bytes_written   bigint
)
RETURNS record
AS 'MODULE_PATHNAME', 'pg_enhanced_query_logging_stats'
LANGUAGE C STRICT VOLATILE;

REVOKE ALL ON FUNCTION pg_enhanced_query_logging_stats() FROM PUBLIC;
