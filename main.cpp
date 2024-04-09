/*
 * @Author: zjl 3106825030@qq.com
 * @Date: 2024-04-09 16:50:02
 * @LastEditors: zjl 3106825030@qq.com
 * @LastEditTime: 2024-04-09 19:47:44
 * @FilePath: /MyWebServer/main.cpp
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
 */
#include "config.h"

int main(int argc, char *argv[])
{
    // 需要修改的数据库信息,登录名,密码,库名
    // root 111
    string user = "curry";
    string passwd = "123456";
    string databasename = "yourdb";

    // 命令行解析
    Config config;
    config.parse_arg(argc, argv);

    WebServer server;
    // 初始化
    server.init(config.PORT, user, passwd, databasename, config.LOGWrite,
                config.OPT_LINGER, config.TRIGMode, config.sql_num, config.thread_num,
                config.close_log, config.actor_model);

    // // 日志
    // server.log_write();

    // 数据库
    server.sql_pool();

    // 线程池
    server.thread_pool();

    // 触发模式
    server.trig_mode();

    // 监听
    server.eventListen();

    // 运行
    server.eventLoop();

    return 0;
}