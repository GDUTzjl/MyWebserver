/*
 * @Author: zjl 3106825030@qq.com
 * @Date: 2024-04-07 22:03:43
 * @LastEditors: zjl 3106825030@qq.com
 * @LastEditTime: 2024-04-09 19:57:29
 * @FilePath: /MyWebServer/CGImysql/sql_connection_pool.h
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
 */
#ifndef _CONNECTION_POOL_
#define _CONNECTION_POOL_

#include <stdio.h>
#include <list>
#include <mysql/mysql.h> //(找不到mysql/mysql.h头文件的时候，需要安装一个库文件：sudo apt install libmysqlclient-dev)
#include <error.h>
#include <string.h>
#include <iostream>
#include <string>
#include "../lock/locker.h"
// #include "../log/log.h"

using namespace std;

class connection_pool
{
public:
    MYSQL *GetConnection();              // 获取数据库连接
    bool ReleaseConnection(MYSQL *conn); // 释放连接
    int GetFreeConn();                   // 获取连接
    void DestroyPool();                  // 销毁所有连接

    // 单例模式
    // 局部静态变量单例模式
    static connection_pool *GetInstance();

    void init(string url, string User, string PassWord, string DataBaseName, int Port, int MaxConn, int close_log);

    // void init(string url, string User, string PassWord, string DataBaseName, int Port, int MaxConn);

private:
    connection_pool();
    ~connection_pool();

    int m_MaxConn;  // 最大连接数
    int m_CurConn;  // 当前已使用的连接数
    int m_FreeConn; // 当前空闲的连接数
    locker lock;
    list<MYSQL *> connList; // 连接池
    sem reserve;            // 信号量

public:
    string m_url;          // 主机地址
    string m_Port;         // 数据库端口号
    string m_User;         // 登陆数据库用户名
    string m_PassWord;     // 登陆数据库密码
    string m_DatabaseName; // 使用数据库名
    // int m_close_log;	   // 日志开关
};

class connectionRAII
{

public:
    // 双指针对MYSQL *con修改
    connectionRAII(MYSQL **con, connection_pool *connPool);
    ~connectionRAII();

private:
    MYSQL *conRAII;
    connection_pool *poolRAII;
};

#endif