#define _POSIX_C_SOURCE 200809L
#include "net.h"
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

    printf("[PASS] test_net unit tests passed successfully.\n");
    return 0;
}
