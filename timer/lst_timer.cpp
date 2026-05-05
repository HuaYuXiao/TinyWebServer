#include "lst_timer.h"
#include "../http/http_conn.h"

sort_timer_lst::~sort_timer_lst() {
  for (util_timer *timer : timer_set) {
    delete timer;
  }
}

void sort_timer_lst::add_timer(util_timer *timer) {
  if (!timer)
    return;
  timer_set.insert(timer);
}

void sort_timer_lst::adjust_timer(util_timer *timer, time_t new_expire) {
  auto it = timer_set.find(timer);
  if (it == timer_set.end())
    return;
  timer_set.erase(it);
  timer->expire = new_expire;
  timer_set.insert(timer);
}

void sort_timer_lst::del_timer(util_timer *timer) {
  if (!timer)
    return;
  auto it = timer_set.find(timer);
  if (it != timer_set.end()) {
    timer_set.erase(it);
    delete timer;
  }
}

void sort_timer_lst::tick() {
  if (timer_set.empty())
    return;

  time_t cur = time(NULL);
  while (!timer_set.empty()) {
    auto it = timer_set.begin();
    util_timer *timer = *it;
    if (cur < timer->expire)
      break;
    timer_set.erase(it);
    timer->cb_func(timer->user_data);
    delete timer;
  }
}

void Utils::init(int timeslot) { m_TIMESLOT = timeslot; }

// 对文件描述符设置非阻塞
int Utils::setNonBlocking(int fd) {
  int old_option = fcntl(fd, F_GETFL);
  int new_option = old_option | O_NONBLOCK;
  fcntl(fd, F_SETFL, new_option);
  return old_option;
}

// 将内核事件表注册读事件（LT 模式），开启 EPOLLONESHOT
void Utils::addfd(int epollfd, int fd) {
  epoll_event event;
  event.data.fd = fd;
  event.events = EPOLLIN | EPOLLRDHUP | EPOLLONESHOT;
  epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
  setNonBlocking(fd);
}

// 信号处理函数
void Utils::sig_handler(int sig) {
  // 为保证函数的可重入性，保留原来的errno
  int save_errno = errno;
  int msg = sig;
  send(u_pipefd[1], (char *)&msg, 1, 0);
  errno = save_errno;
}

// 设置信号函数
void Utils::addsig(int sig, void(handler)(int), bool restart) {
  struct sigaction sa;
  memset(&sa, '\0', sizeof(sa));
  sa.sa_handler = handler;
  if (restart)
    sa.sa_flags |= SA_RESTART;
  sigfillset(&sa.sa_mask);
  assert(sigaction(sig, &sa, NULL) != -1);
}

// 定时处理任务，重新定时以不断触发SIGALRM信号
void Utils::timer_handler() {
  m_timer_lst.tick();
  alarm(m_TIMESLOT);
}

void Utils::show_error(int connfd, const char *info) {
  send(connfd, info, strlen(info), 0);
  close(connfd);
}

int *Utils::u_pipefd = 0;
int Utils::u_epollfd = 0;

class Utils;
void cb_func(client_data *user_data) {
  epoll_ctl(Utils::u_epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
  assert(user_data);
  close(user_data->sockfd);
  http_conn::m_user_count--;
}
