-- 03_filtering.sql: verbosity levels and filtering switches

-- Test that the extension is active and GUCs are independent
SET peql.enabled = on;
SET peql.log_min_duration = 0;
SET peql.log_verbosity = 'minimal';

-- Verify GUC states after setting
SHOW peql.enabled;
SHOW peql.log_min_duration;
SHOW peql.log_verbosity;

-- Switch to standard verbosity
SET peql.log_verbosity = 'standard';
SHOW peql.log_verbosity;

-- Switch to full verbosity
SET peql.log_verbosity = 'full';
SHOW peql.log_verbosity;

-- Test the utility and nested switches
SET peql.log_utility = on;
SET peql.log_nested = on;
SHOW peql.log_utility;
SHOW peql.log_nested;

-- Test the tracking GUCs
SET peql.track_io_timing = off;
SET peql.track_wal = off;
SET peql.track_memory = on;
SET peql.track_planning = on;
SHOW peql.track_io_timing;
SHOW peql.track_wal;
SHOW peql.track_memory;
SHOW peql.track_planning;

-- Test parameter and plan GUCs
SET peql.log_parameter_values = on;
SET peql.log_query_plan = on;
SHOW peql.log_parameter_values;
SHOW peql.log_query_plan;

-- Disabled state: log_min_duration = -1 disables logging
SET peql.log_min_duration = -1;
SHOW peql.log_min_duration;

-- Master switch off
SET peql.enabled = off;
SHOW peql.enabled;

-- Reset everything
RESET peql.enabled;
RESET peql.log_min_duration;
RESET peql.log_verbosity;
RESET peql.log_utility;
RESET peql.log_nested;
RESET peql.track_io_timing;
RESET peql.track_wal;
RESET peql.track_memory;
RESET peql.track_planning;
RESET peql.log_parameter_values;
RESET peql.log_query_plan;
