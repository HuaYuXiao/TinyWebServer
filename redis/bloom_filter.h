#ifndef BLOOM_FILTER_H
#define BLOOM_FILTER_H

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

// 布隆过滤器 —— 防缓存穿透
// 使用双哈希技术生成 k 个哈希函数: g_i(x) = (h1(x) + i * h2(x)) % m
class BloomFilter {
public:
  // n: 预估元素数量, p: 期望误判率 (默认 0.01 = 1%)
  BloomFilter(size_t expected_elements = 1000000, double false_positive_rate = 0.01);

  void insert(const std::string &key);
  bool contains(const std::string &key) const;
  void clear();
  size_t size() const { return bit_count_ / 64; }

private:
  // FNV-1a hash
  static uint64_t fnv1a(const std::string &key);
  // std::hash wrapper
  static uint64_t hash2(const std::string &key);

  size_t num_hashes_;  // k: 哈希函数个数
  size_t bit_count_;   // m: 位数组大小
  std::vector<uint64_t> bits_;  // 位数组
};

inline BloomFilter::BloomFilter(size_t expected_elements,
                                double false_positive_rate) {
  // m = -n * ln(p) / (ln(2))^2   — 最优位数组大小
  // k = (m / n) * ln(2)          — 最优哈希函数个数
  double m = -static_cast<double>(expected_elements) *
             std::log(false_positive_rate) / (std::log(2.0) * std::log(2.0));
  double k = (m / static_cast<double>(expected_elements)) * std::log(2.0);

  bit_count_ = static_cast<size_t>(m) + 1;
  num_hashes_ = static_cast<size_t>(k) + 1;
  bits_.resize((bit_count_ + 63) / 64, 0);
}

inline uint64_t BloomFilter::fnv1a(const std::string &key) {
  uint64_t hash = 14695981039346656037ULL;
  for (char c : key) {
    hash ^= static_cast<uint64_t>(c);
    hash *= 1099511628211ULL;
  }
  return hash;
}

inline uint64_t BloomFilter::hash2(const std::string &key) {
  return std::hash<std::string>{}(key);
}

inline void BloomFilter::insert(const std::string &key) {
  uint64_t h1 = fnv1a(key);
  uint64_t h2 = hash2(key);
  for (size_t i = 0; i < num_hashes_; ++i) {
    uint64_t idx = (h1 + i * h2) % bit_count_;
    bits_[idx / 64] |= (1ULL << (idx % 64));
  }
}

inline bool BloomFilter::contains(const std::string &key) const {
  uint64_t h1 = fnv1a(key);
  uint64_t h2 = hash2(key);
  for (size_t i = 0; i < num_hashes_; ++i) {
    uint64_t idx = (h1 + i * h2) % bit_count_;
    if (!(bits_[idx / 64] & (1ULL << (idx % 64)))) {
      return false;
    }
  }
  return true;
}

inline void BloomFilter::clear() {
  std::fill(bits_.begin(), bits_.end(), 0);
}

#endif
