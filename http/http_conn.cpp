#include "http_conn.h"

#include <mysql/mysql.h>
#include <fstream>

//定义http响应的一些状态信息
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file form this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the request file.\n";

std::mutex m_lock;

// 对文件描述符设置非阻塞
int setNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

//将内核事件表注册读事件（ET 模式），开启 EPOLLONESHOT
void addfd(int epollfd, int fd)
{
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLRDHUP | EPOLLET | EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setNonBlocking(fd);
}

//将事件重置为 EPOLLONESHOT
void modfd(int epollfd, int fd, int ev)
{
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLONESHOT | EPOLLRDHUP | EPOLLET;

    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

//从内核时间表删除描述符
void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

std::atomic<int> http_conn::m_user_count = 0;
std::atomic<int> http_conn::m_epollfd = -1;

//关闭连接，关闭一个连接，客户总量减一
void http_conn::close_conn(bool real_close)
{
    if (real_close && (m_sockfd != -1))
    {
        printf("close %d\n", m_sockfd);
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        --m_user_count;
    }
}

//初始化连接,外部调用初始化套接字地址
void http_conn::init(int sockfd, const sockaddr_in &addr, std::string root,
                     int close_log, std::string user, std::string passwd, std::string sqlname)
{
    m_sockfd = sockfd;
    m_address = addr;

    addfd(m_epollfd, sockfd);
    ++m_user_count;

    //当浏览器出现连接重置时，可能是网站根目录出错或http响应格式出错或者访问的文件中内容完全为空
    doc_root = root;
    m_close_log = close_log;

    sql_user = user;
    sql_passwd = passwd;
    sql_name = sqlname;

    init();
}

//初始化新接受的连接,check_state默认为分析请求行状态
void http_conn::init()
{
    mysql = NULL;
    bytes_to_send = 0;
    bytes_have_send = 0;
    m_check_state = CHECK_STATE_REQUESTLINE;
    m_linger = false;
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

    m_user_agent.clear();
    m_accept.clear();
    m_accept_language.clear();
    m_accept_encoding.clear();
    m_content_type.clear();
    m_origin.clear();
    m_referer.clear();
    m_upgrade_insecure_requests.clear();
    m_sec_fetch_dest.clear();
    m_sec_fetch_mode.clear();
    m_sec_fetch_site.clear();
    m_sec_fetch_user.clear();
    m_priority.clear();

    m_read_buf.assign(READ_BUFFER_SIZE, '\0');
    m_write_buf.assign(WRITE_BUFFER_SIZE, '\0');
    m_real_file.clear();
}

//从状态机，用于分析出一行内容
//返回值为行的读取状态，有LINE_OK,LINE_BAD,LINE_OPEN
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    for (; m_checked_idx < m_read_idx; ++m_checked_idx)
    {
        temp = m_read_buf[m_checked_idx];
        if (temp == '\r')
        {
            if ((m_checked_idx + 1) == m_read_idx)
                return LINE_OPEN;
            else if (m_read_buf[m_checked_idx + 1] == '\n')
            {
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
        else if (temp == '\n')
        {
            if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r')
            {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}

//循环读取客户数据，直到无数据可读或对方关闭连接（ET 模式）
bool http_conn::read_once()
{
    if (m_read_idx >= READ_BUFFER_SIZE)
    {
        return false;
    }

    while (true)
    {
        int bytes_read = recv(m_sockfd, m_read_buf.data() + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        if (bytes_read > 0)
        {
            m_read_idx += bytes_read;
            if (m_read_idx >= READ_BUFFER_SIZE)
            {
                break;
            }
            continue;
        }

        if (bytes_read == 0)
        {
            // Peer closed the connection gracefully.
            return false;
        }

        // bytes_read < 0
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            // Non-blocking socket has no more data for now.
            break;
        }
        if (errno == EINTR)
        {
            // Interrupted by signal, retry recv.
            continue;
        }
        // Other errors indicate this connection should be closed.
        return false;
    }

    return true;
}

//解析http请求行，获得请求方法，目标url及http版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
    m_url = strpbrk(text, " \t");
    if (!m_url)
    {
        return BAD_REQUEST;
    }
    *m_url++ = '\0';
    char *method = text;
    if (strcasecmp(method, "GET") == 0)
        m_method = GET;
    else if (strcasecmp(method, "POST") == 0)
    {
        m_method = POST;
        cgi = 1;
    }
    else
        return BAD_REQUEST;
    m_url += strspn(m_url, " \t");
    m_version = strpbrk(m_url, " \t");
    if (!m_version)
        return BAD_REQUEST;
    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");
    if (strcasecmp(m_version, "HTTP/1.1") != 0)
        return BAD_REQUEST;
    if (strncasecmp(m_url, "http://", 7) == 0)
    {
        m_url += 7;
        m_url = strchr(m_url, '/');
    }

    if (strncasecmp(m_url, "https://", 8) == 0)
    {
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

//解析http请求的一个头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
    if (text[0] == '\0')
    {
        if (m_content_length != 0)
        {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        return GET_REQUEST;
    }
    else if (strncasecmp(text, "Connection:", 11) == 0)
    {
        text += 11;
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0)
        {
            m_linger = true;
        }
    }
    else if (strncasecmp(text, "Content-length:", 15) == 0)
    {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
        // 限制最大请求体大小为 1MB，防止 DoS 攻击
        if (m_content_length > MAX_CONTENT_LENGTH)
        {
            return BAD_REQUEST;
        }
    }
    else if (strncasecmp(text, "Host:", 5) == 0)
    {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    else if (strncasecmp(text, "User-Agent:", 11) == 0)
    {
        text += 11;
        text += strspn(text, " \t");
        m_user_agent = text;
    }
    else if (strncasecmp(text, "Accept:", 7) == 0)
    {
        text += 7;
        text += strspn(text, " \t");
        m_accept = text;
    }
    else if (strncasecmp(text, "Accept-Language:", 16) == 0)
    {
        text += 16;
        text += strspn(text, " \t");
        m_accept_language = text;
    }
    else if (strncasecmp(text, "Accept-Encoding:", 16) == 0)
    {
        text += 16;
        text += strspn(text, " \t");
        m_accept_encoding = text;
    }
    else if (strncasecmp(text, "Content-Type:", 13) == 0)
    {
        text += 13;
        text += strspn(text, " \t");
        m_content_type = text;
    }
    else if (strncasecmp(text, "Origin:", 7) == 0)
    {
        text += 7;
        text += strspn(text, " \t");
        m_origin = text;
    }
    else if (strncasecmp(text, "Referer:", 8) == 0)
    {
        text += 8;
        text += strspn(text, " \t");
        m_referer = text;
    }
    else if (strncasecmp(text, "Upgrade-Insecure-Requests:", 25) == 0)
    {
        text += 25;
        text += strspn(text, " \t");
        m_upgrade_insecure_requests = text;
    }
    else if (strncasecmp(text, "Sec-Fetch-Dest:", 15) == 0)
    {
        text += 15;
        text += strspn(text, " \t");
        m_sec_fetch_dest = text;
    }
    else if (strncasecmp(text, "Sec-Fetch-Mode:", 15) == 0)
    {
        text += 15;
        text += strspn(text, " \t");
        m_sec_fetch_mode = text;
    }
    else if (strncasecmp(text, "Sec-Fetch-Site:", 15) == 0)
    {
        text += 15;
        text += strspn(text, " \t");
        m_sec_fetch_site = text;
    }
    else if (strncasecmp(text, "Sec-Fetch-User:", 15) == 0)
    {
        text += 15;
        text += strspn(text, " \t");
        m_sec_fetch_user = text;
    }
    else if (strncasecmp(text, "Priority:", 9) == 0)
    {
        text += 9;
        text += strspn(text, " \t");
        m_priority = text;
    }

    return NO_REQUEST;
}

//判断http请求是否被完整读入
http_conn::HTTP_CODE http_conn::parse_content(char *text)
{
    if (m_read_idx >= (m_content_length + m_checked_idx))
    {
        text[m_content_length] = '\0';
        // 验证 m_string 指向的缓冲区是否在有效范围内
        if (text >= m_read_buf.data() && text + m_content_length <= m_read_buf.data() + m_read_buf.size())
        {
            m_string = text;
        }
        else
        {
            return BAD_REQUEST; // 缓冲区越界
        }
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::process_read()
{
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char *text = 0;

    while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) || ((line_status = parse_line()) == LINE_OK))
    {
        text = get_line();
        m_start_line = m_checked_idx;
        LOG_INFO("%s", text);
        switch (m_check_state)
        {
        case CHECK_STATE_REQUESTLINE:
        {
            ret = parse_request_line(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            break;
        }
        case CHECK_STATE_HEADER:
        {
            ret = parse_headers(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            else if (ret == GET_REQUEST)
            {
                return do_request();
            }
            break;
        }
        case CHECK_STATE_CONTENT:
        {
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

http_conn::HTTP_CODE http_conn::do_request()
{
    m_real_file = doc_root;
    size_t len = doc_root.length();
    //printf("m_url:%s\n", m_url);
    const char *p = strrchr(m_url, '/');

    //处理cgi
    if (cgi == 1 && *(p + 1) == '4')
    {
        if (*(p + 1) == '4')
        {
            auto hex_value = [](char c) -> int
            {
                if (c >= '0' && c <= '9')
                    return c - '0';
                if (c >= 'a' && c <= 'f')
                    return c - 'a' + 10;
                if (c >= 'A' && c <= 'F')
                    return c - 'A' + 10;
                return -1;
            };

            auto url_decode = [&](const std::string &src) -> std::string
            {
                std::string out;
                out.reserve(src.size());
                for (size_t i = 0; i < src.size(); ++i)
                {
                    char c = src[i];
                    if (c == '+')
                    {
                        out.push_back(' ');
                    }
                    else if (c == '%' && i + 2 < src.size())
                    {
                        int h1 = hex_value(src[i + 1]);
                        int h2 = hex_value(src[i + 2]);
                        if (h1 >= 0 && h2 >= 0)
                        {
                            out.push_back(static_cast<char>(h1 * 16 + h2));
                            i += 2;
                        }
                        else
                        {
                            out.push_back(c);
                        }
                    }
                    else
                    {
                        out.push_back(c);
                    }
                }
                return out;
            };

            auto get_param = [&](const std::string &body, const char *key) -> std::string
            {
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

            auto json_escape = [](const std::string &s) -> std::string
            {
                std::string out;
                out.reserve(s.size());
                for (char c : s)
                {
                    switch (c)
                    {
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

            // 1) 从 POST 请求体中读取参数，并进行 URL 解码后的参数解析
            std::string body(m_string ? m_string : "");
            std::string name = get_param(body, "name");
            std::string id_card = get_param(body, "id_card");

            // 2) 如果缺少任一必要参数，则返回客户端 400 错误
            if (name.empty() || id_card.empty())
            {
                m_cgi_status = 400;
                m_cgi_response = "{\"error\":\"missing name or id_card\"}";
                return CGI_REQUEST;
            }

            // 3) 初始化预编译语句对象，MySQL 将在后续步骤中使用占位符绑定参数
            //    该方式可以把用户输入当成纯数据处理，避免拼接 SQL 字符串时出现注入。
            MYSQL_STMT *stmt = mysql_stmt_init(mysql);
            if (!stmt)
            {
                m_cgi_status = 500;
                m_cgi_response = "{\"error\":\"mysql_stmt_init failed\"}";
                return CGI_REQUEST;
            }

            // 4) SQL 查询模板中的 ? 是占位符，此时用户输入尚未拼接到 SQL 中
            const char *query = "SELECT s.student_id, s.name, s.id_card, s.gender, s.province, s.school, "
                                "subj.subject_name, sc.score "
                                "FROM student s "
                                "JOIN score sc ON sc.student_id = s.student_id "
                                "JOIN subject subj ON subj.subject_id = sc.subject_id "
                                "WHERE s.name=? AND s.id_card=?;";

            if (mysql_stmt_prepare(stmt, query, strlen(query)))
            {
                mysql_stmt_close(stmt);
                m_cgi_status = 500;
                m_cgi_response = "{\"error\":\"mysql_stmt_prepare failed\"}";
                return CGI_REQUEST;
            }

            // 5) 绑定实际参数到 SQL 占位符位置，name 对应第一个 ?, id_card 对应第二个 ?
            MYSQL_BIND bind[2];
            memset(bind, 0, sizeof(bind));

            bind[0].buffer_type = MYSQL_TYPE_STRING;
            bind[0].buffer = (char *)name.c_str();
            bind[0].buffer_length = name.length();

            bind[1].buffer_type = MYSQL_TYPE_STRING;
            bind[1].buffer = (char *)id_card.c_str();
            bind[1].buffer_length = id_card.length();

            if (mysql_stmt_bind_param(stmt, bind))
            {
                mysql_stmt_close(stmt);
                m_cgi_status = 500;
                m_cgi_response = "{\"error\":\"mysql_stmt_bind_param failed\"}";
                return CGI_REQUEST;
            }

            // 6) 执行绑定好的预编译语句，此时 MySQL 会把 name 和 id_card 作为数据处理
            if (mysql_stmt_execute(stmt))
            {
                mysql_stmt_close(stmt);
                m_cgi_status = 500;
                m_cgi_response = "{\"error\":\"mysql_stmt_execute failed\"}";
                return CGI_REQUEST;
            }

            // 7) 获取结果元数据，用于后续将每一列绑定到本地缓冲区
            MYSQL_RES *result = mysql_stmt_result_metadata(stmt);
            if (!result)
            {
                mysql_stmt_close(stmt);
                m_cgi_status = 500;
                m_cgi_response = "{\"error\":\"mysql_stmt_result_metadata failed\"}";
                return CGI_REQUEST;
            }

            // 8) 定义结果列的缓冲区，顺序必须和 SELECT 语句中列的顺序一致
            MYSQL_BIND result_bind[8];
            memset(result_bind, 0, sizeof(result_bind));

            char student_id[50], real_name[100], real_idcard[50], gender[10], province[50], school[100], subject_name[100], score_str[10];
            unsigned long lengths[8];

            result_bind[0].buffer_type = MYSQL_TYPE_STRING;
            result_bind[0].buffer = student_id;
            result_bind[0].buffer_length = sizeof(student_id);
            result_bind[0].length = &lengths[0];

            result_bind[1].buffer_type = MYSQL_TYPE_STRING;
            result_bind[1].buffer = real_name;
            result_bind[1].buffer_length = sizeof(real_name);
            result_bind[1].length = &lengths[1];

            result_bind[2].buffer_type = MYSQL_TYPE_STRING;
            result_bind[2].buffer = real_idcard;
            result_bind[2].buffer_length = sizeof(real_idcard);
            result_bind[2].length = &lengths[2];

            result_bind[3].buffer_type = MYSQL_TYPE_STRING;
            result_bind[3].buffer = gender;
            result_bind[3].buffer_length = sizeof(gender);
            result_bind[3].length = &lengths[3];

            result_bind[4].buffer_type = MYSQL_TYPE_STRING;
            result_bind[4].buffer = province;
            result_bind[4].buffer_length = sizeof(province);
            result_bind[4].length = &lengths[4];

            result_bind[5].buffer_type = MYSQL_TYPE_STRING;
            result_bind[5].buffer = school;
            result_bind[5].buffer_length = sizeof(school);
            result_bind[5].length = &lengths[5];

            result_bind[6].buffer_type = MYSQL_TYPE_STRING;
            result_bind[6].buffer = subject_name;
            result_bind[6].buffer_length = sizeof(subject_name);
            result_bind[6].length = &lengths[6];

            result_bind[7].buffer_type = MYSQL_TYPE_STRING;
            result_bind[7].buffer = score_str;
            result_bind[7].buffer_length = sizeof(score_str);
            result_bind[7].length = &lengths[7];

            // 9) 将结果列绑定到本地缓冲区，fetch 读取时会把每一列写入到这些缓存中
            if (mysql_stmt_bind_result(stmt, result_bind))
            {
                mysql_free_result(result);
                mysql_stmt_close(stmt);
                m_cgi_status = 500;
                m_cgi_response = "{\"error\":\"mysql_stmt_bind_result failed\"}";
                return CGI_REQUEST;
            }

            if (mysql_stmt_store_result(stmt))
            {
                mysql_free_result(result);
                mysql_stmt_close(stmt);
                m_cgi_status = 500;
                m_cgi_response = "{\"error\":\"mysql_stmt_store_result failed\"}";
                return CGI_REQUEST;
            }

            MYSQL_ROW row;
            bool has_student = false;
            std::string scores_json;
            bool first_score = true;

            // 10) 遍历结果集中的每一行
            //     mysql_stmt_fetch 会把已经绑定的 result_bind 缓冲区填充当前行的列值
            while (!mysql_stmt_fetch(stmt))
            {
                if (!has_student)
                {
                    has_student = true;
                }

                if (!first_score)
                    scores_json += ",";
                first_score = false;

                std::string subj_name(subject_name, lengths[6]);
                std::string scr_str(score_str, lengths[7]);
                if (scr_str.empty())
                    scr_str = "0";

                scores_json += "{\"subject\":\"" + json_escape(subj_name) + "\",\"score\":" + scr_str + "}";
            }

            mysql_free_result(result);
            mysql_stmt_close(stmt);

            if (!has_student)
            {
                m_cgi_status = 404;
                m_cgi_response = "{\"error\":\"student not found\"}";
                return CGI_REQUEST;
            }

            std::string json;
            json += "{\"student\":{";
            json += "\"student_id\":\"" + json_escape(std::string(student_id, lengths[0])) + "\",";
            json += "\"name\":\"" + json_escape(std::string(real_name, lengths[1])) + "\",";
            json += "\"id_card\":\"" + json_escape(std::string(real_idcard, lengths[2])) + "\",";
            json += "\"gender\":\"" + json_escape(std::string(gender, lengths[3])) + "\",";
            json += "\"province\":\"" + json_escape(std::string(province, lengths[4])) + "\",";
            json += "\"school\":\"" + json_escape(std::string(school, lengths[5])) + "\"";
            json += "},\"scores\":[";
            json += scores_json;
            json += "]}";

            m_cgi_status = 200;
            m_cgi_response = json;
            return CGI_REQUEST;
        }
    }
    else
        m_real_file.append(m_url);

    if (stat(m_real_file.c_str(), &m_file_stat) < 0)
        return NO_RESOURCE;

    if (!(m_file_stat.st_mode & S_IROTH))
        return FORBIDDEN_REQUEST;

    if (S_ISDIR(m_file_stat.st_mode))
        return BAD_REQUEST;

    int fd = open(m_real_file.c_str(), O_RDONLY);
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    return FILE_REQUEST;
}

void http_conn::unmap()
{
    if (m_file_address)
    {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}
bool http_conn::write()
{
    int temp = 0;

    if (bytes_to_send == 0)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        init();
        return true;
    }

    while (1)
    {
        temp = writev(m_sockfd, m_iv, m_iv_count);

        if (temp < 0)
        {
            if (errno == EAGAIN)
            {
                modfd(m_epollfd, m_sockfd, EPOLLOUT);
                return true;
            }
            unmap();
            return false;
        }

        bytes_have_send += temp;
        bytes_to_send -= temp;
        if (bytes_have_send >= m_iv[0].iov_len)
        {
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        else
        {
            m_iv[0].iov_base = m_write_buf.data() + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }

        if (bytes_to_send <= 0)
        {
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN);

            if (m_linger)
            {
                init();
                return true;
            }
            else
            {
                return false;
            }
        }
    }
}
bool http_conn::add_response(const char *format, ...)
{
    if (m_write_idx >= WRITE_BUFFER_SIZE)
        return false;
    va_list arg_list;
    va_start(arg_list, format);
    int len = vsnprintf(m_write_buf.data() + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx))
    {
        va_end(arg_list);
        return false;
    }
    m_write_idx += len;
    va_end(arg_list);

    LOG_INFO("request:%s", m_write_buf.data());

    return true;
}
bool http_conn::add_status_line(int status, const char *title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}
bool http_conn::add_headers(int content_len)
{
    return add_content_length(content_len) && add_linger() &&
           add_blank_line();
}
bool http_conn::add_content_length(int content_len)
{
    return add_response("Content-Length:%d\r\n", content_len);
}
bool http_conn::add_content_type()
{
    return add_response("Content-Type:%s\r\n", "text/html");
}
bool http_conn::add_linger()
{
    return add_response("Connection:%s\r\n", (m_linger == true) ? "keep-alive" : "close");
}
bool http_conn::add_blank_line()
{
    return add_response("%s", "\r\n");
}
bool http_conn::add_content(const char *content)
{
    return add_response("%s", content);
}
bool http_conn::process_write(HTTP_CODE ret)
{
    switch (ret)
    {
    case CGI_REQUEST:
    {
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
    case INTERNAL_ERROR:
    {
        add_status_line(500, error_500_title);
        add_headers(strlen(error_500_form));
        if (!add_content(error_500_form))
            return false;
        break;
    }
    case BAD_REQUEST:
    {
        add_status_line(404, error_404_title);
        add_headers(strlen(error_404_form));
        if (!add_content(error_404_form))
            return false;
        break;
    }
    case FORBIDDEN_REQUEST:
    {
        add_status_line(403, error_403_title);
        add_headers(strlen(error_403_form));
        if (!add_content(error_403_form))
            return false;
        break;
    }
    case FILE_REQUEST:
    {
        add_status_line(200, ok_200_title);
        if (m_file_stat.st_size != 0)
        {
            add_headers(m_file_stat.st_size);
            m_iv[0].iov_base = m_write_buf.data();
            m_iv[0].iov_len = m_write_idx;
            m_iv[1].iov_base = m_file_address;
            m_iv[1].iov_len = m_file_stat.st_size;
            m_iv_count = 2;
            bytes_to_send = m_write_idx + m_file_stat.st_size;
            return true;
        }
        else
        {
            const char *ok_string = "<html><body></body></html>";
            add_headers(strlen(ok_string));
            if (!add_content(ok_string))
                return false;
            break;
        }
    }
    case NO_RESOURCE:
    {
        add_status_line(404, error_404_title);
        add_headers(strlen(error_404_form));
        if (!add_content(error_404_form))
            return false;
        break;
    }
    default:
        return false;
    }
    m_iv[0].iov_base = m_write_buf.data();
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}
void http_conn::process()
{
    HTTP_CODE read_ret = process_read();
    if (read_ret == NO_REQUEST)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        return;
    }
    bool write_ret = process_write(read_ret);
    if (!write_ret)
    {
        close_conn();
    }
    modfd(m_epollfd, m_sockfd, EPOLLOUT);
}
