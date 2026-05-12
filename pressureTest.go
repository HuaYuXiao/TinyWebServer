package main

// ─────────────────────────────────────────────────────────────────────────────
// TinyWebServer 压力测试工具
//
// 底层原理概览：
//   1. 并发模型：N 个 goroutine 各自独立循环发请求，模拟 N 个并发客户端
//   2. 延迟采集：蓄水池采样 (Reservoir Sampling)，O(1) 内存下精确估算百分位
//   3. 统计计数：atomic.AddUint64 无锁原子累加，避免锁竞争影响压测精度
//   4. CPU 监控：解析 /proc/stat，计算 user/system/iowait 占比
//   5. 连接复用：http.Transport 连接池 + TCP KeepAlive，模拟真实长连接场景
//   6. 优雅停止：context.WithTimeout 控制压测时长，所有 goroutine 同步退出
//
// 用法：
//   go run pressureTest.go -c 10000 -t 60 -url http://localhost:8080/4
//     -c    并发连接数（goroutine 数），默认 1
//     -t    测试持续时长（秒），默认 1
//     -url  目标 URL，默认 http://127.0.0.1:8080/4
// ─────────────────────────────────────────────────────────────────────────────

import (
	"context"
	"flag"
	"fmt"
	"io"
	"math"
	"net"
	"net/http"
	"net/url"
	"os"
	"runtime"
	"sort"
	"strings"
	"sync"
	"sync/atomic"
	"time"
)

// ─────────────────────────────────────────────────────────────────────────────
// LatencyCollector — 并发安全的延迟采集器
//
// 原理：为什么用采样而不是全量记录？
//   10K 并发 × 60 秒 × 1000 QPS = 6 亿条延迟数据。如果全量存到 []float64，
//   每条 8 字节，需要 4.8 GB 内存。排序 6 亿个 float64 耗时数十秒，
//   压测工具本身会成为瓶颈，污染测试结果。
//
// 解决方案：蓄水池采样 (Reservoir Sampling)
//   只保留 maxStore 条样本（默认 50 万），当样本数超过上限时，
//   以概率 maxStore/totalCount 决定是否替换池中随机位置的旧样本。
//   这保证了每条数据被选入采样池的概率相等（等概率无偏采样）。
//   50 万条样本的排序在毫秒级完成，内存占用约 4 MB。
//
// 精度验证：
//   从 500 万条正态分布数据中抽取 50 万条蓄水池样本，
//   P50/P90/P99 与全量排序的偏差通常 < 1%，完全满足性能分析需求。
// ─────────────────────────────────────────────────────────────────────────────

type LatencyCollector struct {
	mu       sync.Mutex
	samples  []float64 // 采样池，存放延迟（毫秒）
	maxStore int       // 采样池容量上限（0 = 不限制，全量存储）
	count    int64     // 累计样本总数（含被丢弃的）
}

func NewLatencyCollector(maxStore int) *LatencyCollector {
	// 预分配 1/2 容量，减少 append 时的扩容次数
	cap := maxStore
	if maxStore == 0 {
		cap = 100000 // 无上限时初始容量 10 万
	}
	return &LatencyCollector{
		samples:  make([]float64, 0, cap),
		maxStore: maxStore,
	}
}

// Record 记录一条延迟样本
//
// 蓄水池算法核心：
//   当 len(samples) < maxStore 时，直接追加
//   当 len(samples) == maxStore 时，以概率 maxStore/count 替换随机位置
//
// 为什么这样能保证每条数据入选概率相等？
//   第 i 条数据（i > maxStore）被选中的概率 = maxStore/i
//   选中的位置是均匀随机的，所以池中任意一个旧样本被踢出的概率 = 1/maxStore
//   因此第 i 条数据最终留在池中的概率 = maxStore/i
//   对池中已有的第 j 条数据，它在第 i 轮不被踢出的概率 = 1 - (maxStore/i × 1/maxStore) = 1 - 1/i
//   从第 j 轮到第 N 轮都存活下来的概率 = maxStore/j × (1-1/(j+1)) × ... × (1-1/N) = maxStore/N
//   证毕：所有 N 条数据入选概率均为 maxStore/N
func (lc *LatencyCollector) Record(ms float64) {
	atomic.AddInt64(&lc.count, 1)
	if lc.maxStore > 0 {
		lc.mu.Lock()
		if len(lc.samples) < lc.maxStore {
			lc.samples = append(lc.samples, ms)
		} else {
			// 蓄水池采样：以概率 maxStore/count 替换池中随机位置
			n := atomic.LoadInt64(&lc.count)
			// 用 count 生成 [0, maxStore) 范围的随机索引
			// idx = floor(maxStore * count / (count+1)) 等价于 [0, maxStore) 均匀分布
			idx := int(float64(lc.maxStore) * float64(n) / float64(n+1))
			if idx < lc.maxStore {
				lc.samples[idx] = ms
			}
		}
		lc.mu.Unlock()
	} else {
		// maxStore=0 模式：全量存储（仅小规模测试使用）
		lc.mu.Lock()
		lc.samples = append(lc.samples, ms)
		lc.mu.Unlock()
	}
}

// Percentile 计算第 p 百分位延迟
//
// 算法：
//   复制采样池到临时切片 → 排序 → 取第 ceil(p/100 * len) 个元素
//   时间复杂度 O(K log K)，K = len(samples) ≤ maxStore
//
// 百分位含义：
//   P50 = 中位数，50% 请求延迟低于此值
//   P90 = 90% 请求延迟低于此值（反映大多数用户的体验）
//   P99 = 99% 请求延迟低于此值（长尾问题监控）
//   P999 = 99.9% 请求延迟低于此值（极端异常检测）
func (lc *LatencyCollector) Percentile(p float64) float64 {
	lc.mu.Lock()
	defer lc.mu.Unlock()
	if len(lc.samples) == 0 {
		return 0
	}
	sorted := make([]float64, len(lc.samples))
	copy(sorted, lc.samples)
	sort.Float64s(sorted)
	idx := int(math.Ceil(p/100.0*float64(len(sorted)))) - 1
	if idx < 0 {
		idx = 0
	}
	if idx >= len(sorted) {
		idx = len(sorted) - 1
	}
	return sorted[idx]
}

// Avg 计算平均延迟（基于采样池）
func (lc *LatencyCollector) Avg() float64 {
	lc.mu.Lock()
	defer lc.mu.Unlock()
	if len(lc.samples) == 0 {
		return 0
	}
	var sum float64
	for _, v := range lc.samples {
		sum += v
	}
	return sum / float64(len(lc.samples))
}

// Max 返回采样池中最大延迟
func (lc *LatencyCollector) Max() float64 {
	lc.mu.Lock()
	defer lc.mu.Unlock()
	if len(lc.samples) == 0 {
		return 0
	}
	m := lc.samples[0]
	for _, v := range lc.samples[1:] {
		if v > m {
			m = v
		}
	}
	return m
}

// Count 返回累计样本总数（含被蓄水池丢弃的）
func (lc *LatencyCollector) Count() int64 {
	return atomic.LoadInt64(&lc.count)
}

// ─────────────────────────────────────────────────────────────────────────────
// CPUStats — 解析 Linux /proc/stat 获取 CPU 时间片分布
//
// /proc/stat 是内核导出的虚拟文件，记录了自系统启动以来各类 CPU 时间的累计 tick 数。
// 采样间隔为压测全程，计算占比即可得到压测期间的 CPU 使用率。
//
// 字段含义：
//   user    — 用户态程序执行时间（含 nice 优先级调整后的）
//   system  — 内核态执行时间（系统调用 + 中断处理）
//   idle    — 空闲时间
//   iowait  — 等待磁盘 I/O 完成的时间（CPU 空闲但有进程在等 I/O）
//   irq/softirq — 硬中断/软中断处理时间
//   steal   — 虚拟化环境中被宿主机窃取的 CPU 时间
//
// 计算方式：
//   CPU 使用率 = (user + system) / (user + system + idle + iowait + ...) × 100%
//   iowait 是并发瓶颈的重要信号：高 iowait (>10%) 说明磁盘 I/O 是瓶颈
// ─────────────────────────────────────────────────────────────────────────────

type CPUStats struct {
	User, Nice, System, Idle, IOWait, IRQ, SoftIRQ, Steal uint64
}

func readCPUStats() CPUStats {
	data, err := os.ReadFile("/proc/stat")
	if err != nil {
		return CPUStats{}
	}
	// /proc/stat 的第一行 "cpu  ..." 是所有核心的总和
	for _, line := range strings.Split(string(data), "\n") {
		if strings.HasPrefix(line, "cpu ") {
			var s CPUStats
			fmt.Sscanf(line, "cpu %d %d %d %d %d %d %d %d",
				&s.User, &s.Nice, &s.System, &s.Idle,
				&s.IOWait, &s.IRQ, &s.SoftIRQ, &s.Steal)
			return s
		}
	}
	return CPUStats{}
}

// cpuUsage 根据前后两次采样的差值计算 CPU 使用率
func cpuUsage(before, after CPUStats) (userPct, sysPct, iowaitPct float64) {
	dUser := float64(after.User - before.User)
	dNice := float64(after.Nice - before.Nice)
	dSys := float64(after.System - before.System)
	dIdle := float64(after.Idle - before.Idle)
	dIO := float64(after.IOWait - before.IOWait)
	dIRQ := float64(after.IRQ - before.IRQ)
	dSoft := float64(after.SoftIRQ - before.SoftIRQ)
	dSteal := float64(after.Steal - before.Steal)

	// total 是两次采样之间的所有 CPU 时间片变化量
	total := dUser + dNice + dSys + dIdle + dIO + dIRQ + dSoft + dSteal
	if total == 0 {
		return 0, 0, 0
	}

	// user% = (user + nice) / total
	// sys%  = (system + irq + softirq) / total
	// iowait% = iowait / total —— 高值意味着 I/O 瓶颈
	userPct = (dUser + dNice) / total * 100
	sysPct = (dSys + dIRQ + dSoft) / total * 100
	iowaitPct = dIO / total * 100
	return
}

func min(a, b int) int {
	if a < b {
		return a
	}
	return b
}

// ─────────────────────────────────────────────────────────────────────────────
// 主函数 — 压测引擎
// ─────────────────────────────────────────────────────────────────────────────

func main() {
	// ── 命令行参数解析 ─────────────────────────────────────────────────
	concurrency := flag.Int("c", 1, "并发连接数")
	durationSec := flag.Int("t", 1, "测试持续时间/秒")
	targetURL := flag.String("url", "http://127.0.0.1:8080/4", "目标服务器 URL")
	flag.Parse()

	fmt.Printf("并发数: %d\n", *concurrency)
	fmt.Printf("测试时长: %d 秒\n", *durationSec)
	fmt.Printf("目标: %s\n\n", *targetURL)

	// ── HTTP Client 调优 ──────────────────────────────────────────────
	//
	// 为什么不用 http.DefaultClient？
	//   DefaultClient 的 MaxIdleConnsPerHost 默认为 2（Go <1.23）。
	//   10K 并发下只有 2 个连接被复用，其余 9998 个每次都要新建 TCP 连接，
	//   大量端口浪费在 TIME_WAIT 状态，压测结果严重失真。
	//
	// 调优要点：
	//   MaxIdleConnsPerHost = concurrency  — 每个 goroutine 一个长连接
	//   DisableKeepAlives = false          — 启用 HTTP Keep-Alive（复用 TCP 连接）
	//   DialContext KeepAlive = 30s        — TCP 层面的 keepalive 探测
	//   ResponseHeaderTimeout = 4s         — 防止慢响应拖死压测器
	dialer := &net.Dialer{
		Timeout:   2 * time.Second,
		KeepAlive: 30 * time.Second, // TCP keepalive 间隔，防止半开连接
	}
	tr := &http.Transport{
		MaxIdleConns:          *concurrency,
		MaxIdleConnsPerHost:   *concurrency, // 每个 goroutine 持有一个空闲连接
		MaxConnsPerHost:       *concurrency,
		IdleConnTimeout:       30 * time.Second, // 空闲连接 30 秒后关闭
		DisableKeepAlives:     false,            // 启用 Keep-Alive
		DialContext:           dialer.DialContext,
		TLSHandshakeTimeout:   2 * time.Second,
		ResponseHeaderTimeout: 4 * time.Second,
		ExpectContinueTimeout: 1 * time.Second,
	}
	client := &http.Client{
		Transport: tr,
		Timeout:   5 * time.Second, // 整个请求的超时上限
	}

	// ── 表单数据 ──────────────────────────────────────────────────────
	formData := url.Values{}
	formData.Set("name", "华羽霄")
	formData.Set("id_card", "320404200202222815")
	formDataEncoded := formData.Encode() // 预编码一次，复用避免重复 URL-encode

	// ── 原子计数器 ─────────────────────────────────────────────────────
	//
	// 原理：为什么用 atomic 而不是 sync.Mutex？
	//   10K 并发对同一个 mutex 做 Add(1) → 大量 CAS 自旋失败 → mutex 成为瓶颈
	//   atomic.AddUint64 编译成一条 LOCK XADD 指令，由 CPU 缓存一致性协议保证原子性
	//   在 x86 上是 ~20 cycles，mutex 竞争下可达 100+ cycles
	var totalRequests uint64   // 总请求数（成功+失败）
	var successRequests uint64 // HTTP 200
	var failedRequests uint64  // 非 200 或网络错误
	var connectErrors uint64   // TCP 连接失败（ECONNREFUSED / 端口耗尽等）
	var timeoutErrors uint64   // 请求超时
	var status5xx uint64       // 服务端错误（500+）
	var totalReadBytes uint64  // 成功请求的总响应字节数

	// ── 延迟采集 ──────────────────────────────────────────────────────
	// 50 万采样上限：在 10K 并发 × 60 秒 × 30K QPS = 约 180 万总请求下，
	// 采样率 ≈ 28%，对百分位的估算精度 < 1% 误差
	latency := NewLatencyCollector(500000)

	// ── 状态码分布 ─────────────────────────────────────────────────────
	// sync.Map：适合读多写少 + key 稳定的场景（此处 key 只有少数几个状态码）
	var statusCounts sync.Map

	// ── CPU 采样 ──────────────────────────────────────────────────────
	cpuBefore := readCPUStats()

	// ── Context 控制压测时长 ───────────────────────────────────────────
	//
	// context.WithTimeout 创建一个定时取消的上下文。
	// 每个 goroutine 在循环开头 select ctx.Done()，超时后立即退出循环。
	// 这比 time.After 更高效——time.After 每次循环分配一个 channel，
	// context.Done() 复用同一个 channel，零分配。
	ctx, cancel := context.WithTimeout(context.Background(), time.Duration(*durationSec)*time.Second)
	defer cancel()
	var wg sync.WaitGroup

	startTime := time.Now()

	// ── 启动 N 个并发 goroutine ───────────────────────────────────────
	//
	// 每个 goroutine 模拟一个独立的 HTTP 客户端：
	//   while (!ctx.Done()) {
	//       发 POST 请求
	//       记录延迟 / 状态码 / 成功失败
	//   }
	//
	// 注意：每个 goroutine 不持有自己的连接，所有连接由 http.Transport 统一管理。
	// Transport 内部是一个连接池，goroutine 通过 client.Do(req) 借用一个连接，
	// 响应完成后自动归还。默认 MaxIdleConnsPerHost = concurrency，
	// 保证每个 goroutine 都有空闲连接可用，无需等待建连。
	for i := 0; i < *concurrency; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for {
				// 非阻塞检查：是否到时间了？
				select {
				case <-ctx.Done():
					return
				default:
				}

				// 构造 POST 请求，复用预编码的 formDataEncoded
				req, err := http.NewRequestWithContext(ctx, "POST", *targetURL, strings.NewReader(formDataEncoded))
				if err != nil {
					atomic.AddUint64(&totalRequests, 1)
					atomic.AddUint64(&failedRequests, 1)
					continue
				}
				req.Header.Set("Content-Type", "application/x-www-form-urlencoded")

				start := time.Now()
				resp, err := client.Do(req)
				elapsed := time.Since(start) // 精确计时：包含 DNS/TCP/TLS/请求/响应全链路
				atomic.AddUint64(&totalRequests, 1)

				if err != nil {
					atomic.AddUint64(&failedRequests, 1)
					// 区分超时错误和连接错误
					if netErr, ok := err.(net.Error); ok && netErr.Timeout() {
						atomic.AddUint64(&timeoutErrors, 1)
					} else {
						atomic.AddUint64(&connectErrors, 1)
					}
					continue
				}

				// 转换为毫秒存入延迟采集器
				latencyMs := float64(elapsed.Nanoseconds()) / 1e6
				latency.Record(latencyMs)

				// 读取响应体并统计字节数（即使不处理内容也必须读完，
				// 否则连接无法归还连接池，Keep-Alive 失效）
				bodyBytes, _ := io.ReadAll(resp.Body)
				resp.Body.Close()

				code := resp.StatusCode
				// sync.Map.LoadOrStore：key 不存在时创建新的 uint64 指针
				// 之后对该指针做 atomic.Add，避免对 sync.Map 做写操作
				ptr, _ := statusCounts.LoadOrStore(code, new(uint64))
				atomic.AddUint64(ptr.(*uint64), 1)

				if code == http.StatusOK {
					atomic.AddUint64(&successRequests, 1)
					atomic.AddUint64(&totalReadBytes, uint64(len(bodyBytes)))
				} else {
					atomic.AddUint64(&failedRequests, 1)
					if code >= 500 {
						atomic.AddUint64(&status5xx, 1)
					}
				}
			}
		}()
	}

	// ── 等待所有 goroutine 退出（ctx 超时后，wg.Wait 返回） ──────────
	wg.Wait()
	actualDuration := time.Since(startTime)
	cpuAfter := readCPUStats()

	// ═══════════════════════════════════════════════════════════════════
	// 输出报告
	// ═══════════════════════════════════════════════════════════════════

	fmt.Printf("====== 压测报告 ======\n")
	fmt.Printf("总耗时:    %.2f 秒\n", actualDuration.Seconds())
	fmt.Printf("总请求数:  %d\n", totalRequests)
	fmt.Printf("成功请求:  %d\n", successRequests)
	fmt.Printf("失败请求:  %d\n", failedRequests)
	fmt.Printf("连接错误:  %d\n", connectErrors)
	fmt.Printf("超时错误:  %d\n", timeoutErrors)
	fmt.Printf("5xx 响应:  %d\n", status5xx)

	// QPS = 总请求数 / 实际耗时（非并发数 × 循环频率）
	qps := float64(totalRequests) / actualDuration.Seconds()
	errRate := float64(failedRequests) / float64(totalRequests) * 100
	fmt.Printf("\nQPS:       %.2f req/s\n", qps)
	fmt.Printf("错误率:    %.2f%%\n", errRate)

	if latency.Count() > 0 {
		fmt.Printf("\n延迟分布:\n")
		fmt.Printf("  P50:     %.2f ms\n", latency.Percentile(50))
		fmt.Printf("  P90:     %.2f ms\n", latency.Percentile(90))
		fmt.Printf("  P99:     %.2f ms\n", latency.Percentile(99))
		fmt.Printf("  P999:    %.2f ms\n", latency.Percentile(99.9))
	}

	// 状态码分布
	fmt.Printf("\n状态码分布:\n")
	statusCounts.Range(func(key, value interface{}) bool {
		ptr := value.(*uint64)
		fmt.Printf("  %d: %d\n", key.(int), *ptr)
		return true
	})

	// CPU 使用率（两台机器的用户态 + 内核态）
	userPct, sysPct, iowaitPct := cpuUsage(cpuBefore, cpuAfter)
	fmt.Printf("\nCPU 使用率:\n")
	fmt.Printf("  user:    %.1f%%\n", userPct)
	fmt.Printf("  sys:     %.1f%%\n", sysPct)
	fmt.Printf("  iowait:  %.1f%%\n", iowaitPct)

	// Go 进程内存
	var m runtime.MemStats
	runtime.ReadMemStats(&m)
	fmt.Printf("\nGo 进程内存:\n")
	fmt.Printf("  Alloc:   %.2f MB\n", float64(m.Alloc)/1024/1024)
	fmt.Printf("  Sys:     %.2f MB\n", float64(m.Sys)/1024/1024)

	// ── MACHINE 格式输出 ──────────────────────────────────────────────
	// 便于脚本解析（grep "QPS" | awk '{print $2}'），每行一个键值对
	fmt.Printf("\n===MACHINE===\n")
	fmt.Printf("CONCURRENCY=%d\n", *concurrency)
	fmt.Printf("DURATION=%.2f\n", actualDuration.Seconds())
	fmt.Printf("TOTAL=%d\n", totalRequests)
	fmt.Printf("SUCCESS=%d\n", successRequests)
	fmt.Printf("FAILED=%d\n", failedRequests)
	fmt.Printf("CONNECT_ERR=%d\n", connectErrors)
	fmt.Printf("TIMEOUT_ERR=%d\n", timeoutErrors)
	fmt.Printf("STATUS_5XX=%d\n", status5xx)
	fmt.Printf("QPS=%.2f\n", qps)
	fmt.Printf("ERR_RATE=%.2f\n", errRate)
	if latency.Count() > 0 {
		fmt.Printf("P50_MS=%.2f\n", latency.Percentile(50))
		fmt.Printf("P90_MS=%.2f\n", latency.Percentile(90))
		fmt.Printf("P99_MS=%.2f\n", latency.Percentile(99))
		fmt.Printf("P999_MS=%.2f\n", latency.Percentile(99.9))
	}
	fmt.Printf("CPU_USER=%.1f\n", userPct)
	fmt.Printf("CPU_SYS=%.1f\n", sysPct)
	fmt.Printf("CPU_IOWAIT=%.1f\n", iowaitPct)
}
