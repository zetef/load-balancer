/* echo_server: blocking, iterative TCP echo server (D1).
 *
 * Usage: echo_server <port>
 *
 * Syscall sequence and error policy:
 *   socket       -> error: exit
 *   setsockopt   SO_REUSEADDR, so a restart works while TIME_WAIT entries exist -> error: exit
 *   bind         127.0.0.1:<port> -> error (EADDRINUSE, EACCES): exit
 *   listen       backlog 16 -> error: exit
 *   loop:
 *     accept     blocks until a client is in the accept queue
 *                -> EINTR: retry; other error: log and continue
 *     read       blocks until data or EOF
 *                -> 0: client closed, close(cfd); EINTR: retry; other error: close(cfd)
 *     write      all bytes of the chunk (lb_write_all loops on short writes)
 *                -> error (EPIPE, ECONNRESET): close(cfd)
 *     close      on every path
 *   An error inside one connection never ends the server.
 *
 * One client at a time: while it serves one, the next waits in the accept queue.
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "net.h"

/* Backlog 16: one client is served at a time, so a short queue is enough for
 * tests; the kernel caps it at net.core.somaxconn anyway. */
#define BACKLOG 16

static void serve(int cfd)
{
    char buf[4096];

    for (;;) {
        ssize_t n = read(cfd, buf, sizeof buf);
        if (n == 0)
            return;                         /* EOF: client sent FIN */
        if (n == -1) {
            if (errno == EINTR)
                continue;
            perror("read");
            return;
        }
        if (lb_write_all(cfd, buf, (size_t)n) == -1) {
            perror("write");
            return;
        }
    }
}

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

    /* A client that disconnects while we write must give EPIPE, not kill us. */
    signal(SIGPIPE, SIG_IGN);

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
    fprintf(stderr, "echo_server listening on 127.0.0.1:%ld\n", port);

    for (;;) {
        int cfd = accept(lfd, NULL, NULL);
        if (cfd == -1) {
            if (errno != EINTR)
                perror("accept");
            continue;
        }
        serve(cfd);
        close(cfd);
    }
}
