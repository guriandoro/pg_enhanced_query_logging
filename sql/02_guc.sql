-- 02_guc.sql: GUC variable validation

-- Show defaults for all peql GUCs
SHOW peql.enabled;
SHOW peql.log_min_duration;
SHOW peql.log_directory;
SHOW peql.log_filename;
SHOW peql.log_verbosity;
SHOW peql.log_utility;
SHOW peql.log_nested;
SHOW peql.rate_limit;
SHOW peql.rate_limit_type;
SHOW peql.rate_limit_always_log_duration;
SHOW peql.log_parameter_values;
SHOW peql.log_query_plan;
SHOW peql.log_query_plan_format;
SHOW peql.track_io_timing;
SHOW peql.track_wal;
SHOW peql.track_memory;
SHOW peql.track_planning;

-- Test setting boolean GUCs
SET peql.enabled = off;
SHOW peql.enabled;
SET peql.enabled = on;

-- Test setting integer GUCs
SET peql.log_min_duration = 500;
SHOW peql.log_min_duration;
SET peql.log_min_duration = 0;
SHOW peql.log_min_duration;
SET peql.log_min_duration = -1;
SHOW peql.log_min_duration;

-- Test enum GUCs
SET peql.log_verbosity = 'minimal';
SHOW peql.log_verbosity;
SET peql.log_verbosity = 'standard';
SHOW peql.log_verbosity;
SET peql.log_verbosity = 'full';
SHOW peql.log_verbosity;

-- Invalid enum value should fail
\set ON_ERROR_STOP 0
SET peql.log_verbosity = 'invalid';
\set ON_ERROR_STOP 1

-- Rate limit GUCs
SET peql.rate_limit = 10;
SHOW peql.rate_limit;
SET peql.rate_limit = 1;

SET peql.rate_limit_type = 'session';
SHOW peql.rate_limit_type;
SET peql.rate_limit_type = 'query';
SHOW peql.rate_limit_type;

-- Invalid rate_limit_type should fail
\set ON_ERROR_STOP 0
SET peql.rate_limit_type = 'invalid';
\set ON_ERROR_STOP 1

-- Query plan format GUCs
SET peql.log_query_plan_format = 'json';
SHOW peql.log_query_plan_format;
SET peql.log_query_plan_format = 'text';
SHOW peql.log_query_plan_format;

-- Rate limit minimum is 1 (not 0)
\set ON_ERROR_STOP 0
SET peql.rate_limit = 0;
\set ON_ERROR_STOP 1

-- Disk space protection GUCs
SHOW peql.disk_threshold_pct;
SHOW peql.disk_check_interval_ms;
SHOW peql.disk_auto_purge;

-- Test setting disk_threshold_pct
SET peql.disk_threshold_pct = 10;
SHOW peql.disk_threshold_pct;
SET peql.disk_threshold_pct = 0;
SHOW peql.disk_threshold_pct;

-- Out-of-range values should fail
\set ON_ERROR_STOP 0
SET peql.disk_threshold_pct = -1;
SET peql.disk_threshold_pct = 101;
\set ON_ERROR_STOP 1

-- Test setting disk_check_interval_ms
SET peql.disk_check_interval_ms = 1000;
SHOW peql.disk_check_interval_ms;

-- Below minimum (100ms) should fail
\set ON_ERROR_STOP 0
SET peql.disk_check_interval_ms = 50;
\set ON_ERROR_STOP 1

-- Test setting disk_auto_purge
SET peql.disk_auto_purge = on;
SHOW peql.disk_auto_purge;
SET peql.disk_auto_purge = off;
SHOW peql.disk_auto_purge;

-- Reset all to defaults
RESET peql.enabled;
RESET peql.log_min_duration;
RESET peql.log_verbosity;
RESET peql.rate_limit;
RESET peql.rate_limit_type;
RESET peql.log_query_plan_format;
RESET peql.disk_threshold_pct;
RESET peql.disk_check_interval_ms;
RESET peql.disk_auto_purge;
