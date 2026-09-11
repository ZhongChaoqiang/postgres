# contrib/tsvector_funcs/Makefile
MODULE_big = tsvector_funcs
OBJS = tsvector_funcs.o

EXTENSION = tsvector_funcs
DATA = tsvector_funcs--1.0.sql
PGFILEDESC = "tsvector_funcs - timeseries vector helper functions (ts2v_moment, trigger, background worker)"

# tsvector_funcs registers a Background Worker, so it must be loaded via
# shared_preload_libraries.  The BGW auto-registration happens in _PG_init()
# when process_shared_preload_libraries_in_progress is true.

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/tsvector_funcs
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
