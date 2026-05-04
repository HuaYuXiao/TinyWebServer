#include "mysql_pool.h"
#include <deque>
#include <iostream>
#include <mutex>
#include <mysql/mysql.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <thread>

using namespace std;

connection_pool::connection_pool()
    : m_CurConn(0) // 初始化列表初始化成员变量
      ,
      m_FreeConn(0) {}

connection_pool *connection_pool::GetInstance() {
  static connection_pool connPool;
  return &connPool;
}

// 构造初始化
void connection_pool::init(const string &url, const string &User,
                           const string &PassWord, const string &DBName,
                           int Port, int MaxConn) {
  m_url = url;
  m_Port = Port;
  m_User = User;
  m_PassWord = PassWord;
  m_DatabaseName = DBName;

  void *ssl_session_data = nullptr;
  unsigned int ssl_session_len = 0;

  for (int i = 0; i < MaxConn; ++i) {
    // 1. 初始化MySQL连接句柄
    MYSQL *mysql_conn = mysql_init(NULL);
    if (!mysql_conn) { // 必须检查返回值是否为NULL
      std::cerr << mysql_error(mysql_conn) << std::endl;
      exit(1);
    }

    // 复用首次连接缓存的 SSL session，后续连接使用 TLS session resumption
    // 避免完整的 TLS 握手，仅做 abbreviated handshake
    if (ssl_session_data) {
      mysql_options(mysql_conn, MYSQL_OPT_SSL_SESSION_DATA, ssl_session_data);
    }

    // 2. 用初始化后的句柄连接数据库（依赖mysql_init的返回值）
    if (!mysql_real_connect(mysql_conn,       // 传入mysql_init返回的句柄
                            url.c_str(),      // MySQL服务器地址
                            User.c_str(),     // 用户名
                            PassWord.c_str(), // 密码
                            DBName.c_str(),   // 要连接的数据库名
                            Port,             // 端口
                            NULL,             // 套接字（通常为NULL）
                            0                 // 连接标志
                            )) {
      mysql_close(mysql_conn); // 失败时也需要关闭句柄释放资源
      std::cerr << mysql_error(mysql_conn) << std::endl;
      exit(1);
    }

    // 缓存第一个连接的 SSL session 数据，供后续连接复用
    if (i == 0) {
      ssl_session_data =
          mysql_get_ssl_session_data(mysql_conn, 0, &ssl_session_len);
    }

    connList.emplace_back(mysql_conn);
    ++m_FreeConn;
  }

  // 释放本地 SSL session 引用（每个连接内部已持有独立拷贝）
  if (ssl_session_data) {
    mysql_free_ssl_session_data(connList.front(), ssl_session_data);
  }

  semaphore_.release(m_FreeConn);

  m_MaxConn = m_FreeConn;
}

// 当有请求时，从数据库连接池中返回一个可用连接，更新使用和空闲连接数
MYSQL *connection_pool::GetConnection() {
  /*
  依赖信号量来保证只有在有可用连接时执行，
  否则阻塞等待，不需要自己判断 connList.empty()
  */
  semaphore_.acquire();

  MYSQL *mysql_conn = nullptr;
  {
    std::lock_guard<std::mutex> lockGuard(lock);
    mysql_conn = connList.front();
    connList.pop_front();

    --m_FreeConn;
    ++m_CurConn;
  }

  // 连接健康检查（带冷却时间，避免每次获取都 ping 造成 RTT 开销）
  time_t now = time(NULL);
  bool need_check = false;
  {
    std::lock_guard<std::mutex> health_lock(m_health_mutex);
    auto it = m_last_health_check.find(mysql_conn);
    if (it == m_last_health_check.end() || now - it->second > 60) {
      need_check = true;
    }
  }

  if (need_check && mysql_ping(mysql_conn) != 0) {
    // 连接已失效，清理旧连接并重连
    {
      std::lock_guard<std::mutex> health_lock(m_health_mutex);
      m_last_health_check.erase(mysql_conn);
    }
    mysql_close(mysql_conn);
    mysql_conn = mysql_init(NULL);
    if (mysql_conn) {
      if (!mysql_real_connect(mysql_conn, m_url.c_str(), m_User.c_str(),
                              m_PassWord.c_str(), m_DatabaseName.c_str(),
                              std::stoi(m_Port), NULL, 0)) {
        mysql_close(mysql_conn);
        mysql_conn = nullptr;
      }
    }

    // 重连失败：归还信号量，避免线程池永久少一个槽位
    if (!mysql_conn) {
      {
        std::lock_guard<std::mutex> lockGuard(lock);
        --m_CurConn;
      }
      semaphore_.release();
      return mysql_conn;
    }
  }

  if (mysql_conn) {
    std::lock_guard<std::mutex> health_lock(m_health_mutex);
    m_last_health_check[mysql_conn] = now;
  }

  return mysql_conn;
}

// 释放当前使用的连接
bool connection_pool::ReleaseConnection(MYSQL *mysql_conn) {
  if (!mysql_conn) {
    return false;
  }

  {
    std::lock_guard<std::mutex> lockGuard(lock);
    connList.emplace_back(mysql_conn);
    ++m_FreeConn;
    --m_CurConn;
  }
  /*
  无论当前计数是多少，都会强制加 1（表示空闲连接数增加），
  若有线程因计数为 0 而阻塞等待，会被唤醒并尝试获取连接。
  */
  semaphore_.release();

  return true;
}

connection_pool::~connection_pool() {
  {
    std::lock_guard<std::mutex> lockGuard(lock);
    if (!connList.empty()) {
      for (MYSQL *mysql_conn : connList) {
        mysql_close(mysql_conn);
      }
      m_CurConn = 0;
      m_FreeConn = 0;
      connList.clear();
    }
  }
  m_last_health_check.clear();
}

connectionRAII::connectionRAII(MYSQL **SQL, connection_pool *connPool) {
  *SQL = connPool->GetConnection();

  conRAII = *SQL;
  poolRAII = connPool;
}

connectionRAII::~connectionRAII() { poolRAII->ReleaseConnection(conRAII); }