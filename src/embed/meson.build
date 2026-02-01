# Embedded Postgres library (workadb)

embed_sources = files(
  'workadb.c',
  'initdb_embed.c',
)

embed_inc = include_directories('..', '../include', '../common')

libworkadb = static_library(
  'workadb',
  embed_sources,
  include_directories: embed_inc,
  install: true,
  install_dir: dir_lib,
)
