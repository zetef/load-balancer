/* Per-connection state and the echo service (temporary, replaced by relay in D3).
 *
 * Invariant: the conn is in exactly one of two modes.
 *   READING: buf is empty, we want EPOLLIN.
 *   FLUSHING: buf holds bytes [off, len) the peer has not accepted yet; we
 *             want EPOLLOUT and do NOT read, so memory use stays bounded.
 */
#include "lb_conn.h"

#include <errno.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <unistd.h>

struct lb_conn {
    int fd;
    char buf[4096];
    size_t len;     /* bytes in buf */
    size_t off;     /* bytes of buf already sent */
};

struct lb_conn *lb_conn_new(int fd)
{
    struct lb_conn *c = calloc(1, sizeof *c);
    if (c != NULL)
        c->fd = fd;
    return c;
}

void lb_conn_free(struct lb_conn *c)
{
    if (c == NULL)
        return;
    close(c->fd);
    free(c);
}

int lb_conn_fd(const struct lb_conn *c)
{
    return c->fd;
}

uint32_t lb_conn_events(const struct lb_conn *c)
{
    return c->off < c->len ? EPOLLOUT : EPOLLIN;
}

/* Send buf[off, len) until done or EAGAIN. Returns -1 on a real error. */
static int flush(struct lb_conn *c)
{
    while (c->off < c->len) {
        ssize_t n = write(c->fd, c->buf + c->off, c->len - c->off);
        if (n == -1) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return 0;               /* peer is slow: wait for EPOLLOUT */
            return -1;                  /* EPIPE, ECONNRESET, ... */
        }
        c->off += (size_t)n;
    }
    c->off = c->len = 0;                /* all sent: back to READING */
    return 0;
}

int lb_conn_on_readable(struct lb_conn *c)
{
    /* Level-triggered: one read per event is enough, epoll_wait reports the
     * fd again if data remains. EPOLLHUP/EPOLLERR also end up here and are
     * found by the read itself (0 or -1). */
    ssize_t n = read(c->fd, c->buf, sizeof c->buf);
    if (n == 0)
        return -1;                      /* EOF: peer sent FIN */
    if (n == -1) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
            return (int)lb_conn_events(c);  /* spurious wakeup */
        return -1;
    }
    c->len = (size_t)n;
    c->off = 0;
    if (flush(c) == -1)
        return -1;
    return (int)lb_conn_events(c);
}

int lb_conn_on_writable(struct lb_conn *c)
{
    if (flush(c) == -1)
        return -1;
    return (int)lb_conn_events(c);
}
