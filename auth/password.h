#ifndef AUTH_PASSWORD_H
#define AUTH_PASSWORD_H

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <cstring>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

// PBKDF2-HMAC-SHA256 password hashing using OpenSSL EVP.
// Output format: $pbkdf2-sha256$<iter>$<salt_hex>$<hash_hex>
class Password {
public:
  static constexpr int SALT_LEN = 16;
  static constexpr int HASH_LEN = 32;
  static constexpr int ITERATIONS = 100000; // OWASP 推荐 ≥ 100,000

  // 对明文密码做 PBKDF2 哈希，返回可存储的格式化字符串
  static std::string hash(const std::string &password) {
    unsigned char salt[SALT_LEN];
    if (RAND_bytes(salt, SALT_LEN) != 1) {
      throw std::runtime_error("RAND_bytes failed");
    }

    unsigned char out[HASH_LEN];
    PKCS5_PBKDF2_HMAC(password.c_str(), (int)password.size(), salt, SALT_LEN,
                      ITERATIONS, EVP_sha256(), HASH_LEN, out);

    std::ostringstream oss;
    oss << "$pbkdf2-sha256$" << ITERATIONS << "$" << to_hex(salt, SALT_LEN)
        << "$" << to_hex(out, HASH_LEN);
    return oss.str();
  }

  // 验证密码是否匹配哈希
  static bool verify(const std::string &password, const std::string &hash_str) {
    auto parts = parse_hash(hash_str);
    if (parts.iterations <= 0)
      return false;

    unsigned char computed[HASH_LEN];
    PKCS5_PBKDF2_HMAC(password.c_str(), (int)password.size(),
                      parts.salt.data(), (int)parts.salt.size(),
                      parts.iterations, EVP_sha256(), HASH_LEN, computed);

    return parts.hash.size() == HASH_LEN &&
           CRYPTO_memcmp(computed, parts.hash.data(), HASH_LEN) == 0;
  }

private:
  struct HashParts {
    int iterations = 0;
    std::vector<unsigned char> salt;
    std::vector<unsigned char> hash;
  };

  static constexpr const char *PREFIX = "$pbkdf2-sha256$";
  static constexpr size_t PREFIX_LEN = 15;

  static HashParts parse_hash(const std::string &s) {
    HashParts parts;
    // 格式: $pbkdf2-sha256$<iter>$<salt_hex>$<hash_hex>
    if (s.compare(0, PREFIX_LEN, PREFIX) != 0)
      return parts;

    size_t p1 = s.find('$', PREFIX_LEN);
    if (p1 == std::string::npos)
      return parts;
    parts.iterations = std::stoi(s.substr(PREFIX_LEN, p1 - PREFIX_LEN));

    size_t p2 = s.find('$', p1 + 1);
    if (p2 == std::string::npos)
      return parts;

    parts.salt = from_hex(s.substr(p1 + 1, p2 - p1 - 1));
    parts.hash = from_hex(s.substr(p2 + 1));
    return parts;
  }

  static std::string to_hex(const unsigned char *data, int len) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (int i = 0; i < len; ++i)
      oss << std::setw(2) << (unsigned)data[i];
    return oss.str();
  }

  static std::vector<unsigned char> from_hex(const std::string &hex) {
    std::vector<unsigned char> bytes;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
      unsigned b;
      std::istringstream ss(hex.substr(i, 2));
      ss >> std::hex >> b;
      bytes.push_back((unsigned char)b);
    }
    return bytes;
  }
};

#endif
