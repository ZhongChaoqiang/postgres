# contrib/pg_embedding_st/meson.build

pg_embedding_st_sources = files(
  'pg_embedding_st.c',
)

python = import('python').find_installation('python3')

pg_embedding_st = shared_module('pg_embedding_st',
  pg_embedding_st_sources,
  include_directories: [python.include_dirs],
  dependencies: [python.dependency()],
  kwargs: contrib_mod_args,
)

contrib_targets += pg_embedding_st

install_data(
  'pg_embedding_st.control',
  'pg_embedding_st--1.0.sql',
  kwargs: contrib_data_args,
)
