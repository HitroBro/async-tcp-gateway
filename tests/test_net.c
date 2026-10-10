#define _POSIX_C_SOURCE 200809L
#include "net.h"
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <unistd.h>
#include <sys/socket.h>

int main(void) {
    printf("[*] Running test_net: verifying rlimit tuning and socket options...\n");
    int ret = net_tune_rlimit_nofile(2048);
    assert(ret == 0);

    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    assert(net_set_buffer_sizes(fds[0], 32768, 32768) == 0);
    close(fds[0]);
    close(fds[1]);

    GatewayConfig kcfg;
    memset(&kcfg, 0, sizeof(GatewayConfig));
    kcfg.tcp_keepalive_idle_secs = 45;
    kcfg.tcp_keepalive_interval_secs = 15;
    kcfg.tcp_keepalive_probes = 5;

    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    assert(net_apply_keepalive_config(sv[0], &kcfg) == 0);
    close(sv[0]);
    close(sv[1]);
    printf("[PASS] net_apply_keepalive_config verified successfully.\n");

    int lsock = socket(AF_INET, SOCK_STREAM, 0);
    assert(lsock >= 0);
    // Attempt TFO (may succeed or return -1 depending on kernel settings, ensure non-crashing)
    net_enable_tcp_fastopen(lsock, 10);
    close(lsock);
    printf("[PASS] net_enable_tcp_fastopen sanity test passed.\n");

    printf("[PASS] test_net unit tests passed successfully.\n");
    return 0;
}
