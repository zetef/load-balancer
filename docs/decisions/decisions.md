# Design decisions (S2)

Stefan (@zetef) and David (@NeluCeapa16) agreed on these on 2026-11-05.
Questions 1 to 13 are the open questions from the shm contract. Question 14 is about how the master starts a worker.

For each one we wrote what the problem was, what we could do, what we picked and what it means for the code.

## 1. Maximum number of backends

The shared memory region is created once with ftruncate and never changes size, so the backend table has to be a fixed array.
We thought about 16, 64 or 256 and picked 64 (LB_MAX_BACKENDS = 64).
The region stays small, a few tens of KiB. If the config has more than 64 backends the parser rejects it and says what the limit is. If we want more later we have to change the layout and bump the version.

## 2. What happens to slots on reload

On reload backends can be added or removed. But workers still have open connections, and when those close they decrement active_conns of the slot they used.
We could either compact the array (indexes move around) or give each slot an in_use flag so a backend keeps the same index.
We picked the in_use flag. A backend keeps its slot for its whole life.
This way a connection that is still open always decrements the right counter. A removed slot can be reused only after its active_conns gets to 0. The table can have holes, so every loop has to skip slots where in_use is 0.

## 3. Round-robin cursor

Round-robin needs to remember which backend is next.
We could have one global atomic cursor in shm, or one cursor per worker in its own memory.
We picked one cursor per worker, so rr_cursor is gone from shm.
Workers don't fight over the same cache line on every connection. Inside one worker the rotation is exact. Across workers it is only roughly even, but with many connections it evens out.

## 4. Weights

The config lets you set a weight for each backend.
We could make all algorithms use it, or only round-robin for the first version.
We picked only round-robin (weighted round-robin).
Least-connections and IP hash ignore the weight. We say this in lb.conf.example. The field is already in the struct, so adding it to the other algorithms later does not change the layout.

## 5. Admin state and health state

A backend can be DOWN because of health checks and DISABLED by the admin at the same time. After lbctl enable it has to show its real health, not just UP (FR-6).
We could keep one state field with priority rules, or have two fields and combine them when picking a backend.
We picked two atomic fields: health_state (UP or DOWN, written by the health checker) and admin_state (ENABLED, DISABLED or DRAIN, written by the admin socket).
A backend can be picked only if admin_state is ENABLED and health_state is UP. Each field has only one writer, so the health checker and lbctl never overwrite each other.

## 6. IPv4 or IPv6

The requirements only ask for IPv4 (requirements.md section 1).
We could use struct sockaddr_in, or struct sockaddr_storage to be ready for IPv6.
We picked sockaddr_in, IPv4 only.
The struct is smaller and the code is simpler. Adding IPv6 later means changing the layout and bumping the version.

## 7. Mutex and a process that dies holding it

The structural fields are protected by a mutex in shm. FR-8 tests kill -9 on a worker, and if a worker dies while holding a normal mutex, everyone else waits forever.
We could use a robust process-shared mutex, or make sure workers never lock at all (seqlock or two copies of the table).
We picked the robust mutex (PTHREAD_PROCESS_SHARED and PTHREAD_MUTEX_ROBUST). Only the master writes under the lock. Workers lock only to copy the table when generation changes.
If a worker dies while holding the lock, it was only reading, so the data is never half written. Whoever locks next (master or worker) gets EOWNERDEAD, calls pthread_mutex_consistent() and goes on, there is nothing to fix. If the master dies holding the lock, the whole load balancer is dead anyway. ENOTRECOVERABLE means we have a bug (someone forgot to call consistent), so we log it and exit.

## 8. Cache line alignment

Workers on different cores update counters of backends that sit next to each other. If two backends share a 64 byte cache line, that line keeps jumping between cores (false sharing) and things get slower.
We could add _Alignas(64) to struct lb_backend now, or wait for the D8 measurements.
We picked to align it now.
It wastes some padding, at most 64 bytes per slot, a few KiB in total. In D8 we can still measure with and without it.

## 9. Who deletes the shm region

If the master crashes or gets kill -9, /dev/shm/lb_shm is left behind.
We could call shm_unlink only on a clean shutdown, or also clean up an old region at startup.
We picked both. The master unlinks on a clean shutdown, and at startup it removes an old region from a previous run if no master is running.
So after a crash you can just start it again without deleting anything by hand. How exactly we check that the old region is stale (admin socket, or a PID saved in the region) we decide in C2.

## 10. Worker copy of the table

Workers need the backend table on every new connection.
We could copy the table under the lock when generation changes, or read the live fields every time we pick a backend.
We picked the copy. When generation changes, the worker copies name, addr, weight and in_use under the lock. health_state, admin_state and the counters are atomic, so they are read live at every pick.
There is no lock on the hot path and a worker never sees a half written address. Health and admin changes work from the next pick, without bumping generation.

## 11. Per-worker stats

lbctl show workers (C6) prints numbers for each worker.
We could have a stats slot for each worker in shm, or only the global atomic counters.
We picked one stats slot per worker index, plus the counters per backend.
Each worker writes only its own slot, so there is only one writer. When the master restarts a worker, the slot for that index is reset.

## 12. Checking that atomics are lock-free

Atomics only work between processes if they are lock-free. If not, the compiler can use a hidden lock that is private to each process and protects nothing.
We could check with static_assert at compile time, or only at runtime.
We picked static_assert(ATOMIC_INT_LOCK_FREE == 2) and static_assert(ATOMIC_LONG_LOCK_FREE == 2).
On a platform where this can't work the build fails, instead of the program breaking quietly at runtime.

## 13. Type of in_use

in_use was first proposed as _Atomic, but the contract also said it is written under the lock.
We could keep it atomic, or make it a normal field that is only read and written under the lock.
We picked a normal uint32_t, only used under the lock.
This matches decision 10, because workers read it only while copying the table under the lock. Each field has one sync rule, not two.

## 14. How the master starts a worker

The master does fork + exec of the same lb binary for every worker (C3). The worker needs its index, the listen address, the shm name and the connect and idle timeouts (D6). The worker main parses these (D5).
We could pass them as argv, as environment variables, or pass only the config path and let the worker read lb.conf again.
We picked argv:

    lb --worker <index> --listen <ip:port> --shm <name> --connect-timeout <ms> --idle-timeout <ms>

You can see the arguments with ps -o args, and you can start a worker by hand without the master. We didn't want the worker to read lb.conf itself, because a restarted worker could read a newer lb.conf than the workers that are already running. The timeouts are read once at start, so SIGHUP does not change them for running workers. Reload only changes backends, weights and the algorithm.
