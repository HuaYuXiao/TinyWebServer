#include <stdio.h>
#include <string>
#include <stdlib.h>
#include <list>
#include <pthread.h>
#include <iostream>
#include "sql_connection_pool.h"
#include <mariadb/conncpp.hpp>
#include <mutex>

using namespace std;
using namespace sql;
using namespace sql::mariadb;

// Use mariadb Connector/C++ driver
static Driver* g_driver = nullptr;

connection_pool::connection_pool()
{
	m_CurConn = 0;
	m_FreeConn = 0;
}

connection_pool *connection_pool::GetInstance()
{
	static connection_pool connPool;
	return &connPool;
}

//构造初始化: url should be like "tcp://127.0.0.1"
void connection_pool::init(string url, string User, string PassWord, string DBName, int Port, int MaxConn, int close_log)
{
	m_url = url;
	m_Port = to_string(Port);
	m_User = User;
	m_PassWord = PassWord;
	m_DatabaseName = DBName;
	m_close_log = close_log;

	// initialize driver once
	if (!g_driver)
	{
		g_driver = get_driver_instance();
	}

	// create connections
	for (int i = 0; i < MaxConn; i++)
	{
		try
		{
			// build connect string with port
			std::string connect_url = "tcp://" + m_url + ":" + m_Port;
			std::cout << "Connecting to database at: " << connect_url << std::endl;
			unique_ptr<Connection> conn(
				g_driver->connect(connect_url, m_User, m_PassWord)
			);
			conn->setSchema(m_DatabaseName);

			if (!conn)
			{
				LOG_ERROR("MariaDB C++ Connector returned null connection");
				exit(1);
			}

			connList.emplace_back(std::move(conn));;
			++m_FreeConn;
		}
		catch (SQLException &e)
		{
			LOG_ERROR("MariaDB Connector error: %s", e.what());
			exit(1);
		}
	}

	reserve = sem(m_FreeConn);

	m_MaxConn = m_FreeConn;
}

//当有请求时，从数据库连接池中返回一个可用连接，更新使用和空闲连接数
unique_ptr<Connection> connection_pool::GetConnection()
{
    // 等待一个可用资源
    reserve.wait();

    std::lock_guard<std::mutex> guard(mtx); // 自动解锁
    if (connList.empty()) {
        // 理论上不会发生：因为信号量保证了可用计数
        return nullptr;
    }

    std::unique_ptr<Connection> con = std::move(connList.front());
    connList.pop_front();

    --m_FreeConn;
    ++m_CurConn;

    return con;
}

//释放当前使用的连接
bool connection_pool::ReleaseConnection(std::unique_ptr<Connection> conn)
{
    if (!conn) return false;

    {
        std::lock_guard<std::mutex> guard(mtx);
        connList.push_back(std::move(conn));
        ++m_FreeConn;
        --m_CurConn;
    }

    reserve.post();
    return true;
}

//销毁数据库连接池
void connection_pool::DestroyPool()
{
    std::lock_guard<std::mutex> guard(mtx);
    for (auto &con : connList) {
        try {
            if (con) con->close();
        } catch(...) {}
    }
    connList.clear();
    m_CurConn = m_FreeConn = 0;
}

//当前空闲的连接数
int connection_pool::GetFreeConn()
{
	return this->m_FreeConn;
}

connection_pool::~connection_pool()
{
	DestroyPool();
}

// 构造时直接从连接池获取连接，内部持有
connectionRAII::connectionRAII(connection_pool* pool) : poolRAII(pool) {
    conRAII = poolRAII->GetConnection();  // 直接将连接存入内部 unique_ptr
}

// 析构时释放连接（放回连接池）
connectionRAII::~connectionRAII() {
    if (conRAII) {
        poolRAII->ReleaseConnection(std::move(conRAII));
    }
}