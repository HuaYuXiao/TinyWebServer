package main

import (
	"context"
	"flag"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/url"
	"strings"
	"sync"
	"sync/atomic"
	"time"
)

func main() {
	// 1. 命令行参数解析
	concurrency := flag.Int("c", 1, "并发连接数 (Concurrent connections)")
	durationSec := flag.Int("t", 1, "测试持续时间/秒 (Test duration in seconds)")
	targetURL := flag.String("url", "http://192.168.19.128:9006/4", "目标服务器 URL")
	flag.Parse()

	fmt.Printf("🚀 并发数: %d\n", *concurrency)
	fmt.Printf("⏱️ 测试时长: %d 秒\n\n", *durationSec)

	// 2. 核心护城河：调优 HTTP Client
	// 默认的 Client 最大空闲连接极小，会导致高并发下频繁创建连接和端口耗尽 (TIME_WAIT)
	dialer := &net.Dialer{
		Timeout:   2 * time.Second,  // 限制 TCP 连接建立时间，避免 dial 阶段长期卡死
		KeepAlive: 30 * time.Second, // 连接复用的保活时间
	}
	tr := &http.Transport{
		MaxIdleConns:          *concurrency,
		MaxIdleConnsPerHost:   *concurrency,
		MaxConnsPerHost:       *concurrency,
		IdleConnTimeout:       30 * time.Second,
		DisableKeepAlives:     false, // 开启 Keep-Alive 复用连接
		DialContext:           dialer.DialContext,
		TLSHandshakeTimeout:   2 * time.Second,
		ResponseHeaderTimeout: 4 * time.Second,
		ExpectContinueTimeout: 1 * time.Second,
	}
	client := &http.Client{
		Transport: tr,
		Timeout:   5 * time.Second, // 设置超时，防止服务端卡死导致发压机协程堆积
	}

	// 3. 准备表单数据 (application/x-www-form-urlencoded 格式)
	formData := url.Values{}
	formData.Set("name", "华羽霄")
	formData.Set("id_card", "320404200202222815")
	formDataEncoded := formData.Encode()

	// 4. 统计指标变量 (使用原子操作保证并发安全)
	var totalRequests uint64
	var successRequests uint64
	var failedRequests uint64
	var totalReadBytes uint64

	// 响应延迟统计（只统计成功获得响应的请求：err == nil）
	// 平均延迟用 sum/count 计算；最大延迟用 CAS 维护全局最大值。
	var totalLatencyNs uint64
	var maxLatencyNs uint64
	var latencySamples uint64

	// 状态码分布统计：按 resp.StatusCode 计数（仅统计 err == nil 的响应）
	var statusCounts sync.Map // map[int]*uint64
	var totalResponseStatusRequests uint64

	// 5. 并发控制与上下文
	ctx, cancel := context.WithTimeout(context.Background(), time.Duration(*durationSec)*time.Second)
	defer cancel()
	var wg sync.WaitGroup

	startTime := time.Now()

	// 6. 启动 Goroutine 发起洪水攻击
	for i := 0; i < *concurrency; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for {
				// 检查测试时间是否结束
				select {
				case <-ctx.Done():
					return
				default:
				}

				req, _ := http.NewRequestWithContext(ctx, "POST", *targetURL, strings.NewReader(formDataEncoded))
				req.Header.Set("Content-Type", "application/x-www-form-urlencoded")

				start := time.Now()
				resp, err := client.Do(req)
				atomic.AddUint64(&totalRequests, 1)

				if err != nil {
					atomic.AddUint64(&failedRequests, 1)
					continue
				}

				// 读取并丢弃 Body 以便连接复用
				bodyBytes, _ := io.ReadAll(resp.Body)
				resp.Body.Close()

				latencyNs := uint64(time.Since(start).Nanoseconds())

				// 统计响应状态码分布（err == nil 的请求）
				code := resp.StatusCode
				ptr, _ := statusCounts.LoadOrStore(code, new(uint64))
				atomic.AddUint64(ptr.(*uint64), 1)
				atomic.AddUint64(&totalResponseStatusRequests, 1)

				atomic.AddUint64(&totalLatencyNs, latencyNs)
				atomic.AddUint64(&latencySamples, 1)
				for {
					old := atomic.LoadUint64(&maxLatencyNs)
					if latencyNs <= old {
						break
					}
					if atomic.CompareAndSwapUint64(&maxLatencyNs, old, latencyNs) {
						break
					}
				}

				if resp.StatusCode == http.StatusOK {
					atomic.AddUint64(&successRequests, 1)
					atomic.AddUint64(&totalReadBytes, uint64(len(bodyBytes)))
				} else {
					atomic.AddUint64(&failedRequests, 1)
				}
			}
		}()
	}

	// 等待所有协程完成
	wg.Wait()
	actualDuration := time.Since(startTime)

	// 7. 打印压测报告
	fmt.Printf("====== 📊 压测报告 ======\n")
	fmt.Printf("总耗时: %.2f 秒\n", actualDuration.Seconds())
	fmt.Printf("总请求数: %d\n", totalRequests)
	fmt.Printf("成功请求: %d\n", successRequests)
	fmt.Printf("失败请求: %d\n", failedRequests)

	qps := float64(totalRequests) / actualDuration.Seconds()
	fmt.Printf("🔥 QPS (每秒请求数): %.2f req/s\n", qps)

	if latencySamples > 0 {
		avgLatencyMs := (float64(totalLatencyNs) / float64(latencySamples)) / 1e6
		maxLatencyMs := float64(maxLatencyNs) / 1e6
		fmt.Printf("📉 平均响应延迟: %.2f ms\n", avgLatencyMs)
		fmt.Printf("📌 最大响应延迟: %.2f ms\n", maxLatencyMs)
	} else {
		fmt.Printf("📉 平均响应延迟: N/A（无可统计样本）\n")
		fmt.Printf("📌 最大响应延迟: N/A（无可统计样本）\n")
	}
}
