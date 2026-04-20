# contrib/pg_embedding_st/Makefile

MODULE_big = pg_embedding_st
OBJS = pg_embedding_st.o

EXTENSION = pg_embedding_st
DATA = pg_embedding_st--1.0.sql
PGFILEDESC = "pg_embedding_st - Sentence Transformers embedding functions"

PYTHON_CONFIG = python3-config
PYTHON_INCLUDES = $(shell $(PYTHON_CONFIG) --includes)
PYTHON_LIBS = $(shell $(PYTHON_CONFIG) --ldflags --embed 2>/dev/null || $(PYTHON_CONFIG) --ldflags)

override CPPFLAGS := $(PYTHON_INCLUDES) -I/usr/local/pgsql/include/server/extension/vector $(CPPFLAGS)

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/pg_embedding_st
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

SHLIB_LINK += $(PYTHON_LIBS)
