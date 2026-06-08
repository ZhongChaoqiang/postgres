# contrib/jolix_embedding/meson.build

jolix_embedding_sources = files(
  'jolix_embedding.c',
)

# pgvector header - now included in contrib/pgvector
pgvector_inc = include_directories('../pgvector/src')

if python3_dep.found()
  jolix_embedding = shared_module('jolix_embedding',
    jolix_embedding_sources,
    include_directories: [pgvector_inc],
    kwargs: contrib_mod_args + {
      'dependencies': [python3_dep, contrib_mod_args['dependencies']],
    },
  )
  contrib_targets += jolix_embedding
endif

install_data(
  'jolix_embedding.control',
  'jolix_embedding--1.0.sql',
  kwargs: contrib_data_args,
)
