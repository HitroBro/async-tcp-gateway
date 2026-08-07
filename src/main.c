#include "logger.h"
#include "config.h"
#include "net.h"
#include "event.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define CONFIG_PATH "config/gateway.conf"

int main(void) {
    LOG_INFO("Starting 404_Team_not_Found High-Concurrency Layer 4 Traffic Gateway...");

    // 1. Load routing configuration
    GatewayConfig config;
    if (config_load(CONFIG_PATH, &config) < 0) {
        LOG_ERROR("Fatal: Could not load configuration from %s.", CONFIG_PATH);
        return EXIT_FAILURE;
    }
    config_print(&config);

    // 2. Initialize primary listener on Route #1 (Port 8080)
    Route *primary_route = &config.routes[0];
    int server_fd = net_create_listener(primary_route->frontend_port);
    if (server_fd < 0) {
        LOG_ERROR("Fatal: Failed to start listener on port %d.", primary_route->frontend_port);
        return EXIT_FAILURE;
    }

    LOG_INFO("Gateway listener bound to port %d.", primary_route->frontend_port);
    LOG_INFO("Handing control over to the Asynchronous Epoll Event Loop...");

    // 3. Launch the high-concurrency event engine
    int status = event_loop_run(server_fd, &config);

    // 4. Clean shutdown
    close(server_fd);
    LOG_INFO("Gateway process terminated.");
    return (status == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}