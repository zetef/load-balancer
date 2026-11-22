/* Single-threaded epoll loop.
 *
 * Decision: level-triggered (LT). It forgives a handler that does not drain
 * the socket (epoll_wait reports the fd again), and the cost of the extra
 * wakeups is small next to the bug risk of edge-triggered; ET can be
 * measured in D8 if the benchmarks ask for it.
 *
 * epoll_data.ptr is either &listener_tag (the listening socket) or a
 * struct lb_conn *. One dereference dispatches, no lookup table.
 */
#include "lb_loop.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "lb_conn.h"
#include "net.h"

#define MAX_EVENTS 64

static char listener_tag;   /* only its address matters */

static void close_conn(int ep, struct lb_conn *c)
{
    /* DEL first: harmless when close would remove it anyway, and correct if
     * the open file description is ever shared (dup, fork). */
    epoll_ctl(ep, EPOLL_CTL_DEL, lb_conn_fd(c), NULL);
    lb_conn_free(c);
}

/* Accept every pending connection (until EAGAIN) and register it. */
static void accept_all(int ep, int lfd)
{
    for (;;) {
        int cfd = accept4(lfd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (cfd == -1) {
            if (errno == EINTR || errno == ECONNABORTED)
                continue;               /* skip this one */
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                perror("accept4");      /* EMFILE etc: log, keep serving */
            return;
        }
        struct lb_conn *c = lb_conn_new(cfd);
        if (c == NULL) {
            close(cfd);
            return;
        }
        struct epoll_event ev = { .events = lb_conn_events(c), .data.ptr = c };
        if (epoll_ctl(ep, EPOLL_CTL_ADD, cfd, &ev) == -1) {
            perror("epoll_ctl ADD");
            lb_conn_free(c);
        }
    }
}

static void handle_conn(int ep, struct lb_conn *c)
{
    uint32_t before = lb_conn_events(c);
    /* Dispatch on the mode the conn is in, not on the event bits: EPOLLHUP
     * and EPOLLERR are always reported, and the handler's own syscall finds
     * the error (read/write returns 0 or -1) and asks for a close. */
    int want = (before & EPOLLOUT) ? lb_conn_on_writable(c)
                                   : lb_conn_on_readable(c);
    if (want == -1) {
        close_conn(ep, c);
        return;
    }
    if ((uint32_t)want != before) {
        struct epoll_event ev = { .events = (uint32_t)want, .data.ptr = c };
        if (epoll_ctl(ep, EPOLL_CTL_MOD, lb_conn_fd(c), &ev) == -1) {
            perror("epoll_ctl MOD");
            close_conn(ep, c);
        }
    }
}

int lb_loop_run(int lfd)
{
    if (lb_set_nonblock(lfd) == -1)
        return -1;
    int ep = epoll_create1(EPOLL_CLOEXEC);
    if (ep == -1)
        return -1;
    struct epoll_event lev = { .events = EPOLLIN, .data.ptr = &listener_tag };
    if (epoll_ctl(ep, EPOLL_CTL_ADD, lfd, &lev) == -1) {
        int saved = errno;
        close(ep);
        errno = saved;
        return -1;
    }

    struct epoll_event evs[MAX_EVENTS];
    for (;;) {
        int n = epoll_wait(ep, evs, MAX_EVENTS, -1);
        if (n == -1) {
            if (errno == EINTR)
                continue;
            int saved = errno;
            close(ep);
            errno = saved;
            return -1;
        }
        for (int i = 0; i < n; i++) {
            if (evs[i].data.ptr == &listener_tag)
                accept_all(ep, lfd);
            else
                handle_conn(ep, evs[i].data.ptr);
        }
    }
}
