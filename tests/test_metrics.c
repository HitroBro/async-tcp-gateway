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
    // Test healthz liveness serializer
    char hz_buf[256];
    assert(gateway_format_healthz(hz_buf, sizeof(hz_buf)) > 0);
    assert(strstr(hz_buf, "HTTP/1.1 200 OK") != NULL);
    assert(strstr(hz_buf, "OK\n") != NULL);
    printf("[PASS] gateway_format_healthz verified successfully.\n");

    // Test readyz readiness serializer
    char rz_buf[256];
    assert(gateway_format_readyz(NULL, rz_buf, sizeof(rz_buf)) > 0);
    assert(strstr(rz_buf, "HTTP/1.1 200 OK") != NULL);
    printf("[PASS] gateway_format_readyz verified successfully.\n");

    printf("[PASS] test_metrics passed successfully.\n");
    return 0;
}
