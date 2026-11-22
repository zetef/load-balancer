/* Stub worker (C2): se ataseaza la regiune si o afiseaza in fiecare secunda.
 *   stub_worker [--shm <nume>] [--bump <slot>] [--lock-and-die]
 * Test de versiune: compileaza cu -DSTUB_EXPECT_VERSION=2 (comanda e in documentul C1-C2). */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "shm.h"

#ifndef STUB_EXPECT_VERSION
#define STUB_EXPECT_VERSION LB_SHM_VERSION
#endif

static volatile sig_atomic_t stop;

static void on_stop(int sig)
{
    (void)sig;
    stop = 1;
}

/* Campurile structurale, copiate sub lock la schimbarea generation (decizia 10) */
struct local_copy {
    uint64_t           generation;
    uint32_t           count;
    char               name[LB_MAX_BACKENDS][LB_NAME_LEN];
    struct sockaddr_in addr[LB_MAX_BACKENDS];
    uint32_t           weight[LB_MAX_BACKENDS];
    uint32_t           in_use[LB_MAX_BACKENDS];
};

static void usage(const char *prog)
{
    fprintf(stderr, "usage: %s [--shm <name>] [--bump <slot>] [--lock-and-die]\n", prog);
    exit(2);
}

/* Workerul doar citeste: la EOWNERDEAD n-are ce repara (repair = NULL) */
static int copy_table(struct lb_shm *shm, struct local_copy *lc)
{
    int rc = lb_shm_lock(shm, NULL, NULL);
    if (rc != 0) {
        return rc;
    }
    lc->generation = atomic_load_explicit(&shm->generation, memory_order_acquire);
    lc->count = shm->backend_count;
    if (lc->count > LB_MAX_BACKENDS) {
        lc->count = LB_MAX_BACKENDS;
    }
    for (uint32_t i = 0; i < lc->count; i++) {
        memcpy(lc->name[i], shm->backends[i].name, LB_NAME_LEN);
        lc->name[i][LB_NAME_LEN - 1] = '\0';
        lc->addr[i]   = shm->backends[i].addr;
        lc->weight[i] = shm->backends[i].weight;
        lc->in_use[i] = shm->backends[i].in_use;
    }
    return lb_shm_unlock(shm);
}

static const char *health_name(int s)
{
    return s == LB_HEALTH_UP ? "UP" : s == LB_HEALTH_DOWN ? "DOWN" : "?";
}

static const char *admin_name(int s)
{
    switch (s) {
    case LB_ADMIN_ENABLED:  return "ENABLED";
    case LB_ADMIN_DISABLED: return "DISABLED";
    case LB_ADMIN_DRAIN:    return "DRAIN";
    default:                return "?";
    }
}

static const char *algo_name(int a)
{
    switch (a) {
    case LB_ALGO_RR:        return "rr";
    case LB_ALGO_LEASTCONN: return "leastconn";
    case LB_ALGO_IPHASH:    return "iphash";
    default:                return "?";
    }
}

static void print_table(struct lb_shm *shm, const struct local_copy *lc)
{
    printf("generation %llu, algorithm %s, backend_count %u\n",
           (unsigned long long)lc->generation,
           algo_name(atomic_load_explicit(&shm->algorithm, memory_order_relaxed)),
           lc->count);
    for (uint32_t i = 0; i < lc->count; i++) {
        if (!lc->in_use[i]) {
            continue;
        }
        const struct lb_backend *b = &shm->backends[i];
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &lc->addr[i].sin_addr, ip, sizeof ip);
        printf("  [%u] %-12s %s:%u weight %u  %-4s %-8s active_conns %u\n",
               i, lc->name[i], ip, ntohs(lc->addr[i].sin_port), lc->weight[i],
               health_name(atomic_load_explicit(&b->health_state, memory_order_relaxed)),
               admin_name(atomic_load_explicit(&b->admin_state, memory_order_relaxed)),
               atomic_load_explicit(&b->active_conns, memory_order_relaxed));
    }
    fflush(stdout);
}

int main(int argc, char **argv)
{
    const char *name = LB_SHM_NAME;
    long bump_slot = -1;
    int lock_and_die = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--shm") == 0 && i + 1 < argc) {
            name = argv[++i];
        } else if (strcmp(argv[i], "--bump") == 0 && i + 1 < argc) {
            char *end;
            errno = 0;
            bump_slot = strtol(argv[++i], &end, 10);
            if (*end != '\0' || errno != 0 || bump_slot < 0 || bump_slot >= LB_MAX_BACKENDS) {
                fprintf(stderr, "stub: --bump needs a slot 0..%d\n", LB_MAX_BACKENDS - 1);
                return 2;
            }
        } else if (strcmp(argv[i], "--lock-and-die") == 0) {
            lock_and_die = 1;
        } else {
            usage(argv[0]);
        }
    }

    /* fara O_CREAT: daca masterul nu ruleaza, numele nu exista */
    int fd = shm_open(name, O_RDWR, 0);
    if (fd == -1) {
        if (errno == ENOENT) {
            fprintf(stderr, "stub: shm not found (%s): is the master running?\n", name);
        } else {
            fprintf(stderr, "stub: shm_open %s: %s\n", name, strerror(errno));
        }
        return 1;
    }

    /* mai mic decat structura = alt layout; accesul dupa capat ar da SIGBUS */
    struct stat st;
    if (fstat(fd, &st) == -1) {
        fprintf(stderr, "stub: fstat: %s\n", strerror(errno));
        close(fd);
        return 1;
    }
    if ((size_t)st.st_size < sizeof(struct lb_shm)) {
        fprintf(stderr, "stub: shm is %lld bytes, expected at least %zu: layout mismatch\n",
                (long long)st.st_size, sizeof(struct lb_shm));
        close(fd);
        return 1;
    }

    void *p = mmap(NULL, lb_shm_size(), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    int saved = errno;
    close(fd);
    if (p == MAP_FAILED) {
        fprintf(stderr, "stub: mmap: %s\n", strerror(saved));
        return 1;
    }
    struct lb_shm *shm = p;

    if (shm->version != STUB_EXPECT_VERSION) {
        fprintf(stderr, "stub: shm version %u, this stub expects %d: rebuild both sides\n",
                shm->version, STUB_EXPECT_VERSION);
        munmap(p, lb_shm_size());
        return 1;
    }

    if (lock_and_die) {
        int rc = lb_shm_lock(shm, NULL, NULL);
        if (rc != 0) {
            return 1;
        }
        fprintf(stderr, "stub: holding the shm lock and dying without unlocking (pid %d)\n",
                (int)getpid());
        _exit(1);   /* fara unlock: ca un crash */
    }

    /* fara SA_RESTART: sleep() e intrerupt de semnal */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_stop;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT, &sa, NULL) == -1 || sigaction(SIGTERM, &sa, NULL) == -1) {
        fprintf(stderr, "stub: sigaction: %s\n", strerror(errno));
        return 1;
    }

    if (bump_slot >= 0) {
        atomic_fetch_add_explicit(&shm->backends[bump_slot].active_conns, 1, memory_order_relaxed);
        printf("stub: holding one connection on slot %ld\n", bump_slot);
    }

    struct local_copy lc;
    memset(&lc, 0, sizeof lc);
    int ret = 0;

    while (!stop) {
        uint64_t gen = atomic_load_explicit(&shm->generation, memory_order_acquire);
        if (gen != lc.generation) {
            int rc = copy_table(shm, &lc);
            if (rc != 0) {
                ret = 1;
                break;
            }
        }
        print_table(shm, &lc);
        sleep(1);
    }

    if (bump_slot >= 0) {
        atomic_fetch_sub_explicit(&shm->backends[bump_slot].active_conns, 1, memory_order_relaxed);
        printf("stub: released the connection on slot %ld\n", bump_slot);
    }
    munmap(p, lb_shm_size());
    return ret;
}
