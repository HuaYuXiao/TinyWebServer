# TinyWebServer

Linux 下高并发 Web 服务器，C++20 实现。

## 特性

- **epoll(LT) + 模拟 Proactor** 并发模型 — 主线程 I/O，线程池处理业务
- **状态机** 解析 HTTP 请求，支持 GET（静态文件 `mmap`）和 POST（CGI 查 MySQL）
- **MySQL 连接池** — RAII + `std::counting_semaphore`
- **Redis 缓存层** — 布隆过滤器（防穿透）+ 互斥锁（防击穿）+ 随机 TTL（防雪崩）+ 熔断器（容错降级）
- **定时器** — `std::set` 管理非活动连接，到期自动清理
- **统一事件源** — `socketpair` 将信号转换为 epoll 事件
- **优雅降级** — Redis 不可用时自动回退 MySQL，不中断服务

## 依赖

| 依赖 | 用途 |
|:---|:---|
| C++20（g++ ≥ 10 / clang++ ≥ 16） | 语言特性 |
| CMake ≥ 3.16 | 构建系统 |
| libmysqlclient | MySQL 客户端 |
| thread | 多线程 |
| hiredis 1.2.0 | Redis 客户端 |
| Redis | 缓存服务，不可用时自动降级 |

‵‵`bash
sudo apt install libhiredis-dev libmysqlclient-dev
```

## 构建与运行

```bash
# 构建
mkdir -p build && cd build && cmake .. && cmake --build .

# 从项目根目录运行
./build/server

# 自定义参数: 端口 9006, MySQL 池 100, 线程 64, Redis 池 8
./build/server -p 9006 -s 100 -t 64 -r 8
```

## CLI 参数

| 标志 | 说明 | 默认值 |
|:---|:---|:---|
| `-p` | 监听端口 | 9006 |
| `-s` | MySQL 连接池大小 | 100 |
| `-t` | 线程池线程数 | 64 |
| `-r` | Redis 连接池大小 | 16 |

## 项目结构

```
.
├── main.cpp                        # 入口，串联各模块并启动事件循环
├── webserver     # epoll 事件循环、连接管理、信号处理
├── config.cpp           # CLI 参数解析与默认配置
├── http/http_conn         # HTTP 状态机解析 + writev 响应
├── thread_pool/thread_pool         # 线程池模板（Proactor 消费者）
├── timer/lst_timer        # 定时器（std::set，到期连接清理）
├── mysql/mysql_pool  # MySQL 连接池（RAII）
├── redis/
│   ├── redis_pool     # Redis 连接池（RAII + 健康检查）
│   ├── redis_cache               # 三级防护缓存工具类
│   ├── bloom_filter                     # 布隆过滤器（防穿透）
│   ├── circuit_breaker                  # 熔断器（CLOSED/OPEN/HALF_OPEN）
│   └── exam_score_handler               # 业务层缓存集成示例
└── root/                          # 静态文件根目录
```

## Redis 缓存层

三级防护体系，对应 `redis/README.md`：

1. **防穿透** — 布隆过滤器（100 万元素 / 1% 误判率 / ~1.2 MB）+ 空值缓存（60s TTL）
2. **防击穿** — SETNX 分布式互斥锁，仅一个线程重建缓存
3. **防雪崩** — 随机 TTL 抖动 ±10%

## 压力测试

```bash
go run pressureTest.go -c <并发数> -t 60 -url http://localhost:9006/4
```

测试环境：POST CGI 接口（Redis 缓存 + MySQL 回退），60 秒持续负载。

| 并发连接 | 总请求 | 成功率 | QPS | P50 | P90 | P99 | P999 |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 1K | 2,168K | 99.96% | 36K | 27 ms | 40 ms | 51 ms | 69 ms |
| 2K | 1,839K | 99.91% | 30K | 65 ms | 80 ms | 101 ms | 129 ms |
| 5K | 1,949K | 99.81% | 32K | 161 ms | 201 ms | 246 ms | 307 ms |
| 10K | 1,886K | 99.59% | 31K | 316 ms | 386 ms | 481 ms | 587 ms |
| 20K | 1,738K | 99.16% | 28K | 700 ms | 858 ms | 1,606 ms | 1,652 ms |
| 30K | 326K | 77.74% | 5.4K | 1,274 ms | 4,883 ms | 13 s | 17 s |
| 50K | 277K | 39.12% | 4.5K | 1,894 ms | 15 s | 25 s | 31 ms |

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
sudo netstat -anp | grep :9006 | awk '{print $6}' | sort | uniq -c
```
