#include "logger.h"
#include "config.h"
#include "net.h"
#include "event.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <getopt.h>

#define DEFAULT_CONFIG_PATH "config/gateway.conf"

static void print_usage(const char *prog_name) {
    fprintf(stderr, "Usage: %s [-c config_file]\n", prog_name);
    fprintf(stderr, "  -c config_file   Path to configuration file (default: %s)\n", DEFAULT_CONFIG_PATH);
    fprintf(stderr, "  -h               Show this help message\n");
}

int main(int argc, char *argv[]) {
    const char *config_path = DEFAULT_CONFIG_PATH;
    int opt;

    while ((opt = getopt(argc, argv, "c:h")) != -1) {
        switch (opt) {
            case 'c':
                config_path = optarg;
                break;
            case 'h':
                print_usage(argv[0]);
                return EXIT_SUCCESS;
            default:
                print_usage(argv[0]);
                return EXIT_FAILURE;
        }
    }

    LOG_INFO("Starting 404_Team_not_Found High-Concurrency Layer 4 Traffic Gateway...");

    // 1. Load routing configuration
    GatewayConfig config;
    if (config_load(config_path, &config) < 0) {
        LOG_ERROR("Fatal: Could not load configuration from %s.", config_path);
        return EXIT_FAILURE;
    }
    config_print(&config);

    LOG_INFO("Handing control over to the Asynchronous Epoll Event Loop...");

    // 2. Launch the high-concurrency event engine (handles all routes)
    int status = event_loop_run(&config);

    // 3. Clean shutdown
    LOG_INFO("Gateway process terminated.");
    return (status == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}