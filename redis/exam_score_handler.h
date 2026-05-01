#ifndef EXAM_SCORE_HANDLER_H
#define EXAM_SCORE_HANDLER_H

// ───────────────────────────────────────────────────────────────────────────
// 考研成绩查询 —— 业务层缓存集成示例
//
// 展示如何将 RedisCache 集成到现有 CGI 流程中，实现:
//   1. 请求到达 → 查 Redis 缓存 → 命中直接返回
//   2. 缓存未命中 → 查 MySQL → 写入 Redis → 返回
//   3. 防穿透: 恶意查询不存在考生 → 布隆过滤器拦截 → 不访问 DB
//   4. 防击穿: 热门考生成绩缓存过期 → 互斥锁保证单线程重建
//   5. 防雪崩: TTL 随机抖动，避免集中过期
//
// 集成方式: 在 http_conn::do_request() 的 CGI 分支中替换直接查 DB 的逻辑。

#include <mysql/mysql.h>
#include <optional>
#include <sstream>
#include <string>

#include "redis_cache.h"

class ExamScoreHandler {
public:
  // 查询考生成绩（带缓存）
  //   name:    考生姓名（来自 POST body URL-encoded）
  //   id_card: 身份证号
  //   mysql:   MySQL 连接句柄
  //   ttl:     缓存基础 TTL（秒），默认 3600
  //
  // 返回值: 成功 → JSON 字符串; 未找到 → nullopt
  static std::optional<std::string> query(const std::string &name,
                                          const std::string &id_card,
                                          MYSQL *mysql, int ttl = 3600) {
    // 缓存键: exam:score:{name}:{id_card}
    std::string cache_key = make_cache_key(name, id_card);

    // 委托给 RedisCache（内部已包含三级防护）
    return RedisCache::GetInstance()->get(
        cache_key,
        [mysql, &name, &id_card]() -> std::optional<std::string> {
          return query_mysql(mysql, name, id_card);
        },
        ttl);
  }

  // 预热布隆过滤器（系统启动时调用，从 DB 加载已有考生集合）
  static void warmup(MYSQL *mysql) {
    std::vector<std::string> keys;
    std::string sql =
        "SELECT CONCAT(name, ':', REPLACE(id_card, ' ', '')) FROM student";

    if (mysql_query(mysql, sql.c_str())) {
      std::cerr << "[ExamScore] 布隆预热查询失败: " << mysql_error(mysql)
                << std::endl;
      return;
    }

    MYSQL_RES *result = mysql_store_result(mysql);
    if (!result) return;

    MYSQL_ROW row;
    while ((row = mysql_fetch_row(result))) {
      if (row[0]) {
        keys.push_back("exam:score:" + std::string(row[0]));
      }
    }
    mysql_free_result(result);

    RedisCache::GetInstance()->warm_bloom(keys);
  }

private:
  static std::string make_cache_key(const std::string &name,
                                     const std::string &id_card) {
    return "exam:score:" + name + ":" + id_card;
  }

  // 直接查 MySQL — 仅缓存未命中时调用
  static std::optional<std::string> query_mysql(MYSQL *mysql,
                                                 const std::string &name,
                                                 const std::string &id_card) {
    // 构建参数化查询（对输入做转义，防 SQL 注入）
    char name_esc[256], id_esc[256];
    mysql_real_escape_string(mysql, name_esc, name.c_str(), name.size());
    mysql_real_escape_string(mysql, id_esc, id_card.c_str(), id_card.size());

    std::ostringstream sql;
    sql << "SELECT s.name, s.id_card, sc.chinese, sc.math, sc.english, "
        << "sc.politics, sc.major_course, sc.total_score "
        << "FROM student s "
        << "JOIN score sc ON s.id = sc.student_id "
        << "WHERE s.name = '" << name_esc << "' "
        << "AND REPLACE(s.id_card, ' ', '') = '" << id_esc << "'";

    if (mysql_query(mysql, sql.str().c_str())) {
      std::cerr << "[ExamScore] MySQL 查询失败: " << mysql_error(mysql)
                << std::endl;
      return std::nullopt;
    }

    MYSQL_RES *result = mysql_store_result(mysql);
    if (!result) return std::nullopt;

    MYSQL_ROW row = mysql_fetch_row(result);
    if (!row) {
      mysql_free_result(result);
      return std::nullopt;
    }

    // 手工构造 JSON（避免引入额外依赖）
    std::ostringstream json;
    json << "{";
    json << "\"name\":\"" << (row[0] ? row[0] : "") << "\",";
    json << "\"id_card\":\"" << (row[1] ? row[1] : "") << "\",";
    json << "\"chinese\":" << (row[2] ? row[2] : "null") << ",";
    json << "\"math\":" << (row[3] ? row[3] : "null") << ",";
    json << "\"english\":" << (row[4] ? row[4] : "null") << ",";
    json << "\"politics\":" << (row[5] ? row[5] : "null") << ",";
    json << "\"major_course\":" << (row[6] ? row[6] : "null") << ",";
    json << "\"total_score\":" << (row[7] ? row[7] : "null");
    json << "}";

    mysql_free_result(result);
    return json.str();
  }
};

#endif
