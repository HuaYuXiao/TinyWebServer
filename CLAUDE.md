# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build

```bash
# CMake (hiredis fetched automatically via FetchContent)
mkdir -p build && cd build && cmake .. && cmake --build .
```

The project requires C++20, thread, and `libmysqlclient`. CI runs on Ubuntu via `.github/workflows/cmake-single-platform.yml`.

## Architecture

This is a high-concurrency HTTP server built on **epoll (LT mode)** using a **simulated Proactor** pattern — the main thread performs I/O (reads and writes) and dispatches completed requests to a worker thread pool.

### Startup flow (`main.cpp` → `webserver.cpp`)

1. `Config` parses CLI flags (`-p` port, `-s` SQL pool size, `-t` thread count, `-r` Redis pool size; defaults: 9006, 100, 64, 16)
2. `WebServer::init()` stores config, then `sql_pool()` / `thread_pool()` / `redis_pool()` / `eventListen()` / `eventLoop()` are called in order

### Event loop (`webserver.cpp:244`)

`epoll_wait` blocks indefinitely, handling these event types in priority order:
- **Listen fd**: accept new connection, create timer
- **Client error** (EPOLLRDHUP/EPOLLHUP/EPOLLERR): remove timer
- **Pipe fd** (unified signal source): SIGALRM → mark timeout; SIGTERM → stop server
- **EPOLLIN**: `dealwithread()` — main thread calls `read_once()`, then pushes the `http_conn*` to thread pool
- **EPOLLOUT**: `dealwithwrite()` — main thread calls `write()` (writev-based scatter/gather)

After each `epoll_wait` iteration, if SIGALRM fired, `timer_handler()` prunes expired timers.

### Signal handling — unified event source (`webserver.cpp:89-109`)

Signals (SIGALRM, SIGTERM) are converted to epoll events via a `socketpair` pipe. The signal handler writes the signal number to the pipe's write end; the event loop reads it from the pipe's read end. This avoids async-signal-safety issues.

### HTTP processing (`http/http_conn.cpp`)

State machine with three phases: `CHECK_STATE_REQUESTLINE` → `CHECK_STATE_HEADER` → `CHECK_STATE_CONTENT`. Each call to `process_read()` advances through `parse_line()` which converts CRLF to null terminators in the read buffer.

- **GET**: serves static files from `doc_root` via `mmap`
- **POST to `/4`**: CGI path — parses `name` and `id_card` from the URL-encoded body, queries MySQL for student records (joined with scores), returns JSON
- Uses `EPOLLONESHOT` on all client fds; after processing, `modfd()` re-arms the fd for the next event

Response is built with `process_write()` which assembles status line, headers, and body into `iovec` scatter/gather buffers for `writev`.

### Thread pool (`threadpool/threadpool.h`)

Template class instantiated with `http_conn`. Uses `std::counting_semaphore` (max 100000) for work notification and a `std::deque` under a mutex for the work queue. Each worker acquires a DB connection via `connectionRAII` before calling `request->process()`. Threads are created via `std::thread` lambdas.

### Timer (`timer/lst_timer.cpp`)

Uses `std::set<util_timer*, timer_cmp>` ordered by `expire` time (with pointer-address tiebreaker). `SIGALRM` fires every `TIMESLOT` (5s); `tick()` pops expired timers from the front. Connections expire 3×TIMESLOT (15s) after last activity. On each read/write, `adjust_timer()` removes and re-inserts the timer with a new expire time.

### MySQL connection pool (`mysql/sql_connection_pool.cpp`)

Singleton (`GetInstance()` via local static). Protected by `std::counting_semaphore` for blocking acquire and `std::mutex` for deque access. `connectionRAII` acquires on construction and releases on destruction — workers use it to get a connection for the duration of `process()`.

### Redis caching layer (`redis/`)

**Connection pool** (`redis_connection_pool.cpp`): Same singleton + RAII pattern as MySQL pool. Uses `std::counting_semaphore` for blocking acquire. Health-checks connections with PING every 60s; automatically reconnects on failure. If initialization fails, the server degrades gracefully to MySQL-only mode.

**Cache** (`redis_cache.h/.cpp`): Singleton wrapping hiredis with three-tier protection:
1. **Bloom filter** (cache penetration): `BloomFilter` (FNV-1a + std::hash dual-hash, default 1M elements @ 1% FP rate) rejects keys known not to exist, avoiding DB hits for malicious queries.
2. **Circuit breaker** (fault tolerance): `CircuitBreaker` (3-state: CLOSED→OPEN→HALF_OPEN, 5 consecutive failures → 30s cooldown → probe) skips Redis and falls back to direct DB queries when Redis is unhealthy.
3. **Distributed mutex lock** (hotspot invalidation): `SETNX`-based lock with double-check prevents thundering-herd cache rebuilds. Retries up to 5 times (100ms apart) before falling back to DB.

TTL randomization (±10% jitter) prevents avalanche expiration. Null values are cached with a short TTL (60s) to prevent penetration. `ExamScoreHandler` (`exam_score_handler.h`) shows the integration pattern for wrapping a DB-backed query with cache.

## Key hardcoded values

- DB host: `192.168.19.1:3306` (in `webserver.cpp:43`)
- Document root: `/home/user/TinyWebServer/root` (in `webserver.cpp:8-9`; allocated via `malloc`, never freed — intentional, lives for process lifetime)
- DB credentials: `user` / `123456` / database `server` (in `main.cpp:5-7`)
- Redis: `127.0.0.1:6379`, no password, pool size 16, db index 0 (in `webserver.cpp:15-20` and `config.cpp:14-18`)
- Listen backlog: 4096
- Max connections: `MAX_FD = 65536`

## Stress testing

```bash
go run pressureTest.go -c 10000 -t 60 -u http://localhost:9006/4
```

Perf flamegraph (commands in README.md).

## C++ conventions

- Standard: C++20 (`CMAKE_CXX_STANDARD 20`)
- Smart pointers preferred (`std::unique_ptr`, `std::make_unique`) for ownership of arrays and objects
- RAII for resource management (connections, mmap regions)
- `std::counting_semaphore` (C++20) for thread synchronization instead of POSIX semaphores
