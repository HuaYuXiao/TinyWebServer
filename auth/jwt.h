#ifndef AUTH_JWT_H
#define AUTH_JWT_H

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <ctime>
#include <sstream>
#include <string>
#include <vector>

// 轻量 JWT（HS256）签发/验证模块，零外部依赖（仅 OpenSSL HMAC）。
// 格式: base64url(header).base64url(payload).base64url(signature)
struct JWTClaims {
  std::string sub; // username
  std::string role; // "user" | "root"
  long exp = 0;     // 过期时间 (unix timestamp)
  long iat = 0;     // 签发时间
};

class JWT {
public:
  static constexpr long DEFAULT_TTL = 86400; // 24 小时

  // ── 签发 ──────────────────────────────────────────────────────────
  // 返回完整的 JWT 字符串，失败返回空串
  static std::string sign(const std::string &username, const std::string &role,
                           const std::string &secret, long ttl = DEFAULT_TTL) {
    long now = std::time(nullptr);

    // header: {"alg":"HS256","typ":"JWT"}
    std::string header = R"({"alg":"HS256","typ":"JWT"})";
    // payload: {"sub":"<user>","role":"<role>","exp":<exp>,"iat":<now>}
    std::string payload = "{\"sub\":\"" + escape(username) +
                          "\",\"role\":\"" + escape(role) +
                          "\",\"exp\":" + std::to_string(now + ttl) +
                          ",\"iat\":" + std::to_string(now) + "}";

    std::string b64_header = base64url_encode(header);
    std::string b64_payload = base64url_encode(payload);
    std::string signing_input = b64_header + "." + b64_payload;

    std::string sig = hmac_sha256(signing_input, secret);
    std::string b64_sig = base64url_encode(sig);

    return signing_input + "." + b64_sig;
  }

  // ── 验证 ──────────────────────────────────────────────────────────
  // 成功返回 true 并填充 claims，失败返回 false
  static bool verify(const std::string &token, const std::string &secret,
                     JWTClaims &claims) {
    // 拆分三段
    size_t p1 = token.find('.');
    if (p1 == std::string::npos)
      return false;
    size_t p2 = token.find('.', p1 + 1);
    if (p2 == std::string::npos)
      return false;

    std::string b64_header = token.substr(0, p1);
    std::string b64_payload = token.substr(p1 + 1, p2 - p1 - 1);
    std::string b64_sig = token.substr(p2 + 1);

    // 1. 验证签名
    std::string signing_input = b64_header + "." + b64_payload;
    std::string expected_sig = base64url_encode(
        hmac_sha256(signing_input, secret));
    if (expected_sig != b64_sig)
      return false;

    // 2. 解码 payload，提取 claims
    std::string payload = base64url_decode(b64_payload);
    if (!extract_string(payload, "sub", claims.sub))
      return false;
    if (!extract_string(payload, "role", claims.role))
      return false;
    if (!extract_long(payload, "exp", claims.exp))
      return false;
    extract_long(payload, "iat", claims.iat);

    // 3. 检查过期
    if (std::time(nullptr) >= claims.exp)
      return false;

    return true;
  }

  // 从 Authorization 头提取 Bearer token
  static bool extract_bearer(const std::string &auth_header,
                             std::string &token) {
    const char *prefix = "Bearer ";
    if (auth_header.compare(0, 7, prefix) != 0)
      return false;
    token = auth_header.substr(7);
    return !token.empty();
  }

private:
  // ── JSON 简易提取 ─────────────────────────────────────────────────
  static bool extract_string(const std::string &json, const std::string &key,
                             std::string &val) {
    std::string needle = "\"" + key + "\":\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos)
      return false;
    pos += needle.size();
    size_t end = json.find('"', pos);
    if (end == std::string::npos)
      return false;
    val = json.substr(pos, end - pos);
    return true;
  }

  static bool extract_long(const std::string &json, const std::string &key,
                           long &val) {
    std::string needle = "\"" + key + "\":";
    size_t pos = json.find(needle);
    if (pos == std::string::npos)
      return false;
    pos += needle.size();
    size_t end = pos;
    while (end < json.size() && (json[end] >= '0' && json[end] <= '9'))
      ++end;
    if (end == pos)
      return false;
    val = std::stol(json.substr(pos, end - pos));
    return true;
  }

  // ── 字符转义（防 JSON 注入） ──────────────────────────────────────
  static std::string escape(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
      switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      default:   out += c;
      }
    }
    return out;
  }

  // ── HMAC-SHA256 ───────────────────────────────────────────────────
  static std::string hmac_sha256(const std::string &data,
                                 const std::string &key) {
    unsigned char buf[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    HMAC(EVP_sha256(), key.c_str(), (int)key.size(),
         (const unsigned char *)data.c_str(), (int)data.size(), buf, &len);
    return std::string((char *)buf, len);
  }

  // ── Base64 URL-safe 编码（RFC 4648 §5） ───────────────────────────
  static std::string base64url_encode(const std::string &data) {
    static const char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    size_t len = data.size();
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
      unsigned a = (unsigned char)data[i];
      unsigned b = (i + 1 < len) ? (unsigned char)data[i + 1] : 0;
      unsigned c = (i + 2 < len) ? (unsigned char)data[i + 2] : 0;
      unsigned triple = (a << 16) | (b << 8) | c;
      out += table[(triple >> 18) & 0x3F];
      out += table[(triple >> 12) & 0x3F];
      if (i + 1 < len)
        out += table[(triple >> 6) & 0x3F];
      else
        out += '=';
      if (i + 2 < len)
        out += table[triple & 0x3F];
      else
        out += '=';
    }
    return out;
  }

  // ── Base64 URL-safe 解码 ──────────────────────────────────────────
  static std::string base64url_decode(const std::string &b64) {
    static const signed char table[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,63,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    };
    std::string out;
    int val = 0, bits = -8;
    for (unsigned char c : b64) {
      signed char d = table[c];
      if (d == -1)
        continue;
      val = (val << 6) + d;
      bits += 6;
      if (bits >= 0) {
        out += (char)((val >> bits) & 0xFF);
        bits -= 8;
      }
    }
    return out;
  }
};

#endif
