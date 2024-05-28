#include <sys/socket.h>
#include <stdio.h>

#include <errno.h>

#include <sys/epoll.h>

#include "./lock/locker.h"
#include "./threadpool/threadpool.h"
#include "./http/http_conn.h"

#define MAX_FD 65536            /* 最大文件描述符 */
#define MAX_EVENT_NUMBER 10000  /* 最大事件数 */

#define listenfdLT /* 水平触发阻塞 */
// #define listenfdET /* 边缘触发非阻塞*/

extern int addfd(int epollfd, int fd, bool one_shot);
extern int removefd(int epollfd, int fd);

static int epollfd = 0;

int main(int argc, char* argv[])
{
    if(argc <= 1)
    {
        printf("usage: %s ip_address port_number\n", basename(argv[0]));
        return 1;
    }

    int port = atoi(argv[1]);

    /* 忽略 SIGPIPE 信号 */
    addsig(SIGPIPE, SIG_IGN);

    /* 创建线程池 */
    threadpool<http_conn>* pool = NULL;
    try
    {
        pool = new threadpool<http_conn>();
    }
    catch(...)
    {
        return 1;
    }

    /* 预先为每个可能的客户连接分配一个 http_conn 对象 */
    http_conn* users = new http_conn[MAX_FD];
    assert(users);
    int user_count = 0;

    /* 创建监听socket文件描述符 */
    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(listenfd >= 0);

    int flag = 1;
    /*
    * SOL_SOCKET: 在套接字级别上设置选项
    * SO_REUSEADDR: 允许端口被重复使用
    * flag = 1 表示打开
    */
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));

    int ret = 0;
    /* 创建监听socket的TCP/IP的IPv4 socket地址 */
    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    /* INADDR_ANY：将套接字绑定到所有可用的接口 */
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);

    /* 绑定socket和它的地址 */
    ret = bind(listenfd, (struct sockaddr *)&address, sizeof(address));
    assert(ret >= 0);

    /* 创建监听队列以存放待处理的客户连接，在这些客户连接被accept()之前 */
    ret = listen(listenfd, 5);
    assert(ret >= 0);

    /* 创建内核事件表 */
    epoll_event events[MAX_EVENT_NUMBER];
    epollfd = epoll_create(5);
    assert(epollfd != -1);

    /* 将 listenfd 放到epoll树上 */
    addfd(epollfd, listenfd, false);
    http_conn::m_epollfd = epollfd;

    bool stop_server = false;

    while (!stop_server)
    {
        /* 等待所监控文件描述符上有事件发生 */
        int number = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
        if((number < 0) && (errno != EINTR))
        {
            printf("epoll failure\n");
            break;
        }

        /* 处理所有就绪事件 */
        for (int i = 0; i < number; i++)
        {
            int sockfd = events[i].data.fd;

            /* 处理新到的客户连接 */
            if( sockfd == listenfd)
            {
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof(client_address);
/* LT 水平触发 */
#ifdef listenfdLT
                int connfd = accept(listenfd, (struct sockaddr*)&client_address, 
                                                &client_addrlength);
                if(connfd < 0)
                {
                    printf("errno is: %d\n", errno);
                    continue;
                }
                if(http_conn::m_user_count >= MAX_FD)
                {
                    show_error(connfd, "Internal server busy");
                    continue;
                }

                /* 初始化客户连接 */
                users[connfd].init(connfd, client_address);
#enfif

/* ET 非阻塞边缘触发 */
#ifdef listenfdET
                while (1)
                {
                    int connfd = accept(listenfd, (struct sockaddr*)&client_address, 
                                                    &client_addrlength);
                    if(connfd < 0)
                    {
                        break;
                    }
                    if(http_conn::m_user_count >= MAX_FD)
                    {
                        show_error(connfd, "Internal server busy");
                        break;
                    }
                    
                    /* 初始化客户连接 */
                    users[connfd].init(connfd, client_address);
                }
                continue;
#endif
            }
            /* 处理异常事件 */
            else if(events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                /* 如果有异常，直接关闭客户连接 */
                users[sockfd].close_conn();
            }
            /* 处理客户端连接上接收到的数据 */
            else if(events[i].events & EPOLLIN)
            {
                /* 根据读的结果，决定是将任务添加到线程池，还是关闭连接 */
                if(users[sockfd].read_once())
                {
                    pool->append(users + sockfd);
                }
                else
                {
                    users[sockfd].close_conn();
                }
            }
            else if(events[i].events & EPOLLOUT)
            {
                /* 根据写的结果，决定是否关闭连接 */
                if(!users[sockfd].write())
                {
                    users[sockfd].close_conn();
                }
            }
        }
        
    }

    close(epollfd);
    close(listenfd);
    delete [] users;
    delete pool;
    return 0;
}