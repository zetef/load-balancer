# Backend-uri de test și rețeta de test manual (S4)

Toate comenzile se rulează din rădăcina repo-ului, după `make`.

## Programe

| Program | Ce face |
|---|---|
| `build/tests/backends/backend <id> <port> [--delay ms] [--bind ip]` | Server HTTP iterativ: la orice cerere răspunde `backend <id>`. Implicit ascultă pe 127.0.0.1. |
| `build/tests/backends/echo_server <port>` | Server echo TCP (D1, Stefan). |
| `build/tests/backends/echo_client <host> <port>` | Client pentru echo_server (D1). |

## 1. Pornește trei backend-uri

Porturile sunt cele din `conf/lb.conf.example` (9001, 9002), plus un al treilea, lent:

```bash
build/tests/backends/backend 1 9001 &
build/tests/backends/backend 2 9002 &
build/tests/backends/backend 3 9003 --delay 300 &
```

## 2. Verifică fiecare backend

```bash
curl -s http://127.0.0.1:9001/                # backend 1
printf 'x\r\n\r\n' | nc -N 127.0.0.1 9002     # răspunsul HTTP întreg (openbsd-netcat: -N)
time curl -s http://127.0.0.1:9003/           # backend 3, real ~0.3 s
```

## 3. Bucla de 30 de cereri

Acum cererile merg direct la backend-uri. Din D3 încolo, înlocuiește portul cu cel al LB-ului (8080), iar numărătoarea arată algoritmul.

```bash
for i in $(seq 30); do curl -s http://127.0.0.1:900$(( i % 3 + 1 ))/; done | sort | uniq -c
#   10 backend 1
#   10 backend 2
#   10 backend 3
```

## 4. Socket-urile cu `ss`

```bash
ss -tlnp | grep 900          # LISTEN: câte un socket de ascultare per backend
ss -tan state time-wait | grep 900   # TIME-WAIT: rămân pe partea care a închis prima (backend-ul)
```

## 5. Simulează căderea unui backend

```bash
kill %2                      # oprește backend 2 (SIGTERM: închide curat)
curl -s http://127.0.0.1:9002/ || echo "refused"   # conexiune refuzată: nimeni nu mai ascultă
build/tests/backends/backend 2 9002 &   # repornire imediată pe același port (SO_REUSEADDR)
```

`kill -STOP %2` simulează un backend blocat: portul încă acceptă conexiuni (kernelul face handshake-ul), dar nu mai răspunde nimeni. Revii cu `kill -CONT %2`. Vezi C5.

## 6. Robustețe

```bash
valgrind --leak-check=full --track-fds=yes build/tests/backends/backend 1 9001 &
for i in $(seq 100); do curl -s http://127.0.0.1:9001/ >/dev/null; done
printf 'GET / HTTP/1.0\r\n' | timeout 0.3 nc 127.0.0.1 9001   # client care pleacă devreme
curl -s http://127.0.0.1:9001/                                 # backend-ul încă răspunde
kill -INT %1   # valgrind: "FILE DESCRIPTORS: 3 open (3 inherited)", "0 errors"
```

## Oprire

```bash
kill %1 %2 %3
```
