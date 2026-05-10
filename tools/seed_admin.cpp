// 命令行工具：哈希密码或初始化管理员账户
// 用法:
//   ./seed_admin                 — 交互式创建 root 管理员
//   ./seed_admin <user> <pass>   — 输出 INSERT SQL 语句

#include <mysql/mysql.h>

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

#include "../auth/password.h"

int main(int argc, char *argv[]) {
  std::string username, password;

  if (argc == 3) {
    username = argv[1];
    password = argv[2];
  } else {
    std::cout << "用户名: ";
    std::getline(std::cin, username);
    std::cout << "密码: ";
    std::getline(std::cin, password);
  }

  if (username.empty() || password.empty()) {
    std::cerr << "用户名和密码不能为空" << std::endl;
    return 1;
  }

  std::string hash = Password::hash(password);

  if (argc == 3) {
    std::cout << "-- 执行以下 SQL 插入管理员:" << std::endl;
    std::cout << "INSERT INTO server_users (username, password_hash, role)"
              << std::endl;
    std::cout << "VALUES ('" << username << "', '" << hash << "', 'root');"
              << std::endl;
  } else {
    // 直接连接 MySQL 执行插入
    MYSQL *mysql = mysql_init(NULL);
    if (!mysql) {
      std::cerr << "mysql_init 失败" << std::endl;
      return 1;
    }

    const char *host = getenv("MYSQL_HOST") ? getenv("MYSQL_HOST") : "192.168.19.1";
    const char *user = getenv("MYSQL_USER") ? getenv("MYSQL_USER") : "user";
    const char *pass = getenv("MYSQL_PASS") ? getenv("MYSQL_PASS") : "123456";
    const char *db   = getenv("MYSQL_DB")   ? getenv("MYSQL_DB")   : "server";
    int port = getenv("MYSQL_PORT") ? atoi(getenv("MYSQL_PORT")) : 3306;

    if (!mysql_real_connect(mysql, host, user, pass, db, port, NULL, 0)) {
      std::cerr << "MySQL 连接失败: " << mysql_error(mysql) << std::endl;
      mysql_close(mysql);
      return 1;
    }

    // 确保表存在
    if (mysql_query(mysql,
        "CREATE TABLE IF NOT EXISTS server_users ("
        "  id BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,"
        "  username VARCHAR(64) NOT NULL,"
        "  password_hash VARCHAR(255) NOT NULL,"
        "  role ENUM('user','root') NOT NULL DEFAULT 'user',"
        "  created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,"
        "  UNIQUE KEY idx_username (username)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4")) {
      std::cerr << "建表失败: " << mysql_error(mysql) << std::endl;
      mysql_close(mysql);
      return 1;
    }

    char escaped_user[128], escaped_role[8];
    mysql_real_escape_string(mysql, escaped_user, username.c_str(), username.size());
    mysql_real_escape_string(mysql, escaped_role, "root", 4);

    char sql[1024];
    snprintf(sql, sizeof(sql),
             "INSERT INTO server_users (username, password_hash, role) "
             "VALUES ('%s', '%s', '%s') "
             "ON DUPLICATE KEY UPDATE password_hash=VALUES(password_hash)",
             escaped_user, hash.c_str(), escaped_role);

    if (mysql_query(mysql, sql)) {
      std::cerr << "插入失败: " << mysql_error(mysql) << std::endl;
      mysql_close(mysql);
      return 1;
    }

    std::cout << "管理员 " << username << " 创建成功 ✓" << std::endl;
    mysql_close(mysql);
  }

  return 0;
}
