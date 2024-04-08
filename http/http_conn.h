/*
 * @Author: zjl 3106825030@qq.com
 * @Date: 2024-04-08 16:02:49
 * @LastEditors: zjl 3106825030@qq.com
 * @LastEditTime: 2024-04-08 20:57:04
 * @FilePath: /MyWebServer/http/http_conn.h
 * @Description: 这是默认设置,请设置`customMade`, 打开koroFileHeader查看配置 进行设置: https://github.com/OBKoro1/koro1FileHeader/wiki/%E9%85%8D%E7%BD%AE
 */

#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H

#include <netinet/in.h> //sockaddr_in
#include <sys/stat.h>   //stat 文件状态函数
#include <map>
#include <string.h>
#include <mysql/mysql.h> //formysql
#include <sys/epoll.h>   // for epoll_event
#include <fcntl.h>       //for fcntl 设置文件描述符

#include <unistd.h> //for close()

// 等待补充

#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"
// 等待补充

class http_conn
{
public:
    // 设置读取文件的名称m_real_file大小
    static const int FILENAME_LEN = 200;
    // 设置读缓冲区m_read_buf大小
    static const int READ_BUFFER_SIZE = 2048;
    // 设置写缓冲区m_write_buf大小
    static const int WRITE_BUFFER_SIZE = 1024;
    // 报文的请求方法，本项目只用到GET和POST
    enum METHOD
    {
        GET = 0,
        POST,
        HEAD,
        PUT,
        DELETE,
        TRACE,
        OPTIONS,
        CONNECT,
        PATH
    };
    // 主状态机的状态
    enum CHECK_STATE
    {
        CHECK_STATE_REQUESTLINE = 0, // 解析请求行
        CHECK_STATE_HEADER,          // 解析请求头
        CHECK_STATE_CONTENT          // 解析消息体，仅用于解析POST请求
    };
    // 报文解析的结果
    enum HTTP_CODE
    {
        NO_REQUEST,        // 请求不完整，需要继续读取请求报文数据 跳转主线程继续监测读事件
        GET_REQUEST,       // 获得了完整的HTTP请求 调用do_request完成请求资源映射
        BAD_REQUEST,       // HTTP请求报文有语法错误或请求资源为目录   跳转process_write完成响应报文
        NO_RESOURCE,       // 请求资源不存在 跳转process_write完成响应报文
        FORBIDDEN_REQUEST, // 请求资源禁止访问，没有读取权限 跳转process_write完成响应报文
        FILE_REQUEST,      // 请求资源可以正常访问 跳转process_write完成响应报文
        INTERNAL_ERROR,    // 服务器内部错误，该结果在主状态机逻辑switch的default下，一般不会触发
        CLOSED_CONNECTION
    };
    // 从状态机的状态
    enum LINE_STATUS
    {
        LINE_OK = 0,
        LINE_BAD,
        LINE_OPEN
    };

public:
    http_conn() {} // 什么都没有做；
    ~http_conn() {}

public:
    // 初始化套接字地址，函数内部会调用私有方法init
    void init(int sockfd, const sockaddr_in &addr, char *, int, int, string user, string passwd, string sqlname);
    // 关闭http连接
    void close_conn(bool real_close = true);
    // 处理函数
    void process();
    // 读取浏览器端发来的全部数据
    bool read_once();
    // 响应报文写入函数
    bool write();
    //???? 得到客户端的地址
    sockaddr_in *get_address()
    {
        return &m_address;
    }
    // 同步线程初始化数据库读取表？？？
    void initmysql_result(connection_pool *connPool);
    // timer_flag应该是timer到期的标志
    // 所以这个timer_flag应该是个用户连接是否异常的标志位
    int timer_flag;
    // improv是线程处理了相应的sockfd后，将其置为1，然后在dealwiththread中判断线程处理过的fd,并再将其置为0,以便后续的连接继续使用该fd。
    // improv标志在read_once和write成功后,都会被设置为1,在相应的request处理完成后,均会被设置位为0,所以这个标志是用来判断上一个请求是否已处理完毕
    int improv;

    /*每个http连接有两个标志位：improv和timer_flag，初始时其值为0，它们只在Reactor模式下发挥作用。因为在Reactor模式中，主线程只负责监听读/写事件（得知有读写数据了），线程池中的线程负责IO操作（真正地读/写数据）。但是如果线程池中的子线程进行客户的IO操作时，出现错误了，那子线程该怎么将这个错误通知给主线程呢？因此作者便让子线程将imporv和timer_flag这两个标志位置1来告知主线程：“子线程我呀，对这个客户连接的IO操作，它，出！错！了！，你关闭掉这个客户的连接吧！”，于是主线程就知道要关闭这个IO出错的客户的连接。

    所以简而言之，这两个标志位的作用就是：“Reactor模式下，当子线程执行读写任务出错时，来通知主线程关闭子线程的客户连接”。对于improv标志，其作用是保持主线程和子线程的同步；对于time_flag标志，其作用是标识子线程读写任务是否成功。

    下面是源代码中这两个标志具体的设置流程：

    在threadpool<T>::run()中的request->read_once()或request->write()完成后imporv会被置1，标志着http连接的读写任务已完成（请求已处理完毕）；如果http连接的读写任务失败（请求处理出错）timer_flag会被置1。

    在 WebServer::dealwith_read()和WebServer::dealwith_write()中，会循环等待连接improv被置1，也就是一直等待该http连接的请求处理完毕，如果请求处理失败则关闭该http连接。*/

private:
    void init();
    // 从m_read_buf读取，并处理请求报文
    HTTP_CODE process_read();
    // 向m_write_buf写入响应报文数据
    bool process_write(HTTP_CODE ret);
    // 主状态机解析报文中的请求行数据
    HTTP_CODE parse_request_line(char *text);
    // 主状态机解析报文中的请求头数据
    HTTP_CODE parse_headers(char *text);
    // 主状态机解析报文中的请求内容
    HTTP_CODE parse_content(char *text);
    // 生成响应报文
    HTTP_CODE do_request();
    // m_start_line是已经解析的字符
    // get_line用于将指针向后偏移，指向未处理的字符
    // m_start_line是行在buffer中的起始位置，将该位置后面的数据赋给text？？？？？
    // 此时从状态机已提前将一行的末尾字符\r\n变为\0\0，所以text可以直接取出完整的行进行解析
    char *get_line() { return m_read_buf + m_start_line; };
    // 从状态机读取一行，分析是请求报文的哪一部分
    LINE_STATUS parse_line();

    // ？？？？
    void unmap();

    // 根据响应报文格式，生成对应8个部分，以下函数均由do_request调用
    bool add_response(const char *format, ...);
    bool add_content(const char *content);
    bool add_status_line(int status, const char *title);
    bool add_headers(int content_length);
    bool add_content_type();
    bool add_content_length(int content_length);
    bool add_linger();
    bool add_blank_line();

public:
    static int m_epollfd; // epoll监听
    // http_conn类对象
    // 预先为每个可能的客户连接分配一个http_conn对象
    // 创建MAX_FD个http类对象
    // MAX_FD = 65536;  最大文件描述符
    static int m_user_count; // 有多少客户
    MYSQL *mysql;
    int m_state; // 读为0, 写为1,，在reactor模式下

private:
    int m_sockfd;          // 客户端请求的socket
    sockaddr_in m_address; // 客户端请求的socket地址

    // 存储读取的请求报文数据
    char m_read_buf[READ_BUFFER_SIZE];
    // 缓冲区中m_read_buf中数据的最后一个字节的下一个位置
    long m_read_idx;
    // m_read_buf读取的位置m_checked_idx
    long m_checked_idx;
    // m_read_buf中已经解析的字符个数？？？
    int m_start_line;
    // 存储发出的响应报文数据
    char m_write_buf[WRITE_BUFFER_SIZE];
    // 指示buffer中的长度
    int m_write_idx;
    // 主状态机的状态
    CHECK_STATE m_check_state;
    // 请求方法
    METHOD m_method;
    // 以下为解析请求报文中对应的6个变量
    // 存储读取文件的名称
    char m_real_file[FILENAME_LEN];
    char *m_url;
    char *m_version;       // HTTP1.1版本。
    char *m_host;          //  HOST，给出请求资源所在服务器的域名。
    long m_content_length; // 说明实现主体的大小
    bool m_linger;         // 连接管理，可以是Keep-Alive或close
    // GET /562f25980001b1b106000338.jpg HTTP/1.1
    // Host:img.mukewang.com
    // User-Agent:Mozilla/5.0 (Windows NT 10.0; WOW64)
    // AppleWebKit/537.36 (KHTML, like Gecko) Chrome/51.0.2704.106 Safari/537.36
    // Accept:image/webp,image/*,*/*;q=0.8
    // Referer:http://www.imooc.com/
    // Accept-Encoding:gzip, deflate, sdch
    // Accept-Language:zh-CN,zh;q=0.8
    // 空行
    // 请求数据为空

    // POST / HTTP1.1
    // Host:www.wrox.com
    // User-Agent:Mozilla/4.0 (compatible; MSIE 6.0; Windows NT 5.1; SV1; .NET CLR 2.0.50727; .NET CLR 3.0.04506.648; .NET CLR 3.5.21022)
    // Content-Type:application/x-www-form-urlencoded
    // Content-Length:40
    // Connection: Keep-Alive
    // 空行
    // name=Professional%20Ajax&publisher=Wiley

    // 读取服务器上的文件地址
    char *m_file_address;
    // 通过stat获取请求资源文件信息，成功则将信息更新到m_file_stat结构体
    // 失败返回NO_RESOURCE状态，表示资源不存在
    struct stat m_file_stat;
    // io向量机制iovec ???
    struct iovec m_iv[2];
    int m_iv_count;
    int cgi;        // 是否启用的POST
    char *m_string; // 存储请求头数据
    // 剩余发送字节数
    int bytes_to_send;
    // 已发送字节数
    int bytes_have_send;

    //?????
    char *doc_root;
    // 这个是干撒的？？？
    // 读取数据库中的账号和密码
    std::map<std::string, std::string> m_users;
    int m_TRIGMode;
    int m_close_log;

    char sql_user[100];
    char sql_passwd[100];
    char sql_name[100];
};

#endif
