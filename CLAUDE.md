# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build

```bash
mkdir -p build && cd build && cmake .. && cmake --build . -j$(nproc)
```

The project requires C++20, pthreads, `libmysqlclient`, `libhiredis`, and `libcrypto` (OpenSSL). Libraries found via `find_library`/`find_package` in CMakeLists.txt (no FetchContent). CI runs on Ubuntu 24.04 via `.github/workflows/cmake-single-platform.yml` (Release build).

CMake produces one target: `server`.

## Architecture

This is a high-concurrency HTTP server built on **epoll (LT mode)** using a **simulated Proactor** pattern — the main thread performs I/O (reads and writes) and dispatches completed requests to a worker thread pool.

### Startup flow (`main.cpp` → `webserver.cpp`)

1. `Config` parses CLI flags (`-p` port, `-s` SQL pool size, `-t` thread count, `-r` Redis pool size, `-a` auth enabled; defaults: 8080, 100, 64, 16, true)
2. `WebServer::init()` stores config (including setting `http_conn::s_auth_enabled`), then `init_mysql_pool()` / `init_thread_pool()` / `init_redis_pool()` / `eventListen()` / `eventLoop()` are called in order

### Event loop (`webserver.cpp:261`)

`epoll_wait` blocks indefinitely, handling these event types in priority order:
- **Listen fd**: accept new connections (loop until EAGAIN), then rate-limit check via `RateLimiter::allow_connection(ip)` — rejected connections receive RST via `SO_LINGER`
- **Client error** (EPOLLRDHUP/EPOLLHUP/EPOLLERR): remove timer
- **Pipe fd** (unified signal source): SIGALRM → mark timeout; SIGTERM → stop server
- **EPOLLIN**: `dealwithread()` — main thread calls `read_once()`, then pushes the `http_conn*` to thread pool
- **EPOLLOUT**: `dealwithwrite()` — main thread calls `write()` (writev-based scatter/gather)

After each `epoll_wait` iteration, if SIGALRM fired, `timer_handler()` prunes expired timers, then `RateLimiter::cleanup_idle()` removes rate-limit entries idle >2min.

### Signal handling — unified event source (`webserver.cpp:111-131`)

Signals (SIGALRM, SIGTERM) are converted to epoll events via a `socketpair` pipe. The signal handler writes the signal number to the pipe's write end; the event loop reads it from the pipe's read end. This avoids async-signal-safety issues.

### HTTP processing (`http/http_conn.cpp`)

State machine with three phases: `CHECK_STATE_REQUESTLINE` → `CHECK_STATE_HEADER` → `CHECK_STATE_CONTENT`. Each call to `process_read()` advances through `parse_line()` which converts CRLF to null terminators in the read buffer.

Methods parsed: GET, POST, PUT, DELETE (PUT/DELETE set `cgi=1` for body parsing).

**Request routing in `do_request()`:**

```
/auth/register   → handle_register()   [no auth]
/auth/login      → handle_login()      [no auth]
/api/*           → verify_token() + require_role("root") → handle_insert/update/delete
/4 (POST)        → verify_token() → exam score query    [auth required]
/static files    → mmap serve          [no auth, no DB]
```

When `s_auth_enabled == false` (via `-a 0`), `/auth/*` and `/api/*` return 403; `/4` and static files work without auth (legacy mode).

All client fds use **EPOLLONESHOT**; after processing, `modfd()` re-arms the fd for the next event. The listen fd does NOT use EPOLLONESHOT.

Response is built with `process_write()` which assembles status line, headers, and body into `iovec` scatter/gather buffers for `writev`. Static files served via `mmap` (zero-copy into writev). `TCP_CORK` coalesces response headers + body into fewer TCP segments.

### Auth module (`auth/`)

**`password.h`** — PBKDF2-HMAC-SHA256 hashing via OpenSSL EVP. Format: `$pbkdf2-sha256$<iter>$<salt_hex>$<hash_hex>`. 100K iterations, 16-byte salt, 32-byte hash. Uses `CRYPTO_memcmp` for constant-time comparison.

**`jwt.h`** — Lightweight HS256 JWT (no external deps, only OpenSSL HMAC). Token: `base64url(header).base64url(payload).base64url(signature)`. Payload: `{"sub":"user","role":"root","exp":...,"iat":...}`. Default TTL 24h.

JWT secret is generated at startup: reads `JWT_SECRET` env var if set, otherwise generates a random 256-bit hex key via `RAND_bytes`. Changing the secret invalidates all existing tokens — each restart does this by default.

### Logging (`log/`)

Header-only structured logging module (`app_log` namespace). `Logger` singleton with `Sink`-based output — `ConsoleSink` (color stderr) and `FileSink` (daily rotation + size-based rotation, default 100MB × 10 files). Six levels: TRACE/DEBUG/INFO/WARN/ERROR/FATAL.

Macros `LOG_INFO(fmt, ...)` etc. auto-capture `__FILE__`, `__LINE__`, `__func__`. Compile-time filtering via `#define LOG_ACTIVE_LEVEL 3` before including `log/log.h` — lower-level calls become `(void)0` (zero cost). Runtime filtering via `logger.set_level()`. Printf-style format strings (not fmtlib).

Initialized in `main.cpp`: console + file sink at `build/`, source root set to strip absolute paths in output. All `std::cout`/`std::cerr` in `webserver.cpp`, `redis_pool.cpp`, `redis_cache.cpp` replaced with `LOG_*` macros.

### Thread pool (`thread_pool/thread_pool.h`)

Template class instantiated with `http_conn`. Uses `std::condition_variable` for work notification and a `std::deque` under a mutex for the work queue (max 10000 requests, `append_p` rejects when full). Each worker acquires a DB connection via `connectionRAII` before calling `request->process()`. Threads are created via `std::thread` lambdas; `m_stop` flag + `notify_all` triggers graceful shutdown.

### Timer (`timer/lst_timer.cpp`)

Uses `std::set<util_timer*, timer_cmp>` ordered by `expire` time (with pointer-address tiebreaker). `SIGALRM` fires every `TIMESLOT` (5s); `tick()` pops expired timers from the front. Connections expire 3×TIMESLOT (15s) after last activity. On each read/write, `adjust_timer()` removes and re-inserts the timer (O(log n)).

### MySQL connection pool (`mysql/mysql_pool.cpp`)

Singleton (`GetInstance()` via local static). Protected by `std::counting_semaphore` for blocking acquire and `std::mutex` for deque access. `connectionRAII` acquires on construction and releases on destruction.

On `init()`, the first connection's SSL session is cached via `mysql_get_ssl_session_data()` and applied to subsequent connections via `MYSQL_OPT_SSL_SESSION_DATA` for TLS session resumption. Connect timeout is 3s, read/write timeout is 10s. On `GetConnection()`, `mysql_ping()` checks liveness every 60s and auto-reconnects if stale.

### Redis caching layer (`redis/`)

**Connection pool** (`redis_pool.cpp`): Same singleton + RAII pattern as MySQL pool. Health-checks with PING every 60s; auto-reconnects on failure. If initialization fails, `m_initialized` stays false — `GetConnection()` returns `nullptr` immediately, and the cache layer deactivates without crashing the server.

**Cache** (`redis_cache.h/.cpp`): Singleton wrapping hiredis with three-tier protection:
1. **Bloom filter** (cache penetration): `BloomFilter` (FNV-1a + std::hash dual-hash, default 1M elements @ 1% FP rate) rejects keys known not to exist.
2. **Circuit breaker** (fault tolerance): `CircuitBreaker` (3-state: CLOSED→OPEN→HALF_OPEN, 5 consecutive failures → 30s cooldown → probe) skips Redis and falls back to direct DB queries.
3. **Distributed mutex lock** (hotspot invalidation): `SETNX`-based lock with double-check prevents thundering-herd cache rebuilds.

TTL randomization (±10% jitter) prevents avalanche expiration. Null values are cached with a short TTL (60s) to prevent penetration.

### Rate limiter (`rate_limiter/`)

**`token_bucket.h`** — Token bucket algorithm with per-bucket `std::mutex`. `consume(cost)` refills tokens at `rate_`/s (max `burst_`), rejects if insufficient.

**`rate_limiter.h`** — Singleton managing per-IP token buckets keyed by `(endpoint, IP)` pair. Endpoint-specific rate/burst defaults:

| Endpoint | Rate | Burst | Purpose |
|:---|:---|:---|:---|
| `register` | 2/s | 5 | Anti-registration flood |
| `login` | 5/s | 10 | Anti-brute-force |
| `api` | 10/s | 20 | Anti-CRUD abuse |
| `connect` | 100/s | 200 | Connection-level flood defense (checked at `accept()` time) |
| `global` | 50/s | 100 | /4 queries, static files |

`cleanup_idle(120)` is called in the event loop after each timer tick to remove entries idle >2min.

## Key hardcoded values

- DB host: `192.168.19.1:3306` (in `webserver.cpp`)
- Document root: `/home/user/TinyWebServer/root` (in `webserver.cpp`; allocated via `malloc`, never freed — intentional)
- DB credentials: `user` / `123456` / database `server` (in `main.cpp`)
- Redis: `127.0.0.1:6379`, no password, pool size 16, db index 0
- JWT secret: from `JWT_SECRET` env var, or random 256-bit hex key generated at startup (in `http_conn.cpp`)
- Listen backlog: 4096
- Max connections: `MAX_FD = 65536`

## Stress testing

```bash
go run pressureTest.go -c 10000 -t 60 -url http://localhost:8080/4
```

Perf flamegraph (commands in README.md).

## C++ conventions

- Standard: C++20 (`CMAKE_CXX_STANDARD 20`)
- Smart pointers preferred (`std::unique_ptr`, `std::make_unique`) for ownership of arrays and objects
- RAII for resource management (connections, mmap regions)
- `std::counting_semaphore` (C++20) for thread synchronization instead of POSIX semaphores
- Header-only template/utility code where possible (`auth/`, `rate_limiter/`, `thread_pool/`, `redis/bloom_filter.h`, `redis/circuit_breaker.h`)

## Formatting

clang-format config at `.vscode/.clangfomat` (note: filename has a typo). Style: Google-based, 120-char column limit, 4-space indent, pointer alignment right, C++20 standard.
