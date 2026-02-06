#ifdef __APPLE__
#define WADB_SYM(name) _##name
#define WADB_SECTION .section __TEXT,__const
#else
#define WADB_SYM(name) name
#define WADB_SECTION .section .rodata
#endif

WADB_SECTION
.globl WADB_SYM(workadb_assets_blob)
.globl WADB_SYM(workadb_assets_blob_end)
WADB_SYM(workadb_assets_blob):
.incbin "/Users/zcourts/projects/worka/worka/vendored/workadb/src/embed/workadb_assets.bin"
WADB_SYM(workadb_assets_blob_end):
