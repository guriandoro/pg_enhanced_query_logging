/* pg_enhanced_query_logging--1.1.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_enhanced_query_logging" to load this extension. \quit

/*
 * pg_enhanced_query_logging_reset()
 *
 * Truncates the current slow query log file. Returns true on success.
 * Requires superuser privileges.
 */
CREATE FUNCTION pg_enhanced_query_logging_reset()
RETURNS bool
AS 'MODULE_PATHNAME', 'pg_enhanced_query_logging_reset'
LANGUAGE C STRICT VOLATILE;

REVOKE ALL ON FUNCTION pg_enhanced_query_logging_reset() FROM PUBLIC;
