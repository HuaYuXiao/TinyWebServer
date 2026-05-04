#include "webserver.h"
#include <cerrno>
#include <cstring>

WebServer::WebServer() {
  // http_conn类对象
  users = std::make_unique<http_conn[]>(MAX_FD);

  // root文件夹用于存放服务器的静态资源文件（如HTML、图片等）。
  m_root = (char *)malloc(100);
  strcpy(m_root, "/home/user/TinyWebServer/root");

  // 定时器
  users_timer = std::make_unique<client_data[]>(MAX_FD);

  // Redis 默认配置
  m_redis_host = "127.0.0.1";
  m_redis_port = 6379;
  m_redis_password = "";
  m_redis_pool_size = 16;
  m_redis_db_index = 0;
  m_cache_ttl = 3600;
}

WebServer::~WebServer() {
  close(m_epollfd);
  close(m_listenfd);
  close(m_pipefd[1]);
  close(m_pipefd[0]);
}

void WebServer::init(int port, string user, string passWord,
                     string databaseName, int sql_num, int thread_num) {
  m_port = port;
  m_user = user;
  m_passWord = passWord;
  m_databaseName = databaseName;
  m_sql_num = sql_num;
  m_thread_num = thread_num;
}

void WebServer::init_mysql_pool() {
  // 初始化数据库连接池
  m_connPool = connection_pool::GetInstance();
  m_connPool->init("192.168.19.1", m_user, m_passWord, m_databaseName, 3306,
                   m_sql_num);
}

void WebServer::init_redis_pool() {
  // 初始化 Redis 连接池
  m_redisPool = redis_pool::GetInstance();
  m_redisPool->init(m_redis_host, m_redis_port, m_redis_password,
                    m_redis_pool_size, m_redis_db_index);

  // 初始化 Redis 缓存（含布隆过滤器 + 熔断器）
  RedisCache::GetInstance()->init(m_redisPool);

  std::cout << "[WebServer] Redis 缓存层初始化完成" << std::endl;
}

void WebServer::init_thread_pool() {
  // 线程池（Proactor 模式）
  m_pool = std::make_unique<thread_pool<http_conn>>(m_connPool, m_thread_num);
}

void WebServer::eventListen() {
  // 网络编程基础步骤
  m_listenfd = socket(PF_INET, SOCK_STREAM, 0);
  assert(m_listenfd >= 0);

  // 优雅关闭连接
  if (0 == m_OPT_LINGER) {
    // close 立即返回，丢弃未发送数据
    struct linger tmp = {0, 1};
    setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
  } else if (1 == m_OPT_LINGER) {
    // 阻塞最多1秒发送剩余数据
    struct linger tmp = {1, 1};
    setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
  }

  int ret = 0;
  struct sockaddr_in address;
  bzero(&address, sizeof(address));
  address.sin_family = AF_INET;
  // 监听所有网卡
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  address.sin_port = htons(m_port);

  int flag = 1;
  // 避免 TIME_WAIT 导致端口无法快速复用
  setsockopt(m_listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
  ret = bind(m_listenfd, (struct sockaddr *)&address, sizeof(address));
  assert(ret >= 0);
  ret = listen(m_listenfd, 4096);
  assert(ret >= 0);

  utils.init(TIMESLOT);

  // 创建 epoll 实例，用于统一监听 I/O 事件和信号事件，实现统一事件源
  // epoll 是 Linux 下的高效 I/O 多路复用机制，可以同时监听多个文件描述符的事件
  epoll_event events[MAX_EVENT_NUMBER];
  // 防止 fd 在 fork + exec 时被子进程继承
  m_epollfd = epoll_create1(EPOLL_CLOEXEC);
  assert(m_epollfd != -1);

  // 将监听套接字添加到 epoll 中，监听新连接事件（EPOLLIN）
  utils.addfd(m_epollfd, m_listenfd, false);
  http_conn::m_epollfd = m_epollfd;

  // 使用 socketpair 创建一对 UNIX 域套接字，用于将异步信号转换为同步 epoll 事件
  // 这样可以将信号处理统一到 epoll 的事件循环中，避免复杂的异步信号处理
  ret = socketpair(PF_UNIX, SOCK_STREAM, 0, m_pipefd);
  assert(ret != -1);
  // 设置管道写端为非阻塞，避免信号处理阻塞
  utils.setNonBlocking(m_pipefd[1]);
  // 将管道读端添加到 epoll 中，监听信号事件（通过管道传递）
  utils.addfd(m_epollfd, m_pipefd[0], false);

  // 注册信号处理器：忽略 SIGPIPE（防止管道破裂导致程序崩溃）
  utils.addsig(SIGPIPE, SIG_IGN);
  // 注册 SIGALRM 和 SIGTERM 信号处理器，当信号到达时，将信号写入管道
  utils.addsig(SIGALRM, utils.sig_handler, false);
  utils.addsig(SIGTERM, utils.sig_handler, false);

  // 设置定时器，每 TIMESLOT 秒触发一次 SIGALRM，用于定时任务（如清理超时连接）
  alarm(TIMESLOT);

  // 设置工具类的全局管道和 epoll 描述符，便于信号处理器访问
  Utils::u_pipefd = m_pipefd;
  Utils::u_epollfd = m_epollfd;
}

void WebServer::timer(int connfd, struct sockaddr_in client_address) {
  users[connfd].init(connfd, client_address, m_root, m_user, m_passWord,
                     m_databaseName);

  // 初始化client_data数据
  // 创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
  users_timer[connfd].address = client_address;
  users_timer[connfd].sockfd = connfd;
  util_timer *timer = new util_timer;
  timer->user_data = &users_timer[connfd];
  timer->cb_func = cb_func;
  time_t cur = time(NULL);
  timer->expire = cur + 3 * TIMESLOT;
  users_timer[connfd].timer = timer;
  utils.m_timer_lst.add_timer(timer);
}

// 若有数据传输，则将定时器往后延迟3个单位
void WebServer::adjust_timer(util_timer *timer) {
  time_t cur = time(NULL);
  utils.m_timer_lst.adjust_timer(timer, cur + 3 * TIMESLOT);
}

void WebServer::deal_timer(util_timer *timer, int sockfd) {
  timer->cb_func(&users_timer[sockfd]);
  if (timer) {
    utils.m_timer_lst.del_timer(timer);
  }

//   std::cout << "close fd " << users_timer[sockfd].sockfd << std::endl;
}

bool WebServer::dealclientdata() {
  struct sockaddr_in client_address;
  socklen_t client_addrlength = sizeof(client_address);

  // 非阻塞 listen fd + epoll LT 模式：
  // 一次 epoll 通知到来时，循环 accept 直到 EAGAIN，
  // 避免 10K 并发下因逐次 accept 导致 backlog 溢出。
  while (true) {
    int connfd = accept(m_listenfd, (struct sockaddr *)&client_address,
                        &client_addrlength);
    if (connfd < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        break; // 已清空 backlog，正常
      if (errno == EMFILE || errno == ENFILE) {
        // std::cerr << "accept error: too many open files" << std::endl;
        break;
      }
      if (errno == ECONNABORTED || errno == EINTR)
        continue; // 瞬态错误，重试
      // std::cerr << "accept error: " << strerror(errno) << std::endl;
      break;
    }

    if (http_conn::m_user_count >= MAX_FD) {
      close(connfd);
      break;
    }
    timer(connfd, client_address);
  }
  return true;
}

bool WebServer::dealwithsignal(bool &timeout, bool &stop_server) {
  int ret = 0;
  int sig;
  char signals[1024];
  ret = recv(m_pipefd[0], signals, sizeof(signals), 0);
  if (ret == -1) {
    return false;
  } else if (ret == 0) {
    return false;
  } else {
    for (int i = 0; i < ret; ++i) {
      switch (signals[i]) {
      case SIGALRM: {
        timeout = true;
        break;
      }
      case SIGTERM: {
        stop_server = true;
        break;
      }
      }
    }
  }
  return true;
}

void WebServer::dealwithread(int sockfd) {
  util_timer *timer = users_timer[sockfd].timer;

  // Proactor 模式：主线程完成读操作后，将请求交给线程池处理
  if (users[sockfd].read_once()) {
    // std::cout << "deal with the client("
    //           << inet_ntoa(users[sockfd].get_address()->sin_addr) << ")"
    //           << std::endl;

    // 将该事件放入请求队列
    m_pool->append_p(users.get() + sockfd);

    if (timer) {
      adjust_timer(timer);
    }
  } else {
    deal_timer(timer, sockfd);
  }
}

void WebServer::dealwithwrite(int sockfd) {
  util_timer *timer = users_timer[sockfd].timer;

  // Proactor 模式：主线程完成写操作
  if (users[sockfd].write()) {
    // std::cout << "send data to the client("
    //           << inet_ntoa(users[sockfd].get_address()->sin_addr) << ")"
    //           << std::endl;

    if (timer) {
      adjust_timer(timer);
    }
  } else {
    deal_timer(timer, sockfd);
  }
}

void WebServer::eventLoop() {
  bool timeout = false;
  bool stop_server = false;

  while (!stop_server) {
    int number = epoll_wait(m_epollfd, events, MAX_EVENT_NUMBER, -1);
    if (number < 0 && errno != EINTR) {
      std::cerr << "epoll failure" << std::endl;
      break;
    }

    for (int i = 0; i < number; i++) {
      int sockfd = events[i].data.fd;

      // 处理新到的客户连接
      if (sockfd == m_listenfd) {
        bool flag = dealclientdata();
        if (false == flag)
          continue;
      } else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
        // 服务器端关闭连接，移除对应的定时器
        util_timer *timer = users_timer[sockfd].timer;
        deal_timer(timer, sockfd);
      }
      // 处理信号
      else if ((sockfd == m_pipefd[0]) && (events[i].events & EPOLLIN)) {
        bool flag = dealwithsignal(timeout, stop_server);
        if (!flag)
          std::cerr << "dealclientdata failure" << std::endl;
      }
      // 处理客户连接上接收到的数据
      else if (events[i].events & EPOLLIN) {
        dealwithread(sockfd);
      } else if (events[i].events & EPOLLOUT) {
        dealwithwrite(sockfd);
      }
    }
    if (timeout) {
      utils.timer_handler();

    //   std::cout << "timer tick" << std::endl;

      timeout = false;
    }
  }
}
