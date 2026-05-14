#ifndef THREADPOOL_H
#define THREADPOOL_H

#include "../mysql/mysql_pool.h"
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

template <typename T> class thread_pool {
public:
  /*thread_number是线程池中线程的数量，max_requests是请求队列中最多允许的、等待处理的请求的数量*/
  thread_pool(connection_pool *connPool, int thread_number = 8,
             int max_request = 10000);
  ~thread_pool();
  // 主线程调用：将已读完数据的请求投入工作队列，并唤醒一个空闲 worker
  bool append_p(T *request);

private:
  /*工作线程运行的函数，它不断从工作队列中取出任务并执行之*/
  static void *worker(void *arg);
  void run();

private:
  int m_thread_number;                // 线程池中的线程数
  int m_max_requests;                 // 请求队列中允许的最大请求数
  std::vector<std::thread> m_threads; // Use std::thread for thread management
  std::deque<T *> m_workqueue;        // 请求队列
  std::mutex m_queuelocker;           // 保护请求队列的互斥锁
  std::condition_variable m_condition; // 条件变量，替代自旋信号量
  bool m_stop = false;                // 停止标志
  connection_pool *m_connPool;        // 数据库
};

template <typename T>
thread_pool<T>::thread_pool(connection_pool *connPool, int thread_number,
                          int max_requests)
    : m_thread_number(thread_number), m_max_requests(max_requests),
      m_connPool(connPool) {
  if (thread_number <= 0 || max_requests <= 0)
    throw std::exception();

  for (int i = 0; i < thread_number; ++i) {
    m_threads.emplace_back(
        [this]() { this->run(); }); // Create threads using std::thread
  }
}

template <typename T> thread_pool<T>::~thread_pool() {
  {
    std::lock_guard<std::mutex> lock(m_queuelocker);
    m_stop = true;
  }
  // 为什么 stop 时需要 notify_all()？
  //   每个 worker 在 wait 时持有自己的锁引用。析构函数里设 m_stop
  //   后 notify_all 唤醒所有 worker，它们各自拿到锁后检查谓词为 true
  //   （m_stop || !empty），随后检查 m_stop && empty → return 退出。
  m_condition.notify_all();
  for (std::thread &t : m_threads) {
    if (t.joinable()) {
      t.join();
    }
  }
}

// ── 生产者（主线程在 EPOLLIN 事件后调用） ──────────────────────
template <typename T> bool thread_pool<T>::append_p(T *request) {
  {
    // lock_guard 加锁
    std::lock_guard<std::mutex> lock(m_queuelocker);
    if (m_workqueue.size() >= m_max_requests) {
      return false;
    }
    // 把 request 指针推入队尾
    m_workqueue.emplace_back(request);
  }
  // notify_one() 唤醒一个在 condition_variable 上等待的 worker 线程
  // 为什么先解锁再 notify？
  //   worker 被唤醒后第一件事是 acquire 锁。如果 notify 时仍持锁，
  //   worker 被唤醒 → 尝试拿锁 → 锁还被 append_p 占着 → worker 又睡回去
  //   （"hurry up and wait" 问题）。先解锁再 notify 让 worker 立刻拿到锁。
  m_condition.notify_one();
  return true;
}

template <typename T> void *thread_pool<T>::worker(void *arg) {
  thread_pool *pool = (thread_pool *)arg;
  pool->run();
  return pool;
}

// ── 消费者（每个 worker 线程的主循环） ───────────────────────────
template <typename T> void thread_pool<T>::run() {
  while (true) {
    T *request = nullptr;
    {
      // 为什么用 unique_lock 而非 lock_guard？
      //   condition_variable::wait() 内部需要 unlock/lock 操作，
      //   lock_guard 不支持手动 unlock，unique_lock 支持。
      std::unique_lock<std::mutex> lock(m_queuelocker);
      // 每次被唤醒都重新检查谓词，防止虚假唤醒
      // 条件变量 wait 的三步原子操作：
      // 1. 调用 wait() → 释放锁，线程挂起到操作系统等待队列
      // 2. 收到 notify_one() → 内核唤醒线程，线程返回用户态重新竞争锁
      // 3. 拿到锁 → 检查谓词（lambda）是否为 true
      //    - false → 回到步骤 1（虚假唤醒 / 锁被其他 worker 抢先拿走）
      //    - true  → wait() 返回，锁保持在当前线程手里
      m_condition.wait(lock,
                       [this] { return m_stop || !m_workqueue.empty(); });
      // 到这里锁已重新持有，队列非空或 stop
      if (m_stop && m_workqueue.empty()) {
        return; // 析构时的正常退出路径
      }
      request = m_workqueue.front();
      m_workqueue.pop_front();
    } // unique_lock 析构 → 自动解锁，其他 worker 或生产者可以拿锁
    if (request) {
      // 在锁外处理请求，不阻塞其他 worker 从队列取任务
      request->process();
    }
  }
}

#endif
