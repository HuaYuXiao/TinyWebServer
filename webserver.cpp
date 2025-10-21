#include "webserver.h"

using namespace std;

WebServer::WebServer()
{
    //http_conn类对象：使用unique_ptr初始化动态数组
    users = std::make_unique<http_conn[]>(MAX_FD);

    //root文件夹路径
    char server_path[200];
    getcwd(server_path, 200);
    char root[6] = "/root";
    m_root = (char *)malloc(strlen(server_path) + strlen(root) + 1);
    strcpy(m_root, server_path);
    strcat(m_root, root);

    //定时器：使用unique_ptr初始化动态数组
    users_timer = std::make_unique<client_data[]>(MAX_FD);
}

WebServer::~WebServer()
{
    close(m_epollfd);
    close(m_listenfd);
    close(m_pipefd[1]);
    close(m_pipefd[0]);
    free(m_root);  // 只需要释放m_root，其他由智能指针自动管理
}

void WebServer::init(
    int port, string user, string passWord, string databaseName, int log_write, int opt_linger, int trigmode, int sql_num, int thread_num, int close_log, int actor_model
){
    m_port = port;
    m_user = user;
    m_passWord = passWord;
    m_databaseName = databaseName;
    m_sql_num = sql_num;
    m_thread_num = thread_num;
    m_log_write = log_write;
    m_OPT_LINGER = opt_linger;
    m_TRIGMode = trigmode;
    m_close_log = close_log;
    m_actormodel = actor_model;

    std::cout << "WebServer initialized with port: " << m_port << ", user: " << m_user 
              << ", database: " << m_databaseName << ", thread_num: " << m_thread_num << std::endl;
}

void WebServer::trig_mode()
{
    /*
        观察 m_TRIGMode 与两个模式的对应关系：
        m_TRIGMode 的二进制只有 2 位（0-3 对应 00、01、10、11）。
        低位（第 0 位）恰好对应 m_CONNTrigmode（0/1）。
        高位（第 1 位）恰好对应 m_LISTENTrigmode（0/1）。
        位运算（&、>>）是 CPU 直接支持的底层操作，执行速度远快于多分支 if-else 判断（避免了条件跳转带来的流水线中断）。
    */
    // 提取第0位（低位）作为 m_CONNTrigmode（0:LT，1:ET）
    m_CONNTrigmode = m_TRIGMode & 1;
    // 提取第1位（高位）作为 m_LISTENTrigmode（右移1位后取第0位）
    m_LISTENTrigmode = (m_TRIGMode >> 1) & 1;
}

void WebServer::log_write()
{
    if (0 == m_close_log)
    {
        //初始化日志
        if (1 == m_log_write)
            Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, 800);
        else
            Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, 0);
    }
}

void WebServer::sql_pool()
{
    //初始化数据库连接池
    m_connPool = connection_pool::GetInstance();
    m_connPool->init("localhost", m_user, m_passWord, m_databaseName, 3306, m_sql_num, m_close_log);

    //初始化数据库读取表
    // users[0] 通过智能指针数组的 [] 运算符获取数组第一个元素的引用
    users[0].initmysql_result(m_connPool);
}

void WebServer::thread_pool()
{
    //线程池
    m_pool = std::make_unique<threadpool<http_conn>>(m_actormodel, m_connPool, m_thread_num);
    std::cout << "Thread pool created with " << m_thread_num << " threads." << std::endl;
}

void WebServer::eventListen()
{
    // 网络编程核心步骤：创建监听套接字、设置选项、绑定地址、监听连接，以及初始化epoll和信号处理
    
    // 1. 创建监听套接字（TCP）
    // PF_INET：使用IPv4协议族
    // SOCK_STREAM：使用面向连接的TCP套接字
    // 0：默认协议（此处为TCP）
    m_listenfd = socket(PF_INET, SOCK_STREAM, 0);
    // 断言检查套接字创建是否成功（若失败，程序终止并提示错误）
    assert(m_listenfd >= 0);


    // 2. 设置套接字关闭选项（SO_LINGER），控制连接关闭时的行为（优雅关闭）
    // m_OPT_LINGER为配置参数，决定关闭连接时是否等待未发送数据
    if (0 == m_OPT_LINGER)
    {
        // 选项1：不等待未发送数据，关闭时立即返回
        // linger结构体：l_onoff=0（关闭SO_LINGER），l_linger=1（忽略，仅占位）
        struct linger tmp = {0, 1};
        // setsockopt：设置套接字选项
        // SOL_SOCKET：通用套接字选项层级
        // SO_LINGER：控制close()调用的行为
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    }
    else if (1 == m_OPT_LINGER)
    {
        // 选项2：等待未发送数据，超时后强制关闭
        // l_onoff=1（启用SO_LINGER），l_linger=1（等待1秒）
        struct linger tmp = {1, 1};
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    }


    // 3. 初始化服务器地址结构体并绑定套接字
    int ret = 0;
    struct sockaddr_in address;  // IPv4地址结构体
    bzero(&address, sizeof(address));  // 清零地址结构体
    address.sin_family = AF_INET;      // 地址族为IPv4
    // 绑定到所有本地网络接口（INADDR_ANY），转换为网络字节序（htonl）
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    // 绑定端口（m_port），转换为网络字节序（htons）
    address.sin_port = htons(m_port);

    // 允许端口复用（解决服务器重启时“地址已被占用”的问题）
    int flag = 1;
    setsockopt(m_listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));

    // 绑定套接字到指定地址和端口
    ret = bind(m_listenfd, (struct sockaddr *)&address, sizeof(address));
    assert(ret >= 0);  // 断言绑定成功

    // 开始监听连接（第二个参数5：监听队列最大长度，超过的连接会被拒绝）
    ret = listen(m_listenfd, 5);
    assert(ret >= 0);  // 断言监听成功


    // 4. 初始化工具类（定时器相关）
    // TIMESLOT：定时器最小超时单位（在类常量中定义为5秒）
    utils.init(TIMESLOT);


    // 5. 初始化epoll（I/O多路复用核心）
    // 创建epoll内核事件表（参数5为历史遗留，无实际意义）
    m_epollfd = epoll_create(5);
    assert(m_epollfd != -1);  // 断言epoll创建成功

    // 将监听套接字添加到epoll事件集
    // 参数：epoll描述符、监听套接字、是否边缘触发（此处为false）、监听触发模式（m_LISTENTrigmode）
    utils.addfd(m_epollfd, m_listenfd, false, m_LISTENTrigmode);
    // 将epoll描述符设置为http_conn类的静态成员，方便所有连接对象访问epoll
    http_conn::m_epollfd = m_epollfd;


    // 6. 创建信号管道（用于处理信号事件）
    // socketpair创建一对相互连接的UNIX域套接字，用于进程内信号传递
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, m_pipefd);
    assert(ret != -1);  // 断言管道创建成功
    // 将管道写端设为非阻塞（避免信号处理时阻塞）
    utils.setnonblocking(m_pipefd[1]);
    // 将管道读端添加到epoll事件集（监听读事件，用于接收信号）
    utils.addfd(m_epollfd, m_pipefd[0], false, 0);


    // 7. 注册信号处理函数
    // 忽略SIGPIPE信号（避免向已关闭的连接写数据时程序崩溃）
    utils.addsig(SIGPIPE, SIG_IGN);
    // 注册SIGALRM（定时信号）处理函数为utils.sig_handler
    utils.addsig(SIGALRM, utils.sig_handler, false);
    // 注册SIGTERM（进程终止信号）处理函数为utils.sig_handler
    utils.addsig(SIGTERM, utils.sig_handler, false);


    // 8. 启动定时器（每隔TIMESLOT秒触发一次SIGALRM信号）
    alarm(TIMESLOT);


    // 9. 将管道和epoll描述符设置为工具类的静态成员，供信号处理函数访问
    Utils::u_pipefd = m_pipefd;    // 信号管道描述符
    Utils::u_epollfd = m_epollfd;  // epoll描述符
}

void WebServer::timer(int connfd, struct sockaddr_in client_address)
{
    users[connfd].init(connfd, client_address, m_root, m_CONNTrigmode, m_close_log, m_user, m_passWord, m_databaseName);

    //初始化client_data数据
    //创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
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

//若有数据传输，则将定时器往后延迟3个单位
//并对新的定时器在链表上的位置进行调整
void WebServer::adjust_timer(util_timer *timer)
{
    time_t cur = time(NULL);
    timer->expire = cur + 3 * TIMESLOT;
    utils.m_timer_lst.adjust_timer(timer);

    LOG_INFO("%s", "adjust timer once");
}

void WebServer::deal_timer(util_timer *timer, int sockfd)
{
    timer->cb_func(&users_timer[sockfd]);
    if (timer)
    {
        utils.m_timer_lst.del_timer(timer);
    }

    LOG_INFO("close fd %d", users_timer[sockfd].sockfd);
}

bool WebServer::dealclientdata()
{
    struct sockaddr_in client_address;
    socklen_t client_addrlength = sizeof(client_address);
    if (0 == m_LISTENTrigmode)
    {
        int connfd = accept(m_listenfd, (struct sockaddr *)&client_address, &client_addrlength);
        if (connfd < 0)
        {
            LOG_ERROR("%s:errno is:%d", "accept error", errno);
            return false;
        }
        if (http_conn::m_user_count >= MAX_FD)
        {
            utils.show_error(connfd, "Internal server busy");
            LOG_ERROR("%s", "Internal server busy");
            return false;
        }
        timer(connfd, client_address);
    }

    else
    {
        while (1)
        {
            int connfd = accept(m_listenfd, (struct sockaddr *)&client_address, &client_addrlength);
            if (connfd < 0)
            {
                LOG_ERROR("%s:errno is:%d", "accept error", errno);
                break;
            }
            if (http_conn::m_user_count >= MAX_FD)
            {
                utils.show_error(connfd, "Internal server busy");
                LOG_ERROR("%s", "Internal server busy");
                break;
            }
            timer(connfd, client_address);
        }
        return false;
    }
    return true;
}

bool WebServer::dealwithsignal(bool &timeout, bool &stop_server)
{
    int ret = 0;
    int sig;
    char signals[1024];
    ret = recv(m_pipefd[0], signals, sizeof(signals), 0);
    if (ret == -1)
    {
        return false;
    }
    else if (ret == 0)
    {
        return false;
    }
    else
    {
        for (int i = 0; i < ret; ++i)
        {
            switch (signals[i])
            {
            case SIGALRM:
            {
                timeout = true;
                break;
            }
            case SIGTERM:
            {
                stop_server = true;
                break;
            }
            }
        }
    }
    return true;
}

void WebServer::dealwithread(int sockfd)
{
    util_timer *timer = users_timer[sockfd].timer;

    //reactor
    if (1 == m_actormodel)
    {
        if (timer)
        {
            adjust_timer(timer);
        }

        //若监测到读事件，将该事件放入请求队列
        m_pool->append(&users[sockfd], 0);

        while (true)
        {
            if (users[sockfd].improv)
            {
                if (users[sockfd].timer_flag)
                {
                    deal_timer(timer, sockfd);
                    users[sockfd].timer_flag = false;
                }
                users[sockfd].improv = false;
                break;
            }
        }
    }
    else
    {
        //proactor
        if (users[sockfd].read_once())
        {
            LOG_INFO("deal with the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));

            //若监测到读事件，将该事件放入请求队列
            m_pool->append_p(&users[sockfd]);

            if (timer)
            {
                adjust_timer(timer);
            }
        }
        else
        {
            deal_timer(timer, sockfd);
        }
    }
}

void WebServer::dealwithwrite(int sockfd)
{
    util_timer *timer = users_timer[sockfd].timer;
    //reactor
    if (1 == m_actormodel)
    {
        if (timer)
        {
            adjust_timer(timer);
        }

        m_pool->append(&users[sockfd], 1);

        while (true)
        {
            if (users[sockfd].improv)
            {
                if (users[sockfd].timer_flag)
                {
                    deal_timer(timer, sockfd);
                    users[sockfd].timer_flag = false;
                }
                users[sockfd].improv = false;
                break;
            }
        }
    }
    else
    {
        //proactor
        if (users[sockfd].write())
        {
            LOG_INFO("send data to the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));

            if (timer)
            {
                adjust_timer(timer);
            }
        }
        else
        {
            deal_timer(timer, sockfd);
        }
    }
}

void WebServer::eventLoop()
{
    bool timeout = false;
    bool stop_server = false;

    while (!stop_server)
    {
        int number = epoll_wait(m_epollfd, events, MAX_EVENT_NUMBER, -1);
        if (number < 0 && errno != EINTR)
        {
            LOG_ERROR("%s", "epoll failure");
            break;
        }

        for (int i = 0; i < number; i++)
        {
            int sockfd = events[i].data.fd;

            //处理新到的客户连接
            if (sockfd == m_listenfd)
            {
                bool flag = dealclientdata();
                if (false == flag)
                    continue;
            }
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                //服务器端关闭连接，移除对应的定时器
                util_timer *timer = users_timer[sockfd].timer;
                deal_timer(timer, sockfd);
            }
            //处理信号
            else if ((sockfd == m_pipefd[0]) && (events[i].events & EPOLLIN))
            {
                bool flag = dealwithsignal(timeout, stop_server);
                if (false == flag)
                    LOG_ERROR("%s", "dealclientdata failure");
            }
            //处理客户连接上接收到的数据
            else if (events[i].events & EPOLLIN)
            {
                dealwithread(sockfd);
            }
            else if (events[i].events & EPOLLOUT)
            {
                dealwithwrite(sockfd);
            }
        }
        if (timeout)
        {
            utils.timer_handler();

            LOG_INFO("%s", "timer tick");

            timeout = false;
        }
    }
}
