/* echo_epoll: the D1 echo server on the worker event loop (D2).
 *
 * Usage: echo_epoll <port>
 * Many clients at once, one thread. Same wire behaviour as echo_server.
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>

#include "lb_loop.h"
#include "net.h"

#define BACKLOG 128

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        return 2;
    }
    char *end;
    long port = strtol(argv[1], &end, 10);
    if (*argv[1] == '\0' || *end != '\0' || port < 1 || port > 65535) {
        fprintf(stderr, "invalid port: %s\n", argv[1]);
        return 2;
    }

    signal(SIGPIPE, SIG_IGN);   /* a vanished client must give EPIPE, not kill us */

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    int lfd = lb_listen_tcp(&addr, BACKLOG);
    if (lfd == -1) {
        perror("listen on 127.0.0.1");
        return 1;
    }
    fprintf(stderr, "echo_epoll listening on 127.0.0.1:%ld\n", port);

    lb_loop_run(lfd);
    perror("lb_loop_run");
    return 1;
}
