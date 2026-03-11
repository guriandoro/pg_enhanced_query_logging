# pg_enhanced_query_logging/Makefile
#
# Builds the pg_enhanced_query_logging extension using PGXS.

MODULE_big = pg_enhanced_query_logging
OBJS = \
	$(WIN32RES) \
	pg_enhanced_query_logging.o
PGFILEDESC = "pg_enhanced_query_logging - enhanced query logging for pt-query-digest"

EXTENSION = pg_enhanced_query_logging
DATA = pg_enhanced_query_logging--1.0.sql \
	pg_enhanced_query_logging--1.1.sql \
	pg_enhanced_query_logging--1.2.sql \
	pg_enhanced_query_logging--1.3.sql \
	pg_enhanced_query_logging--1.0--1.1.sql \
	pg_enhanced_query_logging--1.1--1.2.sql \
	pg_enhanced_query_logging--1.2--1.3.sql

REGRESS = 01_basic 02_guc 03_filtering

TAP_TESTS = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/pg_enhanced_query_logging
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
