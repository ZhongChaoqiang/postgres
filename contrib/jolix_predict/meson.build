# contrib/jolix_predict/meson.build

jolix_predict_sources = files(
  'jolix_predict.c',
)

jolix_predict_deps = []
if libcurl.found()
  jolix_predict_deps += libcurl
endif

jolix_predict = shared_module('jolix_predict',
  jolix_predict_sources,
  dependencies: jolix_predict_deps,
  kwargs: contrib_mod_args,
)

contrib_targets += jolix_predict

install_data(
  'jolix_predict.control',
  'jolix_predict--1.0.sql',
  kwargs: contrib_data_args,
)
