# Copyright (c) 2025-2026, pg_enhanced_query_logging contributors

pg_enhanced_query_logging_sources = files(
  'pg_enhanced_query_logging.c',
)

if host_system == 'windows'
  pg_enhanced_query_logging_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'pg_enhanced_query_logging',
    '--FILEDESC', 'pg_enhanced_query_logging - enhanced query logging for pt-query-digest',])
endif

pg_enhanced_query_logging = shared_module('pg_enhanced_query_logging',
  pg_enhanced_query_logging_sources,
  kwargs: contrib_mod_args + {
    'dependencies': contrib_mod_args['dependencies'],
  },
)
contrib_targets += pg_enhanced_query_logging

install_data(
  'pg_enhanced_query_logging.control',
  'pg_enhanced_query_logging--1.0.sql',
  'pg_enhanced_query_logging--1.1.sql',
  'pg_enhanced_query_logging--1.2.sql',
  'pg_enhanced_query_logging--1.0--1.1.sql',
  'pg_enhanced_query_logging--1.1--1.2.sql',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'pg_enhanced_query_logging',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      '01_basic',
      '02_guc',
      '03_filtering',
    ],
    'regress_args': ['--temp-config', files('pg_enhanced_query_logging.conf')],
    'runningcheck': false,
  },
  'tap': {
    'tests': [
      't/001_basic_logging.pl',
      't/002_rate_limiting.pl',
      't/003_extended_metrics.pl',
      't/004_nested_logging.pl',
      't/005_parameter_values.pl',
      't/006_query_plan.pl',
      't/007_planning_time.pl',
      't/008_min_duration.pl',
      't/009_session_rate_limit.pl',
      't/010_edge_cases.pl',
    ],
  },
}
