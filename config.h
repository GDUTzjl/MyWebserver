/*
 * @Author: zjl 3106825030@qq.com
 * @Date: 2024-04-09 16:43:38
 * @LastEditors: zjl 3106825030@qq.com
 * @LastEditTime: 2024-04-09 16:53:04
 * @FilePath: /MyWebServer/config.h
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
 */
#ifndef CONFIG_H
#define CONFIG_H

#include "webserver.h"
#include <getopt.h>
#include <stdlib.h>
#include <string>

using namespace std;

// 设置参数
class Config
{
public:
    Config();
    ~Config(){};

    void parse_arg(int argc, char *argv[]);

    // 端口号
    int PORT;

    // 日志写入方式
    int LOGWrite;

    // 触发组合模式
    int TRIGMode;

    // listenfd触发模式
    int LISTENTrigmode;

    // connfd触发模式
    int CONNTrigmode;

    // 优雅关闭链接
    int OPT_LINGER;

    // 数据库连接池数量
    int sql_num;

    // 线程池内的线程数量
    int thread_num;

    // 是否关闭日志
    int close_log;

    // 并发模型选择
    int actor_model;
};

#endif