#define _POSIX_C_SOURCE 200809L
#include "logger.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

int main(void) {
    printf("[*] Running test_logger: verifying JSON access log formatting...\n");
    char buf[512];
    int len = access_log_format_json(buf, sizeof(buf),
                                    "192.168.1.50", 12,
                                    "10.0.0.1", 8080,
                                    1024, 4096, 0.125);
    assert(len > 0);
    assert(strstr(buf, "\"type\":\"access\"") != NULL);
    assert(strstr(buf, "\"client_ip\":\"192.168.1.50\"") != NULL);
    assert(strstr(buf, "\"backend_ip\":\"10.0.0.1\"") != NULL);
    assert(strstr(buf, "\"backend_port\":8080") != NULL);
    assert(strstr(buf, "\"duration_ms\":125.00") != NULL);
    printf("[PASS] test_logger JSON formatting assertions passed.\n");
    return 0;
}
