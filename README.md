# Learn_TinyWebServer

## 参考资料

- 项目地址：https://github.com/qinguoyi/TinyWebServer/tree/raw_version
- 博客：
    - [CSDN 网站服务器项目研究](https://blog.csdn.net/qq_59993282/category_12374990.html)
    - [zwiley的随记](https://zwiley.github.io/mybook/webserver/0%20%E9%A1%B9%E7%9B%AE%E6%A6%82%E8%BF%B0/)
    - [一文读懂社长的TinyWebServer](https://huixxi.github.io/2020/06/02/%E5%B0%8F%E7%99%BD%E8%A7%86%E8%A7%92%EF%BC%9A%E4%B8%80%E6%96%87%E8%AF%BB%E6%87%82%E7%A4%BE%E9%95%BF%E7%9A%84TinyWebServer/)
    - [WebServer项目学习总结](https://www.haoxx.site/article/110)
    - [从零开始实现C++ TinyWebServer 全过程记录](https://blog.csdn.net/weixin_51322383/article/details/130464403)

## 01. 功能测试

### 1.1 测试环境

- 服务器测试环境
    - WSL2 + Ubuntu版本16.04
    - MySQL版本5.7.33

- 浏览器测试环境
    - Windows + Chrome

### 1.2 MySQL 安装

测试前确认已安装MySQL数据库

```sh
# 安装
sudo apt-get update
sudo apt-get install mysql-server

# 启动、关闭、重启 MySQL服务
sudo service mysql start
sudo service mysql stop
sudo service mysql restart
sudo service mysql status  #查看状态

# 登录 MySQL服务
mysql -h localhost -P 3306 -u root -p   # 远程  
mysql -u root -p    # 本机
```

### 1.3 数据库建立

```sql
#建立yourdb库
create database yourdb;

#创建user表
USE yourdb;
CREATE TABLE user(
    username char(50) NULL,
    passwd char(50) NULL
)ENGINE=InnoDB;

#添加数据
INSERT INTO user(username, passwd) VALUES('name', 'passwd');
```
### 1.4 代码修改

- 修改main.c中的数据库初始化信息

```cpp
// 第一个 root 为服务器数据库的登录名，一般为 root
// 第二个 root 为服务器数据库的密码，也就是安装mysql时设置的密码
// yourdb修改为上述创建的yourdb库名
connPool->init("localhost", "root", "root", "yourdb", 3306, 8);
```

- 修改http_conn.cpp中的root路径
```cpp
// 修改为root文件夹所在路径
// 也就是将 /home/qgy/ 修改为 TinyWebServer 文件夹所在的目录
const char* doc_root="/home/qgy/TinyWebServer/root";
```

### 1.5 代码运行

- 生成server

```sh
# 在 /home/qgy/TinyWebServer 目录中执行
make server
```

- 启动server

```sh
# 将 port 修改为具体的端口号，如 ./server 9006
./server port
```

- 浏览器端
```sh
# ip 和 port 均为具体值，如 127.0.0.1:9006
ip:port
```

## 02. 框架梳理

项目搭建了一个`半同步/半反应堆线程池`，其中维护了一个`请求队列`
1. 线程池中的`主线程`
    - 通过 epoll 来监听 socket
    - 将请求队列中的任务分配给线程池中的工作线程
2. `工作线程`处理的任务分为：
    - 日志输出
    - 定时器处理非活动连接
    - 处理http请求

## 03. 代码梳理

### 3.1 接受客户端发来的HTTP请求报文

web服务器通过 socket 监听来自用户的请求

#### 3.1.1 socket 编程API

- socket：根据指定的地址族、数据类型和协议来分配一个socket的描述字及其所用的资源

```cpp
int socket(int domain, int type, int protocol);

domain:协议族，常用的有AF_INET、AF_INET6、AF_LOCAL、AF_ROUTE其中AF_INET代表使用ipv4地址
type:socket类型，常用的socket类型有，SOCK_STREAM、SOCK_DGRAM、SOCK_RAW、SOCK_PACKET、SOCK_SEQPACKET等
protocol:协议。常用的协议有，IPPROTO_TCP、IPPTOTO_UDP、IPPROTO_SCTP、IPPROTO_TIPC等
```

#### 3.1.2 函数

htonl， htons:
- 网络字节顺序与本地字节顺序之间的转换函数
```cpp
- htonl(): 针对32位，4字节
- htons(): 针对16位，2字节
- 所占位数小于一个字节，8位时，不需要转换
```

setsockopt:
- 获取或者设置与某个套接字关联的选项
```cpp
// s:       套接字描述符
// level:   被设置的选项的级别，如果想要在套接字级别上设置选项，就必须把level设置为 SOL_SOCKET
// optname: SO_REUSEADDR，打开或关闭地址复用功能
// optval:  当 optval 不等于0时，打开，否则，关闭
// optlen:  optval 缓冲区的长度
int setsockopt(SOCKET s, int level, int optname, const char FAR *optval, int optlen);
```

fcntl:
- 修改已经打开文件的属性
```cpp
int fcntl(int fd, int cmd);

int fcntl(int fd, int cmd, long arg);         

int fcntl(int fd, int cmd, struct flock *lock);

// 获取、设置文件访问状态标志: F_GETFL、F_SETFL
```