#ifndef LST_TIMER
#define LST_TIMER

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <set>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <thread>
#include <time.h>
#include <unistd.h>

class util_timer;

// 面向网络层，存储每个客户端连接的相关数据，包括客户端地址、套接字描述符、定时器等。
struct client_data {
  sockaddr_in address;
  int sockfd;
  util_timer *timer;
};

class util_timer {
public:
  util_timer() {}

  time_t expire;
  void (*cb_func)(client_data *);
  client_data *user_data;
};

class sort_timer_lst {
public:
  sort_timer_lst() = default;
  ~sort_timer_lst();

  void add_timer(util_timer *timer);
  void adjust_timer(util_timer *timer, time_t new_expire);
  void del_timer(util_timer *timer);
  void tick();

private:
  struct timer_cmp {
    bool operator()(const util_timer *a, const util_timer *b) const {
      if (a->expire != b->expire)
        return a->expire < b->expire;
      return a < b; // tiebreaker: 相同 expire 按指针地址排序
    }
  };
  std::set<util_timer *, timer_cmp> timer_set;
};

class Utils {
public:
  Utils() {}
  ~Utils() {}

  void init(int timeslot);

  // 对文件描述符设置非阻塞
  int setNonBlocking(int fd);

  // 将内核事件表注册读事件（LT 模式），开启 EPOLLONESHOT
  void addfd(int epollfd, int fd);

  // 信号处理函数
  static void sig_handler(int sig);

  // 设置信号函数
  void addsig(int sig, void(handler)(int), bool restart = true);

  // 定时处理任务，重新定时以不断触发SIGALRM信号
  void timer_handler();

  void show_error(int connfd, const char *info);

public:
  static int *u_pipefd;
  sort_timer_lst m_timer_lst;
  static int u_epollfd;
  int m_TIMESLOT;
};

void cb_func(client_data *user_data);

#endif
