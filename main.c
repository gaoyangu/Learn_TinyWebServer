#include <sys/socket.h>
#include <stdio.h>

#include <errno.h>

#include <sys/epoll.h>

#include "./lock/locker.h"
#include "./threadpool/threadpool.h"
#include "./timer/min_heap.h"
#include "./http/http_conn.h"
#include "./CGImysql/sql_connection_pool.h"

#define MAX_FD 65536            /* 最大文件描述符 */
#define MAX_EVENT_NUMBER 10000  /* 最大事件数 */
#define TIMESLOT 5              /* 最小超时单位 */

#define listenfdLT /* 水平触发阻塞 */
// #define listenfdET /* 边缘触发非阻塞*/

extern int addfd(int epollfd, int fd, bool one_shot);
extern int removefd(int epollfd, int fd);
extern int setnonblocking(int fd);

static int pipefd[2];
static int epollfd = 0;
static time_heap timer_lst;

/* 信号处理函数 */
void sig_handler(int sig)
{
    /* 为保证函数的可重入性，保留原来的 errno */
    /* 可重入性表示中断后再次进入该函数，环境变量与之前相同，不会丢失数据 */
    int save_errno = errno;
    int msg = sig;

    /* 将信号值从管道写端写入，传输字符类型，而非整形 */
    send(pipefd[1], (char *)&msg, 1, 0);
    errno = save_errno;
}

/* 设置信号函数 */
void addsig(int sig, void(handler)(int), bool restart = true)
{
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));

    /* 信号处理函数中仅发送信号，不做对应的逻辑处理 */
    sa.sa_handler = handler;
    if(restart)
    {
        sa.sa_flags |= SA_RESTART;
    }
    /* 将所有信号添加到信号集中 */
    sigfillset(&sa.sa_mask);

    /* 执行 sigaction 函数 */
    assert(sigaction(sig, &sa, NULL) != -1);
}

/* 定时处理任务 */
void timer_handler()
{
    timer_lst.tick();

    time_t cur = time(NULL);
    alarm(timer_lst.array[0]->expire - cur);
}

/* 定时器回调函数，删除非活动连接在 socket 上的注册事件，并关闭 */
void cb_func(clinet_data* user_data)
{
    assert(user_data);
    epoll_ctl(epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
    close(user_data->sockfd);
    http_conn::m_user_count--;
}

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

    /* 创建数据库连接池 */
    connection_pool* connPool = connection_pool::GetInstance();
    connPool->init("localhost", "root", "root", "qgydb", 3306, 8);

    /* 创建线程池 */
    threadpool<http_conn>* pool = NULL;
    try
    {
        pool = new threadpool<http_conn>(connPool);
    }
    catch(...)
    {
        return 1;
    }

    /* 预先为每个可能的客户连接分配一个 http_conn 对象 */
    http_conn* users = new http_conn[MAX_FD];
    assert(users);

    /* 初始化数据库读取表 */
    users->initmysql_result(connPool);

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

    /* 创建管道套接字 */
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    assert(ret != -1);
    
    /* 设置管道写端为非阻塞 */
    setnonblocking(pipefd[1]);
    /* 设置管道读端为 ET 非阻塞 */
    addfd(epollfd, pipefd[0], false);

    /* 传递给主循环的信号值，这里只关注 SIGALRM 和 SIGTERM */
    addsig(SIGALRM, sig_handler, false);
    addsig(SIGTERM, sig_handler, false);

    bool stop_server = false;

    clinet_data* users_timer = new clinet_data[MAX_FD];

    /* 超时标志 */
    bool timeout = false;
    /* 每隔 TIMESLOT 时间触发 SIGALRM 信号 */
    alarm(TIMESLOT);

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

                users_timer[connfd].address = client_address;
                users_timer[connfd].sockfd = connfd;
                heap_timer* timer = new heap_timer;
                timer->user_data = &users_timer[connfd];
                timer->cb_func = cb_func;
                timer_t cur = time(NULL);
                timer->expire = cur + 3 * TIMESLOT;
                users_timer[connfd].timer = timer;
                timer_lst.add_timer(timer);
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

                    users_timer[connfd].address = client_address;
                    users_timer[connfd].sockfd = connfd;
                    heap_timer* timer = new heap_timer;
                    timer->user_data = &users_timer[connfd];
                    timer->cb_func = cb_func;
                    timer_t cur = time(NULL);
                    timer->expire = cur + 3 * TIMESLOT;
                    users_timer[connfd].timer = timer;
                    timer_lst.add_timer(timer);
                }
                continue;
#endif
            }
            /* 处理异常事件 */
            else if(events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                /* 如果有异常，直接关闭客户连接 */
                // users[sockfd].close_conn();

                cb_func(&users_timer[sockfd]);

                heap_timer* timer = users_timer[sockfd].timer;
                if(timer)
                {
                    timer_lst.del_timer(timer);
                }
            }
            /* 处理信号 */
            else if((sockfd == pipefd[0]) && (events[i].events & EPOLLIN))
            {
                int sig;
                char signals[1024];

                /* 从管道读端读出信号值，成功返回字节数，失败返回 -1*/
                ret = recv(pipefd[0], signals, sizeof(signals), 0);
                if(-1 == ret)
                {
                    continue;
                }
                else if(0 == ret)
                {
                    continue;
                }
                else
                {
                    /* 处理信号值对应的逻辑 */
                    for(int i = 0; i < ret; ++i)
                    {
                        switch (signals[i])
                        {
                        case SIGALRM:
                        {
                            timeout = true;
                            break;
                        }
                        case SIGTERM:
                        {
                            stop_server = true;
                            break;
                        }
                        }
                    }
                }
            }
            /* 处理客户端连接上接收到的数据 */
            else if(events[i].events & EPOLLIN)
            {
                /* 取出该连接对应的定时器 */
                heap_timer* timer = users_timer[sockfd].timer;
                
                /* 根据读的结果，决定是将任务添加到线程池，还是关闭连接 */
                if(users[sockfd].read_once())
                {
                    pool->append(users + sockfd);

                    /* 若有数据传输，则将定时器往后延迟3个单位 */
                    if(timer)
                    {
                        timer_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;
                        timer_lst.adjust(timer);
                    }
                }
                else
                {
                    // users[sockfd].close_conn();
                    cb_func(&users_timer[sockfd]);
                    if(timer)
                    {
                        timer_lst.del_timer(timer);
                    }
                }
            }
            else if(events[i].events & EPOLLOUT)
            {
                /* 取出该连接对应的定时器 */
                heap_timer* timer = users_timer[sockfd].timer;

                /* 根据写的结果，决定是否关闭连接 */
                if(users[sockfd].write())
                {
                    /* 若有数据传输，则将定时器往后延迟3个单位 */
                    if(timer)
                    {
                        timer_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;
                        timer_lst.adjust(timer);
                    }
                }
                else
                {
                    // users[sockfd].close_conn();
                    cb_func(&users_timer[sockfd]);
                    if(timer)
                    {
                        timer_lst.del_timer(timer);
                    }
                }
            }
        }
        if(timeout)
        {
            timer_handler();
            timeout = false;
        }
        
    }

    close(epollfd);
    close(listenfd);
    delete [] users;
    delete pool;
    return 0;
}