/*
 * @Author: zjl 3106825030@qq.com
 * @Date: 2024-04-09 17:05:31
 * @LastEditors: zjl 3106825030@qq.com
 * @LastEditTime: 2024-04-09 21:47:00
 * @FilePath: /MyWebServer/timer/lst_timer.h
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
 */
#ifndef LST_TIMER
#define LST_TIMER

#include <signal.h>     //for sigaction
#include <unistd.h>     //for alarm
#include <sys/types.h>  //for send
#include <sys/socket.h> //for socket
#include <assert.h>     //for assert
#include <netinet/in.h> //for sockaddr_in

/*定时器容器设计
项目中的定时器容器为带头尾结点的升序双向链表，具体的为每个连接创建一个定时器，将其添加到链表中，并按照超时时间升序排列。执行定时任务时，将到期的定时器从链表中删除。
从实现上看，主要涉及双向链表的插入，删除操作，其中添加定时器的事件复杂度是O(n),删除定时器的事件复杂度是O(1)。
升序双向链表主要逻辑如下，具体的，
    创建头尾节点，其中头尾节点没有意义，仅仅统一方便调整
    add_timer函数，将目标定时器添加到链表中，添加时按照升序添加
        若当前链表中只有头尾节点，直接插入
        否则，将定时器按升序插入
    adjust_timer函数，当定时任务发生变化,调整对应定时器在链表中的位置
        客户端在设定时间内有数据收发,则当前时刻对该定时器重新设定时间，这里只是往后延长超时时间
        被调整的目标定时器在尾部，或定时器新的超时值仍然小于下一个定时器的超时，不用调整
        否则先将定时器从链表取出，重新插入链表
    del_timer函数将超时的定时器从链表中删除
        常规双向链表删除结点
*/

// 定时器容器类
class util_timer;
class sort_timer_lst
{
public:
    sort_timer_lst();
    // 常规销毁链表
    ~sort_timer_lst();
    // 添加定时器，内部调用私有成员add_timer
    void add_timer(util_timer *timer);
    // 调整定时器，任务发生变化时，调整定时器在链表中的位置
    void adjust_timer(util_timer *timer);
    // 删除定时器
    void del_timer(util_timer *timer);
    // 定时任务处理函数
    void tick();

private:
    // 私有成员，被公有成员add_timer和adjust_time调用
    // 主要用于调整链表内部结点
    void add_timer(util_timer *timer, util_timer *lst_head);
    // 头尾结点
    util_timer *head;
    util_timer *tail;
};

/*
定时器设计  util_timer
项目中将连接资源、定时事件和超时时间封装为定时器类，具体的，
连接资源包括客户端套接字地址、文件描述符和定时器
定时事件为回调函数，将其封装起来由用户自定义，这里是删除非活动socket上的注册事件，并关闭
定时器超时时间 = 浏览器和服务器连接时刻 + 固定时间(TIMESLOT)，可以看出，定时器使用绝对时间作为超时值，这里alarm设置为5秒，连接超时为15秒。
*/

// 连接资源结构体成员需要用到定时器类
// 需要前向声明
class util_timer;

// 连接资源
struct client_data
{
    // 客户端socket地址
    sockaddr_in address;
    // socket文件描述符
    int sockfd;
    // 定时器
    util_timer *timer;
};

// 定时器类
class util_timer
{
public:
    util_timer() : prev(NULL), next(NULL) {}

public:
    // 超时时间
    time_t expire;
    // 回调函数
    void (*cb_func)(client_data *);
    // 连接资源
    client_data *user_data;
    // 前向定时器
    util_timer *prev;
    // 后继定时器
    util_timer *next;
};

class Utils
{
public:
    Utils() {}
    ~Utils() {}

    void init(int timeslot);

    // 对文件描述符设置非阻塞
    int setnonblocking(int fd);

    // 将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
    void addfd(int epollfd, int fd, bool one_shot, int TRIGMode);

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
/*定时事件，具体的，从内核事件表删除事件，关闭文件描述符，释放连接资源。*/
void cb_func(client_data *user_data);

#endif
