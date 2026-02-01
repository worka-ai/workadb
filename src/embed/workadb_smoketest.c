#include "workadb.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void
ensure_dir(const char *path)
{
	if (!path || path[0] == '\0')
		return;
	mkdir(path, 0700);
}

int
main(int argc, char **argv)
{
	const char *pgdata = "./workadb_data";
	uint8_t response_bytes[16384];
	const char *sql = "SELECT 1;";
	wepg_handle handle = NULL;
	wepg_status status;
	wepg_config config;
	wepg_request request;
	wepg_response response;

	(void) argc;
	(void) argv;

	ensure_dir(pgdata);

	memset(&config, 0, sizeof(config));
	config.pgdata_path = pgdata;
	config.temp_path = pgdata;

	status = wepg_init(&config, &handle);
	if (status != WEPG_OK)
	{
		fprintf(stderr, "init failed: %s\n", wepg_last_error(handle));
		return 1;
	}

	memset(&request, 0, sizeof(request));
	request.mode = WEPG_MODE_SQL;
	request.bytes = (const uint8_t *) sql;
	request.len = strlen(sql);

	memset(&response, 0, sizeof(response));
	response.bytes = response_bytes;
	response.capacity = sizeof(response_bytes);

	status = wepg_step(handle, &request, &response);
	if (status != WEPG_OK)
	{
		fprintf(stderr, "step failed: %s\n", wepg_last_error(handle));
		wepg_shutdown(handle);
		return 1;
	}

	printf("response_len=%zu\n", response.len);

	wepg_shutdown(handle);
	return 0;
}
