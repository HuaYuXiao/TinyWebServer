/*************************************************************
*循环数组实现的阻塞队列，m_back = (m_back + 1) % m_max_size;  
*线程安全，每个操作前都要先加互斥锁，操作完后，再解锁
**************************************************************/

#ifndef BLOCK_QUEUE_H
#define BLOCK_QUEUE_H

#include <iostream>
#include <stdlib.h>
#include <thread>
#include <sys/time.h>
#include <mutex>
#include <semaphore>

using namespace std;

const int MAX_SEM = 10000; // 最大信号量值

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
        std::lock_guard<std::mutex> lock(m_mutex);
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
        if (m_size == 0)
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
        if (m_size == 0)
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
    //往队列添加元素
    //当有元素push进队列,相当于生产者生产了一个元素
    bool push(const T &item)
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_size >= m_max_size)
            {
                return false;
            }

            m_back = (m_back + 1) % m_max_size;
            m_array[m_back] = item;
            m_size++;
        }
        filled_semaphore.release(1);
        return true;
    }
    //pop时,如果当前队列没有元素,将会等待信号量
    bool pop(T &item)
    {
        filled_semaphore.acquire();
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_front = (m_front + 1) % m_max_size;
            item = m_array[m_front];
            m_size--;
        }
        return true;
    }

private:
    std::mutex m_mutex;
    std::counting_semaphore<MAX_SEM> filled_semaphore{0};

    T *m_array;
    int m_size;
    int m_max_size;
    int m_front;
    int m_back;
};

#endif
