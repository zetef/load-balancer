/* Teste pentru regiunea shm (C2). Nume proprii, ca sa nu deranjeze un master pe /lb_shm. */
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "shm.h"

static char name_buf[64];

static const char *test_name(int n)
{
    snprintf(name_buf, sizeof name_buf, "/lb_shm_test_%d_%d", (int)getpid(), n);
    return name_buf;
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void child_lock_and_die(struct lb_shm *shm, int corrupt)
{
    fflush(stdout);   /* altfel bufferul e copiat in copil */
    pid_t pid = fork();
    assert(pid != -1);
    if (pid == 0) {
        if (pthread_mutex_lock(&shm->lock) != 0) {
            _exit(2);
        }
        if (corrupt) {
            memcpy(shm->backends[0].name, "half-writ", 9);
        }
        _exit(0);   /* fara unlock */
    }
    int st;
    assert(waitpid(pid, &st, 0) == pid);
    assert(WIFEXITED(st) && WEXITSTATUS(st) == 0);
}

static void test_create(const struct lb_config *cfg)
{
    const char *name = test_name(1);
    struct lb_shm *shm = lb_shm_create(name, cfg);
    assert(shm != NULL);

    int fd = shm_open(name, O_RDONLY, 0);
    assert(fd != -1);
    struct stat st;
    assert(fstat(fd, &st) == 0);
    close(fd);
    assert((st.st_mode & 0777) == 0600);
    assert((size_t)st.st_size == lb_shm_size());
    assert(lb_shm_size() >= sizeof(struct lb_shm));
    assert(lb_shm_size() % (size_t)sysconf(_SC_PAGESIZE) == 0);

    assert(shm->version == LB_SHM_VERSION);
    assert(atomic_load(&shm->generation) == 1);
    assert(atomic_load(&shm->algorithm) == LB_ALGO_LEASTCONN);
    assert(shm->backend_count == 2);
    for (int i = 0; i < cfg->n_backends; i++) {
        const struct lb_backend *b = &shm->backends[i];
        assert(b->in_use == 1);
        assert(strcmp(b->name, cfg->backends[i].name) == 0);
        assert(b->addr.sin_addr.s_addr == cfg->backends[i].addr.sin_addr.s_addr);
        assert(b->addr.sin_port == cfg->backends[i].addr.sin_port);
        assert(b->weight == cfg->backends[i].weight);
        assert(atomic_load(&b->health_state) == LB_HEALTH_UP);
        assert(atomic_load(&b->admin_state) == LB_ADMIN_ENABLED);
        assert(atomic_load(&b->active_conns) == 0);
    }
    assert(shm->backends[2].in_use == 0);

    assert(lb_shm_destroy(shm, name) == 0);
    assert(shm_open(name, O_RDONLY, 0) == -1 && errno == ENOENT);
    printf("ok   create: mode 0600, %zu bytes, version %d, generation 1, table matches\n",
           lb_shm_size(), LB_SHM_VERSION);
}

static void test_stale(const struct lb_config *cfg)
{
    const char *name = test_name(2);
    struct lb_shm *old = lb_shm_create(name, cfg);
    assert(old != NULL);
    atomic_store(&old->backends[0].active_conns, 7);

    struct lb_shm *fresh = lb_shm_create(name, cfg);
    assert(fresh != NULL);
    assert(fresh != old);
    assert(atomic_load(&fresh->backends[0].active_conns) == 0);
    /* vechea mapare e alt obiect */
    assert(atomic_load(&old->backends[0].active_conns) == 7);

    assert(munmap(old, lb_shm_size()) == 0);
    assert(lb_shm_destroy(fresh, name) == 0);
    printf("ok   stale region: removed and re-created, old mapping is a different object\n");
}

static void test_cross_process_lock(const struct lb_config *cfg)
{
    const char *name = test_name(3);
    struct lb_shm *shm = lb_shm_create(name, cfg);
    assert(shm != NULL);

    int pfd[2];
    assert(pipe(pfd) == 0);
    fflush(stdout);   /* altfel bufferul e copiat in copil */
    pid_t pid = fork();
    assert(pid != -1);
    if (pid == 0) {
        close(pfd[0]);
        if (lb_shm_lock(shm, NULL, NULL) != 0) {
            _exit(2);
        }
        char c = 'L';
        if (write(pfd[1], &c, 1) != 1) {
            _exit(3);
        }
        close(pfd[1]);
        struct timespec ts = { 0, 500 * 1000 * 1000 };
        nanosleep(&ts, NULL);
        lb_shm_unlock(shm);
        _exit(0);
    }
    close(pfd[1]);
    char c;
    assert(read(pfd[0], &c, 1) == 1);   /* copilul are lock-ul */
    close(pfd[0]);

    double t0 = now_s();
    assert(lb_shm_lock(shm, NULL, NULL) == 0);
    double waited = now_s() - t0;
    assert(lb_shm_unlock(shm) == 0);

    int st;
    assert(waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0);
    assert(waited > 0.3);

    assert(lb_shm_destroy(shm, name) == 0);
    printf("ok   cross-process lock: parent waited %.2f s for the child\n", waited);
}

static void test_eownerdead(const struct lb_config *cfg)
{
    const char *name = test_name(4);
    struct lb_shm *shm = lb_shm_create(name, cfg);
    assert(shm != NULL);

    child_lock_and_die(shm, 1);
    assert(strncmp(shm->backends[0].name, "half-writ", 9) == 0);

    assert(lb_shm_apply_config(shm, cfg) == 0);
    assert(strcmp(shm->backends[0].name, cfg->backends[0].name) == 0);
    assert(atomic_load(&shm->generation) == 2);

    assert(pthread_mutex_lock(&shm->lock) == 0);
    assert(pthread_mutex_unlock(&shm->lock) == 0);

    assert(lb_shm_destroy(shm, name) == 0);
    printf("ok   EOWNERDEAD: table repaired from config, generation 2, mutex usable again\n");
}

static void test_enotrecoverable(const struct lb_config *cfg)
{
    const char *name = test_name(5);
    struct lb_shm *shm = lb_shm_create(name, cfg);
    assert(shm != NULL);

    child_lock_and_die(shm, 0);

    /* intentionat: unlock fara consistent */
    assert(pthread_mutex_lock(&shm->lock) == EOWNERDEAD);
    assert(pthread_mutex_unlock(&shm->lock) == 0);

    assert(pthread_mutex_lock(&shm->lock) == ENOTRECOVERABLE);
    assert(lb_shm_lock(shm, NULL, NULL) == ENOTRECOVERABLE);
    assert(lb_shm_apply_config(shm, cfg) == ENOTRECOVERABLE);

    assert(lb_shm_destroy(shm, name) == 0);
    printf("ok   ENOTRECOVERABLE: unlock without consistent breaks the mutex for good\n");
}

int main(void)
{
    struct lb_config cfg;
    char err[LB_CONFIG_ERR_LEN];
    if (lb_config_load("tests/unit/configs/good.conf", &cfg, err, sizeof err) != 0) {
        printf("FAIL %s\n", err);
        return 1;
    }

    test_create(&cfg);
    test_stale(&cfg);
    test_cross_process_lock(&cfg);
    test_eownerdead(&cfg);
    test_enotrecoverable(&cfg);

    printf("all shm tests passed\n");
    return 0;
}
