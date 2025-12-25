#include <mysql/mysql.h>
#include <stdio.h>
#include <string>
#include <string.h>
#include <stdlib.h>
#include <deque>
#include <thread>
#include <iostream>
#include <mutex>
#include "sql_connection_pool.h"

using namespace std;

connection_pool::connection_pool() 
    : m_CurConn(0)   // 初始化列表初始化成员变量
    , m_FreeConn(0) {
}

connection_pool *connection_pool::GetInstance()
{
	static connection_pool connPool;
	return &connPool;
}

//构造初始化
void connection_pool::init(const string& url, 
                           const string& User, 
                           const string& PassWord, 
                           const string& DBName, 
                           int Port, 
                           int MaxConn, 
                           int close_log) {
	m_url = url;
	m_Port = Port;
	m_User = User;
	m_PassWord = PassWord;
	m_DatabaseName = DBName;
	m_close_log = close_log;

	for (int i = 0; i < MaxConn; ++i)
	{

		// 1. 初始化MySQL连接句柄
		MYSQL *mysql_conn = mysql_init(NULL);
		if (!mysql_conn) {  // 必须检查返回值是否为NULL
			LOG_ERROR(mysql_error(mysql_conn));
			exit(1);
		}

		// 2. 用初始化后的句柄连接数据库（依赖mysql_init的返回值）
		if (!mysql_real_connect(
			mysql_conn,        // 传入mysql_init返回的句柄
			url.c_str(),       // MySQL服务器地址
			User.c_str(),            // 用户名
			PassWord.c_str(),   // 密码
			DBName.c_str(),   // 要连接的数据库名
			Port,              // 端口
			NULL,              // 套接字（通常为NULL）
			0                  // 连接标志
		)) {
			mysql_close(mysql_conn);  // 失败时也需要关闭句柄释放资源
			LOG_ERROR(mysql_error(mysql_conn));
			exit(1);
		}

		connList.emplace_back(mysql_conn);
		++m_FreeConn;
	}

	semaphore_ = sem(m_FreeConn);

	m_MaxConn = m_FreeConn;
}

//当有请求时，从数据库连接池中返回一个可用连接，更新使用和空闲连接数
MYSQL *connection_pool::GetConnection() {
	if (connList.empty()){
		return NULL;
	}

	MYSQL *mysql_conn = NULL;

	/*
	若当前计数 > 0，计数会减 1（表示一个空闲连接被占用）；
	若计数 = 0（无空闲连接），线程会阻塞等待，直到计数 > 0 再执行减 1 操作。
	*/
	semaphore_.wait();
	{
		std::lock_guard<std::mutex> lockGuard(lock);
		mysql_conn = connList.front();
		connList.pop_front();

		--m_FreeConn;
		++m_CurConn;
	}
	return mysql_conn;
}

//释放当前使用的连接
bool connection_pool::ReleaseConnection(MYSQL *mysql_conn){
	if (!mysql_conn){
		return false;
	}

	{
		std::lock_guard<std::mutex> lockGuard(lock);
		connList.emplace_back(mysql_conn);
		++m_FreeConn;
		--m_CurConn;
	}
	/*
	无论当前计数是多少，都会强制加 1（表示空闲连接数增加），
	若有线程因计数为 0 而阻塞等待，会被唤醒并尝试获取连接。
	*/
	semaphore_.post();

	return true;
}

//当前空闲的连接数
int connection_pool::GetFreeConn() {
    return m_FreeConn;
}

connection_pool::~connection_pool() {
	{
		std::lock_guard<std::mutex> lockGuard(lock);
		if (!connList.empty()){
			for (MYSQL* mysql_conn : connList){
				mysql_close(mysql_conn);
			}
			m_CurConn = 0;
			m_FreeConn = 0;
			connList.clear();
		}
	}
}

connectionRAII::connectionRAII(MYSQL **SQL, connection_pool *connPool){
	*SQL = connPool->GetConnection();
	
	conRAII = *SQL;
	poolRAII = connPool;
}

connectionRAII::~connectionRAII(){
	poolRAII->ReleaseConnection(conRAII);
}