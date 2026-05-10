package main

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

// LatencyCollector 并发安全的延迟采集器，使用采样 + 排序计算百分位
type LatencyCollector struct {
	mu       sync.Mutex
	samples  []float64 // 毫秒
	maxStore int       // 最大采样数（0=不限）
	count    int64     // 总样本计数（含被丢弃的）
}

func NewLatencyCollector(maxStore int) *LatencyCollector {
	return &LatencyCollector{
		samples:  make([]float64, 0, min(maxStore, 1000000)),
		maxStore: maxStore,
	}
}

func (lc *LatencyCollector) Record(ms float64) {
	atomic.AddInt64(&lc.count, 1)
	if lc.maxStore > 0 {
		lc.mu.Lock()
		if len(lc.samples) < lc.maxStore {
			lc.samples = append(lc.samples, ms)
		} else {
			// 蓄水池采样：以概率 maxStore/count 替换随机位置
			n := atomic.LoadInt64(&lc.count)
			idx := int(float64(lc.maxStore) * float64(n) / float64(n+1))
			if idx < lc.maxStore {
				lc.samples[idx] = ms
			}
		}
		lc.mu.Unlock()
	} else {
		lc.mu.Lock()
		lc.samples = append(lc.samples, ms)
		lc.mu.Unlock()
	}
}

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

func (lc *LatencyCollector) Count() int64 {
	return atomic.LoadInt64(&lc.count)
}

func min(a, b int) int {
	if a < b {
		return a
	}
	return b
}

// CPUStats 采集 /proc/stat 的 CPU 使用率
type CPUStats struct {
	User, Nice, System, Idle, IOWait, IRQ, SoftIRQ, Steal uint64
}

func readCPUStats() CPUStats {
	data, err := os.ReadFile("/proc/stat")
	if err != nil {
		return CPUStats{}
	}
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

func cpuUsage(before, after CPUStats) (userPct, sysPct, iowaitPct float64) {
	dUser := float64(after.User - before.User)
	dNice := float64(after.Nice - before.Nice)
	dSys := float64(after.System - before.System)
	dIdle := float64(after.Idle - before.Idle)
	dIO := float64(after.IOWait - before.IOWait)
	dIRQ := float64(after.IRQ - before.IRQ)
	dSoft := float64(after.SoftIRQ - before.SoftIRQ)
	dSteal := float64(after.Steal - before.Steal)
	total := dUser + dNice + dSys + dIdle + dIO + dIRQ + dSoft + dSteal
	if total == 0 {
		return 0, 0, 0
	}
	userPct = (dUser + dNice) / total * 100
	sysPct = (dSys + dIRQ + dSoft) / total * 100
	iowaitPct = dIO / total * 100
	return
}

func main() {
	concurrency := flag.Int("c", 1, "并发连接数")
	durationSec := flag.Int("t", 1, "测试持续时间/秒")
	targetURL := flag.String("url", "http://127.0.0.1:9006/4", "目标服务器 URL")
	flag.Parse()

	fmt.Printf("并发数: %d\n", *concurrency)
	fmt.Printf("测试时长: %d 秒\n", *durationSec)
	fmt.Printf("目标: %s\n\n", *targetURL)

	// HTTP Client 调优
	dialer := &net.Dialer{
		Timeout:   2 * time.Second,
		KeepAlive: 30 * time.Second,
	}
	tr := &http.Transport{
		MaxIdleConns:          *concurrency,
		MaxIdleConnsPerHost:   *concurrency,
		MaxConnsPerHost:       *concurrency,
		IdleConnTimeout:       30 * time.Second,
		DisableKeepAlives:     false,
		DialContext:           dialer.DialContext,
		TLSHandshakeTimeout:   2 * time.Second,
		ResponseHeaderTimeout: 4 * time.Second,
		ExpectContinueTimeout: 1 * time.Second,
	}
	client := &http.Client{
		Transport: tr,
		Timeout:   5 * time.Second,
	}

	// 表单数据
	formData := url.Values{}
	formData.Set("name", "华羽霄")
	formData.Set("id_card", "320404200202222815")
	formDataEncoded := formData.Encode()

	// 统计变量
	var totalRequests uint64
	var successRequests uint64
	var failedRequests uint64
	var connectErrors uint64
	var timeoutErrors uint64
	var status5xx uint64
	var totalReadBytes uint64

	// 延迟采集（采样上限 50 万，足够精确计算百分位）
	latency := NewLatencyCollector(500000)

	// 状态码分布
	var statusCounts sync.Map

	// CPU 采样
	cpuBefore := readCPUStats()

	ctx, cancel := context.WithTimeout(context.Background(), time.Duration(*durationSec)*time.Second)
	defer cancel()
	var wg sync.WaitGroup

	startTime := time.Now()

	for i := 0; i < *concurrency; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for {
				select {
				case <-ctx.Done():
					return
				default:
				}

				req, err := http.NewRequestWithContext(ctx, "POST", *targetURL, strings.NewReader(formDataEncoded))
				if err != nil {
					atomic.AddUint64(&totalRequests, 1)
					atomic.AddUint64(&failedRequests, 1)
					continue
				}
				req.Header.Set("Content-Type", "application/x-www-form-urlencoded")

				start := time.Now()
				resp, err := client.Do(req)
				elapsed := time.Since(start)
				atomic.AddUint64(&totalRequests, 1)

				if err != nil {
					atomic.AddUint64(&failedRequests, 1)
					if netErr, ok := err.(net.Error); ok && netErr.Timeout() {
						atomic.AddUint64(&timeoutErrors, 1)
					} else {
						atomic.AddUint64(&connectErrors, 1)
					}
					continue
				}

				latencyMs := float64(elapsed.Nanoseconds()) / 1e6
				latency.Record(latencyMs)

				bodyBytes, _ := io.ReadAll(resp.Body)
				resp.Body.Close()

				code := resp.StatusCode
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

	wg.Wait()
	actualDuration := time.Since(startTime)
	cpuAfter := readCPUStats()

	// ── 输出报告 ──
	fmt.Printf("====== 压测报告 ======\n")
	fmt.Printf("总耗时:    %.2f 秒\n", actualDuration.Seconds())
	fmt.Printf("总请求数:  %d\n", totalRequests)
	fmt.Printf("成功请求:  %d\n", successRequests)
	fmt.Printf("失败请求:  %d\n", failedRequests)
	fmt.Printf("连接错误:  %d\n", connectErrors)
	fmt.Printf("超时错误:  %d\n", timeoutErrors)
	fmt.Printf("5xx 响应:  %d\n", status5xx)

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

	// CPU 使用率
	userPct, sysPct, iowaitPct := cpuUsage(cpuBefore, cpuAfter)
	fmt.Printf("\nCPU 使用率:\n")
	fmt.Printf("  user:    %.1f%%\n", userPct)
	fmt.Printf("  sys:     %.1f%%\n", sysPct)
	fmt.Printf("  iowait:  %.1f%%\n", iowaitPct)

	// 内存
	var m runtime.MemStats
	runtime.ReadMemStats(&m)
	fmt.Printf("\nGo 进程内存:\n")
	fmt.Printf("  Alloc:   %.2f MB\n", float64(m.Alloc)/1024/1024)
	fmt.Printf("  Sys:     %.2f MB\n", float64(m.Sys)/1024/1024)

	// 机器输出格式（便于脚本解析）
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
		fmt.Printf("AVG_MS=%.2f\n", latency.Avg())
		fmt.Printf("P50_MS=%.2f\n", latency.Percentile(50))
		fmt.Printf("P90_MS=%.2f\n", latency.Percentile(90))
		fmt.Printf("P99_MS=%.2f\n", latency.Percentile(99))
		fmt.Printf("P999_MS=%.2f\n", latency.Percentile(99.9))
		fmt.Printf("MAX_MS=%.2f\n", latency.Max())
	}
	fmt.Printf("CPU_USER=%.1f\n", userPct)
	fmt.Printf("CPU_SYS=%.1f\n", sysPct)
	fmt.Printf("CPU_IOWAIT=%.1f\n", iowaitPct)
}
