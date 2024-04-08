/*
 * @Author: zjl 3106825030@qq.com
 * @Date: 2024-04-08 16:53:07
 * @LastEditors: zjl 3106825030@qq.com
 * @LastEditTime: 2024-04-08 21:02:22
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
        }
    }
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
        // LOG_INFO("oop!unknow header: %s", text);
    }
    return NO_REQUEST;
}
