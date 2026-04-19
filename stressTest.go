package main

import (
	"context"
	"flag"
	"fmt"
	"net/http"
	"sync"
	"sync/atomic"
	"time"
)

type Result struct {
	StatusCode int
	Duration   time.Duration
	Err        error
}

func main() {
	// 命令行参数
	url := flag.String("u", "http://127.0.0.1:9006/", "测试的目标 URL")
	concurrency := flag.Int("c", 100, "并发协程数")
	durationSec := flag.Int("d", 60, "测试持续时间(秒)")
	timeout := flag.Int("t", 10, "每个请求的超时时间(秒)")
	flag.Parse()

	fmt.Printf("开始压测: %s\n", *url)
	fmt.Printf("并发数: %d, 持续时间: %d 秒\n\n", *concurrency, *durationSec)

	// 使用 Context 控制持续时间
	ctx, cancel := context.WithTimeout(context.Background(), time.Duration(*durationSec)*time.Second)
	defer cancel()

	results := make(chan Result, 10000) // 结果缓冲池
	var wg sync.WaitGroup

	// 统计数据
	var successCount int64
	var failureCount int64
	var totalRequestCount int64

	client := &http.Client{
		Timeout: time.Duration(*timeout) * time.Second,
		Transport: &http.Transport{
			MaxIdleConns:        *concurrency,
			MaxIdleConnsPerHost: *concurrency,
			DisableKeepAlives:   false, // 启用长连接以更真实地模拟并发
		},
	}

	startTime := time.Now()

	// 启动并发协程
	for i := 0; i < *concurrency; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for {
				select {
				case <-ctx.Done():
					return
				default:
					atomic.AddInt64(&totalRequestCount, 1)
					reqStart := time.Now()
					resp, err := client.Get(*url)
					reqDuration := time.Since(reqStart)

					res := Result{Duration: reqDuration, Err: err}
					if err != nil {
						atomic.AddInt64(&failureCount, 1)
					} else {
						res.StatusCode = resp.StatusCode
						resp.Body.Close()
						if resp.StatusCode >= 200 && resp.StatusCode < 300 {
							atomic.AddInt64(&successCount, 1)
						} else {
							atomic.AddInt64(&failureCount, 1)
						}
					}
					// 尝试发送结果，如果通道满了则丢弃（避免阻塞测试）
					select {
					case results <- res:
					default:
					}
				}
			}
		}()
	}

	// 等待时间结束或手动取消
	<-ctx.Done()

	// 给一点点时间让最后的请求完成
	wg.Wait()
	close(results)

	totalDuration := time.Since(startTime)

	// 处理结果
	var totalRespTime time.Duration
	statusCodes := make(map[int]int)
	processedResults := 0
	for res := range results {
		totalRespTime += res.Duration
		if res.StatusCode != 0 {
			statusCodes[res.StatusCode]++
		}
		processedResults++
	}

	// 输出统计信息
	fmt.Printf("实际总耗时:   %v\n", totalDuration)
	fmt.Printf("总请求数:     %d\n", atomic.LoadInt64(&totalRequestCount))
	fmt.Printf("成功请求数:   %d\n", atomic.LoadInt64(&successCount))
	fmt.Printf("失败请求数:   %d\n", atomic.LoadInt64(&failureCount))

	if totalRequestCount > 0 {
		avgTime := totalRespTime / time.Duration(processedResults)
		fmt.Printf("平均响应时间: %v (基于采样数据)\n", avgTime)
		fmt.Printf("QPS (每秒请求): %.2f\n", float64(totalRequestCount)/totalDuration.Seconds())
	}

	fmt.Println("\n状态码分布:")
	for code, count := range statusCodes {
		fmt.Printf("  [%d]: %d 次\n", code, count)
	}
}
