# contrib/tsvector_funcs/Makefile
MODULE_big = tsvector_funcs
OBJS = tsvector_funcs.o

EXTENSION = tsvector_funcs
DATA = tsvector_funcs--1.0.sql
PGFILEDESC = "tsvector_funcs - timeseries vector helper functions"

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
