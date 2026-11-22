#ifndef LB_NET_H
#define LB_NET_H

#include <stddef.h>
#include <sys/types.h>
#include <netinet/in.h>

/* Write all len bytes to fd. Loops on short writes and retries on EINTR.
 * Returns len on success, -1 on error (errno set by write). */
ssize_t lb_write_all(int fd, const void *buf, size_t len);

/* socket + SO_REUSEADDR + bind + listen on addr. Returns the listening fd,
 * or -1 on error (errno set, no fd leaked). */
int lb_listen_tcp(const struct sockaddr_in *addr, int backlog);

/* Set O_NONBLOCK on the open file description of fd (keeps the other flags).
 * Returns 0, or -1 with errno set. */
int lb_set_nonblock(int fd);

#endif
