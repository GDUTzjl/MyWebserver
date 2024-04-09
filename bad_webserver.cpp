/*
 * @Author: zjl 3106825030@qq.com
 * @Date: 2024-04-09 16:39:59
 * @LastEditors: zjl 3106825030@qq.com
 * @LastEditTime: 2024-04-09 23:30:26
 * @FilePath: /MyWebServer/webserver.cpp
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
 */
#include "webserver.h"

WebServer::WebServer()
{
    // http_conn类对象
    // 预先为每个可能的客户连接分配一个http_conn对象
    // 创建MAX_FD个http类对象
    users = new http_conn[MAX_FD];

    // root文件夹路径
    char server_path[200];
    // getcwd()会将当前工作目录的绝对路径复制到参数buffer所指的内存空间中,参数size为buf的空间大小
    getcwd(server_path, 200);
    char root[6] = "/root";
    m_root = (char *)malloc(strlen(server_path) + strlen(root) + 1);
    strcpy(m_root, server_path);
    strcat(m_root, root);

    // 定时器
    // 创建连接资源数组
    users_timer = new client_data[MAX_FD];
}

WebServer::~WebServer()
{
    close(m_epollfd);
    close(m_listenfd);
    close(m_pipefd[1]);
    close(m_pipefd[0]);
    delete[] users;
    delete[] users_timer;
    delete m_pool;
}

void WebServer::init(int port, string user, string passWord, string databaseName, int log_write, int opt_linger, int trigmode, int sql_num, int thread_num, int close_log, int actor_model)
{
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
}

void WebServer::sql_pool()
{
    // 初始化数据库连接池
    m_connPool = connection_pool::GetInstance();
    m_connPool->init("localhost", m_user, m_passWord, m_databaseName, 3306, m_sql_num, m_close_log);

    // 初始化数据库读取表
    users->initmysql_result(m_connPool);
}

void WebServer::thread_pool()
{
    // 线程池
    m_pool = new threadpool<http_conn>(m_actormodel, m_connPool, m_thread_num);
}

void WebServer::trig_mode()
{
    // LT + LT
    if (0 == m_TRIGMode)
    {
        m_LISTENTrigmode = 0;
        m_CONNTrigmode = 0;
    }
    // LT + ET
    else if (1 == m_TRIGMode)
    {
        m_LISTENTrigmode = 0;
        m_CONNTrigmode = 1;
    }
    // ET + LT
    else if (2 == m_TRIGMode)
    {
        m_LISTENTrigmode = 1;
        m_CONNTrigmode = 0;
    }
    // ET + ET
    else if (3 == m_TRIGMode)
    {
        m_LISTENTrigmode = 1;
        m_CONNTrigmode = 1;
    }
}

void WebServer::eventListen()
{
    // 网络编程基础步骤
    /* 创建监听socket文件描述符 */
    m_listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(m_listenfd >= 0);

    // 优雅关闭连接
    // struct linger 用法  https://blog.csdn.net/teethfairy/article/details/10917145

    if (0 == m_OPT_LINGER)
    {
        struct linger tmp = {0, 1};
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    }

    else if (1 == m_OPT_LINGER)
    {
        struct linger tmp = {1, 1};
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    }
    int ret = 0;
    /* 创建监听socket的TCP/IP的IPV4 socket地址 */
    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;                // IPV4
    address.sin_addr.s_addr = htonl(INADDR_ANY); /* INADDR_ANY：将套接字绑定到所有可用的接口IP */
    address.sin_port = htons(m_port);

    int flag = 1; // ?????
    /* SO_REUSEADDR 允许端口被重复使用 */
    // 服务器重新启动，设置端口复用
    setsockopt(m_listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
    /* 绑定socket和它的地址 */
    ret = bind(m_listenfd, (struct sockaddr *)&address, sizeof(address));
    assert(ret >= 0);
    /* 创建监听队列以存放待处理的客户连接，在这些客户连接被accept()之前 */
    ret = listen(m_listenfd, 5);

    assert(ret >= 0);

    // 设置最小超时单位
    utils.init(TIMESLOT);

    // epoll创建内核事件表
    // struct epoll_event {
    //          __uint32_t events; /* Epoll events */
    //          epoll_data_t data; /* User data variable */
    //     };
    //     typedef union epoll_data {
    //          void *ptr;
    //          int fd;
    //          uint32_t u32;
    //          uint64_t u64;
    //     } epoll_data_t;
    /* 用于存储epoll事件表中就绪事件的event数组 */
    // 创建内核事件表
    epoll_event events[MAX_EVENT_NUMBER];
    /* 创建一个额外的文件描述符来唯一标识内核中的epoll事件表 */
    // 该描述符将用作其他epoll系统调用的第一个参数
    // size不起作用。
    m_epollfd = epoll_create(5);
    assert(m_epollfd != -1);

    // 将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT,上树，设置非阻塞
    /* 将fd上的EPOLLIN和EPOLLET事件注册到epollfd指示的epoll内核事件中 */
    /* 主线程往epoll内核事件表中注册监听socket事件，当listen到新的客户连接时，listenfd变为就绪事件 */
    // 将listenfd放在epoll树上 利用工具类上树
    utils.addfd(m_epollfd, m_listenfd, false, m_LISTENTrigmode); // 不用oneshot

    // 将上述epollfd赋值给http类对象的m_epollfd属性 静态变量方便操作，唯一标识
    http_conn::m_epollfd = m_epollfd;

    // socketpair的用法和理解
    // https://blog.csdn.net/weixin_40039738/article/details/81095013
    // sock_stream 是有保障的(即能保证数据正确传送到对方)面向连接的SOCKET，多用于资料(如文件)传送。
    // PF_UNIX 本地进程间通信——Unix域套接字
    ///* 创建管道，注册pipefd[0]上的可读事件 */
    // 创建管道套接字     利用管道在主线程和调用线程间传递信号，这里主要是超时的信号
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, m_pipefd);
    assert(ret != -1);
    /* 设置管道写端为非阻塞 */
    // 设置管道写端为非阻塞，为什么写端要非阻塞？
    // send是将信息发送给套接字缓冲区，如果缓冲区满了，则会阻塞，这时候会进一步增加信号处理函数的执行时间，为此，将其修改为非阻塞。
    utils.setnonblocking(m_pipefd[1]);
    /* 设置管道读端为ET非阻塞，并添加到epoll内核事件表 */
    // 设置管道读端为ET非阻塞
    utils.addfd(m_epollfd, m_pipefd[0], false, 0);

    // SIGPIPE   终止进程     向一个没有读进程的管道写数据  SIG_IGN， 忽视信号
    // 客户端程序向服务器端程序发送了消息，然后关闭客户端(处于close状态)，服务器端返回消息的时候就会收到内核给的SIGPIPE信号
    utils.addsig(SIGPIPE, SIG_IGN);
    // alarm函数会定期触发SIGALRM信号，这个信号交由sig_handler来处理，每当监测到有这个信号的时候，都会将这个信号写到pipefd[1]里面，传递给主循环：
    // 传递给主循环的信号值，这里只关注SIGALRM和SIGTERM
    // SIGTERM产生方式: 和任何控制字符无关,用kill函数发送
    utils.addsig(SIGALRM, utils.sig_handler, false);
    utils.addsig(SIGTERM, utils.sig_handler, false);

    // 每隔TIMESLOT时间触发SIGALRM信号
    alarm(TIMESLOT);
    // 工具类,信号和描述符基础操作
    Utils::u_pipefd = m_pipefd;
    Utils::u_epollfd = m_epollfd;
}

// 运行，循环监听事件。
void WebServer::eventLoop()
{
    // 超时标志
    // 超时默认为False
    bool timeout = false;
    // 循环条件
    bool stop_server = false;
    while (!stop_server)
    {
        /* 主线程调用epoll_wait等待一组文件描述符上的事件，并将当前所有就绪的epoll_event复制到events数组中 */
        // 监听事件
        // 监测发生事件的文件描述符
        //-1，阻塞直到监听的一个fd上有一个感兴趣事件发生
        int number = epoll_wait(m_epollfd, events, MAX_EVENT_NUMBER, -1);
        // 在epoll_wait时，因为设置了alarm定时触发警告，导致每次返回-1，errno为EINTR，对于这种错误返回
        // 忽略这种错误，让epoll报错误号为4时，再次做一次epoll_wait ????
        if (number < 0 && errno != EINTR)
        {
            LOG_ERROR("%s", "epoll failure");
            break;
        }
        // 对所有就绪事件进行处理
        // 轮询文件描述符
        for (int i = 0; i < number; i++)
        {
            int sockfd = events[i].data.fd; // 事件表中就绪的socket文件描述符

            // 处理新到的客户连接
            if (sockfd == m_listenfd) // 当listen到新的用户连接，listenfd上则产生就绪事件
            {
                bool flag = dealclientdata();
                if (false == flag)
                    continue;
            }
            // 如有异常，则直接关闭客户连接，并删除该用户的timer
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                // 服务器端关闭连接，移除对应的定时器
                // alarm函数会定期触发SIGALRM信号，这个信号交由sig_handler来处理，每当监测到有这个信号的时候，都会将这个信号写到pipefd[1]里面，传递给主循环：
                util_timer *timer = users_timer[sockfd].timer;
                deal_timer(timer, sockfd);
            }
            // 处理定时器信号
            // 处理信号
            // 管道读端对应文件描述符发生读事件
            else if ((sockfd == m_pipefd[0]) && (events[i].events & EPOLLIN))
            {
                // 接收到SIGALRM信号，timeout设置为True
                bool flag = dealwithsignal(timeout, stop_server);
                if (false == flag)
                    LOG_ERROR("%s", "dealclientdata failure");
            }
            // 处理客户连接上接收到的数据
            /* 当这一sockfd上有可读事件时，epoll_wait通知主线程。*/
            // 通过epoll_wait发现这个connfd上有可读事件了（EPOLLIN），主线程就将这个HTTP的请求报文读进这个连接socket的读缓存中users[sockfd].read()，然后将该任务对象（指针）插入线程池的请求队列中pool->append(users + sockfd);
            else if (events[i].events & EPOLLIN)
            {
                dealwithread(sockfd);
            }
            /* 当这一sockfd上有可写事件时，epoll_wait通知主线程。主线程往socket上写入服务器处理客户请求的结果 */
            else if (events[i].events & EPOLLOUT)
            {
                dealwithwrite(sockfd);
            }
        }
        // 当我们在读端pipefd[0]读到这个信号的的时候，就会将timeout变量置为true并跳出循环，让timer_handler()函数取出来定时器容器上的到期任务，该定时器容器是通过升序链表来实现的，从头到尾对检查任务是否超时，若超时则调用定时器的回调函数cb_func()，关闭该socket连接，并删除其对应的定时器del_timer。
        // 处理定时器为非必须事件，收到信号并不是立马处理
        // 完成读写事件后，再进行处理
        if (timeout)
        {
            utils.timer_handler();

            LOG_INFO("%s", "timer tick");

            timeout = false;
        }
    }
}

// void WebServer::eventLoop()调用  处理新到的客户连接
bool WebServer::dealclientdata()
{
    // 初始化客户端连接地址
    struct sockaddr_in client_address;
    socklen_t client_addrlength = sizeof(client_address);
    /* LT模式 （电平触发）*/
    if (0 == m_LISTENTrigmode)
    {
        // 接收
        // 该连接分配的文件描述符
        int connfd = accept(m_listenfd, (struct sockaddr *)&client_address, &client_addrlength);
        if (connfd < 0)
        {
            LOG_ERROR("%s:errno is:%d", "accept error", errno);
            return false;
        }
        // ???用户太多
        if (http_conn::m_user_count >= MAX_FD)
        {
            utils.show_error(connfd, "Internal server busy");
            LOG_ERROR("%s", "Internal server busy");
            return false;
        }
        // /* 并将connfd注册到内核事件表中 */
        timer(connfd, client_address);
    }
    /* ET模式 （边缘触发）*/
    else
    {
        // 需要循环接收数据
        while (1)
        {
            /* accept()返回一个新的socket文件描述符用于send()和recv() */
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
            // /* 并将connfd注册到内核事件表中 */
            timer(connfd, client_address);
        }
        return false; ///???????????????????
    }
    return true;
}

////被 void WebServer::eventLoop()调用  处理新到的客户连接 ->  接着WebServer::dealclientdata() -> WebServer::timer(int connfd, struct sockaddr_in client_address)
void WebServer::timer(int connfd, struct sockaddr_in client_address)
{
    /* 并将connfd注册到内核事件表中 */
    users[connfd].init(connfd, client_address, m_root, m_CONNTrigmode, m_close_log, m_user, m_passWord, m_databaseName);

    // 初始化client_data数据
    // 初始化该连接对应的连接资源
    // 创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
    users_timer[connfd].address = client_address;
    users_timer[connfd].sockfd = connfd;
    // 创建定时器临时变量
    util_timer *timer = new util_timer;
    // 设置定时器对应的连接资源
    timer->user_data = &users_timer[connfd];
    // 设置回调函数
    timer->cb_func = cb_func;

    time_t cur = time(NULL);
    // 设置绝对超时时间
    timer->expire = cur + 3 * TIMESLOT;
    // 创建该连接对应的定时器，初始化为前述临时变量
    users_timer[connfd].timer = timer;
    // 将该定时器添加到链表中
    utils.m_timer_lst.add_timer(timer);
}

void WebServer::deal_timer(util_timer *timer, int sockfd)
{
    // 服务器端关闭连接，移除对应的定时器
    timer->cb_func(&users_timer[sockfd]);

    if (timer)
    {
        utils.m_timer_lst.del_timer(timer);
    }

    LOG_INFO("close fd %d", users_timer[sockfd].sockfd);
}

// 当我们在读端pipefd[0]读到这个信号的的时候，就会将timeout变量置为true并跳出循环，让timer_handler()函数取出来定时器容器上的到期任务，该定时器容器是通过升序链表来实现的，从头到尾对检查任务是否超时，若超时则调用定时器的回调函数cb_func()，关闭该socket连接，并删除其对应的定时器del_timer
bool WebServer::dealwithsignal(bool &timeout, bool &stop_server)
{
    int ret = 0;
    int sig; //????????????????????
    char signals[1024];
    // 从管道读端读出信号值，成功返回字节数，失败返回-1
    // 正常情况下，这里的ret返回值总是1，只有14和15两个ASCII码对应的字符
    ret = recv(m_pipefd[0], signals, sizeof(signals), 0);

    if (ret == -1)
    {
        // handle the error
        return false;
    }
    else if (ret == 0)
    {
        return false;
    }
    else
    {
        // 处理信号值对应的逻辑
        for (int i = 0; i < ret; ++i)
        {
            /*信号本身是整型数值，管道中传递的是ASCII码表中整型数值对应的字符。
            switch的变量一般为字符或整型，当switch的变量为字符时，case中可以是字符，也可以是字符对应的ASCII码。*/
            // 这里面明明是字符
            switch (signals[i])
            {
            // 这里是整型
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
    // 创建定时器临时变量，将该连接对应的定时器取出来
    util_timer *timer = users_timer[sockfd].timer;

    // reactor
    // Reactor模式：要求主线程（I/O处理单元）只负责监听文件描述符上是否有事件发生（可读、可写），若有，则立即通知工作线程（逻辑单元），将socket可读可写事件放入请求队列，交给工作线程处理。
    if (1 == m_actormodel)
    {
        if (timer)
        {
            adjust_timer(timer);
        }

        // 若监测到读事件，将该事件放入请求队列
        m_pool->append(users + sockfd, 0);

        while (true)
        {
            if (1 == users[sockfd].improv)
            {
                if (1 == users[sockfd].timer_flag)
                {
                    deal_timer(timer, sockfd);
                    users[sockfd].timer_flag = 0;
                }
                users[sockfd].improv = 0;
                break;
            }
        }
    }
    else
    {
        // proactor
        // Proactor模式：将所有的I/O操作都交给主线程和内核来处理（进行读、写），工作线程仅负责处理逻辑，如主线程读完成后users[sockfd].read()，选择一个工作线程来处理客户请求pool->append(users + sockfd)。
        // 读入对应缓冲区
        if (users[sockfd].read_once())
        {
            LOG_INFO("deal with the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));

            // 若监测到读事件，将该事件放入请求队列
            m_pool->append_p(users + sockfd);

            // 若有数据传输，则将定时器往后延迟3个单位
            // 对其在链表上的位置进行调整
            if (timer)
            {
                adjust_timer(timer);
            }
        }
        else
        {
            // 服务器端关闭连接，移除对应的定时器
            deal_timer(timer, sockfd);
        }
    }
}
void WebServer::adjust_timer(util_timer *timer)
{

    time_t cur = time(NULL);
    timer->expire = cur + 3 * TIMESLOT;
    utils.m_timer_lst.adjust_timer(timer);

    LOG_INFO("%s", "adjust timer once");
}

void WebServer::dealwithwrite(int sockfd)
{
    util_timer *timer = users_timer[sockfd].timer;
    // reactor
    if (1 == m_actormodel)
    {
        if (timer)
        {
            adjust_timer(timer);
        }

        m_pool->append(users + sockfd, 1);

        while (true)
        {
            if (1 == users[sockfd].improv)
            {
                if (1 == users[sockfd].timer_flag)
                {
                    deal_timer(timer, sockfd);
                    users[sockfd].timer_flag = 0;
                }
                users[sockfd].improv = 0;
                break;
            }
        }
    }
    else
    {
        // proactor
        if (users[sockfd].write())
        {
            LOG_INFO("send data to the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));
            // 若有数据传输，则将定时器往后延迟3个单位
            // 并对新的定时器在链表上的位置进行调整
            if (timer)
            {
                adjust_timer(timer);
            }
        }
        else
        {
            // 服务器端关闭连接，移除对应的定时器
            deal_timer(timer, sockfd);
        }
    }
}
void WebServer::log_write()
{
    if (0 == m_close_log)
    {
        // 初始化日志  同步、异步
        if (1 == m_log_write)
            Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, 800);
        else
            Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, 0);
    }
}