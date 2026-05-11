# TinyWebServer

Linux 下高并发 Web 服务器，C++20 实现。

## 特性

- **epoll(LT) + 模拟 Proactor** 并发模型 — 主线程 I/O，线程池处理业务
- **状态机** 解析 HTTP 请求，支持 GET/POST/PUT/DELETE
- **JWT 认证 + RBAC 权限** — PBKDF2 密码哈希，user（只读）/ root（CRUD）两级角色
- **MySQL 连接池** — RAII + `std::counting_semaphore` + SSL session 复用 + 健康检查
- **Redis 缓存层** — 布隆过滤器（防穿透）+ 互斥锁（防击穿）+ 随机 TTL（防雪崩）+ 熔断器（容错降级）
- **定时器** — `std::set` 管理非活动连接，O(log n) 到期清理
- **统一事件源** — `socketpair` 将信号转换为 epoll 事件
- **审计日志** — root 的 INSERT/UPDATE/DELETE 操作自动记录到 `audit_log` 表
- **优雅降级** — Redis 不可用时自动回退 MySQL；`-a 0` 关闭认证回退到纯 SELECT 模式

## 整体技术架构

```
浏览器 ─── HTTP ──→ [epoll LT 监听] ──→ 主线程 accept
                                           │
                                     read_once() 读取请求
                                           │
                                     推入线程池队列
                                           │
                           ┌───────────────┼───────────────┐
                           ▼               ▼               ▼
                      worker 线程     worker 线程     worker 线程
                           │               │               │
                     process() ──→ HTTP 状态机解析
                           │
                    ┌──────┴──────┐
                    ▼             ▼
              静态文件        动态路由
             mmap 直接返回    (认证 + CRUD)
                                │
                    ┌───────────┼───────────┐
                    ▼           ▼           ▼
                 Redis       MySQL       JWT/PBKDF2
               (缓存层)    (连接池)     (认证模块)
```

### 核心组件

| 组件 | 文件 | 职责 |
|:---|:---|:---|
| **事件循环** | `webserver.cpp` | epoll LT 监听，统一事件源（信号→socketpair→epoll），accept 新连接，分发读写事件 |
| **HTTP 状态机** | `http/http_conn.cpp` | 三阶段解析（请求行→头部→正文），路由分发，writev 响应 |
| **线程池** | `thread_pool/thread_pool.h` | Proactor 消费者，`std::counting_semaphore` 通知，每个 worker 获取 DB 连接后执行 `process()` |
| **MySQL 连接池** | `mysql/mysql_pool.cpp` | 单例，RAII + semaphore 管理，SSL session 复用，60s 冷却健康检查 + 自动重连 |
| **Redis 缓存层** | `redis/` | 三级防护（布隆过滤器→熔断器→互斥锁），Cache Aside 模式，随机 TTL 防雪崩 |
| **认证模块** | `auth/` | PBKDF2-HMAC-SHA256 密码哈希（100K 迭代），HS256 JWT 签发/验证（24h TTL） |
| **定时器** | `timer/lst_timer.cpp` | `std::set` 按过期时间排序，SIGALRM 每 5s 触发 tick，清理 15s 不活跃连接 |

### 请求处理流程

1. **静态文件**（GET `/`, `/login.html` 等）— `mmap` 映射文件到内存，`writev` 零拷贝发送，不经过数据库
2. **认证路由**（POST `/auth/register`, `/auth/login`）— 从 `server_users` 表查询/插入用户，返回 JWT
3. **成绩查询**（POST `/4`）— 先查 Redis 缓存，未命中则查 MySQL 并回写缓存，需携带 JWT
4. **CRUD 操作**（POST/PUT/DELETE `/api/student`）— 需 root 角色 JWT，执行后写审计日志

## 依赖

| 依赖 | 用途 |
|:---|:---|
| C++20（g++ ≥ 10 / clang++ ≥ 16） | 语言特性 |
| CMake ≥ 3.16 | 构建系统 |
| libmysqlclient | MySQL 客户端 |
| OpenSSL (libcrypto) | PBKDF2 密码哈希 + HMAC-SHA256 JWT 签名 |
| pthread | 多线程 |
| hiredis 1.2.0 | Redis 客户端 |
| Redis | 缓存服务，不可用时自动降级 |

```bash
sudo apt install libhiredis-dev libmysqlclient-dev libssl-dev
```

## 构建与运行

```bash
mkdir -p build && cd build && cmake .. && cmake --build .
./build/server -p 8080 -s 100 -t 64 -r 8 -a 0
```

## CLI 参数

| 标志 | 说明 | 默认值 |
|:---|:---|:---|
| `-p` | 监听端口 | 8080 |
| `-s` | MySQL 连接池大小 | 100 |
| `-t` | 线程池线程数 | 64 |
| `-r` | Redis 连接池大小 | 16 |
| `-a` | 认证开关（0=关闭, 1=开启） | 1 |

## API 接口

### 认证路由（无需令牌）

| 端点 | 方法 | 说明 |
|:---|:---|:---|
| `/auth/register` | POST | 注册用户，body: `username=xxx&password=xxx` |
| `/auth/login` | POST | 登录，body: `username=xxx&password=xxx`，返回 JWT |

### 数据路由（需 `Authorization: Bearer <token>`）

| 端点 | 方法 | 权限 | 说明 |
|:---|:---|:---|:---|
| `/4` | POST | user+ | 学生成绩查询（SELECT，原有功能） |
| `/api/student` | POST | root | 新增学生 |
| `/api/student` | PUT | root | 修改学生（需 `student_id`） |
| `/api/student` | DELETE | root | 删除学生（需 `student_id`） |

### 示例

```bash
# 注册
curl -X POST http://localhost:8080/auth/register \
  -d "username=admin&password=secret"

# 登录
TOKEN=$(curl -s -X POST http://localhost:8080/auth/login \
  -d "username=root&password=123456" | jq -r .token)

# 查询（user 权限）
curl -X POST http://localhost:8080/4 \
  -H "Authorization: Bearer $TOKEN" \
  -d "name=张三&id_card=3204..."

# 新增学生（root 权限）
curl -X POST http://localhost:8080/api/student \
  -H "Authorization: Bearer $TOKEN" \
  -d "name=张三&id_card=3204...&gender=男&province=江苏&school=清华"
```

## 浏览器网页端功能

服务器启动后，浏览器访问 `http://<服务器IP>:8080`，自动跳转到登录页面。

### 页面一览

| 页面 | URL | 功能 |
|:---|:---|:---|
| **登录** | `/login.html` | 输入用户名密码登录，成功后跳转查询页 |
| **注册** | `/register.html` | 注册新用户（密码 ≥6 位，前端校验一致性），成功后自动登录 |
| **成绩查询** | `/index.html` | 输入姓名 + 身份证号查询成绩，展示学生信息和各科分数表格 |

### 交互流程

```
打开浏览器 → login.html（未登录自动跳转这里）
                │
        ┌───────┴───────┐
        ▼               ▼
    已有账号          新用户
    直接登录      register.html 注册
        │               │
        └───────┬───────┘
                ▼
         index.html（查询页）
                │
        输入姓名 + 身份证号
                │
        POST /4（自动携带 JWT）
                │
        ┌───────┴───────┐
        ▼               ▼
    Redis 命中       Redis 未命中
    直接返回          查 MySQL → 回写 Redis
        │               │
        └───────┬───────┘
                ▼
        展示学生信息 + 成绩表格
```

### 前端技术细节

- **认证状态管理**：JWT token 存储在浏览器 `localStorage`，页面加载时检查，未登录自动跳转
- **请求携带令牌**：`fetch` 请求自动添加 `Authorization: Bearer <token>` 头
- **密码安全**：密码最小 6 位，注册时前端校验两次输入一致，后端 PBKDF2 100K 迭代哈希存储
- **错误处理**：401 自动跳转登录页清除 token，其他错误在页面内显示提示

## 数据库初始化

```bash
mysql -u user -p123456 server < sql/init.sql
```

创建 `server_users`（用户表）和 `audit_log`（审计日志表），并预置 root 管理员（密码 `123456`）。

## Redis 缓存层

三级防护体系，对应 `redis/README.md`：

1. **防穿透** — 布隆过滤器（100 万元素 / 1% 误判率 / ~1.2 MB）+ 空值缓存（60s TTL）
2. **防击穿** — SETNX 分布式互斥锁，仅一个线程重建缓存
3. **防雪崩** — 随机 TTL 抖动 ±10%

## 压力测试

```bash
go run pressureTest.go -c <并发数> -t 60 -url http://localhost:8080/4
```

测试环境：POST CGI 接口（Redis 缓存 + MySQL 回退），60 秒持续负载。

| 并发连接 | 总请求 | 成功率 | QPS | P50 | P90 | P99 | P999 |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 1K | 2,168K | 99.96% | 36K | 27 ms | 40 ms | 51 ms | 69 ms |
| 2K | 1,839K | 99.91% | 30K | 65 ms | 8080 ms | 101 ms | 129 ms |
| 5K | 1,949K | 99.81% | 32K | 161 ms | 201 ms | 246 ms | 307 ms |
| 10K | 1,886K | 99.59% | 31K | 316 ms | 386 ms | 481 ms | 587 ms |
| 20K | 1,738K | 99.16% | 28K | 700 ms | 858 ms | 1,606 ms | 1,652 ms |
| 30K | 326K | 77.74% ↓ | 5.4K ↓ | 1,274 ms | 4,883 ms | 13 s | 17 s |
| 50K | 277K | 39.12% ↓ | 4.5K ↓ | 1,894 ms | 15 s | 25 s | 31 ms |

## perf 火焰图

```bash
sudo perf record -F 99 -p $(pidof server) -g -- sleep 10
sudo perf script > perf.perf
./stackcollapse-perf.pl perf.perf > perf.folded
./flamegraph.pl perf.folded > perf.svg
```

![perf](log/perf.svg)

## TCP 连接状态

```bash
sudo netstat -anp | grep :8080 | awk '{print $6}' | sort | uniq -c
```
