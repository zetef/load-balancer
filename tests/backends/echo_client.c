/* echo_client: sends stdin line by line to an echo server and prints the echo (D1).
 *
 * Usage: echo_client <ip> <port>
 *
 * Syscall sequence and error policy:
 *   socket   -> error: exit 1
 *   connect  -> ECONNREFUSED: "connection refused", exit 1; other error: exit 1
 *   loop over stdin lines (fgets):
 *     write  the whole line (lb_write_all) -> error: exit 1
 *     read   until the same number of bytes came back
 *            -> 0 before that: server closed, exit 1; EINTR: retry
 *     print  the echo on stdout
 *   stdin EOF -> close, exit 0
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include "net.h"

/* Read exactly len bytes. Returns 0 on success, -1 on error or early EOF. */
static int read_full(int fd, char *buf, size_t len)
{
    size_t got = 0;

    while (got < len) {
        ssize_t n = read(fd, buf + got, len - got);
        if (n == 0)
            return -1;
        if (n == -1) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        got += (size_t)n;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s <ip> <port>\n", argv[0]);
        return 2;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    char *end;
    long port = strtol(argv[2], &end, 10);
    if (inet_pton(AF_INET, argv[1], &addr.sin_addr) != 1 ||
        *argv[2] == '\0' || *end != '\0' || port < 1 || port > 65535) {
        fprintf(stderr, "invalid address: %s %s\n", argv[1], argv[2]);
        return 2;
    }
    addr.sin_port = htons((uint16_t)port);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) {
        perror("socket");
        return 1;
    }
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) == -1) {
        if (errno == ECONNREFUSED)
            fprintf(stderr, "connection refused: nothing listens on %s:%ld\n",
                    argv[1], port);
        else
            perror("connect");
        close(fd);
        return 1;
    }

    char line[4096], echo[4096];
    while (fgets(line, sizeof line, stdin) != NULL) {
        size_t len = strlen(line);
        if (lb_write_all(fd, line, len) == -1) {
            perror("write");
            close(fd);
            return 1;
        }
        if (read_full(fd, echo, len) == -1) {
            fprintf(stderr, "server closed the connection\n");
            close(fd);
            return 1;
        }
        fwrite(echo, 1, len, stdout);
    }

    close(fd);
    return 0;
}
