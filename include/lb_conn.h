#ifndef LB_CONN_H
#define LB_CONN_H

#include <stdint.h>

/* Per-connection state of the worker (Model in MVCS). Opaque: the fields live
 * in src/worker/conn.c, callers only hold a pointer. */
struct lb_conn;

/* Allocate a conn for an already accepted, non-blocking fd. It starts waiting
 * for input. Returns NULL on out-of-memory (the fd stays with the caller). */
struct lb_conn *lb_conn_new(int fd);

/* Close the fd and free the conn. The one place where a connection dies. */
void lb_conn_free(struct lb_conn *c);

int lb_conn_fd(const struct lb_conn *c);

/* epoll events the conn wants right now: EPOLLIN, or EPOLLOUT while it holds
 * bytes it could not send yet. */
uint32_t lb_conn_events(const struct lb_conn *c);

/* Handlers: -1 = close the conn, otherwise the events it wants next
 * (compare with lb_conn_events before the call to know if EPOLL_CTL_MOD is
 * needed). Echo is the temporary service; D3 replaces it with the relay. */
int lb_conn_on_readable(struct lb_conn *c);
int lb_conn_on_writable(struct lb_conn *c);

#endif
