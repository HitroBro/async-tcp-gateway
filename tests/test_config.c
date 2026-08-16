#define _POSIX_C_SOURCE 200809L
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

int main(void) {
    printf("[*] Running test_config: verifying config_load and dynamic options...\n");
    char tmppath[] = "/tmp/test_gateway_XXXXXX";
    int fd = mkstemp(tmppath);
    assert(fd >= 0);

    const char *cfg_data =
        "[global]\n"
        "max_routes = 5\n"
        "max_backends = 4\n"
        "max_active_connections = 512\n"
        "io_buffer_size = 16384\n"
        "max_consecutive_failures = 2\n"
        "connection_idle_timeout_secs = 45\n"
        "max_connections_per_sec = 500\n"
        "max_connections_per_ip_per_sec = 25\n"
        "\n"
        "[route]\n"
        "frontend_port = 8080\n"
        "backend = 127.0.0.1:9001\n"
        "backend = 127.0.0.1:9002\n";

    ssize_t written = write(fd, cfg_data, strlen(cfg_data));
    assert(written == (ssize_t)strlen(cfg_data));
    close(fd);

    GatewayConfig config;
    int ret = config_load(tmppath, &config);
    unlink(tmppath);

    assert(ret == 0);
    assert(config.route_count == 1);
    assert(config.io_buffer_size == 16384);
    assert(config.connection_idle_timeout_secs == 45);
    assert(config.max_routes == 5);
    assert(config.max_backends == 4);
    assert(config.max_active_connections == 512);
    assert(config.max_consecutive_failures == 2);
    assert(config.max_connections_per_sec == 500);
    assert(config.max_connections_per_ip_per_sec == 25);
    assert(config.routes[0].frontend_port == 8080);
    assert(config.routes[0].backend_count == 2);

    printf("[PASS] test_config passed successfully.\n");
    return 0;
}
