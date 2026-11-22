#ifndef LB_LOOP_H
#define LB_LOOP_H

/* Worker event loop (Controller in MVCS): one epoll instance, a listening fd
 * and the connections accepted from it. Sets lfd non-blocking.
 * Runs until a fatal error; returns -1 with errno set. */
int lb_loop_run(int lfd);

#endif
