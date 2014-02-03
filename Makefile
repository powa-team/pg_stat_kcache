MODULE_big = pg_stat_kcache
OBJS = pg_stat_kcache.o

EXTENSION = pg_stat_kcache
DATA = pg_stat_kcache--1.0.sql

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

