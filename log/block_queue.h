/*************************************************************
*循环数组实现的阻塞队列，m_back = (m_back + 1) % m_max_size;  
*线程安全，每个操作前都要先加互斥锁，操作完后，再解锁
**************************************************************/

#ifndef BLOCK_QUEUE_H
#define BLOCK_QUEUE_H

#include <iostream>
#include <stdlib.h>
#include <pthread.h>
#include <sys/time.h>
#include <mutex>
#include <condition_variable>  // 新增：标准条件变量头文件

using namespace std;

template <class T>
class block_queue
{
public:
    block_queue(int max_size = 1000)
    {
        if (max_size <= 0)
        {
            exit(-1);
        }

        m_max_size = max_size;
        m_array = new T[max_size];
        m_size = 0;
        m_front = -1;
        m_back = -1;
    }

    void clear()
    {
        std::lock_guard<std::mutex> lock(m_mutex);  // 替换为lock_guard自动管理锁
        m_size = 0;
        m_front = -1;
        m_back = -1;
    }

    ~block_queue()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_array != NULL)
            delete [] m_array;
    }

    //判断队列是否满了
    bool full() 
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_size >= m_max_size;
    }

    //判断队列是否为空
    bool empty() 
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_size == 0;
    }

    //返回队首元素
    bool front(T &value) 
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (0 == m_size)
        {
            return false;
        }
        value = m_array[m_front];
        return true;
    }

    //返回队尾元素
    bool back(T &value) 
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (0 == m_size)
        {
            return false;
        }
        value = m_array[m_back];
        return true;
    }

    int size() 
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_size;
    }

    int max_size()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_max_size;
    }

    //往队列添加元素，唤醒等待线程
    bool push(const T &item)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_size >= m_max_size)
        {
            m_cond.notify_all();  // 替换broadcast为notify_all
            return false;
        }

        m_back = (m_back + 1) % m_max_size;
        m_array[m_back] = item;
        m_size++;

        m_cond.notify_all();
        return true;
    }

    //pop时,如果当前队列没有元素,将会等待条件变量
    bool pop(T &item)
    {
        std::unique_lock<std::mutex> lock(m_mutex);  // 条件变量需要unique_lock
        // 循环等待防止虚假唤醒
        while (m_size <= 0)
        {
            m_cond.wait(lock);  // 等待时自动释放锁，唤醒后重新获取
        }

        m_front = (m_front + 1) % m_max_size;
        item = m_array[m_front];
        m_size--;
        return true;
    }

    //增加了超时处理
    bool pop(T &item, int ms_timeout)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (m_size <= 0)
        {
            // 转换超时时间为system_clock时间点
            auto now = std::chrono::system_clock::now();
            auto timeout_time = now + std::chrono::milliseconds(ms_timeout);
            
            // 等待超时或被唤醒
            if (m_cond.wait_until(lock, timeout_time) == std::cv_status::timeout)
            {
                return false;
            }
            
            // 唤醒后再次检查队列状态
            if (m_size <= 0)
            {
                return false;
            }
        }

        m_front = (m_front + 1) % m_max_size;
        item = m_array[m_front];
        m_size--;
        return true;
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_cond;

    T *m_array;
    int m_size;
    int m_max_size;
    int m_front;
    int m_back;
};

#endif