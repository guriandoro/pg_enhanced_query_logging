-- 01_basic.sql: extension load/unload and reset function

-- Extension can be created
CREATE EXTENSION pg_enhanced_query_logging;

-- Extension shows up in pg_extension
SELECT extname, extversion FROM pg_extension
 WHERE extname = 'pg_enhanced_query_logging';

-- The reset function exists and is callable by superuser.
-- The NOTICE contains an absolute path that varies per system, so suppress it.
SET client_min_messages = warning;
SELECT pg_enhanced_query_logging_reset();
RESET client_min_messages;

-- Extension can be dropped cleanly
DROP EXTENSION pg_enhanced_query_logging;

-- Verify it's gone
SELECT count(*) FROM pg_extension
 WHERE extname = 'pg_enhanced_query_logging';
