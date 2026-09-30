#define _POSIX_C_SOURCE 200809L
#include "gateway.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

int main(void) {
    printf("[*] Running test_metrics: verifying Prometheus text format serializer...\n");
    char buf[2048];
    size_t len = metrics_format_prometheus(NULL, buf, sizeof(buf));
    assert(len > 0);
    assert(strstr(buf, "gateway_total_connections_total") != NULL);
    assert(strstr(buf, "gateway_active_connections") != NULL);
    assert(strstr(buf, "gateway_bytes_read_total") != NULL);
    printf("[PASS] test_metrics passed successfully.\n");
    return 0;
}
