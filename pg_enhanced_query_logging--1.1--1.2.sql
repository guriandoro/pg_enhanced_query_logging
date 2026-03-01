/* pg_enhanced_query_logging--1.1--1.2.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION pg_enhanced_query_logging UPDATE" to load this script. \quit

CREATE FUNCTION pg_enhanced_query_logging_stats(
    OUT queries_logged  bigint,
    OUT queries_skipped bigint,
    OUT bytes_written   bigint
)
RETURNS record
AS 'MODULE_PATHNAME', 'pg_enhanced_query_logging_stats'
LANGUAGE C STRICT VOLATILE;

REVOKE ALL ON FUNCTION pg_enhanced_query_logging_stats() FROM PUBLIC;
