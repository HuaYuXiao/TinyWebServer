#ifndef THREADPOOL_H
#define THREADPOOL_H

#include "../CGImysql/sql_connection_pool.h"
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

template <typename T> class threadpool {
public:
  /*thread_number是线程池中线程的数量，max_requests是请求队列中最多允许的、等待处理的请求的数量*/
  threadpool(connection_pool *connPool, int thread_number = 8,
             int max_request = 10000);
  ~threadpool();
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
threadpool<T>::threadpool(connection_pool *connPool, int thread_number,
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

template <typename T> threadpool<T>::~threadpool() {
  {
    std::lock_guard<std::mutex> lock(m_queuelocker);
    m_stop = true;
  }
  m_condition.notify_all();
  for (std::thread &t : m_threads) {
    if (t.joinable()) {
      t.join();
    }
  }
}

template <typename T> bool threadpool<T>::append_p(T *request) {
  {
    std::lock_guard<std::mutex> lock(m_queuelocker);
    if (m_workqueue.size() >= m_max_requests) {
      return false;
    }
    m_workqueue.emplace_back(request);
  }
  m_condition.notify_one();
  return true;
}

template <typename T> void *threadpool<T>::worker(void *arg) {
  threadpool *pool = (threadpool *)arg;
  pool->run();
  return pool;
}

template <typename T> void threadpool<T>::run() {
  while (true) {
    T *request = nullptr;
    {
      std::unique_lock<std::mutex> lock(m_queuelocker);
      m_condition.wait(lock,
                       [this] { return m_stop || !m_workqueue.empty(); });
      if (m_stop && m_workqueue.empty()) {
        return;
      }
      request = m_workqueue.front();
      m_workqueue.pop_front();
    }
    if (request) {
      request->process();
    }
  }
}

#endif
