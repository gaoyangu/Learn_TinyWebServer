#include <pthread.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <stdarg.h>

#include "log.h"

Log::Log()
{
    m_count = 0;
    m_is_async = false;
}

Log::~Log()
{
    if (NULL != m_fp)
    {
        fclose(m_fp);
    }
}

/* 异步需要设置阻塞队列的长度，同步不需要设置 */
bool Log::init(const char* file_name, int log_buf_size, 
            int split_lines, int max_queue_size)
{
    if(max_queue_size >= 1)
    {
        m_is_async = true;
        m_log_queue = new block_queue<string>(max_queue_size);
        pthread_t tid;
        /* 创建线程异步写日志 */
        pthread_create(&tid, NULL, flush_log_thread, NULL);
    }

    /* 输出内容的长度 */
    m_log_buf_size = log_buf_size;
    m_buf = new char[m_log_buf_size];
    memset(m_buf, '\0', sizeof(m_buf));
    m_split_lines = split_lines;

    time_t t = time(NULL);
    struct tm *sys_tm = localtime(&t);
    struct tm my_tm = *sys_tm;

    /* 从后往前找第一个 / 的位置 */
    const char *p = strrchr(file_name, '/');
    char log_full_name[256] = {0};

    /* 若输入的文件名没有 /，则直接将时间+文件名作为日志名 */
    if(NULL == p)
    {
        snprintf(log_full_name, 255, "%d_%02d_%02d_%s", 
                my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday, file_name);
    }
    else
    {
        strcpy(log_name, p + 1);
        strncpy(dir_name, file_name, p - file_name + 1);

        snprintf(log_full_name, 255, "%s%d_%02d_%02d_%s", dir_name,
                my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday, log_name);
    }

    m_today = my_tm.tm_mday;

    m_fp = fopen(log_full_name, "a");   /* a: 追加 */
    if(NULL == m_fp)
    {
        return false;
    }
    return true;
}

void Log::write_log(int level, const char* format, ...)
{
    struct timeval now = {0, 0};
    gettimeofday(&now, NULL);
    time_t t = now.tv_sec;
    struct tm *sys_tm = localtime(&t);
    struct tm my_tm = *sys_tm;
    char s[16] = {0};

    /* 日志分级 */
    switch (level)
    {
    case 0:
        strcpy(s, "[debug]");
        break;
    case 1:
        strcpy(s, "[info]");
        break;
    case 2:
        strcpy(s, "[warn]");
        break;
    case 3:
        strcpy(s, "[erro]");
        break;
    default:
        strcpy(s, "[info]");
        break;
    }

    m_mutex.lock();

    /* 更新现有行数 */
    m_count++;

    /* 日志不是今天或写入的日志行数是最大行的倍数 */
    if(m_today != my_tm.tm_mday || m_count % m_split_lines == 0)
    {
        char new_log[256] = {0};
        /* 强迫将缓冲区内的数据写回参数stream 指定的文件中 */
        fflush(m_fp);
        fclose(m_fp);
        char tail[16] = {0};

        /* 格式化日志名中的时间部分 */
        snprintf(tail, 16, "%d_%02d_%02d",
                    my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday);

        /* 如果时间不是今天，则创建今天的日志 */
        if(m_today != my_tm.tm_mday)
        {
            snprintf(new_log, 255, "%s%s%s", dir_name, tail, log_name);
        }
        else
        {
            snprintf(new_log, 255, "%s%s%s.%lld",
                    dir_name, tail, log_name, m_count / m_split_lines);
        }

        m_fp = fopen(new_log, "a");
    }

    m_mutex.unlock();

    va_list valst;
    va_start(valst, format);

    string log_str;
    m_mutex.lock();

    /* 写入内容格式：时间+内容 */
    /* 时间格式化 */
    int n = snprintf(m_buf, 48, "%d-%02d-%02d %02d:%02d:%02d.%06ld %s ",
                    my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday,
                    my_tm.tm_hour, my_tm.tm_min, my_tm.tm_sec, now.tv_usec, s);
    /* 内容格式化 */
    int m = vsnprintf(m_buf + n, m_log_buf_size - 1, format, valst);
    m_buf[n + m] = '\n';
    m_buf[n + m + 1] = '\0';

    log_str = m_buf;

    m_mutex.unlock();

    if(m_is_async && !m_log_queue->full())
    {
        m_log_queue->push(log_str);
    }
    else
    {
        m_mutex.lock();
        fputs(log_str.c_str(), m_fp);
        m_mutex.unlock();
    }

    va_end(valst);
}

void Log::flush(void)
{
    m_mutex.lock();
    fflush(m_fp);
    m_mutex.unlock();
}