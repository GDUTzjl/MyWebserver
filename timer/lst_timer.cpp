/*
 * @Author: zjl 3106825030@qq.com
 * @Date: 2024-04-09 17:05:46
 * @LastEditors: zjl 3106825030@qq.com
 * @LastEditTime: 2024-04-09 19:31:46
 * @FilePath: /MyWebServer/timer/lst_timer.cpp
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
 */
#include "lst_timer.h"
#include "../http/http_conn.h"

/*---------------------工具类---------------------------------*/

int *Utils::u_pipefd = 0;
int Utils::u_epollfd = 0;

//  自定义信号处理函数，创建sigaction结构体变量，设置信号函数。
//  信号处理函数
//  信号处理函数中仅仅通过管道发送信号值，不处理信号对应的逻辑，缩短异步执行时间，减少对主程序的影响。
void Utils::sig_handler(int sig)
{
    // 为保证函数的可重入性，保留原来的errno
    // 可重入性表示中断后再次进入该函数，环境变量与之前相同，不会丢失数据
    int save_errno = errno;
    int msg = sig;

    // 将信号值从管道写端写入，传输字符类型，而非整型
    send(u_pipefd[1], (char *)&msg, 1, 0);

    // 将原来的errno赋值为当前的errno
    errno = save_errno;
}

// 项目中设置信号函数，仅关注SIGTERM和SIGALRM两个信号 即sig取值SIGALRM，SIGTERM。
// 设置信号函数
//
void Utils::addsig(int sig, void(handler)(int), bool restart)
{
    // 创建sigaction结构体变量
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    // 信号处理函数中仅仅发送信号值，不做对应逻辑处理
    sa.sa_handler = handler;
    if (restart)
        sa.sa_flags |= SA_RESTART;

    // 将所有信号添加到信号集中
    sigfillset(&sa.sa_mask);
    // 执行sigaction函数
    assert(sigaction(sig, &sa, NULL) != -1);
}
void Utils::init(int timeslot)
{
    m_TIMESLOT = timeslot;
}

// 对文件描述符设置非阻塞
int Utils::setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

/* 主线程往epoll内核事件表中注册监听socket事件，当listen到新的客户连接时，listenfd变为就绪事件 */
// 将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
//  // TRIGMode listenfd触发模式
void Utils::addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
{
    // epoll_event结构体定义
    // struct epoll_event {
    //    uint32_t events;  // epoll 事件类型，包括可读，可写等
    //    epoll_data_t data; // 用户数据，可以是一个指针或文件描述符等
    // };
    // EPOLLIN：表示对应的文件描述符上有数据可读
    // EPOLLOUT：表示对应的文件描述符上可以写入数据
    // EPOLLRDHUP：表示对端已经关闭连接，或者关闭了写操作端的写入
    // EPOLLPRI：表示有紧急数据可读
    // EPOLLERR：表示发生错误
    // EPOLLHUP：表示文件描述符被挂起
    // EPOLLET：表示将epoll设置为边缘触发模式
    // EPOLLONESHOT：表示将事件设置为一次性事件

    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP; //
    else
        event.events = EPOLLIN | EPOLLRDHUP;
    /* 针对connfd，开启EPOLLONESHOT，因为我们希望每个socket在任意时刻都只被一个线程处理 */
    if (one_shot)
        event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    // 对文件描述符设置非阻塞
    setnonblocking(fd);
}
// 定时处理任务，重新定时以不断触发SIGALRM信号
void Utils::timer_handler()
{
    m_timer_lst.tick();
    alarm(m_TIMESLOT);
}

void Utils::show_error(int connfd, const char *info)
{
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

// class Utils; //什么用？？？？
/*---------------------定时器回调函数---------------------------------*/
// 定时器回调函数  user_data
void cb_func(client_data *user_data)
{
    // 删除非活动连接在socket上的注册事件
    epoll_ctl(Utils::u_epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
    assert(user_data);
    // 关闭文件描述符
    close(user_data->sockfd);
    // 减少连接数
    http_conn::m_user_count--;
}
/*---------------------定时器容器类---------------------------------*/
// 创建头尾节点，其中头尾节点没有意义，仅仅统一方便调整
sort_timer_lst::sort_timer_lst()
{
    head = NULL;
    tail = NULL;
}

// 常规销毁链表
sort_timer_lst::~sort_timer_lst()
{
    util_timer *tmp = head;
    while (tmp)
    {
        head = tmp->next;
        delete tmp;
        tmp = head;
    }
}

// add_timer函数，将目标定时器添加到链表中，添加时按照升序添加
// 若当前链表中只有头尾节点，直接插入
// 否则，将定时器按升序插入
// 添加定时器，内部调用私有成员add_timer
void sort_timer_lst::add_timer(util_timer *timer)
{
    if (!timer)
    {
        return;
    }
    if (!head)
    {
        head = tail = timer;
        return;
    }
    // 头插，小于第一个时间
    if (timer->expire < head->expire)
    {
        timer->next = head;
        head->prev = timer;
        head = timer;
        return;
    }
    add_timer(timer, head);
}

// adjust_timer函数，当定时任务发生变化,调整对应定时器在链表中的位置
// 客户端在设定时间内有数据收发,则当前时刻对该定时器重新设定时间，这里只是往后延长超时时间
// 被调整的目标定时器在尾部，或定时器新的超时值仍然小于下一个定时器的超时，不用调整
// 否则先将定时器从链表取出，重新插入链表
// 调整定时器，任务发生变化时，调整定时器在链表中的位置
void sort_timer_lst::adjust_timer(util_timer *timer)
{
    if (!timer)
    {
        return;
    }

    util_timer *tmp = timer->next;
    // 被调整的定时器在链表尾部
    // 定时器超时值仍然小于下一个定时器超时值，不调整
    if (!tmp || (timer->expire < tmp->expire))
    {
        return;
    }

    // 被调整定时器是链表头结点，将定时器取出，重新插入
    if (timer == head)
    {
        head = head->next;
        head->prev = NULL;
        timer->next = NULL;
        add_timer(timer, head);
    }
    // 被调整定时器在内部，将定时器取出，重新插入
    else
    {
        timer->prev->next = timer->next;
        timer->next->prev = timer->prev;
        add_timer(timer, timer->next);
    }
}
// 删除定时器
void sort_timer_lst::del_timer(util_timer *timer)
{
    if (!timer)
    {
        return;
    }
    // 链表中只有一个定时器，需要删除该定时器
    if ((timer == head) && (timer == tail))
    {
        delete timer;
        head = NULL;
        tail = NULL;
        return;
    }
    // 被删除的定时器为头结点
    if (timer == head)
    {
        head = head->next;
        head->prev = NULL;
        delete timer;
        return;
    }
    // 被删除的定时器为尾结点
    if (timer == tail)
    {
        tail = tail->prev;
        tail->next = NULL;
        delete timer;
        return;
    }
    // 被删除的定时器在链表内部，常规链表结点删除
    timer->prev->next = timer->next;
    timer->next->prev = timer->prev;
    delete timer;
}

/*---------------------定时器容器类--定时任务处理函数------------------*/
// 定时任务处理函数
// 使用统一事件源，SIGALRM信号每次被触发，主循环中调用一次定时任务处理函数，处理链表容器中到期的定时器。
// 具体的逻辑如下，
// 遍历定时器升序链表容器，从头结点开始依次处理每个定时器，直到遇到尚未到期的定时器
// 若当前时间小于定时器超时时间，跳出循环，即未找到到期的定时器
// 若当前时间大于定时器超时时间，即找到了到期的定时器，执行回调函数，然后将它从链表中删除，然后继续遍历
// 定时任务处理函数
void sort_timer_lst::tick()
{
    if (!head)
    {
        return;
    }

    // 获取当前时间
    time_t cur = time(NULL);
    util_timer *tmp = head;
    // 遍历定时器链表
    while (tmp)
    {
        // 链表容器为升序排列
        // 当前时间小于定时器的超时时间，后面的定时器也没有到期
        if (cur < tmp->expire)
        {
            break;
        }
        // 当前定时器到期，则调用回调函数，执行定时事件
        tmp->cb_func(tmp->user_data);
        // 将处理后的定时器从链表容器中删除，并重置头结点
        head = tmp->next;
        if (head)
        {
            head->prev = NULL;
        }
        delete tmp;
        tmp = head;
    }
}

/*---------------------定时器容器类--私有成员-------------------------*/

// 私有成员，被公有成员add_timer和adjust_time调用
// 主要用于调整链表内部结点
void sort_timer_lst::add_timer(util_timer *timer, util_timer *lst_head)
{
    util_timer *prev = lst_head;
    util_timer *tmp = prev->next;
    // 遍历当前结点之后的链表，按照超时时间找到目标定时器对应的位置，常规双向链表插入操作
    while (tmp)
    {
        if (timer->expire < tmp->expire)
        {
            prev->next = timer;
            timer->next = tmp;
            tmp->prev = timer;
            timer->prev = prev;
            break;
        }
        prev = tmp;
        tmp = tmp->next;
    }
    // 遍历完发现，目标定时器需要放到尾结点处
    if (!tmp)
    {
        prev->next = timer;
        timer->prev = prev;
        timer->next = NULL;
        tail = timer;
    }
}
