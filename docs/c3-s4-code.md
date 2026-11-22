# C3 (master supervisor) și S4 (backend-uri de test): ce s-a adăugat

Documentul explică fiecare fișier, folder și funcție adăugate sau modificate pentru P4 (2026-11-23), plus comenzile cu care se demonstrează fiecare checkpoint. Codul pleacă de la branch-ul `c1-c2-shm` (C1 și C2 gata).

## 1. Imaginea de ansamblu

```
lb (master)
 |-- citește lb.conf (C1)              src/master/config.c
 |-- creează /dev/shm/lb_shm (C2)       src/common/shm.c
 |-- blochează SIGINT/TERM/HUP/CHLD
 |-- supervisor (C3)                    src/master/supervisor.c
 |     fork + exec x N  ->  stub_worker --worker i --listen ... --shm /lb_shm ...
 |     waitpid la SIGCHLD, respawn cu backoff, crash-loop
 |-- bucla: sigtimedwait(semnale, timeout = următorul restart)
 '-- la SIGTERM: oprește workerii (SIGTERM, deadline 5 s, SIGKILL), șterge regiunea

backend 1/2/3 (S4)                      tests/backends/backend.c
     server HTTP iterativ, răspunde "backend <id>"
```

Până când Stefan termină workerul real (D5), master-ul pornește `stub_worker`. Când apare workerul, se schimbă doar binarul executat (`--worker-bin` sau `/proc/self/exe`). Argv-ul rămâne cel din decizia 14.

## 2. Fișiere noi și modificate

| Fișier | Stare | Rol |
|---|---|---|
| `include/supervisor.h` | nou | Constantele de backoff, `struct lb_worker_slot`, `struct lb_supervisor`, API-ul supervisorului |
| `src/master/supervisor.c` | nou | fork + exec, waitpid, decodarea statusului, backoff, crash-loop, oprire |
| `src/master/main.c` | modificat | Pornește supervisorul, bucla cu `sigtimedwait`, opțiunile `--worker-bin`, `--worker-extra`, `--fd-audit[-leak]` |
| `tools/stub_worker.c` | modificat | Acceptă argv-ul din decizia 14, plus `--crash-after`, `--exit-after`, `--ignore-term` |
| `tests/unit/test_supervisor.c` | nou | Unit test pentru backoff, crash-loop, fork + exec real |
| `tests/backends/backend.c` | nou | Backend-ul HTTP de test (S4) |
| `tests/backends/README.md` | nou | Rețeta de test manual (S4 pasul 7) |
| `Makefile` | modificat | `all` construiește și `backends` |
| `docs/c3-s4-code.md` | nou | Documentul acesta |

`lb_shm.h` **nu** s-a schimbat, deci contractul cu Stefan rămâne același. Supervisorul scrie doar câmpurile pe care contractul i le dă master-ului: `workers[i].pid` și `restarts`. Tot el resetează `active_conns` și `total_conns` ale slotului înainte de respawn (shm-contract, secțiunea 5).

## 3. Folderele

| Folder | Ce conține |
|---|---|
| `include/` | Header-ele comune: `lb_shm.h` (contractul), `config.h`, `shm.h`, `net.h`, iar acum și `supervisor.h` |
| `src/common/` | Cod folosit de master și de workeri, compilat în `build/liblbcore.a` (`shm.c`, `net.c`) |
| `src/master/` | Codul master-ului: `main.c`, `config.c`, `supervisor.c`. Tot ce nu e `main.c` e legat și în unit teste |
| `src/worker/` | Gol încă: workerul real e al lui Stefan (D5) |
| `tools/` | `stub_worker.c` (workerul de test) și `lbctl.c` (placeholder pentru C6) |
| `tests/unit/` | Unit teste, fiecare e un program care iese cu 0 la succes. Le rulează `make test` |
| `tests/backends/` | Programe de test pentru trafic: `echo_server`/`echo_client` (D1) și `backend` (S4) |

## 4. `include/supervisor.h`

**Constantele** (alese de noi, de explicat la oral):

| Constantă | Valoare | Înseamnă |
|---|---|---|
| `LB_SUP_BASE_MS` | 100 | prima întârziere de restart |
| `LB_SUP_MAX_MS` | 10000 | plafonul întârzierii |
| `LB_SUP_STABLE_MS` | 30000 | un worker care a mers 30 s e „stabil”, iar backoff-ul revine la 100 ms |
| `LB_SUP_LOOP_WINDOW_MS` | 60000 | fereastra de crash-loop |
| `LB_SUP_LOOP_LIMIT` | 10 | 10 ieșiri în 60 s înseamnă că slotul nu mai e repornit |

Formula este `delay = min(BASE * 2^k, MAX)`, unde `k` = numărul de crash-uri rapide consecutive. Rezultă întârzierile 100, 200, 400, 800, 1600, 3200, 6400, 10000, 10000 ms.

**Politica de crash-loop** (decisă aici): slotul care intră în crash-loop e abandonat cu un mesaj în log, iar ceilalți workeri continuă să servească. Dacă **toate** sloturile au abandonat, master-ul iese cu `EXIT_FAILURE`, ca systemd să aplice politica lui de restart.

**`enum lb_slot_state`**: starea unui slot.
- `LB_SLOT_EMPTY`: nu rulează (nu a fost pornit sau a fost oprit la cerere).
- `LB_SLOT_RUNNING`: are un pid valid.
- `LB_SLOT_WAITING`: workerul a murit și așteaptă `restart_at_ms`.
- `LB_SLOT_GAVE_UP`: slotul e în crash-loop și nu mai e repornit.

**`struct lb_worker_slot`**: un slot per worker, indexat după numărul workerului.
- `pid`: pid-ul curent, 0 dacă slotul nu rulează.
- `state`: una din stările de mai sus.
- `started_ms`: momentul fork-ului. Din el se calculează cât a mers workerul.
- `restart_at_ms`: când trebuie repornit (doar în starea `WAITING`).
- `backoff_k`: exponentul din formulă.
- `loop_count`, `loop_start_ms`: numărătoarea și începutul ferestrei de crash-loop.
- `restarts`: de câte ori a fost repornit (copiat în shm pentru `lbctl show workers`).
- `last_status`: statusul brut primit de la `waitpid`.

**`struct lb_supervisor`**: tot ce ține supervisorul.
- `slots[]` și `n`: sloturile și câte sunt folosite (`cfg->workers`).
- `master_pid`: pid-ul master-ului, cu care copilul își verifică părintele după `prctl`.
- `child_mask`: masca de semnale pe care copilul o restaurează înainte de exec.
- `shm`: regiunea, sau NULL în teste.
- `stopping`: 1 în timpul opririi, când ieșirile nu se mai numără drept crash-uri.
- `bin`, `listen`, `shm_name`, `connect_ms`, `idle_ms`, `extra[]`: **stringurile din argv, construite o singură dată, în părinte**, pentru că după fork nu avem voie la `snprintf`.

Toate timpurile sunt `CLOCK_MONOTONIC`, în milisecunde. Ceasul monoton nu sare când se schimbă ora sistemului, așa că o întârziere nu poate deveni negativă sau uriașă.

## 5. `src/master/supervisor.c`, funcție cu funcție

| Funcție | Ce face | De reținut la oral |
|---|---|---|
| `lb_now_ms()` | `clock_gettime(CLOCK_MONOTONIC)`, convertit în ms | monoton, nu `time()` |
| `supervisor_backoff_ms(k)` | `min(100 * 2^k, 10000)` | dublează pas cu pas, ca un `k` mare să nu facă overflow |
| `copy_arg()` | `snprintf` care raportează trunchierea | argumentele au maxim 63 de caractere |
| `supervisor_init()` | Pune structura pe zero, salvează `getpid()`, masca și shm-ul, construiește `listen` (`ip:port`), timeout-urile ca text și împarte `--worker-extra` în cuvinte (`strtok_r`) | totul se pregătește înainte de orice fork |
| `supervisor_spawn(slot)` | Construiește `argv[]` (decizia 14 + extra), resetează contoarele slotului în shm, apoi `fork()`. **În copil**: `sigprocmask(SIG_SETMASK, &child_mask)`, `prctl(PR_SET_PDEATHSIG, SIGTERM)`, verificarea `getppid() == master_pid`, `execv`, iar dacă exec eșuează, `write(2)` + `_exit(127)`. **În părinte**: salvează pid-ul și `started_ms`, scrie `pid` și `restarts` în shm. Dacă fork eșuează (`EAGAIN`), programează o reîncercare | în copil rulează doar funcții async-signal-safe; `_exit` în loc de `exit`, ca bufferele stdio să nu fie golite de două ori |
| `supervisor_start_all()` | `spawn` pentru fiecare slot | |
| `log_exit()` | Decodează statusul: `WIFEXITED` + `WEXITSTATUS` sau `WIFSIGNALED` + `WTERMSIG` (+ `WCOREDUMP`) | 139 în shell = 128 + 11 (SIGSEGV); `WEXITSTATUS` nu are sens aici, procesul nu a ieșit normal |
| `supervisor_on_exit(slot, status, now)` | Contabilitatea după o ieșire: log, `pid = 0`. Dacă oprirea e în curs, slotul devine `EMPTY`. Altfel: dacă workerul a mers mai mult de 30 s, `k = 0`; actualizează fereastra și numărătoarea de crash-loop; la 10 ieșiri trece în `GAVE_UP`, altfel în `WAITING` cu `restart_at = now + backoff(k)` și `k++` | primește `now` ca parametru, ca testul să poată folosi un timp fals |
| `find_slot(pid)` | Caută liniar slotul cu pid-ul dat | N <= 64, deci căutarea liniară ajunge |
| `supervisor_reap()` | `waitpid(-1, WNOHANG)` **în buclă**: `> 0` = un copil cules, `0` = mai sunt copii dar niciunul n-a ieșit, `-1/ECHILD` = nu mai e niciun copil, `-1/EINTR` = reia | semnalele nu se adună: un singur SIGCHLD poate însemna mai mulți copii morți |
| `supervisor_tick()` | Repornește sloturile `WAITING` al căror `restart_at` a trecut (`restarts++`) | |
| `supervisor_next_timeout_ms()` | Cât mai e până la cel mai apropiat restart, sau -1 dacă nu e niciunul | master-ul așteaptă exact atât, fără `sleep` și fără polling |
| `supervisor_running()` / `supervisor_all_gave_up()` | Numără sloturile care rulează / verifică dacă toate au abandonat | |
| `kill_running(sig)` | `kill` la toți workerii vii, cu `ESRCH` ignorat (workerul a murit între timp) | |
| `nap_ms()` | `nanosleep`, reluat cu timpul rămas la `EINTR` | |
| `supervisor_stop_all(deadline)` | `stopping = 1`, SIGTERM la toți, culege workerii până la deadline (polling la 20 ms), apoi SIGKILL pentru cei rămași și culege tot | C4 face din asta oprirea grațioasă completă (drain); aici e doar varianta minimă |

## 6. `src/master/main.c`, ce s-a schimbat

- **Opțiuni noi**:
  - `--worker-bin <path>`: programul executat pentru fiecare worker. Implicit e `stub_worker` din directorul lui `lb`, găsit cu `readlink("/proc/self/exe")`.
  - `--worker-extra "<cuvinte>"`: cuvinte adăugate la argv-ul workerilor, de exemplu `"--crash-after 0"` pentru testul de crash-loop.
  - `--fd-audit` și `--fd-audit-leak`: deschid fd-urile din pasul 5 (vezi mai jos).
- **`block_signals()`**: blochează SIGINT, SIGTERM, SIGHUP și SIGCHLD înainte de orice altceva și păstrează masca originală în `orig`. `orig` e masca pe care o primesc copiii.
- **Bucla principală**: `sigtimedwait(&set, &info, timeout)`, cu timeout-ul dat de `supervisor_next_timeout_ms()`. Dacă nu e niciun restart programat, folosește `sigwaitinfo` și așteaptă oricât. Semnalele blocate sunt citite sincron, așa că nu există handlere și nu există fereastră în care un semnal se pierde. C4 înlocuiește apelul cu `signalfd` + `epoll`/`poll`. Ce înseamnă fiecare rezultat:
  - `EAGAIN`: a expirat timeout-ul, deci a venit ora unui restart. Se apelează `supervisor_tick()`.
  - `SIGCHLD`: se apelează `supervisor_reap()`.
  - `SIGHUP`: `lb_shm_apply_config()`, ca în C2.
  - `SIGINT` sau `SIGTERM`: se iese din buclă.
- **`access(bin, X_OK)`** la start: un binar lipsă dă imediat un mesaj clar, nu 10 ieșiri cu 127 urmate de crash-loop.
- **`open_audit_fds()`** (C3 pasul 5): deschide config-ul cu `O_CLOEXEC`, un socket Unix cu `SOCK_CLOEXEC` (în locul socketului admin din C6) și un `timerfd` cu `TFD_CLOEXEC` (în locul timerului din C4). Cu `--fd-audit-leak`, socketul e deschis **fără** `SOCK_CLOEXEC`, ca să se vadă cum ajunge în workeri.
- **Oprirea**: `supervisor_stop_all(5000)`, închide fd-urile de audit, `lb_shm_destroy()`.

## 7. `tools/stub_worker.c`, ce s-a schimbat

- Acceptă argv-ul din decizia 14: `--worker <i> --listen <ip:port> --shm <nume> --connect-timeout <ms> --idle-timeout <ms>`. `--listen` și timeout-urile sunt acceptate, dar nu folosite: stub-ul nu ascultă pe niciun port.
- La start afișează `pid`, `ppid` și argv-ul primit.
- `--crash-after <s>`: după s secunde apelează `crash_now()`, adică `signal(SIGSEGV, SIG_DFL)` + `raise(SIGSEGV)`. Am ales un SIGSEGV adevărat în locul lui `*(int *)0 = 0`. Dereferențierea NULL e comportament nedefinit: compilatorul o poate elimina, iar ASan/UBSan o prind și ies cu codul lor.
- `--exit-after <s> <cod>`: după s secunde iese cu codul dat.
- `--ignore-term`: SIGTERM e ignorat. E pentru testul de deadline (master-ul trebuie să dea SIGKILL) și pentru a vedea orfanii (PDEATHSIG trimite SIGTERM, care e ignorat).
- Pornit de master (cu `--worker`), tabela se afișează doar la start și la schimbarea lui `generation`, cu prefixul `stub[i]:`. Pornit de mână, se afișează în fiecare secundă, ca în C2.
- `arg_long()`: parsează un număr cu interval și oprește programul cu `usage` la o valoare greșită. `elapsed_s()`: secundele trecute de la start, pe ceasul monoton.

## 8. `tests/unit/test_supervisor.c`

| Test | Verifică |
|---|---|
| `test_backoff` | 100, 200, 6400, apoi plafonul de 10000; fără overflow la `k = 1000` |
| `test_init_extra` | `--worker-extra` e împărțit în cuvinte; mai mult de 8 cuvinte e refuzat |
| `test_crash_loop` | cu timp fals: întârzierile se dublează, iar la a 10-a ieșire slotul trece în `GAVE_UP` |
| `test_stable_resets` | după 30 s de rulare backoff-ul revine la 100 ms; după 60 s crash-urile vechi ies din fereastră |
| `test_stopping` | o ieșire în timpul opririi nu e numărată drept crash |
| `test_real_spawn` | fork + exec real pe `/bin/false` (exit 1 decodat, restart în <= 100 ms); exec pe un program inexistent dă 127; `reap` fără copii (ECHILD) nu strică nimic |

`status_of_exit()` obține statusul real al unui copil care iese cu un cod dat, ca testul să nu depindă de felul în care kernelul codifică statusul.

## 9. `tests/backends/backend.c` (S4)

Rulează ca `backend <id> <port> [--delay ms] [--bind ip]`. E POSIX simplu (`_POSIX_C_SOURCE 200809L`) și nu depinde de `liblbcore`, pentru că D10 îl compilează singur pe Alpine (musl).

| Funcție | Ce face |
|---|---|
| `on_stop()` | Handler-ul pentru SIGINT/SIGTERM: setează `stop = 1` (variabilă `volatile sig_atomic_t`) |
| `parse_num()` | `strtol` cu verificare de interval |
| `write_all()` | Bucla de `write` până trec toți octeții, reluată la `EINTR` |
| `read_request()` | Citește până la linia goală (`\r\n\r\n` sau `\n\n`), EOF sau buffer plin (4096). Cererea poate veni în mai multe bucăți, așa că caută în tot ce s-a adunat |
| `sleep_ms()` | `nanosleep` pentru `--delay`, reluat cu timpul rămas |
| `serve()` | `SO_RCVTIMEO` de 5 s (un client tăcut nu blochează serverul iterativ), citește cererea, așteaptă `--delay`, scrie `HTTP/1.0 200 OK` + `Content-Length` + `backend <id>` |
| `main()` | Parsează argumentele, ignoră SIGPIPE, instalează handlerele **fără SA_RESTART** (ca `accept` blocat să se întoarcă cu EINTR), apoi `socket`, `SO_REUSEADDR`, `bind`, `listen` și bucla `accept`, `serve`, `close` |

Politica de erori: o eroare într-o conexiune duce doar la `close` și trece la următorul `accept`, nu oprește serverul. `accept` eșuat cu `EMFILE` așteaptă 100 ms, ca să nu intre în buclă strânsă. La SIGINT închide socketul de ascultare și iese cu 0.

## 10. Comenzile pentru checkpoint-uri

Rulează din rădăcina repo-ului, după `make`. Pentru testul de crash-loop, `ulimit -c 0` oprește core dump-urile (altfel apar 10 core-uri în `coredumpctl`).

### C3

```bash
# 1. stub: 139 (128 + SIGSEGV 11) și 3
./stub_worker --worker 0 --crash-after 0; echo $?
./stub_worker --worker 0 --exit-after 0 3; echo $?

# 2. N workeri cu argv-ul agreat
./lb conf/lb.conf.example &
ps -o pid,ppid,stat,args --ppid $(pgrep -x lb)
kill %1
strace -f -o /tmp/st.txt ./lb conf/lb.conf.example & sleep 1; kill $(pgrep -x lb); wait
grep -E 'clone|execve|prctl|rt_sigprocmask' /tmp/st.txt | head   # în copil: doar sigprocmask, prctl, getppid, execve

# 3 + 4. reap și respawn
./lb conf/lb.conf.example &
kill -9 $(pgrep -P $(pgrep -x lb) | head -1)                  # o linie "killed by signal 9", restart în 100 ms
kill -9 $(pgrep -P $(pgrep -x lb) | head -2)                  # două linii, câte una per worker
ps -o pid,stat --ppid $(pgrep -x lb)                          # niciun Z/<defunct>
kill %1

ulimit -c 0
time ./lb --worker-extra "--crash-after 0" conf/lb.conf.example
# 100, 200, ..., 10000 ms, apoi "crash loop ... giving up"; user+sys aproape 0

# 5. fd audit
./lb --fd-audit conf/lb.conf.example &
ls -l /proc/$(pgrep -P $(pgrep -x lb) | head -1)/fd           # doar 0, 1, 2
kill %1
./lb --fd-audit-leak conf/lb.conf.example &
ls -l /proc/$(pgrep -P $(pgrep -x lb) | head -1)/fd           # apare socket:[...] pe fd 4
kill %1

# 6. orfani
./lb --worker-extra "--ignore-term" conf/lb.conf.example &
W=$(pgrep -P $(pgrep -x lb) | paste -sd,); kill -9 %1
ps -o pid,ppid,args -p $W                                     # PPID = 1 sau systemd --user (subreaper)
kill -9 ${W//,/ }; rm -f /dev/shm/lb_shm
./lb conf/lb.conf.example &
kill -9 %1; sleep 0.5; pgrep stub_worker || echo "PDEATHSIG: workerii au ieșit"
```

### S4

Vezi [tests/backends/README.md](../tests/backends/README.md).

## 11. Ce rămâne pentru după P4

- C4 înlocuiește `sigtimedwait` cu `signalfd` și un `timerfd` (fd-ul din `--fd-audit` devine cel real) și face oprirea grațioasă completă.
- D5 (Stefan): workerul real. Master-ul îl pornește cu `--worker-bin` sau, după ce `lb --worker` există, cu `/proc/self/exe`.
- Opțiunile `--worker-extra` și `--fd-audit` sunt pentru teste. Pot rămâne, pentru că nu schimbă nimic dacă nu sunt date.
