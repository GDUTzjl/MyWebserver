/*
 * @Author: error: error: git config user.name & please set dead value or install git && error: git config user.email & please set dead value or install git & please set dead value or install git
 * @Date: 2024-04-07 18:42:19
 * @LastEditors: error: error: git config user.name & please set dead value or install git && error: git config user.email & please set dead value or install git & please set dead value or install git
 * @LastEditTime: 2024-04-07 20:04:40
 * @FilePath: /MyWebServer/lock/locker.h
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
 */
/*
锁机制的功能
实现多线程同步，通过锁机制，确保任一时刻只能有一个线程能进入关键代码段.

封装的功能
类中主要是Linux下三种锁进行封装，将锁的创建于销毁函数封装在类的构造与析构函数中，实现RAII机制
将重复使用的代码封装为函数，减少代码的重复，使其更简洁

*/
#ifndef LOCKER_H
#define LOCKER_H

#include <exception>
#include <pthread.h>
#include <semaphore.h>

class sem
{
private:
    sem_t m_sem;

public:
    sem()
    {
        // 信号量初始化
        if (sem_init(&m_sem, 0, 0) != 0)
        {
            // 错误 抛出异常
            throw std::exception();
        }
    }
    sem(int num)
    {
        // 信号量初始化
        if (sem_init(&m_sem, 0, num) != 0)
        {
            // 错误 抛出异常
            throw std::exception();
        }
    }

    ~sem()
    {
        // 信号量销毁
        sem_destroy(&m_sem);
    }

    bool wait()
    { // 将以原子操作方式将信号量减一,信号量为0时,sem_wait阻塞
        return sem_wait(&m_sem) == 0;
    }

    bool post()
    { // 以原子操作方式将信号量加一,信号量大于0时,唤醒调用sem_post的线程
        return sem_post(&m_sem) == 0;
    }
};

class locker
{
private:
    pthread_mutex_t m_mutex;

public:
    locker()
    {
        if (pthread_mutex_init(&m_mutex, NULL) != 0)
        {
            // 错误 抛出异常
            throw std::exception();
        }
    }
    ~locker()
    {
        pthread_mutex_destroy(&m_mutex);
    }
    bool lock()
    {
        return pthread_mutex_lock(&m_mutex) == 0;
    }
    bool unlock()
    {
        return pthread_mutex_unlock(&m_mutex) == 0;
    }
    pthread_mutex_t *get()
    {
        return &m_mutex;
    }
};

// 条件变量

class cond
{
private:
    pthread_cond_t m_cond;

public:
    cond()
    {
        if (pthread_cond_init(&m_cond, NULL) != 0)
        {
            throw std::exception();
        }
    }
    ~cond()
    {
        pthread_cond_destroy(&m_cond);
    }
    bool wait(pthread_mutex_t *m_mutex)
    {
        int ret = 0;
        // pthread_mutex_lock(&m_mutex);
        ret = pthread_cond_wait(&m_cond, m_mutex);
        // pthread_mutex_unlock(&m_mutex);
        return ret == 0;
    }
    bool timewait(pthread_mutex_t *m_mutex, struct timespec t)
    {
        int ret = 0;
        // pthread_mutex_lock(&m_mutex);
        ret = pthread_cond_timedwait(&m_cond, m_mutex, &t);
        // pthread_mutex_unlock(&m_mutex);
        return ret == 0;
    }
    bool signal()
    {
        return pthread_cond_signal(&m_cond) == 0;
    }
    // pthread_cond_broadcast函数以广播的方式唤醒所有等待目标条件变量的线程
    bool broadcast()
    {
        return pthread_cond_broadcast(&m_cond) == 0;
    }
};

#endif