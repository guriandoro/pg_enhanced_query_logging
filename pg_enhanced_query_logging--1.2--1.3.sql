/* pg_enhanced_query_logging--1.2--1.3.sql */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "ALTER EXTENSION pg_enhanced_query_logging UPDATE" to load this script. \quit

DROP FUNCTION IF EXISTS pg_enhanced_query_logging_stats();

CREATE FUNCTION pg_enhanced_query_logging_stats(
    OUT queries_logged  bigint,
    OUT queries_skipped bigint,
    OUT bytes_written   bigint,
    OUT disk_paused     boolean,
    OUT disk_skipped    bigint
)
RETURNS record
AS 'MODULE_PATHNAME', 'pg_enhanced_query_logging_stats'
LANGUAGE C STRICT VOLATILE;

REVOKE ALL ON FUNCTION pg_enhanced_query_logging_stats() FROM PUBLIC;
