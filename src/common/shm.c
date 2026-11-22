#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "shm.h"

size_t lb_shm_size(void)
{
    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) {
        page = 4096;
    }
    size_t p = (size_t)page;
    return (sizeof(struct lb_shm) + p - 1) / p * p;
}

/* Doar campurile structurale, doar sub lock. Starile si contoarele au alti scriitori. */
static void fill_table(struct lb_shm *shm, const struct lb_config *cfg)
{
    for (int i = 0; i < LB_MAX_BACKENDS; i++) {
        struct lb_backend *b = &shm->backends[i];

        if (i < cfg->n_backends) {
            const struct lb_config_backend *c = &cfg->backends[i];
            snprintf(b->name, sizeof b->name, "%s", c->name);
            b->addr   = c->addr;
            b->weight = c->weight;
            b->in_use = 1;
        } else {
            b->in_use = 0;
        }
    }
    /* unu dupa ultimul slot folosit; buclele sar peste in_use == 0 */
    shm->backend_count = (uint32_t)cfg->n_backends;
    atomic_store_explicit(&shm->algorithm, (int)cfg->algorithm, memory_order_relaxed);
}

static void repair_from_config(struct lb_shm *shm, const void *arg)
{
    fill_table(shm, arg);
}

int lb_shm_lock(struct lb_shm *shm, lb_shm_repair_fn repair, const void *arg)
{
    int rc = pthread_mutex_lock(&shm->lock);
    if (rc == 0) {
        return 0;
    }
    if (rc != EOWNERDEAD) {
        fprintf(stderr, "lb: shm lock failed: %s\n", strerror(rc));
        return rc;
    }

    fprintf(stderr, "lb: shm lock: EOWNERDEAD (previous owner died), recovering\n");
    if (repair != NULL) {
        repair(shm, arg);
    }
    /* obligatoriu inainte de unlock, altfel ENOTRECOVERABLE pentru totdeauna */
    rc = pthread_mutex_consistent(&shm->lock);
    if (rc != 0) {
        fprintf(stderr, "lb: pthread_mutex_consistent: %s\n", strerror(rc));
        pthread_mutex_unlock(&shm->lock);
        return rc;
    }
    return 0;
}

int lb_shm_unlock(struct lb_shm *shm)
{
    int rc = pthread_mutex_unlock(&shm->lock);
    if (rc != 0) {
        fprintf(stderr, "lb: shm unlock failed: %s\n", strerror(rc));
    }
    return rc;
}

int lb_shm_apply_config(struct lb_shm *shm, const struct lb_config *cfg)
{
    int rc = lb_shm_lock(shm, repair_from_config, cfg);
    if (rc != 0) {
        return rc;
    }

    fill_table(shm, cfg);
    /* ultima scriere: cine vede generation nou vede si tabela noua */
    atomic_fetch_add_explicit(&shm->generation, 1, memory_order_release);

    return lb_shm_unlock(shm);
}

static int init_mutex(pthread_mutex_t *m)
{
    pthread_mutexattr_t a;
    int rc = pthread_mutexattr_init(&a);
    if (rc != 0) {
        return rc;
    }
    rc = pthread_mutexattr_setpshared(&a, PTHREAD_PROCESS_SHARED);
    if (rc == 0) {
        rc = pthread_mutexattr_setrobust(&a, PTHREAD_MUTEX_ROBUST);
    }
    if (rc == 0) {
        rc = pthread_mutex_init(m, &a);
    }
    pthread_mutexattr_destroy(&a);
    return rc;
}

static void init_atomics(struct lb_shm *shm)
{
    atomic_init(&shm->generation, 0);
    atomic_init(&shm->algorithm, LB_ALGO_RR);

    for (int i = 0; i < LB_MAX_BACKENDS; i++) {
        struct lb_backend *b = &shm->backends[i];
        /* UP pana exista health checker-ul (C5) */
        atomic_init(&b->health_state, LB_HEALTH_UP);
        atomic_init(&b->admin_state, LB_ADMIN_ENABLED);
        atomic_init(&b->active_conns, 0);
        atomic_init(&b->total_conns, 0);
        atomic_init(&b->passive_failures, 0);
        atomic_init(&b->bytes_in, 0);
        atomic_init(&b->bytes_out, 0);
    }
    for (int i = 0; i < LB_MAX_WORKERS; i++) {
        atomic_init(&shm->workers[i].active_conns, 0);
        atomic_init(&shm->workers[i].total_conns, 0);
    }
}

static int open_exclusive(const char *name)
{
    int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd != -1 || errno != EEXIST) {
        return fd;
    }

    /* ramasa de la un master omorat cu kill -9 (decizia 9) */
    fprintf(stderr, "lb: stale shm region %s found (previous master did not exit cleanly), removing it\n",
            name);
    if (shm_unlink(name) == -1 && errno != ENOENT) {
        return -1;
    }
    return shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
}

struct lb_shm *lb_shm_create(const char *name, const struct lb_config *cfg)
{
    size_t size = lb_shm_size();

    int fd = open_exclusive(name);
    if (fd == -1) {
        return NULL;
    }

    if (ftruncate(fd, (off_t)size) == -1) {
        int saved = errno;
        close(fd);
        shm_unlink(name);
        errno = saved;
        return NULL;
    }

    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    int saved = errno;
    close(fd);
    if (p == MAP_FAILED) {
        shm_unlink(name);
        errno = saved;
        return NULL;
    }

    struct lb_shm *shm = p;

    int rc = init_mutex(&shm->lock);
    if (rc != 0) {
        munmap(p, size);
        shm_unlink(name);
        errno = rc;
        return NULL;
    }
    init_atomics(shm);

    rc = lb_shm_apply_config(shm, cfg);
    if (rc != 0) {
        munmap(p, size);
        shm_unlink(name);
        errno = rc;
        return NULL;
    }

    shm->version = LB_SHM_VERSION;
    return shm;
}

int lb_shm_destroy(struct lb_shm *shm, const char *name)
{
    int ret = 0;
    int saved = 0;

    if (munmap(shm, lb_shm_size()) == -1) {
        ret = -1;
        saved = errno;
    }
    if (shm_unlink(name) == -1 && ret == 0) {
        ret = -1;
        saved = errno;
    }
    if (ret == -1) {
        errno = saved;
    }
    return ret;
}
