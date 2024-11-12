#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H

#include <signal.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>

#include <sys/stat.h>
#include <string.h>

#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <map>
#include <sys/uio.h>

#include "../CGImysql/sql_connection_pool.h"

class http_conn
{
public:
    /* 文件名的最大长度 */
    static const int FILENAME_LEN = 200;
    /* 读缓冲区的大小 */
    static const int READ_BUFFER_SIZE = 2048;
    /* 写缓冲区的大小 */
    static const int WRITE_BUFFER_SIZE = 1024;
    /* HTTP请求方法，仅支持GET*/
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
        PATCH
    };
    /* 解析客户请求时，主状态机所处的状态 */
    enum CHECK_STATE
    {
        CHECK_STATE_REQUESTLINE = 0,    /* 解析请求行 */
        CHECK_STATE_HEADER,             /* 解析请求头 */
        CHECK_STATE_CONTENT             /* 解析消息体，仅用于解析 POST 请求 */
    };
    /* 服务器处理HTTP请求的可能结果 */
    enum HTTP_CODE
    {
        NO_REQUEST,     /* 请求不完整，需要继续读取请求报文数据 */
        GET_REQUEST,    /* 获得完整的 HTTP 请求 */
        BAD_REQUEST,    /* HTTP 请求报文有语法错误 */
        NO_RESOURCE,        /* 请求资源不存在 */
        FORBIDDEN_REQUEST,  /* 请求资源禁止访问，没有读取权限  */
        FILE_REQUEST,       /* 请求资源可以正常访问 */
        INTERNAL_ERROR, /* 服务器内部错误，该结果在主状态机逻辑switch的default下，一般不会触发 */
        CLOSED_CONNECTION
    };
    /* 行的读取状态 */
    enum LINE_STATUS
    {
        LINE_OK = 0,    /* 完整读取一行 */
        LINE_BAD,       /* 报文语法有误 */
        LINE_OPEN       /* 读取的行不完整 */
    };

public:
    http_conn(){ }
    ~http_conn(){ }

public:
    /* 初始化新接受的连接 */
    void init(int sockfd, const sockaddr_in& addr);
    /* 关闭连接 */
    void close_conn(bool real_close = true);
    /* 处理客户请求 */
    void process();
    /* 非阻塞读操作 */
    bool read_once();
    /* 非阻塞写操作 */
    bool write();

    void initmysql_result(connection_pool *connPool);

private:
    /* 初始化连接 */
    void init();
    /* 解析 HTTP 请求 */
    HTTP_CODE process_read();
    /* 填充 HTTP 应答 */
    bool process_wirte(HTTP_CODE ret);

    /* 下面这组函数被 process_read 调用以分析 HTTP 请求 */
    HTTP_CODE parse_request_line(char* text);
    HTTP_CODE parse_headers(char* text);
    HTTP_CODE parse_content(char* text);
    HTTP_CODE do_request();
    char* get_line() {return m_read_buf + m_start_line;}
    LINE_STATUS parse_line();

    /* 下面这组函数被 process_write 调用以填充 HTTP 应答 */
    void unmap();
    bool add_response(const char* format, ...);
    bool add_content(const char* content);
    bool add_status_line(int status, const char* title);
    bool add_headers(int content_length);
    bool add_content_length(int content_length);
    bool add_linger();
    bool add_blank_line();

public:
    /* 所有 socket 上的事件都被注册到同一个 epoll 内核事件表中，
        所以将 epoll 文件描述符设置为静态的 */
    static int m_epollfd;
    /* 统计用户数量 */
    static int m_user_count;
    MYSQL* mysql;

private:
    /* 该 HTTP 连接的 socket */
    int m_sockfd;
    /* 该 HTTP 连接对方的 socket 地址*/
    sockaddr_in m_address;

    /* 读缓冲区 */
    char m_read_buf[READ_BUFFER_SIZE];
    /* 标识读缓冲中已经读入到客户数据的最后一个字节的下一个位置 */
    int m_read_idx;
    /* 当前正在分析的字符在读缓冲区中的位置 */
    int m_checked_idx;
    /* 当前正在解析的行的起始位置 */
    int m_start_line;

    /* 写缓冲区*/
    char m_write_buf[WRITE_BUFFER_SIZE];
    /* 写缓冲区中待发送的字节数 */
    int m_write_idx;

    /* 主状态机当前所处的状态 */
    CHECK_STATE m_check_state;
    /* 请求方法 */
    METHOD m_method;

    /* 客户请求的目标文件的完整路径，其内容等于 doc_root + m_url, 
        doc_root 是网站根目录 */
    char m_real_file[FILENAME_LEN];
    /* 客户请求的目标文件的文件名 */
    char* m_url;
    /* HTTP 协议版本号，仅支持HTTP/1.1 */
    char* m_version;
    /* 主机名 */
    char* m_host;
    /* HTTP 请求消息体的长度 */
    int m_content_length;
    /* HTTP 请求是否要求保持连接 */
    bool m_linger;

    /* 客户请求的目标文件被 mmap 到内存中的起始位置 */
    char* m_file_address;
    /* 目标文件的状态。通过它可以判断文件是否存在、是否为目录、是否可读，
        并获取文件大小等信息 */
    struct stat m_file_stat;
    /* 采用 writev 来执行写操作，其中 m_iv_count 表示被写内存块的数量 */
    struct iovec m_iv[2];
    int m_iv_count;

    int cgi;

    /* 存储请求头数据 */
    char* m_string;
    int bytes_to_send;
    int bytes_have_send;
};

#endif