#include "net.h"

#include <errno.h>
#include <sys/socket.h>
#include <unistd.h>

ssize_t lb_write_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    size_t left = len;

    while (left > 0) {
        ssize_t n = write(fd, p, left);
        if (n == -1) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += n;
        left -= (size_t)n;
    }
    return (ssize_t)len;
}

int lb_listen_tcp(const struct sockaddr_in *addr, int backlog)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1)
        return -1;

    /* Allow an immediate restart while old connections sit in TIME_WAIT. */
    int one = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) == -1)
        goto fail;
    if (bind(fd, (const struct sockaddr *)addr, sizeof *addr) == -1)
        goto fail;
    if (listen(fd, backlog) == -1)
        goto fail;
    return fd;

fail: {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
}
