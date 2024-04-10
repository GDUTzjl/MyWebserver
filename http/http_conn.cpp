/*
 * @Author: zjl 3106825030@qq.com
 * @Date: 2024-04-08 16:53:07
 * @LastEditors: zjl 3106825030@qq.com
 * @LastEditTime: 2024-04-10 09:56:07
 * @FilePath: /MyWebServer/http/http_conn.cpp
 * @Description:
 */
#include "http_conn.h"

// 定义http响应的一些状态信息
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file form this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the request file.\n";

/*-----------------epoll相关代码---------------------------*/
// 非http类成员函数
/* 项目中epoll相关代码部分包括
非阻塞模式、
内核事件表注册事件、
删除事件、
重置EPOLLONESHOT事件四种*/
// 对文件描述符设置非阻塞
int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}
// 将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
// 内核事件表注册新事件，开启EPOLLONESHOT，针对客户端连接的描述符，listenfd不用开启
//  一个线程读取某个socket上的数据后开始处理数据，在处理过程中该socket上又有新数据可读，此时另一个线程被唤醒读取，此时出现两个线程处理同一个socket
//   我们期望的是一个socket连接在任一时刻都只被一个线程处理，通过epoll_ctl对该文件描述符注册epolloneshot事件，一个线程处理socket时，其他线程将无法处理，当该线程处理完后，需要通过epoll_ctl重置epolloneshot事件
// 上树
void addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    // ET边缘触发模式
    if (1 == TRIGMode)
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    // 触发组合模式TRIGMode = 0,默认listenfd LT + connfd LT LT水平触发模式
    else
        event.events = EPOLLIN | EPOLLRDHUP;

    if (one_shot)
        event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

// 从内核时间表删除描述符
void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

// 将事件重置为EPOLLONESHOT
void modfd(int epollfd, int fd, int ev, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;
    // 客户端直接调用close，会触犯EPOLLRDHUP事件
    // 通过EPOLLRDHUP属性，来判断是否对端已经关闭，这样可以减少一次系统调用。在2.6.17的内核版本之前，只能再通过调用一次recv函数来判断
    if (1 == TRIGMode)
        event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    else
        event.events = ev | EPOLLONESHOT | EPOLLRDHUP;

    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

/*------------------http_conn类数据库和锁-----------------------*/

locker m_lock;
map<string, string> users;

// 将数据库中的用户名和密码载入到服务器的map中来，map中的key为用户名，value为密码。
void http_conn::initmysql_result(connection_pool *connPool)
{
    // 先从连接池中取一个连接
    MYSQL *mysql = NULL;
    connectionRAII mysqlcon(&mysql, connPool);

    // 在user表中检索username，passwd数据，浏览器端输入
    if (mysql_query(mysql, "SELECT username,passwd FROM user"))
    {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }

    // 从表中检索完整的结果集
    MYSQL_RES *result = mysql_store_result(mysql);

    // 返回结果集中的列数
    int num_fields = mysql_num_fields(result);

    // 返回所有字段结构的数组
    MYSQL_FIELD *fields = mysql_fetch_fields(result);

    // 从结果集中获取下一行，将对应的用户名和密码，存入map中
    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        string temp1(row[0]);
        string temp2(row[1]);
        users[temp1] = temp2;
    }
}

/*--------------------http_conn类------------------------------*/
// 静态变量初始化
int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;
// 初始化连接,外部调用初始化套接字地址
void http_conn::init(int sockfd, const sockaddr_in &addr, char *root, int TRIGMode, int close_log, string user, string passwd, string sqlname)
{
    m_sockfd = sockfd;
    m_address = addr;

    addfd(m_epollfd, sockfd, true, m_TRIGMode);
    m_user_count++;

    // 当浏览器出现连接重置时，可能是网站根目录出错或http响应格式出错或者访问的文件中内容完全为空
    doc_root = root;
    m_TRIGMode = TRIGMode;
    m_close_log = close_log;

    // 数据库相关
    strcpy(sql_user, user.c_str());
    strcpy(sql_passwd, passwd.c_str());
    strcpy(sql_name, sqlname.c_str());

    init();
}
void http_conn::init()
{
    timer_flag = 0;
    improv = 0;

    mysql = NULL;
    m_state = 0;

    m_read_idx = 0;
    m_checked_idx = 0;
    m_start_line = 0;

    m_write_idx = 0;

    m_check_state = CHECK_STATE_REQUESTLINE;
    m_method = GET;

    m_url = 0;
    m_version = 0;
    m_host = 0;
    m_content_length = 0;
    m_linger = false;

    cgi = 0;

    bytes_to_send = 0;
    bytes_have_send = 0;

    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
}

/*--------------------http_conn类读函数------------------------------*/

// 循环读取客户数据，直到无数据可读或对方关闭连接
// 非阻塞ET工作模式下，需要一次性将数据读完
bool http_conn::read_once()
{
    if (m_read_idx >= READ_BUFFER_SIZE)
    {
        return false;
    }
    // 读取的字符数
    int bytes_read = 0;
    // LT读取数据 LT水平触发模式
    if (0 == m_TRIGMode)
    {
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        m_read_idx += bytes_read;

        if (bytes_read <= 0)
        {
            return false;
        }

        return true;
    }
    // ET边缘触发模式,需要一次读完
    else
    {
        // socket网络编程中read与recv区别,补：
        // https://blog.csdn.net/superbfly/article/details/72782264
        while (true)
        {
            // 返回值：成功时返回实际读取的字节数，失败时返回-1，并设置errno变量来指示错误的原因。
            //  从套接字接收数据，存储在m_read_buf缓冲区
            bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
            if (bytes_read == -1)
            {
                // 非阻塞ET模式下，需要一次性将数据读完
                // 必须要一次性将数据读取完，使用非阻塞I/O，读取到出现eagain
                // 写数据时，若一次发送的数据超过TCP发送缓冲区，则返EAGAIN/EWOULDBLOCK，表示数据没用发送完，你一定要继续注册检测可写事件，否则你剩余的数据就再也没有机会发送了，因为 ET 模式的可写事件再也不会触发。
                // 返回值bytes_read>0,则读取正确
                // 返回值bytes_readt=0,客户端连接关闭
                // 返回值bytes_read<0,则需要看errno，当errno为EAGAIN或EWOULDBLOCK时，表明读取完毕，接收缓冲为空，在非阻塞IO下会立即返回-1.若errno不是上述标志，则说明读取数据出错，因该关闭连接，进行错误处理。
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;
                return false;
            }
            else if (bytes_read == 0)
            {
                return false;
            }
            // 修改m_read_idx的读取字节数
            m_read_idx += bytes_read;
        }
        return true;
    }
}
/*--------------------http_conn类，主函数 --流程的开始------------------------------*/
void http_conn::process()
{
    // 首先，process_read()，也就是对我们读入该connfd读缓冲区的请求报文进行解析。
    // HTTP_CODE含义
    // 表示HTTP请求的处理结果，在头文件中初始化了八种情形，在报文解析时只涉及到四种。
    // NO_REQUEST 请求不完整，需要继续读取请求报文数据
    // GET_REQUEST 获得了完整的HTTP请求
    // BAD_REQUEST HTTP请求报文有语法错误
    // INTERNAL_ERROR 服务器内部错误，该结果在主状态机逻辑switch的default下，一般不会触发
    HTTP_CODE read_ret = process_read();
    if (read_ret == NO_REQUEST)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        return;
    }
    bool write_ret = process_write(read_ret);
    if (!write_ret)
    {
        close_conn();
    }
    modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
}

/*--------------------http_conn类，各个解析流程------------------------------*/

/*
项目中使用主从状态机的模式进行解析，从状态机（parse_line）负责读取报文的一行，主状态机负责对该行数据进行解析，主状态机内部调用从状态机，从状态机驱动主状态机。
每解析一部分都会将整个请求的m_check_state状态改变，状态机也就是根据这个状态来进行不同部分的解析跳转的：

判断条件:
    主状态机转移到CHECK_STATE_CONTENT，该条件涉及解析消息体(POST专有)
    从状态机转移到LINE_OK，该条件涉及解析请求行和请求头部
    两者为或关系，当条件为真则继续循环，否则退出
循环体:
    从状态机读取数据
    调用get_line函数，通过m_start_line将从状态机读取数据间接赋给text
    其中char *get_line() { return m_read_buf + m_start_line; };
        m_start_line是已经解析的字符
        get_line用于将指针向后偏移，指向未处理的字符
        m_start_line是行在buffer中的起始位置，将该位置后面的数据赋给text
        此时从状态机已提前将一行的末尾字符\r\n变为\0\0，所以text可以直接取出完整的行进行解析
    主状态机解析text
这里为什么要写两个判断条件？第一个判断条件为什么这样写？:
    具体的在主状态机逻辑中会讲解。
    parse_line为从状态机的具体实现，取一行
那么，这里的判断条件为什么要写成这样呢？
    在GET请求报文中，每一行都是\r\n作为结束，所以对报文进行拆解时，仅用从状态机的状态line_status=parse_line())==LINE_OK语句即可。
    但，在POST请求报文中，消息体的末尾没有任何字符，所以不能使用从状态机的状态，这里转而使用主状态机的状态作为循环入口条件。
    那后面的&& line_status==LINE_OK又是为什么？
    解析完消息体后，报文的完整解析就完成了，但此时主状态机的状态还是CHECK_STATE_CONTENT，也就是说，符合循环入口条件，还会再次进入循环，这并不是我们所希望的。
*/

/*
POST / HTTP1.1
Host:www.wrox.com
User-Agent:Mozilla/4.0 (compatible; MSIE 6.0; Windows NT 5.1; SV1; .NET CLR 2.0.50727; .NET CLR 3.0.04506.648; .NET CLR 3.5.21022)
Content-Type:application/x-www-form-urlencoded
Content-Length:40
Connection: Keep-Alive
空行
name=Professional%20Ajax&publisher=Wiley
*/

// 主状态机初始状态是CHECK_STATE_REQUESTLINE，通过调用从状态机来驱动主状态机，
// 在主状态机进行解析前，从状态机已经将每一行的末尾\r\n符号改为\0\0，以便于主状态机直接取出对应字符串进行处理。
/*
CHECK_STATE_REQUESTLINE
    主状态机的初始状态，调用parse_request_line函数解析请求行
    解析函数从m_read_buf中解析HTTP请求行，获得请求方法、目标URL及HTTP版本号
    解析完成后主状态机的状态变为CHECK_STATE_HEADER
解析完请求行后，主状态机继续分析请求头。在报文中，请求头和空行的处理使用的同一个函数，这里通过判断当前的text首位是不是\0字符，若是，则表示当前处理的是空行，若不是，则表示当前处理的是请求头。

CHECK_STATE_HEADER
    调用parse_headers函数解析请求头部信息
    判断是空行还是请求头，若是空行，进而判断content-length是否为0，如果不是0，表明是POST请求，则状态转移到CHECK_STATE_CONTENT，否则说明是GET请求，则报文解析结束。
    若解析的是请求头部字段，则主要分析connection字段，content-length字段，其他字段可以直接跳过，各位也可以根据需求继续分析。
    connection字段判断是keep-alive还是close，决定是长连接还是短连接
    content-length字段，这里用于读取post请求的消息体长度
    解析http请求的一个头部信息
CHECK_STATE_CONTENT
    仅用于解析POST请求，调用parse_content函数解析消息体
    用于保存post请求消息体，为后面的登录和注册做准备

*/
//  process_read通过while循环，将主从状态机进行封装，对报文的每一行进行循环处理。
http_conn::HTTP_CODE http_conn::process_read()
{
    // 初始化从状态机状态、HTTP请求解析结果
    LINE_STATUS line_status = LINE_OK;
    // 报文解析的结果
    HTTP_CODE ret = NO_REQUEST;
    char *text = 0;
    while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) || ((line_status = parse_line()) == LINE_OK))
    {
        // 此时从状态机已提前将一行的末尾字符\r\n变为\0\0，所以text可以直接取出完整的行进行解析
        text = get_line(); // 遇到\0\0直接取出完整的行进行解析
        // m_start_line是每一个数据行在m_read_buf中的起始位置
        // m_checked_idx表示从状态机在m_read_buf中读取的位置
        m_start_line = m_checked_idx; // 跳到下一行的起始位置方便下一次读取，因为这一行已经解析完了，这里的行指的是readbuff里面遇到\r\n
        LOG_INFO("%s", text);
        // 主状态机的三种状态转移逻辑
        switch (m_check_state)
        {
        // 解析请求行 初始状态
        case CHECK_STATE_REQUESTLINE:
        {
            // parse_request_line(text)，解析请求行
            ret = parse_request_line(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            break;
        }
        case CHECK_STATE_HEADER:
        {
            // parse_headers(text);，解析请求头部
            ret = parse_headers(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            // 完整解析GET请求后，跳转到报文响应函数
            else if (ret == GET_REQUEST)
            {
                return do_request();
            }
            break;
        }
        case CHECK_STATE_CONTENT:
        {
            // parse_content(text);，解析请求数据
            ret = parse_content(text);

            // 完整解析POST请求后，跳转到报文响应函数
            if (ret == GET_REQUEST)
                return do_request();
            // 解析完消息体即完成报文解析，避免再次进入循环，更新line_status
            line_status = LINE_OPEN;
            break;
        }
        default:
            return INTERNAL_ERROR;
        }
    }
    return NO_REQUEST;
}
// 从状态机，用于分析出一行内容
// 返回值为行的读取状态，有LINE_OK,LINE_BAD,LINE_OPEN
// // 存储读取的请求报文数据
// char m_read_buf[READ_BUFFER_SIZE];
// // 缓冲区中m_read_buf中数据的最后一个字节的下一个位置
// long m_read_idx;
// // m_read_buf读取的位置m_checked_idx  m_checked_idx指向从状态机当前正在分析的字节
// long m_checked_idx;
// // m_read_buf中已经解析的字符个数？？？
// int m_start_line;
// 找\r\n 换成 \0\0
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    for (; m_checked_idx < m_read_idx; ++m_checked_idx)
    {
        // temp为将要分析的字节
        temp = m_read_buf[m_checked_idx];
        // 如果当前是\r字符，则有可能会读取到完整行
        if (temp == '\r')
        {
            if (m_checked_idx + 1 == m_read_idx)
            {
                return LINE_OPEN;
            }
            // 下一个字符是\n，将\r\n改为\0\0
            else if (m_read_buf[m_checked_idx + 1] == '\n')
            {
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            // 如果都不符合，则返回语法错误
            return LINE_BAD;
        }
        // 如果当前字符是\n，也有可能读取到完整行
        // 一般是上次读取到\r就到buffer末尾了，没有接收完整，再次接收时会出现这种情况
        else if (temp == '\n')
        {
            // 前一个字符是\r，则接收完整
            if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r')
            {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    // 并没有找到\r\n，需要继续接收
    return LINE_OPEN;
}
// GET /562f25980001b1b106000338.jpg HTTP/1.1
// POST / HTTP1.1
// 解析http请求行，获得请求方法，目标url及http版本号
// CHECK_STATE_REQUESTLINE
//     主状态机的初始状态，调用parse_request_line函数解析请求行
//     解析函数从m_read_buf中解析HTTP请求行，获得请求方法、目标URL及HTTP版本号
//     解析完成后主状态机的状态变为CHECK_STATE_HEADER
http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
    // 在HTTP报文中，请求行用来说明请求类型,要访问的资源以及所使用的HTTP版本，其中各个部分之间通过\t或空格分隔。
    // 请求行中最先含有空格和\t任一字符的位置并返回
    // C 库函数 char *strpbrk(const char *str1, const char *str2) 检索字符串 str1 中第一个匹配字符串 str2 中字符的字符，不包含空结束字符。
    // 也就是说，依次检验字符串 str1 中的字符，当被检验字符在字符串 str2 中也包含时，则停止检验，并返回该字符位置
    m_url = strpbrk(text, " \t"); // GET -> /562f25980001b1b106000338.jpg HTTP/1.1
    // 如果没有空格或\t，则报文格式有误
    if (!m_url)
    {
        return BAD_REQUEST;
    }
    // 将该位置改为\0，用于将前面数据取出
    *m_url++ = '\0';
    char *method = text;
    // 取出数据，并通过与GET和POST比较，以确定请求方式
    // C语言中判断字符串是否相等的函数，忽略大小写。s1和s2中的所有字母字符在比较之前都转换为小写。该strcasecmp()函数对空终止字符串进行操作。函数的字符串参数应包含一个(’\0’)标记字符串结尾的空字符。
    if (strcasecmp(method, "GET") == 0)
        m_method = GET;
    else if (strcasecmp(method, "POST") == 0)
    {
        m_method = POST;
        // 是否启用POST
        cgi = 1;
    }
    else
        return BAD_REQUEST;
    // m_url此时跳过了第一个空格或\t字符，但不知道之后是否还有
    // 将m_url向后偏移，通过查找，继续跳过空格和\t字符，指向请求资源的第一个字符
    m_url += strspn(m_url, " \t"); // GET /562f25980001b1b106000338.jpg HTTP/1.1

    // 使用与判断请求方式的相同逻辑，判断HTTP版本号
    m_version = strpbrk(m_url, " \t");
    if (!m_version)
        return BAD_REQUEST;

    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");
    // 仅支持HTTP/1.1
    if (strcasecmp(m_version, "HTTP/1.1") != 0)
        return BAD_REQUEST;
    // 对请求资源前7个字符进行判断
    // 这里主要是有些报文的请求资源中会带有http://，这里需要对这种情况进行单独处理
    if (strncasecmp(m_url, "http://", 7) == 0)
    {
        m_url += 7;
        m_url = strchr(m_url, '/');
    }
    // 同样增加https情况
    if (strncasecmp(m_url, "https://", 8) == 0)
    {
        m_url += 8;
        m_url = strchr(m_url, '/');
    }
    // 一般的不会带有上述两种符号，直接是单独的/或/后面带访问资源
    if (!m_url || m_url[0] != '/')
        return BAD_REQUEST;

    // 当url为/时，显示判断界面
    if (strlen(m_url) == 1)
        strcat(m_url, "judge.html");
    // 请求行处理完毕，将主状态机转移处理请求头
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
}

// 解析完请求行后，主状态机继续分析请求头。在报文中，请求头和空行的处理使用的同一个函数，这里通过判断当前的text首位是不是\0字符，若是，则表示当前处理的是空行，若不是，则表示当前处理的是请求头。

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
// CHECK_STATE_HEADER
// 调用parse_headers函数解析请求头部信息
// 判断是空行还是请求头，若是空行，进而判断content-length是否为0，如果不是0，表明是POST请求，则状态转移到CHECK_STATE_CONTENT，否则说明是GET请求，则报文解析结束。
// 若解析的是请求头部字段，则主要分析connection字段，content-length字段，其他字段可以直接跳过，各位也可以根据需求继续分析。
// connection字段判断是keep-alive还是close，决定是长连接还是短连接
// content-length字段，这里用于读取post请求的消息体长度
// 解析http请求的一个头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
    // 判断是空行还是请求头
    // 一般是先请求头在空行，所以m_content_length如果有就已经赋值了
    if (text[0] == '\0')
    {
        // 判断是GET还是POST请求 什么时候赋值的？？？？⬆⬆⬆⬆⬆⬆
        if (m_content_length != 0)
        {
            // POST需要跳转到消息体处理状态
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        return GET_REQUEST;
    }
    // 解析请求头部连接字段
    else if (strncasecmp(text, "Connection:", 11) == 0)
    {
        text += 11;
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0)
        {
            m_linger = true; // connection字段判断是keep-alive还是close，决定是长连接还是短连接
        }
    }
    // 解析请求头部内容长度字段
    else if (strncasecmp(text, "Content-length:", 15) == 0)
    {
        text += 15;
        text += strspn(text, " \t"); // Content-Length:40
        m_content_length = atol(text);
    }
    // 解析请求头部HOST字段
    else if (strncasecmp(text, "Host:", 5) == 0)
    {
        text += 5;
        text += strspn(text, " \t"); // Host:www.wrox.com
        m_host = text;
    }
    else
    {
        LOG_INFO("oop!unknow header: %s", text);
    }
    return NO_REQUEST;
}

// 服务器端解析浏览器的请求报文，当解析为POST请求时，cgi标志位设置为1，并将请求报文的消息体赋值给m_string，进而提取出用户名和密码。
//  判断http请求是否被完整读入
http_conn::HTTP_CODE http_conn::parse_content(char *text)
{
    // 判断buffer中是否读取了消息体
    if (m_read_idx >= (m_content_length + m_checked_idx))
    {
        text[m_content_length] = '\0';
        // POST请求中最后为输入的用户名和密码
        m_string = text;
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

/*
do_request
process_read函数的返回值是对请求的文件分析后的结果，一部分是语法错误导致的BAD_REQUEST，一部分是do_request的返回结果.该函数将网站根目录和url文件拼接，然后通过stat判断该文件属性。另外，为了提高访问速度，通过mmap进行映射，将普通文件映射到内存逻辑地址。
为了更好的理解请求资源的访问流程，这里对各种各页面跳转机制进行简要介绍。其中，浏览器网址栏中的字符，即url，可以将其抽象成ip:port/xxx，xxx通过html文件的action属性进行设置。
m_url为请求报文中解析出的请求资源，以/开头，也就是/xxx，项目中解析后的m_url有8种情况。
    /
    GET请求，跳转到judge.html，即欢迎访问页面
    /0
    POST请求，跳转到register.html，即注册页面
    /1
    POST请求，跳转到log.html，即登录页面
    /2CGISQL.cgi
    POST请求，进行登录校验
    验证成功跳转到welcome.html，即资源请求成功页面
    验证失败跳转到logError.html，即登录失败页面
    /3CGISQL.cgi
    POST请求，进行注册校验
    注册成功跳转到log.html，即登录页面
    注册失败跳转到registerError.html，即注册失败页面
    /5
    POST请求，跳转到picture.html，即图片请求页面
    /6
    POST请求，跳转到video.html，即视频请求页面
    /7
    POST请求，跳转到fans.html，即关注页面
//见/root/README.md
如果大家对上述设置方式不理解，不用担心。具体的登录和注册校验功能会在第12节进行详解，到时候还会针对html进行介绍。
*/

// do_request代码部分，我们需要首先对GET请求和不同POST请求（登录，注册，请求图片，视频等等）做不同的预处理，然后分析目标文件的属性，若目标文件存在、对所有用户可读且不是目录时，则使用mmap将其映射到内存地址m_file_address处，并告诉调用者获取文件成功。
http_conn::HTTP_CODE http_conn::do_request()
{
    // 将初始化的m_real_file赋值为网站根目录
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    // printf("m_url:%s\n", m_url);
    // 找到m_url中/的位置
    // strrchr()函数：
    //     strrchr()函数的作用是：
    //     查找一个字符串在另一个字符串中 末次 出现的位置，并返回从字符串中的这个位置起，一直到字符串结束的所有字符；
    //     如果未能找到指定字符，那么函数将返回False。
    const char *p = strrchr(m_url, '/');
    // 处理cgi = 1 允许post
    // 实现登录和注册校验
    // 根据标志判断是登录检测还是注册检测
    // 同步线程登录校验
    // CGI多进程登录校验
    // 服务器端解析浏览器的请求报文，当解析为POST请求时，cgi标志位设置为1，并将请求报文的消息体赋值给m_string，进而提取出用户名和密码。
    if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3'))
    {
        // 根据标志判断是登录检测还是注册检测
        char flag = m_url[1]; // 好像也没用上
        // m_url = '/3CGISQL.cgi' or '/2CGISQL.cgi'

        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/");
        strcat(m_url_real, m_url + 2);
        strncpy(m_real_file + len, m_url_real, FILENAME_LEN - len - 1);
        free(m_url_real);

        // 将用户名和密码提取出来
        // user=123&passwd=123
        char name[100], password[100];
        int i;
        // 以&为分隔符，前面的为用户名    user=
        for (i = 5; m_string[i] != '&'; ++i)
            name[i - 5] = m_string[i];
        name[i - 5] = '\0';

        int j = 0;
        // 以&为分隔符，后面的是密码  i+10    password= ??????
        for (i = i + 10; m_string[i] != '\0'; ++i, ++j)
            password[j] = m_string[i];
        password[j] = '\0';
        //  注册校验
        if (*(p + 1) == '3')
        {
            // 如果是注册，先检测数据库中是否有重名的
            // 没有重名的，进行增加数据
            char *sql_insert = (char *)malloc(sizeof(char) * 200);
            strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");
            strcat(sql_insert, "'"); // 单引号？？
            strcat(sql_insert, name);
            strcat(sql_insert, "', '");
            strcat(sql_insert, password);
            strcat(sql_insert, "')");
            // 判断map中能否找到重复的用户名
            if (users.find(name) == users.end())
            {
                // 向数据库中插入数据时，需要通过锁来同步数据
                m_lock.lock();
                int res = mysql_query(mysql, sql_insert);
                users.insert(pair<string, string>(name, password));
                m_lock.unlock();

                // 校验成功，跳转登录页面
                if (!res)
                    strcpy(m_url, "/log.html");
                // 校验失败，跳转注册失败页面
                else
                    strcpy(m_url, "/registerError.html");
            }
            else
                strcpy(m_url, "/registerError.html");
        }
        //  登录校验
        else if (*(p + 1) == '2')
        {
            if (users.find(name) != users.end() && users[name] == password)
                strcpy(m_url, "/welcome.html");
            else
                strcpy(m_url, "/logError.html");
        }
    }
    // 如果请求资源为/0，表示跳转注册界面
    if (*(p + 1) == '0')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/register.html");
        // 将网站目录和/register.html进行拼接，更新到m_real_file中
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    }
    // 如果请求资源为/1，表示跳转登录界面
    else if (*(p + 1) == '1')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    // 显示图片页面，POST
    else if (*(p + 1) == '5')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/picture.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    // 显示视频页面，POST
    else if (*(p + 1) == '6')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/video.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    //  显示关注页面，POST
    else if (*(p + 1) == '7')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/fans.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else
        // 如果以上均不符合，即不是登录和注册，直接将url与网站目录拼接
        // 这里的情况是welcome界面，请求服务器上的一个图片
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);
    /*
        stat函数用于取得指定文件的文件属性，并将文件属性存储在结构体stat里，这里仅对其中用到的成员进行介绍。
        //获取文件属性，存储在statbuf中
        int stat(const char *pathname, struct stat *statbuf);

        struct stat
        {
        mode_t    st_mode;         文件类型和权限
        off_t     st_size;        文件大小，字节数
        };
    */

    if (stat(m_real_file, &m_file_stat) < 0)
        return NO_RESOURCE;
    // 判断文件的权限，是否可读，不可读则返回FORBIDDEN_REQUEST状态
    if (!(m_file_stat.st_mode & S_IROTH))
        return FORBIDDEN_REQUEST;
    // 判断文件类型，如果是目录，则返回BAD_REQUEST，表示请求报文有误
    if (S_ISDIR(m_file_stat.st_mode))
        return BAD_REQUEST;
    // 以只读方式获取文件描述符，通过mmap将该文件映射到内存中
    int fd = open(m_real_file, O_RDONLY);
    // mmap用于将一个文件或其他对象映射到内存，提高文件的访问速度。
    // 成功：返回创建的映射区首地址  失败：MAP_FAILED宏
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    return FILE_REQUEST;
}

// C语言中可变参数的用法——va_list、va_start、va_arg、va_end参数定义  https://blog.csdn.net/edonlii/article/details/8497704
// 根据响应报文格式，生成对应8个部分，以下函数均由do_request -> process_write调用
bool http_conn::add_response(const char *format, ...)
{
    // 如果写入内容超出m_write_buf大小则报错
    if (m_write_idx >= WRITE_BUFFER_SIZE)
        return false;
    // 定义可变参数列表
    va_list arg_list;
    va_start(arg_list, format);
    // 将数据format从可变参数列表写入缓冲区写，返回写入数据的长度
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
    // 如果写入的数据长度超过缓冲区剩余空间，则报错
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx))
    {
        va_end(arg_list);
        return false;
    }
    // 更新m_write_idx位置
    m_write_idx += len;
    // 清空可变参列表
    va_end(arg_list);

    // LOG_INFO("request:%s", m_write_buf);

    return true;
}

bool http_conn::add_status_line(int status, const char *title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}
// 添加Content-Length，表示响应报文的长度
bool http_conn::add_content_length(int content_len)
{
    return add_response("Content-Length:%d\r\n", content_len);
}
// 添加连接状态，通知浏览器端是保持连接还是关闭
bool http_conn::add_linger()
{
    return add_response("Connection:%s\r\n", (m_linger == true) ? "keep-alive" : "close");
}
// 添加空行
bool http_conn::add_blank_line()
{
    return add_response("%s", "\r\n");
}
// 添加文本类型，这里是html 没有用到
bool http_conn::add_content_type()
{
    return add_response("Content-Type:%s\r\n", "text/html");
}

// 添加消息报头，具体的添加文本长度、连接状态和空行
bool http_conn::add_headers(int content_len)
{
    return add_content_length(content_len) && add_linger() &&
           add_blank_line();
}
// 添加文本content
bool http_conn::add_content(const char *content)
{
    return add_response("%s", content);
}

// 响应报文分为两种，一种是请求文件的存在，通过io向量机制iovec，声明两个iovec，第一个指向m_write_buf，第二个指向mmap的地址m_file_address；一种是请求出错，这时候只申请一个iovec，指向m_write_buf。
// iovec是一个结构体，里面有两个元素，指针成员iov_base指向一个缓冲区，这个缓冲区是存放的是writev将要发送的数据。
// 成员iov_len表示实际写入的长度

bool http_conn::process_write(HTTP_CODE ret)
{
    switch (ret)
    {
    // 内部错误，500
    case INTERNAL_ERROR:
    {
        // 状态行
        add_status_line(500, error_500_title);
        // 消息报头
        add_headers(strlen(error_500_form));
        if (!add_content(error_500_form))
            return false;
        break;
    }
    // 报文语法有误，404
    case BAD_REQUEST:
    {
        add_status_line(404, error_404_title);
        add_headers(strlen(error_404_form));
        if (!add_content(error_404_form))
            return false;
        break;
    }
    // 资源没有访问权限，403
    case FORBIDDEN_REQUEST:
    {
        add_status_line(403, error_403_title);
        add_headers(strlen(error_403_form));
        if (!add_content(error_403_form))
            return false;
        break;
    }
    // 文件存在，200
    case FILE_REQUEST:
    {
        add_status_line(200, ok_200_title);
        // 如果请求的资源存在
        if (m_file_stat.st_size != 0)
        {
            add_headers(m_file_stat.st_size);
            // 第一个iovec指针指向响应报文缓冲区，长度指向m_write_idx
            m_iv[0].iov_base = m_write_buf;
            m_iv[0].iov_len = m_write_idx;
            // 第二个iovec指针指向mmap返回的文件指针，长度指向文件大小
            m_iv[1].iov_base = m_file_address;
            m_iv[1].iov_len = m_file_stat.st_size;
            m_iv_count = 2;
            // 发送的全部数据为响应报文头部信息和文件大小
            bytes_to_send = m_write_idx + m_file_stat.st_size;
            return true;
        }
        else
        {
            // 如果请求的资源大小为0，则返回空白html文件
            const char *ok_string = "<html><body></body></html>";
            add_headers(strlen(ok_string));
            if (!add_content(ok_string))
                return false;
        }
    }
    default:
        return false;
    }
    // 除FILE_REQUEST状态外，其余状态只申请一个iovec，指向响应报文缓冲区
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}

// 关闭连接，关闭一个连接，客户总量减一
void http_conn::close_conn(bool real_close)
{
    if (real_close && (m_sockfd != -1))
    {
        printf("close %d\n", m_sockfd);
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

/*
http_conn::write
    服务器子线程调用process_write完成响应报文，随后注册epollout事件。服务器主线程检测写事件，并调用http_conn::write函数将响应报文发送给浏览器端。
该函数具体逻辑如下：
    在生成响应报文时初始化byte_to_send，包括头部信息和文件数据大小。通过writev函数循环发送响应报文数据，根据返回值更新byte_have_send和iovec结构体的指针和长度，并判断响应报文整体是否发送成功。
    若writev单次发送成功，更新byte_to_send和byte_have_send的大小，若响应报文整体发送成功,则取消mmap映射,并判断是否是长连接.
    长连接重置http类实例，注册读事件，不关闭连接，
    短连接直接关闭连接
    若writev单次发送不成功，判断是否是写缓冲区满了。
    若不是因为缓冲区满了而失败，取消mmap映射，关闭连接
    若eagain则满了，更新iovec结构体的指针和长度，并注册写事件，等待下一次写事件触发（当写缓冲区从不可写变为可写，触发epollout），因此在此期间无法立即接收到同一用户的下一请求，但可以保证连接的完整性。
*/

/*---------------------http_conn写函数------------------------------------------------*/

void http_conn::unmap()
{
    if (m_file_address)
    {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}
bool http_conn::write()
{
    int temp = 0;

    // 若要发送的数据长度为0
    // 表示响应报文为空，一般不会出现这种情况
    if (bytes_to_send == 0)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        init();
        return true;
    }
    // 疑问往一个缓冲区发送函数这个缓存区不用清嘛，发送或者接收是怎么清理缓冲区的？？？？？？？？？？？？
    while (1)
    {
        // 将响应报文的状态行、消息头、空行和响应正文发送给浏览器端
        temp = writev(m_sockfd, m_iv, m_iv_count);

        // 正常发送，temp为发送的字节数
        if (temp < 0)
        {
            // 判断缓冲区是否满了
            if (errno == EAGAIN)
            {
                modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
                return true;
            }
            unmap();
            return false;
        }
        // 更新已发送字节
        bytes_have_send += temp;
        // 偏移文件iovec的指针
        bytes_to_send -= temp;
        // 第一个iovec头部信息的数据已发送完，发送第二个iovec数据
        if (bytes_have_send >= m_iv[0].iov_len)
        {
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        // 继续发送第一个iovec头部信息的数据
        else
        {
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }
        // 判断条件，数据已全部发送完
        if (bytes_to_send <= 0)
        {
            unmap();
            // 在epoll树上重置EPOLLONESHOT事件
            modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);

            // 浏览器的请求为长连接
            if (m_linger)
            {
                // 重新初始化HTTP对象
                init();
                return true;
            }
            else
            {
                return false;
            }
        }
    }
}
