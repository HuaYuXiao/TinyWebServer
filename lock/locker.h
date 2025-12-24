#ifndef LOCKER_H
#define LOCKER_H

#include <exception>
#include <thread>
#include <semaphore.h>
#include <mutex>

class sem
{
public:
    sem()
    {
        if (sem_init(&m_sem, 0, 0) != 0)
        {
            throw std::exception();
        }
    }
    sem(int num)
    {
        if (sem_init(&m_sem, 0, num) != 0)
        {
            throw std::exception();
        }
    }
    ~sem()
    {
        sem_destroy(&m_sem);
    }
    bool wait()
    {
        return sem_wait(&m_sem) == 0;
    }
    bool post()
    {
        return sem_post(&m_sem) == 0;
    }

private:
    sem_t m_sem;
};

class cond
{
public:
    cond()
    {
        if (pthread_cond_init(&m_cond, NULL) != 0)
        {
            throw std::exception();
        }
    }
    ~cond()
    {
        pthread_cond_destroy(&m_cond);
    }
    bool wait(std::mutex &m_mutex)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return pthread_cond_wait(&m_cond, reinterpret_cast<pthread_mutex_t *>(lock.mutex())) == 0;
    }
    bool timewait(std::mutex &m_mutex, struct timespec t)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return pthread_cond_timedwait(&m_cond, reinterpret_cast<pthread_mutex_t *>(lock.mutex()), &t) == 0;
    }
    bool signal()
    {
        return pthread_cond_signal(&m_cond) == 0;
    }
    bool broadcast()
    {
        return pthread_cond_broadcast(&m_cond) == 0;
    }

private:
    pthread_cond_t m_cond;
};

#endif
