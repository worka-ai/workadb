#ifndef WORKADB_IO_H
#define WORKADB_IO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "libpq/libpq-be.h"

void workadb_io_enable(bool enable);
bool workadb_io_is_enabled(void);

void workadb_io_set_input(const uint8_t *bytes, size_t len);
void workadb_io_set_output(uint8_t *bytes, size_t capacity);
size_t workadb_io_output_len(void);
void workadb_io_reset(void);

ssize_t workadb_io_read(Port *port, void *ptr, size_t len);
ssize_t workadb_io_write(Port *port, const void *ptr, size_t len);

#endif
