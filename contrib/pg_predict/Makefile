# contrib/pg_predict/Makefile

MODULE_big = pg_predict
OBJS = pg_predict.o

EXTENSION = pg_predict
DATA = pg_predict--1.0.sql
PGFILEDESC = "pg_predict - LLM inference functions for PostgreSQL"

override CPPFLAGS := $(LIBCURL_CPPFLAGS) $(CPPFLAGS)

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/pg_predict
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

SHLIB_LINK += $(LIBCURL_LDFLAGS) $(LIBCURL_LDLIBS)
