#pragma once

#include "command.h"
#include "config.h"

#include <stdbool.h>

/* The control terminal (CRX, after the control receiver): a Unix socket where an
 * operator logged into the host enters DTMF keys, as from a receiver, and follows the
 * log. Each connection is a receiver of its own, with its own command session.
 *
 * One line each way. Client to daemon: the keys of one entry, and the newline is the
 * unkey. Daemon to client: log lines, "HH:MM:SS text", and lines that start with '@':
 *
 *   @live            the recent lines are done; what follows is new
 *   @status k=v ...  the repeater state, sent when it changes
 *   @done ok|fail    after each entry: a command ran, or none did
 *   @bye reason      the daemon closes the connection
 *
 * Nothing here may stall the audio loop: a client that can't take a line at once is
 * dropped, and it reconnects. */

typedef struct crx crx_t;

/* Runs a command entered at the terminal. False if it was dropped. */
typedef bool (*crx_run_fn)(void *arg, const command_t *c, const char *who);

/* *out is NULL when crx.socket is empty, or the socket can't be made: the repeater
 * runs on without a terminal. Only an out-of-memory fails. */
int  crx_create(crx_t **out, const config_t *cfg, int epfd, crx_run_fn run, void *arg);
void crx_destroy(crx_t *x);

/* An epoll event: false if `fd` isn't the terminal's. */
bool crx_handle(crx_t *x, int fd);

/* Anyone connected? Only then is a status line worth building. */
bool crx_connected(const crx_t *x);

/* The repeater state, "k=v ..."; sent only if it differs from the last. */
void crx_status(crx_t *x, const char *status);
