#include "security/Password.h"

#include <sodium.h>
#include <stdexcept>

namespace security {

// 交互式后台登录，选 INTERACTIVE 档（约 64MB 内存 / 数十毫秒），
// 安全足够且不至于拖垮登录吞吐。需要更强可改 MODERATE/SENSITIVE。
static constexpr unsigned long long kOpsLimit = crypto_pwhash_OPSLIMIT_INTERACTIVE;
static constexpr size_t             kMemLimit = crypto_pwhash_MEMLIMIT_INTERACTIVE;

bool initSodium() {
    return sodium_init() >= 0;
}

std::string hashPassword(const std::string& plain) {
    char encoded[crypto_pwhash_STRBYTES];
    if (crypto_pwhash_str(encoded,
                          plain.c_str(), plain.size(),
                          kOpsLimit, kMemLimit) != 0) {
        // 通常是内存不足
        throw std::runtime_error("argon2 hashing failed (out of memory?)");
    }
    return std::string(encoded);
}

bool verifyPassword(const std::string& plain, const std::string& encodedHash) {
    if (encodedHash.empty()) return false;
    return crypto_pwhash_str_verify(encodedHash.c_str(),
                                    plain.c_str(), plain.size()) == 0;
}

bool needsRehash(const std::string& encodedHash) {
    if (encodedHash.empty()) return true;
    return crypto_pwhash_str_needs_rehash(encodedHash.c_str(),
                                          kOpsLimit, kMemLimit) != 0;
}

} // namespace security
