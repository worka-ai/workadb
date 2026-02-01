#ifndef WORKADB_H
#define WORKADB_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WEPG_ABI_MAJOR 1
#define WEPG_ABI_MINOR 2
#define WORKA_ABI_MAJOR WEPG_ABI_MAJOR
#define WORKA_ABI_MINOR WEPG_ABI_MINOR

typedef struct wepg_engine wepg_engine;
typedef wepg_engine *wepg_handle;

typedef enum {
    WEPG_OK = 0,
    WEPG_ERR_INVALID_CONFIG,
    WEPG_ERR_INITDB_FAILED,
    WEPG_ERR_EXEC_FAILED,
    WEPG_ERR_BUFFER_TOO_SMALL,
    WEPG_ERR_INCOMPATIBLE_PGDATA,
    WEPG_ERR_UPGRADE_REQUIRED,
    WEPG_ERR_UNSUPPORTED,
    WEPG_ERR_INTERNAL
} wepg_status;

typedef enum {
    WEPG_MODE_SQL = 1,
    WEPG_MODE_WIRE = 2
} wepg_mode;

enum {
    WEPG_FLAG_ENABLE_WIRE = 1 << 0,
    WEPG_FLAG_READONLY = 1 << 1,
    WEPG_FLAG_DISABLE_DYNAMIC_EXT = 1 << 2,
    WEPG_FLAG_NO_AUTO_INITDB = 1 << 3
};

typedef struct {
    const char *pgdata_path;
    const char *temp_path;
    uint32_t flags;
    uint32_t max_response_bytes;
} wepg_config;

typedef struct {
    wepg_mode mode;
    const uint8_t *bytes;
    size_t len;
} wepg_request;

typedef struct {
    uint32_t type_oid;
    const uint8_t *value;
    size_t len;
    uint8_t is_null;
} wepg_param;

typedef struct {
    wepg_mode mode;
    const uint8_t *bytes;
    size_t len;
    const wepg_param *params;
    size_t param_count;
} wepg_request_params;

typedef struct {
    uint8_t *bytes;
    size_t len;
    size_t capacity;
    int64_t rows;
} wepg_response;

typedef void (*wepg_log_fn)(int level, const char *msg, void *ctx);

uint32_t wepg_abi_version(void);
const char *wepg_version_string(void);
void wepg_set_logger(wepg_log_fn fn, void *ctx);

wepg_status wepg_init(const wepg_config *config, wepg_handle *out);
wepg_status wepg_initdb(wepg_handle handle);
wepg_status wepg_step(wepg_handle handle, const wepg_request *request, wepg_response *response);
wepg_status wepg_step_params(wepg_handle handle, const wepg_request_params *request, wepg_response *response);
wepg_status wepg_reset(wepg_handle handle);
wepg_status wepg_shutdown(wepg_handle handle);
const char *wepg_last_error(wepg_handle handle);
wepg_status workadb_assets_install(const char *dir_path);
const char *workadb_assets_last_error(void);
const char *workadb_assets_version_string(void);

/* Worka-prefixed aliases */
typedef wepg_engine worka_engine;
typedef wepg_handle worka_handle;
typedef wepg_status worka_status;
typedef wepg_mode worka_mode;
typedef wepg_config worka_config;
typedef wepg_request worka_request;
typedef wepg_param worka_param;
typedef wepg_request_params worka_request_params;
typedef wepg_response worka_response;
typedef wepg_log_fn worka_log_fn;

#define WORKA_FLAG_FAIL_IF_MISSING WEPG_FLAG_NO_AUTO_INITDB

uint32_t worka_abi_version(void);
const char *worka_version_string(void);
void worka_set_logger(worka_log_fn fn, void *ctx);

worka_status worka_open(const worka_config *config, worka_handle *out);
worka_status worka_step(worka_handle handle, const worka_request *request, worka_response *response);
worka_status worka_step_params(worka_handle handle, const worka_request_params *request, worka_response *response);
worka_status worka_reset(worka_handle handle);
worka_status worka_shutdown(worka_handle handle);
const char *worka_last_error(worka_handle handle);

#ifdef __cplusplus
}
#endif

#endif
