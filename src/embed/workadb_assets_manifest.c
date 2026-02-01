#include <stddef.h>
#include <stdint.h>

typedef struct workadb_asset_entry {
  const char *path;
  uint32_t offset;
  uint32_t size;
  uint32_t mode;
} workadb_asset_entry;

extern const unsigned char workadb_assets_blob[];
extern const unsigned char workadb_assets_blob_end[];
extern const workadb_asset_entry workadb_assets_manifest[];
extern const size_t workadb_assets_manifest_len;
extern const char workadb_assets_version[];

const workadb_asset_entry workadb_assets_manifest[] = {
};
const size_t workadb_assets_manifest_len = 0;
const char workadb_assets_version[] = "sha256:none";
