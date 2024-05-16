# Learn_TinyWebServer

## 参考资料

- 项目地址：https://github.com/qinguoyi/TinyWebServer/tree/raw_version
- 博客：
    - [CSDN 网站服务器项目研究](https://blog.csdn.net/qq_59993282/category_12374990.html)
    - [zwiley的随记](https://zwiley.github.io/mybook/webserver/0%20%E9%A1%B9%E7%9B%AE%E6%A6%82%E8%BF%B0/)
    - [一文读懂社长的TinyWebServer](https://huixxi.github.io/2020/06/02/%E5%B0%8F%E7%99%BD%E8%A7%86%E8%A7%92%EF%BC%9A%E4%B8%80%E6%96%87%E8%AF%BB%E6%87%82%E7%A4%BE%E9%95%BF%E7%9A%84TinyWebServer/)
    - [WebServer项目学习总结](https://www.haoxx.site/article/110)
    - [从零开始实现C++ TinyWebServer 全过程记录](https://blog.csdn.net/weixin_51322383/article/details/130464403)

## 01. 基础测试

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

