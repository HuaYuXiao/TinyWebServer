#ifndef _CONNECTION_POOL_
#define _CONNECTION_POOL_

#include <stdio.h>
#include <deque>
#include <error.h>
#include <iostream>
#include <string>
#include <mariadb/conncpp.hpp>
#include <mutex>
#include "../log/log.h"
#include "../lock/locker.h"

using namespace std;
using namespace sql;
using namespace sql::mariadb;

class connection_pool
{
public:
	unique_ptr<Connection> GetConnection();				 //获取数据库连接
	bool ReleaseConnection(unique_ptr<Connection> conn); //释放连接
	int GetFreeConn();					 //获取连接
	void DestroyPool();					 //销毁所有连接

	//单例模式
	static connection_pool *GetInstance();

	void init(string url, string User, string PassWord, string DataBaseName, int Port, int MaxConn, bool close_log); 

private:
	connection_pool();
	~connection_pool();

	int m_MaxConn;  //最大连接数
	int m_CurConn;  //当前已使用的连接数
	int m_FreeConn; //当前空闲的连接数
	// std::mutex 会自动构造/析构，线程安全且易用；配合 RAII（std::lock_guard / std::unique_lock）能保证异常安全。
	std::mutex mtx;
	deque<unique_ptr<Connection>> connList; //连接池
	sem reserve;

public:
	string m_url;			 //主机地址
	string m_Port;		 //数据库端口号
	string m_User;		 //登陆数据库用户名
	string m_PassWord;	 //登陆数据库密码
	string m_DatabaseName; //使用数据库名
	bool m_close_log;	//日志开关
};

class connectionRAII {
public:
    connectionRAII(connection_pool *connPool);
    ~connectionRAII();
    // 新增获取连接的方法
    unique_ptr<Connection>& get_conn() { return conRAII; }

private:
    unique_ptr<Connection> conRAII;
    connection_pool *poolRAII;
};

#endif
