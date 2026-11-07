# Design decisions (S2)
1 to 13 come from the open questions in the shm contract. 14 is about how the master starts a worker.

## 1. Max number of backends
The shm region has a fixed size, so the backend table is a fixed array.
Picked: 64 (LB_MAX_BACKENDS). The region stays small. The parser rejects a config with more than 64 backends. More later means a new layout and a new version.

## 2. Slots on reload
Open connections decrement active_conns of the slot they used, so a backend must not move.
Picked: an in_use flag per slot, so a backend keeps its index. A removed slot is reused only when its active_conns is 0. Every loop skips slots with in_use == 0.

## 3. Round-robin cursor
Picked: one cursor per worker, in its own memory (no rr_cursor in shm).
Workers don't fight over one cache line. Inside a worker the rotation is exact, across workers it is only roughly even.

## 4. Weights
Picked: only round-robin uses the weight. Least-conn and IP hash ignore it (we say this in lb.conf.example).
The field is already in the struct, so using it later does not change the layout.

## 5. Admin state and health state
A backend can be DOWN and DISABLED at the same time, and after lbctl enable it must show its real health (FR-6).
Picked: two atomic fields. health_state (UP/DOWN) is written by the health checker, admin_state (ENABLED/DISABLED/DRAIN) by the admin socket.
A backend is picked only if it is ENABLED and UP. One writer per field, so they never overwrite each other.

## 6. IPv4 only
The requirements only ask for IPv4.
Picked: struct sockaddr_in. Smaller and simpler. IPv6 later means a new layout and a new version.

## 7. Mutex when a process dies holding it
kill -9 on a worker (FR-8) must not block everyone else.
Picked: a robust, process-shared mutex. Only the master writes under the lock, workers lock only to copy the table.
If a worker dies holding the lock it was only reading, so the next one gets EOWNERDEAD, calls pthread_mutex_consistent() and goes on. ENOTRECOVERABLE means a bug: log it and exit.

## 8. Cache line alignment
Two backends on the same 64 byte cache line cause false sharing between cores.
Picked: _Alignas(64) on struct lb_backend now. It costs a few KiB of padding. In D8 we can measure with and without it.

## 9. Who deletes the shm region
Picked: the master unlinks it on a clean shutdown, and at start it removes an old region if no master is running.
So after a crash you just start it again. How we detect a stale region we decide in C2.

## 10. Worker copy of the table
Picked: when generation changes, the worker copies name, addr, weight and in_use under the lock. health_state, admin_state and the counters are atomic and read live.
No lock on the hot path and no half written address.

## 11. Per-worker stats
Picked: one stats slot per worker index, plus counters per backend.
Each worker writes only its own slot. The master resets the slot when it restarts that worker.

## 12. Lock-free atomics
Atomics work between processes only if they are lock-free.
Picked: static_assert(ATOMIC_INT_LOCK_FREE == 2) and static_assert(ATOMIC_LONG_LOCK_FREE == 2). If they are not lock-free, the build fails.

## 13. Type of in_use
Picked: a plain uint32_t, used only under the lock (same as decision 10). One sync rule per field, not two.

## 14. How the master starts a worker
The master does fork + exec of lb for every worker (C3).
Picked: argv:

    lb --worker <index> --listen <ip:port> --shm <name> --connect-timeout <ms> --idle-timeout <ms>

You can see it with ps -o args and start a worker by hand. The worker does not read lb.conf, so a restarted worker can't get a newer config than the others. Timeouts are read once at start. Reload changes only backends, weights and the algorithm.
