#pragma once

#include <zephyr/kernel.h>

/* Starts the HTTP server thread. Returns 0 on success. */
int web_server_start(void);
