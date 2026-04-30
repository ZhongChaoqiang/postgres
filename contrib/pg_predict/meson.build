# contrib/pg_predict/meson.build

pg_predict_sources = files(
  'pg_predict.c',
)

pg_predict_deps = []
if libcurl.found()
  pg_predict_deps += libcurl
endif

pg_predict = shared_module('pg_predict',
  pg_predict_sources,
  dependencies: pg_predict_deps,
  kwargs: contrib_mod_args,
)

contrib_targets += pg_predict

install_data(
  'pg_predict.control',
  'pg_predict--1.0.sql',
  kwargs: contrib_data_args,
)
