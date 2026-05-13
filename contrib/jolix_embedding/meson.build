# contrib/jolix_embedding/meson.build

jolix_embedding_sources = files(
  'jolix_embedding.c',
)

python = import('python').find_installation('python3')

jolix_embedding = shared_module('jolix_embedding',
  jolix_embedding_sources,
  include_directories: [python.include_dirs],
  dependencies: [python.dependency()],
  kwargs: contrib_mod_args,
)

contrib_targets += jolix_embedding

install_data(
  'jolix_embedding.control',
  'jolix_embedding--1.0.sql',
  kwargs: contrib_data_args,
)
