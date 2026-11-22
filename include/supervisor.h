#ifndef LB_SUPERVISOR_H
#define LB_SUPERVISOR_H

#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <sys/types.h>

#include "config.h"
#include "lb_shm.h"

/* delay = min(BASE * 2^k, MAX); k revine la 0 dupa STABLE ms de rulare */
#define LB_SUP_BASE_MS        100
#define LB_SUP_MAX_MS         10000
#define LB_SUP_STABLE_MS      30000
/* LIMIT iesiri in WINDOW ms -> slotul nu mai e repornit */
#define LB_SUP_LOOP_WINDOW_MS 60000
#define LB_SUP_LOOP_LIMIT     10

#define LB_SUP_MAX_EXTRA      8
#define LB_SUP_ARG_LEN        64

enum lb_slot_state {
    LB_SLOT_EMPTY,
    LB_SLOT_RUNNING,
    LB_SLOT_WAITING,
    LB_SLOT_GAVE_UP
};

struct lb_worker_slot {
    pid_t    pid;
    int      state;
    int64_t  started_ms;
    int64_t  restart_at_ms;
    unsigned backoff_k;
    unsigned loop_count;
    int64_t  loop_start_ms;
    uint32_t restarts;
    int      last_status;
};

struct lb_supervisor {
    int                   n;
    struct lb_worker_slot slots[LB_MAX_WORKERS];
    pid_t                 master_pid;
    sigset_t              child_mask;
    struct lb_shm        *shm;
    int                   stopping;

    /* argv construit in parinte: dupa fork nu avem voie la snprintf */
    char bin[PATH_MAX];
    char shm_name[LB_SUP_ARG_LEN];
    char listen[LB_SUP_ARG_LEN];
    char connect_ms[LB_SUP_ARG_LEN];
    char idle_ms[LB_SUP_ARG_LEN];
    char extra[LB_SUP_MAX_EXTRA][LB_SUP_ARG_LEN];
    int  n_extra;
};

int64_t lb_now_ms(void);
long supervisor_backoff_ms(unsigned k);

int supervisor_init(struct lb_supervisor *s, const struct lb_config *cfg,
                    const char *bin, const char *shm_name, const char *extra,
                    struct lb_shm *shm, const sigset_t *child_mask);
int supervisor_spawn(struct lb_supervisor *s, int slot);
int supervisor_start_all(struct lb_supervisor *s);
void supervisor_on_exit(struct lb_supervisor *s, int slot, int status, int64_t now_ms);
int supervisor_reap(struct lb_supervisor *s);
void supervisor_tick(struct lb_supervisor *s);
long supervisor_next_timeout_ms(const struct lb_supervisor *s);
int supervisor_running(const struct lb_supervisor *s);
int supervisor_all_gave_up(const struct lb_supervisor *s);
void supervisor_stop_all(struct lb_supervisor *s, long deadline_ms);

#endif
