#include <stdalign.h>
#include "lb_shm.h"

static_assert(alignof(struct lb_backend) == 64, "struct lb_backend must start on a cache line (decision 8)");
static_assert(sizeof(struct lb_shm) <= 64 * 1024, "shm region must stay under 64 KiB");

int main(void) { return 0; }

//gcc -std=c11 -Wall -Wextra -D_GNU_SOURCE -Iinclude -c tests/unit/test_shm_layout.c -o /tmp/t.o && echo OK
//Stricare
//sed 's/== 64,/== 32,/' tests/unit/test_shm_layout.c > /tmp/broken1.c
//gcc -std=c11 -Wall -Wextra -D_GNU_SOURCE -Iinclude -c /tmp/broken1.c -o /tmp/b1.o
