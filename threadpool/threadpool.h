/*
 * @Author: zjl 3106825030@qq.com
 * @Date: 2024-04-07 21:07:53
 * @LastEditors: zjl 3106825030@qq.com
 * @LastEditTime: 2024-04-09 21:48:51
 * @FilePath: /MyWebServer/threadpool/threadpool.h
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
 */
#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>
#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"

/*线程池类定义 需要注意，线程处理函数和运行函数设置为私有属性*/
// T指的是线程运行的任务，这里指的是http_conn类
template <typename T>
class threadpool
{
private:
    /*thread_number是线程池中线程的数量，max_requests是请求队列中最多允许的、等待处理的请求的数量*/
    // connPool是数据库连接池指针
    // 模型切换
    int m_thread_number;         // 线程池中的线程数
    int m_max_requests;          // 请求队列中允许的最大请求数
    pthread_t *m_threads;        // 描述线程池的数组，其大小为m_thread_number
    std::list<T *> m_workqueue;  // 请求队列
    locker m_queuelocker;        // 保护请求队列的互斥锁
    sem m_queuestat;             // 是否有任务需要处理 信号量
    connection_pool *m_connPool; // 数据库
    int m_actor_model;           // 模型切换
private:
    /*工作线程运行的函数，它不断从工作队列中取出任务并执行之*/
    static void *worker(void *arg);
    void run();

public:
    threadpool(int actor_model, connection_pool *connPool, int thread_number = 8, int max_request = 10000);
    ~threadpool();
    // 像请求队列中插入任务请求
    // 两种模式 append  reactor模式 需要读写状态  append_p又是 proactor模式 不需要读写状态，因为主线程已经完成读写状态
    bool append(T *request, int state);
    bool append_p(T *request);
};
// 注意写法
template <typename T>
threadpool<T>::threadpool(int actor_model, connection_pool *connPool, int thread_number, int max_requests) : m_actor_model(actor_model), m_thread_number(thread_number), m_max_requests(max_requests), m_threads(NULL), m_connPool(connPool)
{
    if (thread_number <= 0 || max_requests <= 0)
        throw std::exception();
    // 线程id初始化  描述线程池的数组，其大小为m_thread_number
    m_threads = new pthread_t[m_thread_number];
    // 创建失败
    if (!m_threads)
        throw std::exception();
    for (int i = 0; i < thread_number; i++)
    {
        if (pthread_create(m_threads + i, NULL, worker, this) != 0)
        {
            delete[] m_threads;
            throw std::exception();
        }
        if (pthread_detach(m_threads[i]))
        {
            delete[] m_threads;
            throw std::exception();
        }
    }
}
// 注意写法
template <typename T>
threadpool<T>::~threadpool()
{
    delete[] m_threads;
}

template <typename T>
void *threadpool<T>::worker(void *arg)
{
    // 线程池指针
    threadpool *pool = (threadpool *)arg;
    pool->run();
    return pool;
}

template <typename T>
void threadpool<T>::run()
{
    while (true)
    {
        // 信号量等待
        m_queuestat.wait();
        // 被唤醒后先加互斥锁
        m_queuelocker.lock();
        if (m_workqueue.empty())
        {
            m_queuelocker.unlock();
            continue;
        }
        // 从请求队列中取出第一个任务
        // 将任务从请求队列删除
        T *request = m_workqueue.front();
        m_workqueue.pop_front();
        m_queuelocker.unlock();
        if (!request)
            continue;

        // Reactor模式：要求主线程（I/O处理单元）只负责监听文件描述符上是否有事件发生（可读、可写），若有，则立即通知工作线程（逻辑单元），将socket可读可写事件放入请求队列，交给工作线程处理。
        if (1 == m_actor_model)
        {
            // http 中的成员函数 m_state; // 读为0, 写为1
            if (0 == request->m_state)
            {
                if (request->read_once())
                {
                    request->improv = 1;
                    // 从连接池中取出一个数据库连接
                    connectionRAII mysqlcon(&request->mysql, m_connPool);
                    // process(模板类中的方法,这里是http类)进行处理
                    request->process();
                }
                else
                {
                    // improv标志在read_once和write成功后,都会被设置为1,在相应的request处理完成后,均会被设置位为0,所以这个标志是用来判断上一个请求是否已处理完毕
                    // improv是线程处理了相应的sockfd后，将其置为1，然后在dealwiththread中判断线程处理过的fd,并再将其置为0,以便后续的连接继续使用该fd。
                    /*每个http连接有两个标志位：improv和timer_flag，初始时其值为0，它们只在Reactor模式下发挥作用。因为在Reactor模式中，主线程只负责监听读/写事件（得知有读写数据了），线程池中的线程负责IO操作（真正地读/写数据）。但是如果线程池中的子线程进行客户的IO操作时，出现错误了，那子线程该怎么将这个错误通知给主线程呢？因此作者便让子线程将imporv和timer_flag这两个标志位置1来告知主线程：“子线程我呀，对这个客户连接的IO操作，它，出！错！了！，你关闭掉这个客户的连接吧！”，于是主线程就知道要关闭这个IO出错的客户的连接。

                   所以简而言之，这两个标志位的作用就是：“Reactor模式下，当子线程执行读写任务出错时，来通知主线程关闭子线程的客户连接”。对于improv标志，其作用是保持主线程和子线程的同步；对于time_flag标志，其作用是标识子线程读写任务是否成功。

                   下面是源代码中这两个标志具体的设置流程：

                   在threadpool<T>::run()中的request->read_once()或request->write()完成后imporv会被置1，标志着http连接的读写任务已完成（请求已处理完毕）；如果http连接的读写任务失败（请求处理出错）timer_flag会被置1。

                   在 WebServer::dealwith_read()和WebServer::dealwith_write()中，会循环等待连接improv被置1，也就是一直等待该http连接的请求处理完毕，如果请求处理失败则关闭该http连接。*/
                    request->improv = 1;
                    // timer_flag应该是timer到期的标志 错误
                    // 所以这个timer_flag应该是个用户连接是否异常的标志位
                    request->timer_flag = 1;
                }
            }
            // http 中的成员函数 m_state; // 读为0, 写为1
            else
            {

                if (request->write())
                {
                    request->improv = 1;
                }
                else
                {
                    request->improv = 1;
                    request->timer_flag = 1;
                }
            }
        }
        // Proactor模式：将所有的I/O操作都交给主线程和内核来处理（进行读、写），工作线程仅负责处理逻辑，如主线程读完成后users[sockfd].read()，选择一个工作线程来处理客户请求pool->append(users + sockfd)。
        else
        {
            // 从连接池中取出一个数据库连接
            connectionRAII mysqlcon(&request->mysql, m_connPool);
            // process(模板类中的方法,这里是http类)进行处理
            request->process();
        }
    }
}

/*向请求队列中添加任务
通过list容器创建请求队列，向队列中添加时，通过互斥锁保证线程安全，添加完成后通过信号量提醒有任务要处理，最后注意线程同步。*/
//
template <typename T>
bool threadpool<T>::append(T *request, int state)
{
    m_queuelocker.lock();
    if (m_workqueue.size() >= m_max_requests)
    {
        m_queuelocker.unlock();
        return false;
    }
    request->m_state = state; // 读写状态
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post(); // 以原子操作方式将信号量加一,信号量大于0时,唤醒调用sem_post的线程
    return true;
}
template <typename T>
bool threadpool<T>::append_p(T *request)
{
    m_queuelocker.lock();
    if (m_workqueue.size() >= m_max_requests)
    {
        m_queuelocker.unlock();
        return false;
    }
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post(); // 以原子操作方式将信号量加一,信号量大于0时,唤醒调用sem_post的线程
    return true;
}

#endif