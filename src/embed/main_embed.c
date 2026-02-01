#include "postgres.h"

#define main workadb_backend_main
#include "backend/main/main.c"
#undef main
