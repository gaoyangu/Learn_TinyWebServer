#include "http_conn.h"

// #define connfdLT /* 水平触发阻塞 */
#define connfdET /* 边缘触发非阻塞*/

/* 定义 http 响应的一些状态信息 */
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file form this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the request file.\n";

/* 当浏览器出现连接重置时，可能是网站根目录出错或 http 响应格式出错
   或者访问的文件中内容完全为空 */
const char* doc_root = "/home/qgy/github/TinyWebServer/root";

/* 将表中的用户名和密码放入 map */
map<string, string> users;
locker lock;

void http_conn::initmysql_result(connection_pool *connPool)
{
    /* 先从连接池中取一个连接 */
    MYSQL* mysql = NULL;
    connectionRAII mysqlcon(&mysql, connPool);

    if(mysql_query(mysql, "SELECT username,passwd FROM user"))
    {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }

    /* 从表中检索完整的结果集 */
    MYSQL_RES* result = mysql_store_result(mysql);

    /* 返回结果集中的列数 */
    int num_fields = mysql_num_fields(result);

    /* 返回所有字段结构的数组 */
    MYSQL_FIELD* fields = mysql_fetch_fields(result);

    /* 从结果集中获取下一行，将对应的用户名和密码，存入 map 中*/
    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        string temp1(row[0]);
        string temp2(row[1]);
        users[temp1] = temp2;
    }
}

int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

void addfd(int epollfd, int fd, bool one_shot)
{
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    if (one_shot)
    {
        event.events |= EPOLLONESHOT;
    }
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

void modfd(int epollfd, int fd, int ev)
{
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

void http_conn::close_conn(bool real_close)
{
    if(real_close && (m_sockfd != -1))
    {
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

/* 初始化连接，外部调用初始化套接字地址 */
void http_conn::init(int sockfd, const sockaddr_in& addr)
{
    m_sockfd = sockfd;
    m_address = addr;
    /* 如下两行是为了避免 TIME_WAIT 状态，仅用于调试，实际使用时应该去掉 */
    // int reuse = 1;
    // setsockopt(m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    addfd(m_epollfd, sockfd, true);
    m_user_count++;

    init();
}

/* 初始化新接受的连接 */
/* m_check_state 默认为分析请求行状态 */
void http_conn::init()
{
    m_check_state = CHECK_STATE_REQUESTLINE;
    m_linger = false;

    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
}

/* 从状态机，用于分析出一行的内容 */
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    for(; m_checked_idx < m_read_idx; ++m_checked_idx)
    {
        /* temp 为将要分析的字节 */
        temp = m_read_buf[m_checked_idx];

        /* 如果当前是 \r 字符，则有可能会读取到完整行 */
        if(temp == '\r')
        {
            /* 下一个字符到达了 buffer 结尾，则接受不完整，需要继续接收 */
            if((m_checked_idx + 1) == m_read_idx)
            {
                return LINE_OPEN;
            }
            /* 下一个字符是 \n，则 \r\n 改为 \0\0 */
            else if(m_read_buf[m_checked_idx + 1] == '\n')
            {
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
        else if(temp == '\n')
        {
            /* 前一个字符是 \r，则接收完整 */
            if(m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r')
            {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    /* 没有找到 \r\n，需要继续接收 */
    return LINE_OPEN;
}

/* 循环读取客户数据，直到无数据可读或对方关闭连接 */
/* 非阻塞ET工作模式下，需要一次性将数据读完 */
bool http_conn::read_once()
{
    if(m_read_idx >= READ_BUFFER_SIZE)
    {
        return false;
    }

    int bytes_read = 0;
#ifdef connfdLT
    bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, 
                                READ_BUFFER_SIZE - m_read_idx, 0);
    if(bytes_read <= 0)
    {
        return false;
    }

    m_read_idx += bytes_read;

    return true;
#endif

#ifdef connfdET
    while (true)
    {
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, 
                                READ_BUFFER_SIZE - m_read_idx, 0);
        if(bytes_read == -1)
        {
            if(errno == EAGAIN || errno == EWOULDBLOCK)
            {
                break;
            }
            return false;
        }
        else if(bytes_read == 0)
        {
            return false;
        }

        m_read_idx += bytes_read;
    }
    return true;
#endif
}

/* 解析 http 请求行，获得请求方法，目标url及http版本号 */
http_conn::HTTP_CODE http_conn::parse_request_line(char* text)
{
    /* 请求行中最先含有空格和 \t 任一字符的位置并返回 */
    m_url = strpbrk(text, " \t");
    
    if(!m_url)
    {
        return BAD_REQUEST;
    }

    /* 将该位置改为 \0，用于将前面数据取出 */
    *m_url++ = '\0';

    char* method = text;
    if(strcasecmp(method, "GET") == 0)
    {
        m_method = GET;
    }
    else if(strcasecmp(method, "POST") == 0)
    {
        m_method = POST;
    }
    else
    {
        return BAD_REQUEST;
    }

    /* m_url 跳过了第一个空格或\t字符，但不知道之后是否还有 */
    /* 将 m_url 向后偏移，通过查找，继续跳过空格和\t字符，指向请求资源的第一个字符 */
    m_url += strspn(m_url, " \t");

    /* 判断 HTTP 版本号 */
    m_version = strpbrk(m_url, " \t");
    if(!m_version)
    {
        return BAD_REQUEST;
    }
    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");

    /* 仅支持 HTTP/1.1 */
    if(strcasecmp(m_version, "HTTP/1.1") != 0)
    {
        return BAD_REQUEST;
    }

    /* 对请求资源前7个字符进行判断 */
    if(strncasecmp(m_url, "http://", 7) == 0)
    {
        m_url += 7;
        m_url = strchr(m_url, '/'); /* 查找字符串中的一个字符，并返回该字符在字符串中第一次出现的位置 */
    }
    if(strncasecmp(m_url, "https://", 8) == 0)
    {
        m_url += 8;
        m_url = strchr(m_url, '/');
    }
    /* 一般不会带有上述两种符号，直接是单独的 / 或 / 后面带访问资源 */
    if(!m_url || m_url[0] != '/')
    {
        return BAD_REQUEST;
    }
    /* 当 url 为 / 时，显示欢迎界面 */
    if(strlen(m_url) == 1)
    {
        strcat(m_url, "judge.html"); /*  把 src 所指向的字符串追加到 dest 所指向的字符串的结尾 */
    }

    /* 请求行处理完毕，将主状态机转移处理请求头 */
    m_check_state = CHECK_STATE_HEADER;

    return NO_REQUEST;
}

/* 解析 http 请求的一个头部信息 */
http_conn::HTTP_CODE http_conn::parse_headers(char* text)
{
    /* 判断是否是空行 */
    if(text[0] == '\0')
    {
        /* 判断是否是 POST 请求 */
        if(m_content_length != 0)
        {
            /* POST 请求需要跳转到消息体处理状态 */
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        return GET_REQUEST;
    }
    /* 解析请求头部的连接字段 */
    else if(strncasecmp(text, "Connection:", 11) == 0)
    {
        text += 11;
        text += strspn(text, " \t");
        if(strcasecmp(text, "keep-alive") == 0)
        {
            m_linger = true;
        }
    }
    /* 解析请求头部的内容长度字段 */
    else if(strncasecmp(text, "Content-length:", 15) == 0)
    {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    }
    /* 解析请求头部的 HOST 字段 */
    else if(strncasecmp(text, "HOST:", 5) == 0)
    {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    else
    {
        printf("oop! unkonw header: %s\n", text);
    }
    return NO_REQUEST;
}

/* 解析 http 请求的消息体 */
http_conn::HTTP_CODE http_conn::parse_content(char* text)
{
    /* 判断 buffer 中是否读取了消息体 */
    if(m_read_idx >= (m_content_length + m_checked_idx))
    {
        text[m_content_length] = '\0';
        /* POST 请求中最后为输入的用户名和密码 */
        m_string = text;
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::process_read()
{
    LINE_STATUS line_stats = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char* text = 0;

    while ((m_check_state == CHECK_STATE_CONTENT && line_stats == LINE_OK) ||
            ((line_stats = parse_line()) == LINE_OK))
    {
        text = get_line();
        m_start_line = m_checked_idx;

        /* 主状态机的三种状态转移逻辑 */
        switch (m_check_state)
        {
        case CHECK_STATE_REQUESTLINE:
        {
            /* 解析请求行 */
            ret = parse_request_line(text);
            if(BAD_REQUEST == ret)
            {
                return BAD_REQUEST;
            }
            break;
        }
        case CHECK_STATE_HEADER:
        {
            /* 解析请求头 */
            ret = parse_headers(text);
            if(BAD_REQUEST == ret)
            {
                return BAD_REQUEST;
            }
            /* 完整解析 GET 请求后，跳转到报文响应函数 */
            else if(GET_REQUEST == ret)
            {
                return do_request();
            }
            break;
        }
        case CHECK_STATE_CONTENT:
        {
            /* 解析消息体 */
            ret = parse_content(text);
            /* 完整解析 POST 请求后，跳转到报文响应函数 */
            if(GET_REQUEST == ret)
            {
                return do_request();
            }

            /* 完成报文解析，避免再次进入循环，更新 line_status */
            line_stats = LINE_OPEN;
            break;
        }
        default:
            return INTERNAL_ERROR;
        }
    }
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::do_request()
{
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);

    /* 找到 m_url 中 / 的位置 */
    const char* p = strrchr(m_url, '/');

    /* 实现登录和注册校验 */
    if(cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3'))
    {

    }

    /* 如果请求资源为 /0，表示跳转注册界面 */
    if(*(p + 1) == '0')
    {
        char* m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/register.html");

        /* 将网站目录和 /register.html 进行拼接，更新到 m_real_file 中 */
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    /* /1，跳转登录界面 */
    else if(*(p + 1) == '1')
    {
        char* m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");

        /* 将网站目录和 /log.html 进行拼接，更新到 m_real_file 中 */
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else
    {
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);
    }

    if(stat(m_real_file, &m_file_stat) < 0)
    {
        return NO_RESOURCE;
    }
    /* 判断文件的权限，是否可读 */
    if(!(m_file_stat.st_mode & S_IROTH))
    {
        return FORBIDDEN_REQUEST;
    }
    /* 判断文件类型 */
    if(S_ISDIR(m_file_stat.st_mode))
    {
        return BAD_REQUEST;
    }

    /* 以只读方式获取文件描述符，通过mmap 将该文件映射到内存中 */
    int fd = open(m_real_file, O_RDONLY);
    m_file_address = (char*)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);

    return FILE_REQUEST;
}

void http_conn::unmap()
{
    if(m_file_address)
    {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}

bool http_conn::wirte()
{
    int temp = 0;
    int newadd = 0;

    /* 响应报文为空，一般不会发生这种情况 */
    if(bytes_to_send == 0)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        init();
        return true;
    }

    while (1)
    {
        /* 将响应报文的状态行、消息头、空行和响应正文发送给浏览器 */
        temp = write(m_sockfd, m_iv, m_iv_count);

        /* 正常发送，temp 为发送的字节数 */
        if(temp > 0 )
        {
            bytes_have_send += temp;
            /* 偏移文件 iovec 的指针 */
            newadd = bytes_have_send - m_write_idx;
        }
        if(temp <= -1)
        {
            /* 判断缓冲区是否填满 */
            if(errno == EAGAIN)
            {
                /* 第一个 iovec 头部信息的数据已发送完，发送第二个 iovec 数据 */
                if(bytes_have_send >= m_iv[0].iov_len)
                {
                    m_iv[0].iov_len = 0;
                    m_iv[1].iov_base = m_file_address + newadd;
                    m_iv[1].iov_len = bytes_to_send;
                }
                /* 继续发送第一个 iovec 头部信息的数据 */
                else
                {
                    m_iv[0].iov_base = m_write_buf + bytes_to_send;
                    m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
                }
                /* 重新注册写事件 */
                modfd(m_epollfd, m_sockfd, EPOLLOUT);
                return true;
            }
            
            unmap();
            return false;
        }

        /* 更新已发送字节数 */
        bytes_to_send -= temp;

        /* 数据已全部发送完 */
        if(bytes_to_send <= 0)
        {
            unmap();

            /* 在 epoll 树上重置 EPOLLONESHOT 事件 */
            modfd(m_epollfd, m_sockfd, EPOLLIN);

            /* 浏览器的请求为长连接 */
            if(m_linger)
            {
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

/* 往写缓冲区中写入待发送的数据 */
bool http_conn::add_response(const char* format, ...)
{
    if(m_write_idx >= WRITE_BUFFER_SIZE)
    {
        return false;
    }

    /* 定义可变参数列表 */
    va_list arg_list;
    va_start(arg_list, format);

    /* 将数据format 从可变参数列表写入写缓冲区，返回写入数据的长度 */
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - m_write_idx - 1,
                        format, arg_list);
    if(len >= (WRITE_BUFFER_SIZE - m_write_idx - 1))
    {
        va_end(arg_list);
        return false;
    }

    m_write_idx += len;
    va_end(arg_list);

    return true;
}

/* 添加状态行 */
bool http_conn::add_status_line(int status, const char* title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

/* 添加消息报头，具体的添加文本长度、连接状态和空行 */
bool http_conn::add_headers(int content_length)
{
    add_content_length(content_length);
    add_linger();
    add_blank_line();
}

/* 添加 Content-Length，表示响应报文的长度 */
bool http_conn::add_content_length(int content_length)
{
    return add_response("Content-Length:%d\r\n", content_length);
}

/* 添加连接状态，通知浏览器端是保持连接还是关闭 */
bool http_conn::add_linger()
{
    return add_response("Connection:%s\r\n", 
                        (m_linger == true ? "keep-alive" : "close"));
}

/* 添加空行 */
bool http_conn::add_blank_line()
{
    return add_response("%s", "\r\n");
}

/* 添加文本 content */
bool http_conn::add_content(const char* content)
{
    return add_response("%s", content);
}

/* 根据服务器处理 HTTP 请求的结果，决定返回给客户端的内容 */
bool http_conn::process_wirte(HTTP_CODE ret)
{
    switch (ret)
    {
    /* 内部错误，500 */    
    case INTERNAL_ERROR:
    {
        add_status_line(500, error_500_title);
        add_headers(strlen(error_500_form));
        if(!add_content(error_500_form))
        {
            return false;
        }
        break;
    }
    /* 报文语法有误，404 */    
    case BAD_REQUEST:
    {
        add_status_line(404, error_404_title);
        add_headers(strlen(error_404_form));
        if(!add_content(error_404_form))
        {
            return false;
        }
        break;
    }
    /* 资源没有访问权限，403 */    
    case FORBIDDEN_REQUEST:
    {
        add_status_line(403, error_403_title);
        add_headers(strlen(error_403_form));
        if(!add_content(error_403_form))
        {
            return false;
        }
        break;
    }
    /* 文件存在，200*/
    case FILE_REQUEST:
    {
        add_status_line(200, ok_200_title);
        /* 如果请求的资源存在 */
        if(m_file_stat.st_size != 0)
        {
            add_headers(m_file_stat.st_size);
            /* 第一个 iovec 指针指向响应报文缓冲区，长度指向 m_write_index */
            m_iv[0].iov_base = m_write_buf;
            m_iv[0].iov_len = m_write_idx;
            /* 第二个 iovec 指针指向 mmap 返回的文件指针，长度指向文件大小 */
            m_iv[0].iov_base = m_file_address;
            m_iv[0].iov_len = m_file_stat.st_size;
            m_iv_count = 2;
            /* 发送的全部数据为响应报文头部信息和文件大小 */
            bytes_to_send = m_write_idx + m_file_stat.st_size;
        }
        else
        {
            /* 如果请求的资源大小为0，则返回空白 html 文件 */
            const char* ok_string = "<html><body></body></html>";
            add_headers(strlen(ok_string));
            if(!add_content(ok_string))
            {
                return false;
            }
        }
        break;
    }
    default:
        return false;
    }

    /* 除 FILE_REQUEST 状态外，其余状态只申请一个 iovec，指向响应报文缓冲区 */
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 2;

    return true;
}

void http_conn::process()
{
    HTTP_CODE read_ret = process_read();

    if(NO_REQUEST == read_ret)
    {
        /* 注册并监听 读事件 */
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        return;
    }

    /* 完成报文响应 */
    bool write_ret = process_wirte(read_ret);
    if(!write_ret)
    {
        close_conn();
    }

    /* 注册并监听 写事件 */
    modfd(m_epollfd, m_sockfd, EPOLLOUT);
}