#include "http_conn.h"

#include <fstream>
#include <netinet/tcp.h>
#include <mysql/mysql.h>
#include "auth/jwt.h"
#include "auth/password.h"
#include "redis/redis_cache.h"

// JWT 签名密钥（生产环境应从环境变量或配置文件读取）
static const char *JWT_SECRET = "tinywebserver-jwt-secret-2024";

// 定义http响应的一些状态信息
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form =
    "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form =
    "You do not have permission to get file form this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form =
    "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form =
    "There was an unusual problem serving the request file.\n";

std::mutex m_lock;

// 对文件描述符设置非阻塞
int setNonBlocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// 将内核事件表注册读事件（LT 模式），开启 EPOLLONESHOT
void addfd(int epollfd, int fd) {
  epoll_event event;
  event.data.fd = fd;
  event.events = EPOLLIN | EPOLLRDHUP | EPOLLONESHOT;
  epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
  setNonBlocking(fd);
}

// 将事件重置为 EPOLLONESHOT
void modfd(int epollfd, int fd, int ev) {
  epoll_event event;
  event.data.fd = fd;
  event.events = ev | EPOLLONESHOT | EPOLLRDHUP;

  epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

// 从内核时间表删除描述符
void removefd(int epollfd, int fd) {
  epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
  close(fd);
}

int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;
bool http_conn::s_auth_enabled = true;

// 关闭连接，关闭一个连接，客户总量减一
void http_conn::close_conn(bool real_close) {
  if (real_close && (m_sockfd != -1)) {
    // printf("close %d\n", m_sockfd);
    removefd(m_epollfd, m_sockfd);
    m_sockfd = -1;
    m_user_count--;
  }
}

// 初始化连接,外部调用初始化套接字地址
void http_conn::init(int sockfd, const sockaddr_in &addr, char *root,
                     string user, string passwd, string sqlname) {
  m_sockfd = sockfd;
  m_address = addr;

  addfd(m_epollfd, sockfd);
  ++m_user_count;

  // 当浏览器出现连接重置时，可能是网站根目录出错或http响应格式出错或者访问的文件中内容完全为空
  doc_root = root;

  strcpy(sql_user, user.c_str());
  strcpy(sql_passwd, passwd.c_str());
  strcpy(sql_name, sqlname.c_str());

  init();
}

// 初始化新接受的连接
// check_state默认为分析请求行状态
void http_conn::init() {
  mysql = NULL;
  bytes_to_send = 0;
  bytes_have_send = 0;
  m_check_state = CHECK_STATE_REQUESTLINE;
  m_linger = true;
  m_method = GET;
  m_url = 0;
  m_version = 0;
  m_content_length = 0;
  m_host = 0;
  m_start_line = 0;
  m_checked_idx = 0;
  m_read_idx = 0;
  m_write_idx = 0;
  cgi = 0;
  m_cgi_response.clear();
  m_cgi_status = 200;
  m_auth_token.clear();
  m_role.clear();
  m_username.clear();
  m_user_id = 0;

  memset(m_read_buf, '\0', READ_BUFFER_SIZE);
  memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
  memset(m_real_file, '\0', FILENAME_LEN);
}

// 从状态机，用于分析出一行内容
// 返回值为行的读取状态，有LINE_OK,LINE_BAD,LINE_OPEN
http_conn::LINE_STATUS http_conn::parse_line() {
  char temp;
  for (; m_checked_idx < m_read_idx; ++m_checked_idx) {
    temp = m_read_buf[m_checked_idx];
    if (temp == '\r') {
      if ((m_checked_idx + 1) == m_read_idx)
        return LINE_OPEN;
      else if (m_read_buf[m_checked_idx + 1] == '\n') {
        m_read_buf[m_checked_idx++] = '\0';
        m_read_buf[m_checked_idx++] = '\0';
        return LINE_OK;
      }
      return LINE_BAD;
    } else if (temp == '\n') {
      if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r') {
        m_read_buf[m_checked_idx - 1] = '\0';
        m_read_buf[m_checked_idx++] = '\0';
        return LINE_OK;
      }
      return LINE_BAD;
    }
  }
  return LINE_OPEN;
}

// 读取客户数据（LT 模式）
bool http_conn::read_once() {
  if (m_read_idx >= READ_BUFFER_SIZE) {
    return false;
  }
  int bytes_read =
      recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
  if (bytes_read <= 0) {
    return false;
  }
  m_read_idx += bytes_read;
  return true;
}

// 解析http请求行，获得请求方法，目标url及http版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char *text) {
  m_url = strpbrk(text, " \t");
  if (!m_url) {
    return BAD_REQUEST;
  }
  *m_url++ = '\0';
  char *method = text;
  if (strcasecmp(method, "GET") == 0)
    m_method = GET;
  else if (strcasecmp(method, "POST") == 0) {
    m_method = POST;
    cgi = 1;
  } else if (strcasecmp(method, "PUT") == 0) {
    m_method = PUT;
    cgi = 1;
  } else if (strcasecmp(method, "DELETE") == 0) {
    m_method = DELETE;
    cgi = 1;
  } else
    return BAD_REQUEST;
  m_url += strspn(m_url, " \t");
  m_version = strpbrk(m_url, " \t");
  if (!m_version)
    return BAD_REQUEST;
  *m_version++ = '\0';
  m_version += strspn(m_version, " \t");
  if (strcasecmp(m_version, "HTTP/1.1") != 0)
    return BAD_REQUEST;
  if (strncasecmp(m_url, "http://", 7) == 0) {
    m_url += 7;
    m_url = strchr(m_url, '/');
  }

  if (strncasecmp(m_url, "https://", 8) == 0) {
    m_url += 8;
    m_url = strchr(m_url, '/');
  }

  if (!m_url || m_url[0] != '/')
    return BAD_REQUEST;
  // 当url为/时，显示查询界面
  if (strlen(m_url) == 1)
    strcat(m_url, "index.html");
  m_check_state = CHECK_STATE_HEADER;
  return NO_REQUEST;
}

// 解析http请求的一个头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char *text) {
  if (text[0] == '\0') {
    if (m_content_length != 0) {
      m_check_state = CHECK_STATE_CONTENT;
      return NO_REQUEST;
    }
    return GET_REQUEST;
  } else if (strncasecmp(text, "Connection:", 11) == 0) {
    text += 11;
    text += strspn(text, " \t");
    if (strcasecmp(text, "keep-alive") == 0) {
      m_linger = true;
    }
  } else if (strcasecmp(text, "close") == 0) {
    m_linger = false;
  } else if (strncasecmp(text, "Content-length:", 15) == 0) {
    text += 15;
    text += strspn(text, " \t");
    m_content_length = atol(text);
  } else if (strncasecmp(text, "Host:", 5) == 0) {
    text += 5;
    text += strspn(text, " \t");
    m_host = text;
  } else if (strncasecmp(text, "Authorization:", 14) == 0) {
    text += 14;
    text += strspn(text, " \t");
    // 提取 "Bearer <token>"
    const char *bearer = "Bearer ";
    if (strncasecmp(text, bearer, 7) == 0) {
      text += 7;
      m_auth_token = text;
    }
  }

  return NO_REQUEST;
}

// 判断http请求是否被完整读入
http_conn::HTTP_CODE http_conn::parse_content(char *text) {
  if (m_read_idx >= (m_content_length + m_checked_idx)) {
    text[m_content_length] = '\0';
    // POST请求中最后为输入的用户名和密码
    m_string = text;
    return GET_REQUEST;
  }
  return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::process_read() {
  LINE_STATUS line_status = LINE_OK;
  HTTP_CODE ret = NO_REQUEST;
  char *text = 0;

  while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) ||
         ((line_status = parse_line()) == LINE_OK)) {
    text = get_line();
    m_start_line = m_checked_idx;
    switch (m_check_state) {
    case CHECK_STATE_REQUESTLINE: {
      ret = parse_request_line(text);
      if (ret == BAD_REQUEST)
        return BAD_REQUEST;
      break;
    }
    case CHECK_STATE_HEADER: {
      ret = parse_headers(text);
      if (ret == BAD_REQUEST)
        return BAD_REQUEST;
      else if (ret == GET_REQUEST) {
        return do_request();
      }
      break;
    }
    case CHECK_STATE_CONTENT: {
      ret = parse_content(text);
      if (ret == GET_REQUEST)
        return do_request();
      line_status = LINE_OPEN;
      break;
    }
    default:
      return INTERNAL_ERROR;
    }
  }
  return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::do_request() {
  strcpy(m_real_file, doc_root);
  int len = strlen(doc_root);
  // printf("m_url:%s\n", m_url);
  const char *p = strrchr(m_url, '/');

  // ── 认证已关闭：仅允许旧版 SELECT 路由 ────────────────────────
  if (!s_auth_enabled) {
    // /auth/* 和 /api/* 全部禁用
    if (strncmp(m_url, "/auth/", 6) == 0 || strncmp(m_url, "/api/", 5) == 0) {
      m_cgi_status = 403;
      m_cgi_response = "{\"error\":\"auth is disabled\"}";
      return CGI_REQUEST;
    }
    // /4 等旧路由继续走原有逻辑（无需令牌）
  } else {

  // ── /auth/* 认证路由（无需令牌） ──────────────────────────────
  if (cgi == 1 && strncmp(m_url, "/auth/register", 14) == 0) {
    return handle_register();
  }
  if (cgi == 1 && strncmp(m_url, "/auth/login", 11) == 0) {
    return handle_login();
  }

  // ── /api/* CRUD 路由（需要 root 权限） ────────────────────────
  if (cgi == 1 && strncmp(m_url, "/api/", 5) == 0) {
    if (!verify_token())
      return CGI_REQUEST;
    if (!require_role("root"))
      return CGI_REQUEST;
    if (m_method == POST)
      return handle_insert();
    if (m_method == PUT)
      return handle_update();
    if (m_method == DELETE)
      return handle_delete();
    m_cgi_status = 405;
    m_cgi_response = "{\"error\":\"method not allowed\"}";
    return CGI_REQUEST;
  }

  } // end if (s_auth_enabled)

  // 处理cgi
  if (cgi == 1 && *(p + 1) == '4') {
    // 认证开启时，成绩查询也需要登录
    if (s_auth_enabled) {
      if (!verify_token())
        return CGI_REQUEST;
    }
    if (*(p + 1) == '4') {
      auto hex_value = [](char c) -> int {
        if (c >= '0' && c <= '9')
          return c - '0';
        if (c >= 'a' && c <= 'f')
          return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
          return c - 'A' + 10;
        return -1;
      };

      auto url_decode = [&](const std::string &src) -> std::string {
        std::string out;
        out.reserve(src.size());
        for (size_t i = 0; i < src.size(); ++i) {
          char c = src[i];
          if (c == '+') {
            out.push_back(' ');
          } else if (c == '%' && i + 2 < src.size()) {
            int h1 = hex_value(src[i + 1]);
            int h2 = hex_value(src[i + 2]);
            if (h1 >= 0 && h2 >= 0) {
              out.push_back(static_cast<char>(h1 * 16 + h2));
              i += 2;
            } else {
              out.push_back(c);
            }
          } else {
            out.push_back(c);
          }
        }
        return out;
      };

      auto get_param = [&](const std::string &body,
                           const char *key) -> std::string {
        std::string k = std::string(key) + "=";
        size_t pos = body.find(k);
        if (pos == std::string::npos)
          return "";
        pos += k.size();
        size_t end = body.find('&', pos);
        if (end == std::string::npos)
          end = body.size();
        return url_decode(body.substr(pos, end - pos));
      };

      auto json_escape = [](const std::string &s) -> std::string {
        std::string out;
        out.reserve(s.size());
        for (char c : s) {
          switch (c) {
          case '"':
            out += "\\\"";
            break;
          case '\\':
            out += "\\\\";
            break;
          case '\b':
            out += "\\b";
            break;
          case '\f':
            out += "\\f";
            break;
          case '\n':
            out += "\\n";
            break;
          case '\r':
            out += "\\r";
            break;
          case '\t':
            out += "\\t";
            break;
          default:
            out.push_back(c);
            break;
          }
        }
        return out;
      };

      std::string body(m_string ? m_string : "");
      std::string name = get_param(body, "name");
      std::string id_card = get_param(body, "id_card");

      if (name.empty() || id_card.empty()) {
        m_cgi_status = 400;
        m_cgi_response = "{\"error\":\"missing name or id_card\"}";
        return CGI_REQUEST;
      }

      // ── Redis 缓存 + MySQL 回退 ────────────────────────
      // Cache Aside 模式：先查 Redis，命中直接返回，未命中查 DB 并回写缓存。
      // RedisCache::get() 内部已包含三级防护：
      //   1. 布隆过滤器防穿透  2. 熔断器容错降级  3. SETNX 互斥锁防击穿
      std::string cache_key = "exam:score:" + name + ":" + id_card;

      auto cached = RedisCache::GetInstance()->get(
          cache_key,
          // 缓存未命中回调：查 MySQL 并构建 JSON
          [&]() -> std::optional<std::string> {
            connectionRAII mysqlcon(&mysql, connection_pool::GetInstance());

            char esc_name[256]{0};
            char esc_idcard[256]{0};
            mysql_real_escape_string(mysql, esc_name, name.c_str(), name.size());
            mysql_real_escape_string(mysql, esc_idcard, id_card.c_str(),
                                     id_card.size());

            char sql_query[2048]{0};
            snprintf(sql_query, sizeof(sql_query),
                     "SELECT s.student_id, s.name, s.id_card, s.gender, "
                     "s.province, s.school, "
                     "subj.subject_name, sc.score "
                     "FROM student s "
                     "JOIN score sc ON sc.student_id = s.student_id "
                     "JOIN subject subj ON subj.subject_id = sc.subject_id "
                     "WHERE s.name='%s' AND s.id_card='%s';",
                     esc_name, esc_idcard);

            if (mysql_query(mysql, sql_query))
              return std::nullopt;

            MYSQL_RES *result = mysql_store_result(mysql);
            if (!result)
              return std::nullopt;

            MYSQL_ROW row;
            bool has_student = false;
            std::string student_id, real_name, real_idcard, gender, province, school;
            std::string scores_json;
            bool first_score = true;

            while ((row = mysql_fetch_row(result))) {
              auto cell = [&](int idx) -> std::string {
                if (!row[idx])
                  return "";
                return std::string(row[idx]);
              };

              if (!has_student) {
                student_id = cell(0);
                real_name = cell(1);
                real_idcard = cell(2);
                gender = cell(3);
                province = cell(4);
                school = cell(5);
                has_student = true;
              }

              std::string subject_name = cell(6);
              std::string score_str = cell(7);

              if (!first_score)
                scores_json += ",";
              first_score = false;

              if (score_str.empty())
                score_str = "0";

              scores_json += "{\"subject\":\"" + json_escape(subject_name) +
                             "\",\"score\":" + score_str + "}";
            }

            mysql_free_result(result);

            if (!has_student)
              return std::nullopt;

            std::string json;
            json += "{\"student\":{";
            json += "\"student_id\":\"" + json_escape(student_id) + "\",";
            json += "\"name\":\"" + json_escape(real_name) + "\",";
            json += "\"id_card\":\"" + json_escape(real_idcard) + "\",";
            json += "\"gender\":\"" + json_escape(gender) + "\",";
            json += "\"province\":\"" + json_escape(province) + "\",";
            json += "\"school\":\"" + json_escape(school) + "\"";
            json += "},\"scores\":[";
            json += scores_json;
            json += "]}";

            return json;
          },
          3600);

      if (cached.has_value()) {
        m_cgi_status = 200;
        m_cgi_response = cached.value();
      } else {
        m_cgi_status = 404;
        m_cgi_response = "{\"error\":\"student not found\"}";
      }
      return CGI_REQUEST;
    }
  }

  else
    strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);

  if (stat(m_real_file, &m_file_stat) < 0)
    return NO_RESOURCE;

  if (!(m_file_stat.st_mode & S_IROTH))
    return FORBIDDEN_REQUEST;

  if (S_ISDIR(m_file_stat.st_mode))
    return BAD_REQUEST;

  int fd = open(m_real_file, O_RDONLY);
  m_file_address =
      (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);
  return FILE_REQUEST;
}

// ── /auth/register ───────────────────────────────────────────────
http_conn::HTTP_CODE http_conn::handle_register() {
  auto hex_value = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  auto url_decode = [&](const std::string &src) -> std::string {
    std::string out;
    out.reserve(src.size());
    for (size_t i = 0; i < src.size(); ++i) {
      char c = src[i];
      if (c == '+')
        out.push_back(' ');
      else if (c == '%' && i + 2 < src.size()) {
        int h1 = hex_value(src[i + 1]), h2 = hex_value(src[i + 2]);
        if (h1 >= 0 && h2 >= 0) { out.push_back((char)(h1 * 16 + h2)); i += 2; }
        else out.push_back(c);
      } else out.push_back(c);
    }
    return out;
  };
  auto get_param = [&](const std::string &body, const char *key) -> std::string {
    std::string k = std::string(key) + "=";
    size_t pos = body.find(k);
    if (pos == std::string::npos) return "";
    pos += k.size();
    size_t end = body.find('&', pos);
    if (end == std::string::npos) end = body.size();
    return url_decode(body.substr(pos, end - pos));
  };

  std::string body(m_string ? m_string : "");
  std::string username = get_param(body, "username");
  std::string password = get_param(body, "password");

  if (username.empty() || password.empty()) {
    m_cgi_status = 400;
    m_cgi_response = "{\"error\":\"missing username or password\"}";
    return CGI_REQUEST;
  }
  if (username.size() > 64 || password.size() > 128) {
    m_cgi_status = 400;
    m_cgi_response = "{\"error\":\"username or password too long\"}";
    return CGI_REQUEST;
  }

  connectionRAII mysqlcon(&mysql, connection_pool::GetInstance());

  // 检查用户名是否已存在
  char esc_user[128]{0};
  mysql_real_escape_string(mysql, esc_user, username.c_str(), username.size());
  char check_sql[256];
  snprintf(check_sql, sizeof(check_sql),
           "SELECT id FROM server_users WHERE username='%s'", esc_user);
  if (mysql_query(mysql, check_sql)) {
    m_cgi_status = 500;
    m_cgi_response = "{\"error\":\"internal error\"}";
    return CGI_REQUEST;
  }
  MYSQL_RES *res = mysql_store_result(mysql);
  if (res && mysql_num_rows(res) > 0) {
    mysql_free_result(res);
    m_cgi_status = 409;
    m_cgi_response = "{\"error\":\"username already exists\"}";
    return CGI_REQUEST;
  }
  if (res) mysql_free_result(res);

  // 插入新用户
  std::string hash = Password::hash(password);
  char insert_sql[1024];
  snprintf(insert_sql, sizeof(insert_sql),
           "INSERT INTO server_users (username, password_hash, role) "
           "VALUES ('%s', '%s', 'user')",
           esc_user, hash.c_str());
  if (mysql_query(mysql, insert_sql)) {
    m_cgi_status = 500;
    m_cgi_response = "{\"error\":\"internal error\"}";
    return CGI_REQUEST;
  }

  // 签发 JWT
  std::string token = JWT::sign(username, "user", JWT_SECRET);
  m_cgi_status = 201;
  m_cgi_response = "{\"token\":\"" + token + "\",\"role\":\"user\"}";
  return CGI_REQUEST;
}

// ── /auth/login ──────────────────────────────────────────────────
http_conn::HTTP_CODE http_conn::handle_login() {
  auto hex_value = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  auto url_decode = [&](const std::string &src) -> std::string {
    std::string out;
    out.reserve(src.size());
    for (size_t i = 0; i < src.size(); ++i) {
      char c = src[i];
      if (c == '+')
        out.push_back(' ');
      else if (c == '%' && i + 2 < src.size()) {
        int h1 = hex_value(src[i + 1]), h2 = hex_value(src[i + 2]);
        if (h1 >= 0 && h2 >= 0) { out.push_back((char)(h1 * 16 + h2)); i += 2; }
        else out.push_back(c);
      } else out.push_back(c);
    }
    return out;
  };
  auto get_param = [&](const std::string &body, const char *key) -> std::string {
    std::string k = std::string(key) + "=";
    size_t pos = body.find(k);
    if (pos == std::string::npos) return "";
    pos += k.size();
    size_t end = body.find('&', pos);
    if (end == std::string::npos) end = body.size();
    return url_decode(body.substr(pos, end - pos));
  };

  std::string body(m_string ? m_string : "");
  std::string username = get_param(body, "username");
  std::string password = get_param(body, "password");

  if (username.empty() || password.empty()) {
    m_cgi_status = 400;
    m_cgi_response = "{\"error\":\"missing username or password\"}";
    return CGI_REQUEST;
  }

  connectionRAII mysqlcon(&mysql, connection_pool::GetInstance());

  char esc_user[128]{0};
  mysql_real_escape_string(mysql, esc_user, username.c_str(), username.size());
  char sql[512];
  snprintf(sql, sizeof(sql),
           "SELECT id, password_hash, role FROM server_users "
           "WHERE username='%s' LIMIT 1",
           esc_user);
  if (mysql_query(mysql, sql)) {
    m_cgi_status = 500;
    m_cgi_response = "{\"error\":\"internal error\"}";
    return CGI_REQUEST;
  }

  MYSQL_RES *result = mysql_store_result(mysql);
  if (!result || mysql_num_rows(result) == 0) {
    if (result) mysql_free_result(result);
    m_cgi_status = 401;
    m_cgi_response = "{\"error\":\"invalid username or password\"}";
    return CGI_REQUEST;
  }

  MYSQL_ROW row = mysql_fetch_row(result);
  std::string stored_hash = row[1] ? row[1] : "";
  std::string role = row[2] ? row[2] : "user";
  mysql_free_result(result);

  if (!Password::verify(password, stored_hash)) {
    m_cgi_status = 401;
    m_cgi_response = "{\"error\":\"invalid username or password\"}";
    return CGI_REQUEST;
  }

  std::string token = JWT::sign(username, role, JWT_SECRET);
  m_cgi_status = 200;
  m_cgi_response = "{\"token\":\"" + token + "\",\"role\":\"" + role + "\"}";
  return CGI_REQUEST;
}

// ── 权限中间件 ───────────────────────────────────────────────────
bool http_conn::verify_token() {
  if (m_auth_token.empty()) {
    m_cgi_status = 401;
    m_cgi_response = "{\"error\":\"missing Authorization header\"}";
    return false;
  }
  JWTClaims claims;
  if (!JWT::verify(m_auth_token, JWT_SECRET, claims)) {
    m_cgi_status = 401;
    m_cgi_response = "{\"error\":\"invalid or expired token\"}";
    return false;
  }
  m_role = claims.role;
  m_username = claims.sub;
  return true;
}

bool http_conn::require_role(const char *required) {
  if (m_role != required) {
    m_cgi_status = 403;
    m_cgi_response = "{\"error\":\"forbidden: " +
                     std::string(required) + " role required\"}";
    return false;
  }
  return true;
}

void http_conn::write_audit_log(const char *operation, const char *target,
                                const char *detail) {
  connectionRAII mysqlcon(&mysql, connection_pool::GetInstance());
  char esc_user[128]{0}, esc_target[256]{0}, esc_detail[4096]{0};
  mysql_real_escape_string(mysql, esc_user,   m_username.c_str(), m_username.size());
  mysql_real_escape_string(mysql, esc_target, target,  strlen(target));
  mysql_real_escape_string(mysql, esc_detail, detail,  strlen(detail));
  char sql[4608];
  snprintf(sql, sizeof(sql),
           "INSERT INTO audit_log (username, operation, target, detail) "
           "VALUES ('%s','%s','%s','%s')",
           esc_user, operation, esc_target, esc_detail);
  mysql_query(mysql, sql); // best-effort, 不影响主流程
}

// ── URL 解码 / 表单参数提取（局部复用） ─────────────────────────
static std::string url_decode_str(const std::string &src) {
  auto hex_val = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  std::string out;
  out.reserve(src.size());
  for (size_t i = 0; i < src.size(); ++i) {
    char c = src[i];
    if (c == '+') out.push_back(' ');
    else if (c == '%' && i + 2 < src.size()) {
      int h1 = hex_val(src[i + 1]), h2 = hex_val(src[i + 2]);
      if (h1 >= 0 && h2 >= 0) { out.push_back((char)(h1 * 16 + h2)); i += 2; }
      else out.push_back(c);
    } else out.push_back(c);
  }
  return out;
}

static std::string get_form_param(const std::string &body, const char *key) {
  std::string k = std::string(key) + "=";
  size_t pos = body.find(k);
  if (pos == std::string::npos) return "";
  pos += k.size();
  size_t end = body.find('&', pos);
  if (end == std::string::npos) end = body.size();
  return url_decode_str(body.substr(pos, end - pos));
}

// ── POST /api/student — 新增学生（root） ────────────────────────
http_conn::HTTP_CODE http_conn::handle_insert() {
  std::string body(m_string ? m_string : "");
  std::string name     = get_form_param(body, "name");
  std::string id_card  = get_form_param(body, "id_card");
  std::string gender   = get_form_param(body, "gender");
  std::string province = get_form_param(body, "province");
  std::string school   = get_form_param(body, "school");

  if (name.empty() || id_card.empty()) {
    m_cgi_status = 400;
    m_cgi_response = "{\"error\":\"name and id_card are required\"}";
    return CGI_REQUEST;
  }

  connectionRAII mysqlcon(&mysql, connection_pool::GetInstance());

  char esc_name[256]{0}, esc_idcard[256]{0};
  char esc_gender[64]{0}, esc_province[128]{0}, esc_school[256]{0};
  mysql_real_escape_string(mysql, esc_name,     name.c_str(),     name.size());
  mysql_real_escape_string(mysql, esc_idcard,   id_card.c_str(),  id_card.size());
  mysql_real_escape_string(mysql, esc_gender,   gender.c_str(),   gender.size());
  mysql_real_escape_string(mysql, esc_province, province.c_str(), province.size());
  mysql_real_escape_string(mysql, esc_school,   school.c_str(),   school.size());

  char sql[2048];
  snprintf(sql, sizeof(sql),
           "INSERT INTO student (name, id_card, gender, province, school) "
           "VALUES ('%s','%s','%s','%s','%s')",
           esc_name, esc_idcard, esc_gender, esc_province, esc_school);

  if (mysql_query(mysql, sql)) {
    m_cgi_status = 500;
    m_cgi_response = "{\"error\":\"insert failed: " +
                     std::string(mysql_error(mysql)) + "\"}";
    return CGI_REQUEST;
  }

  long long new_id = mysql_insert_id(mysql);

  char audit_detail[1024];
  snprintf(audit_detail, sizeof(audit_detail),
           "{\"student_id\":%lld,\"name\":\"%s\"}", new_id, esc_name);
  char audit_target[64];
  snprintf(audit_target, sizeof(audit_target), "student#%lld", new_id);
  write_audit_log("INSERT", audit_target, audit_detail);

  m_cgi_status = 201;
  m_cgi_response = "{\"student_id\":" + std::to_string(new_id) +
                   ",\"message\":\"student created\"}";
  return CGI_REQUEST;
}

// ── PUT /api/student — 修改学生（root） ─────────────────────────
http_conn::HTTP_CODE http_conn::handle_update() {
  std::string body(m_string ? m_string : "");
  std::string sid      = get_form_param(body, "student_id");
  std::string name     = get_form_param(body, "name");
  std::string id_card  = get_form_param(body, "id_card");
  std::string gender   = get_form_param(body, "gender");
  std::string province = get_form_param(body, "province");
  std::string school   = get_form_param(body, "school");

  if (sid.empty()) {
    m_cgi_status = 400;
    m_cgi_response = "{\"error\":\"student_id is required\"}";
    return CGI_REQUEST;
  }

  // 至少要有一个可更新字段
  if (name.empty() && id_card.empty() && gender.empty() &&
      province.empty() && school.empty()) {
    m_cgi_status = 400;
    m_cgi_response = "{\"error\":\"at least one field to update is required\"}";
    return CGI_REQUEST;
  }

  connectionRAII mysqlcon(&mysql, connection_pool::GetInstance());

  // 动态拼接 SET 子句
  std::string set_clause;
  char buf[512];

  auto append_field = [&](const char *col, const std::string &val) {
    if (val.empty()) return;
    char esc[512]{0};
    mysql_real_escape_string(mysql, esc, val.c_str(), val.size());
    if (!set_clause.empty()) set_clause += ", ";
    snprintf(buf, sizeof(buf), "%s='%s'", col, esc);
    set_clause += buf;
  };

  append_field("name",     name);
  append_field("id_card",  id_card);
  append_field("gender",   gender);
  append_field("province", province);
  append_field("school",   school);

  char esc_sid[32]{0};
  mysql_real_escape_string(mysql, esc_sid, sid.c_str(), sid.size());

  char sql[2048];
  snprintf(sql, sizeof(sql),
           "UPDATE student SET %s WHERE student_id='%s'",
           set_clause.c_str(), esc_sid);

  if (mysql_query(mysql, sql)) {
    m_cgi_status = 500;
    m_cgi_response = "{\"error\":\"update failed: " +
                     std::string(mysql_error(mysql)) + "\"}";
    return CGI_REQUEST;
  }

  unsigned long affected = mysql_affected_rows(mysql);
  if (affected == 0) {
    m_cgi_status = 404;
    m_cgi_response = "{\"error\":\"student not found\"}";
    return CGI_REQUEST;
  }

  char audit_target[64];
  snprintf(audit_target, sizeof(audit_target), "student#%s", esc_sid);
  char audit_detail[1024];
  snprintf(audit_detail, sizeof(audit_detail),
           "{\"set\":\"%s\",\"affected\":%lu}", set_clause.c_str(), affected);
  write_audit_log("UPDATE", audit_target, audit_detail);

  m_cgi_status = 200;
  m_cgi_response = "{\"message\":\"student updated\",\"affected\":" +
                   std::to_string(affected) + "}";
  return CGI_REQUEST;
}

// ── DELETE /api/student — 删除学生（root） ──────────────────────
http_conn::HTTP_CODE http_conn::handle_delete() {
  std::string body(m_string ? m_string : "");
  std::string sid = get_form_param(body, "student_id");

  if (sid.empty()) {
    m_cgi_status = 400;
    m_cgi_response = "{\"error\":\"student_id is required\"}";
    return CGI_REQUEST;
  }

  connectionRAII mysqlcon(&mysql, connection_pool::GetInstance());

  char esc_sid[32]{0};
  mysql_real_escape_string(mysql, esc_sid, sid.c_str(), sid.size());

  char sql[512];
  snprintf(sql, sizeof(sql),
           "DELETE FROM student WHERE student_id='%s'", esc_sid);

  if (mysql_query(mysql, sql)) {
    m_cgi_status = 500;
    m_cgi_response = "{\"error\":\"delete failed: " +
                     std::string(mysql_error(mysql)) + "\"}";
    return CGI_REQUEST;
  }

  unsigned long affected = mysql_affected_rows(mysql);
  if (affected == 0) {
    m_cgi_status = 404;
    m_cgi_response = "{\"error\":\"student not found\"}";
    return CGI_REQUEST;
  }

  char audit_target[64];
  snprintf(audit_target, sizeof(audit_target), "student#%s", esc_sid);
  write_audit_log("DELETE", audit_target, "{\"affected\":1}");

  m_cgi_status = 200;
  m_cgi_response = "{\"message\":\"student deleted\",\"affected\":" +
                   std::to_string(affected) + "}";
  return CGI_REQUEST;
}

void http_conn::unmap() {
  if (m_file_address) {
    munmap(m_file_address, m_file_stat.st_size);
    m_file_address = 0;
  }
}
bool http_conn::write() {
  if (bytes_to_send == 0) {
    modfd(m_epollfd, m_sockfd, EPOLLIN);
    init();
    return true;
  }

  // 在 writev 循环前开启 TCP_CORK，告诉 TCP 栈先积累数据不要立即发送。直到全部数据写完（bytes_to_send <= 0）才清除 cork                   
  int cork = 1;
  setsockopt(m_sockfd, IPPROTO_TCP, TCP_CORK, &cork, sizeof(cork));

  while (1) {
    int temp = writev(m_sockfd, m_iv, m_iv_count);

    if (temp < 0) {
      if (errno == EAGAIN) {
        modfd(m_epollfd, m_sockfd, EPOLLOUT);
        return true;
      }
      unmap();
      cork = 0;
      setsockopt(m_sockfd, IPPROTO_TCP, TCP_CORK, &cork, sizeof(cork));
      return false;
    }

    bytes_have_send += temp;
    bytes_to_send -= temp;
    if (bytes_have_send >= m_iv[0].iov_len) {
      m_iv[0].iov_len = 0;
      m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
      m_iv[1].iov_len = bytes_to_send;
    } else {
      m_iv[0].iov_base = m_write_buf + bytes_have_send;
      m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
    }

    if (bytes_to_send <= 0) {
      unmap();
      cork = 0;
      setsockopt(m_sockfd, IPPROTO_TCP, TCP_CORK, &cork, sizeof(cork));
      modfd(m_epollfd, m_sockfd, EPOLLIN);

      if (m_linger) {
        init();
        return true;
      } else {
        return false;
      }
    }
  }
}
bool http_conn::add_response(const char *format, ...) {
  if (m_write_idx >= WRITE_BUFFER_SIZE)
    return false;
  va_list arg_list;
  va_start(arg_list, format);
  int len = vsnprintf(m_write_buf + m_write_idx,
                      WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
  if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx)) {
    va_end(arg_list);
    return false;
  }
  m_write_idx += len;
  va_end(arg_list);

  return true;
}

bool http_conn::add_status_line(int status, const char *title) {
  return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}
bool http_conn::add_headers(int content_len) {
  return add_content_length(content_len) && add_linger() && add_blank_line();
}
bool http_conn::add_content_length(int content_len) {
  return add_response("Content-Length:%d\r\n", content_len);
}
bool http_conn::add_content_type() {
  return add_response("Content-Type:%s\r\n", "text/html");
}
bool http_conn::add_linger() {
  return add_response("Connection:%s\r\n",
                      (m_linger == true) ? "keep-alive" : "close");
}
bool http_conn::add_blank_line() { return add_response("%s", "\r\n"); }
bool http_conn::add_content(const char *content) {
  return add_response("%s", content);
}
bool http_conn::process_write(HTTP_CODE ret) {
  switch (ret) {
  case CGI_REQUEST: {
    const char *title = ok_200_title;
    if (m_cgi_status == 400)
      title = error_400_title;
    else if (m_cgi_status == 403)
      title = error_403_title;
    else if (m_cgi_status == 404)
      title = error_404_title;
    else if (m_cgi_status == 500)
      title = error_500_title;

    if (m_cgi_response.empty())
      m_cgi_response = "{}";

    add_status_line(m_cgi_status, title);
    add_response("Content-Type:%s\r\n", "application/json");
    add_headers(m_cgi_response.size());
    if (!add_content(m_cgi_response.c_str()))
      return false;
    break;
  }
  case INTERNAL_ERROR: {
    add_status_line(500, error_500_title);
    add_headers(strlen(error_500_form));
    if (!add_content(error_500_form))
      return false;
    break;
  }
  case BAD_REQUEST: {
    add_status_line(404, error_404_title);
    add_headers(strlen(error_404_form));
    if (!add_content(error_404_form))
      return false;
    break;
  }
  case FORBIDDEN_REQUEST: {
    add_status_line(403, error_403_title);
    add_headers(strlen(error_403_form));
    if (!add_content(error_403_form))
      return false;
    break;
  }
  case FILE_REQUEST: {
    add_status_line(200, ok_200_title);
    if (m_file_stat.st_size != 0) {
      add_headers(m_file_stat.st_size);
      m_iv[0].iov_base = m_write_buf;
      m_iv[0].iov_len = m_write_idx;
      m_iv[1].iov_base = m_file_address;
      m_iv[1].iov_len = m_file_stat.st_size;
      m_iv_count = 2;
      bytes_to_send = m_write_idx + m_file_stat.st_size;
      return true;
    } else {
      const char *ok_string = "<html><body></body></html>";
      add_headers(strlen(ok_string));
      if (!add_content(ok_string))
        return false;
    }
  }
  case NO_RESOURCE: {
    add_status_line(404, error_404_title);
    add_headers(strlen(error_404_form));
    if (!add_content(error_404_form))
      return false;
    break;
  }
  default:
    return false;
  }
  m_iv[0].iov_base = m_write_buf;
  m_iv[0].iov_len = m_write_idx;
  m_iv_count = 1;
  bytes_to_send = m_write_idx;
  return true;
}
void http_conn::process() {
  HTTP_CODE read_ret = process_read();
  if (read_ret == NO_REQUEST) {
    modfd(m_epollfd, m_sockfd, EPOLLIN);
    return;
  }
  bool write_ret = process_write(read_ret);
  if (!write_ret) {
    close_conn();
  }
  modfd(m_epollfd, m_sockfd, EPOLLOUT);
}
