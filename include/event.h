#ifndef EVENT_H
#define EVENT_H

#include "config.h"

// Initializes the epoll event loop, registers the listening sockets for all routes,
// and asynchronously handles concurrent connection events.
// Returns 0 on clean termination, or -1 on fatal error.
int event_loop_run(const GatewayConfig *config);

#endif // EVENT_H