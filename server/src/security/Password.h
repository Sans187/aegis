#pragma once
// Password.h —— 代理后台账号的密码哈希。
// 用 libsodium 的 Argon2id（crypto_pwhash），每个用户独立随机盐，
// 编码串自带盐与参数，校验时无需单独取盐。彻底取代旧的 md5(pw + 固定盐)。

#include <string>

namespace security {

// 进程启动时调用一次（sodium_init）。返回 false 表示 libsodium 初始化失败。
bool initSodium();

// 生成 Argon2id 编码哈希串（形如 "$argon2id$v=19$m=...,t=...,p=...$salt$hash"）。
// 失败抛 std::runtime_error。
std::string hashPassword(const std::string& plain);

// 校验明文与编码哈希串是否匹配。恒定时间，内部已防侧信道。
bool verifyPassword(const std::string& plain, const std::string& encodedHash);

// 判断一个哈希串是否需要重新计算（参数升级时为 true）。
bool needsRehash(const std::string& encodedHash);

} // namespace security
