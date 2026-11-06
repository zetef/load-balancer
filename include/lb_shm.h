#ifndef LB_SHM_H
#define LB_SHM_H

#include <assert.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <sys/types.h>

#define LB_SHM_NAME      "/lb_shm"
#define LB_SHM_VERSION   1
#define LB_MAX_BACKENDS  64
#define LB_MAX_WORKERS   64
#define LB_NAME_LEN      32

static_assert(ATOMIC_INT_LOCK_FREE == 2, "int atomics must be lock-free");
static_assert(ATOMIC_LONG_LOCK_FREE == 2, "64-bit atomics must be lock-free");

enum lb_health_state {
    LB_HEALTH_UP,
    LB_HEALTH_DOWN
};

enum lb_admin_state {
    LB_ADMIN_ENABLED,    // = 0, primește clienți noi
    LB_ADMIN_DISABLED,   // = 1, fără clienți noi, cei existenți sunt tăiați
    LB_ADMIN_DRAIN      // = 2, fără clienți noi, cei existenți termină normal
};

enum lb_algorithm {
    LB_ALGO_RR,
    LB_ALGO_LEASTCONN,
    LB_ALGO_IPHASH
};

struct lb_backend {
    _Alignas(64) char    name[LB_NAME_LEN];
    struct sockaddr_in   addr;
    uint32_t             weight;
    uint32_t             in_use;

    _Atomic int          health_state;
    _Atomic int          admin_state;

    uint32_t             hc_consecutive_ok;
    uint32_t             hc_consecutive_fail;

    _Atomic uint32_t     active_conns;
    _Atomic uint64_t     total_conns;
    _Atomic uint64_t     passive_failures;
    _Atomic uint64_t     bytes_in;
    _Atomic uint64_t     bytes_out;
};

struct lb_worker_stats {

    pid_t                pid;
    uint32_t             restarts;

    _Atomic uint32_t     active_conns;
    _Atomic uint64_t     total_conns;
};

struct lb_shm {
    uint32_t             version;
    pthread_mutex_t      lock;
    _Atomic uint64_t     generation;
    _Atomic int          algorithm;
    uint32_t             backend_count;
    struct lb_backend    backends[LB_MAX_BACKENDS];
    struct lb_worker_stats workers[LB_MAX_WORKERS];
};

#endif
