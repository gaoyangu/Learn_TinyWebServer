#include <mysql/mysql.h>

#include "sql_connection_pool.h"

using namespace std;

connection_pool* connection_pool::GetInstance()
{
    static connection_pool connPool;
    return &connPool;
}

connection_pool::connection_pool()
{
    this->CurConn = 0;
    this->FreeConn = 0;
}

connection_pool::~connection_pool()
{
    DestroyPool();
}

void connection_pool::init(string url, string User, string PassWord, string DatabaseName,
                int Port, unsigned int MaxConn)
{
    /* 初始化数据库信息 */
    this->url = url;
    this->Port = Port;
    this->User = User;
    this->PassWord = PassWord;
    this->DatabaseName = DatabaseName;

    /* 创建 MaxConn 条数据库连接 */
    for(int i = 0; i < MaxConn; i++)
    {
        MYSQL* con = NULL;
        con = mysql_init(con);
        if(NULL == con)
        {
            cout << "Error:" << mysql_errno(con);
            exit(1);
        }
        con = mysql_real_connect(con, url.c_str(), User.c_str(), PassWord.c_str(), 
                                    DatabaseName.c_str(), Port, NULL, 0);
        if(NULL == con)
        {
            cout << "Error:" << mysql_errno(con);
            exit(1);
        }

        connList.push_back(con);
        ++FreeConn;
    }
    reserve = sem(FreeConn);

    this->MaxConn = FreeConn;
}

MYSQL* connection_pool::GetConnection()
{
    if(0 == connList.size())
    {
        return NULL;
    }

    MYSQL* con = NULL;

    reserve.wait();

    lock.lock();
    con = connList.front();
    connList.pop_front();
    --FreeConn;
    ++CurConn;
    lock.unlock();

    return con;
}

bool connection_pool::ReleaseConnection(MYSQL *con)
{
    if(NULL == con)
    {
        return false;
    }

    lock.lock();
    connList.push_back(con);
    ++FreeConn;
    --CurConn;
    lock.unlock();

    reserve.post();
    return true;
}

/* 销毁数据库连接池 */
void connection_pool::DestroyPool()
{
    lock.lock();
    if(connList.size() > 0)
    {
        list<MYSQL*>::iterator it;
        for(it = connList.begin(); it != connList.end(); ++it)
        {
            MYSQL* con = *it;
            mysql_close(con);
        }
        CurConn = 0;
        FreeConn = 0;

        connList.clear();

        // lock.unlock();
    }
    lock.unlock();
}

int connection_pool::GetFreeConn()
{
    return this->FreeConn;
}

connectionRAII::connectionRAII(MYSQL **con, connection_pool *connPool)
{
    *con = connPool->GetConnection();

    conRAII = *con;
    poolRAII = connPool;

}

connectionRAII::~connectionRAII()
{
    poolRAII->ReleaseConnection(conRAII);
}