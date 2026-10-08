# contrib/tsvector_funcs/meson.build
# meson build for tsvector_funcs (timeseries vector helper functions + BGW)

tsvector_funcs_sources = files(
  'tsvector_funcs.c',
)

if host_system == 'windows'
  tsvector_funcs_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'tsvector_funcs',
    '--FILEDESC', 'tsvector_funcs - timeseries vector helper functions (ts2v_moment, trigger, background worker)',])
endif

tsvector_funcs = shared_module('tsvector_funcs',
  tsvector_funcs_sources,
  kwargs: contrib_mod_args,
)
contrib_targets += tsvector_funcs

install_data(
  'tsvector_funcs.control',
  'tsvector_funcs--1.0.sql',
  kwargs: contrib_data_args,
)
