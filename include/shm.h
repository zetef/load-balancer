#ifndef LB_SHM_API_H
#define LB_SHM_API_H

#include <stddef.h>
#include "config.h"
#include "lb_shm.h"

/* sizeof(struct lb_shm) rotunjit la pagina; acelasi in master si in stub */
size_t lb_shm_size(void);

/* Creeaza, initializeaza si umple regiunea. NULL + errno la eroare. */
struct lb_shm *lb_shm_create(const char *name, const struct lb_config *cfg);

typedef void (*lb_shm_repair_fn)(struct lb_shm *shm, const void *arg);

/* 0 = lock luat (si dupa EOWNERDEAD reparat), altfel codul de eroare */
int lb_shm_lock(struct lb_shm *shm, lb_shm_repair_fn repair, const void *arg);
int lb_shm_unlock(struct lb_shm *shm);

/* Rescrie tabela din cfg si creste generation. 0 sau codul de eroare. */
int lb_shm_apply_config(struct lb_shm *shm, const struct lb_config *cfg);

/* munmap + shm_unlink. 0 sau -1 cu errno. */
int lb_shm_destroy(struct lb_shm *shm, const char *name);

#endif
