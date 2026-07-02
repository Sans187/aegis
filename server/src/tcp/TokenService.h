#pragma once
// TokenService —— 终端用户会话 token（PostgreSQL，统一 tokens 表 + app_id 列）。
// 安全改进：落库只存 SHA-256(token)，明文 token 仅在签发那一刻返回给客户端。

#include <drogon/drogon.h>
#include <string>
#include <optional>
#include <cstdint>

namespace aegis::tcp {

struct IssuedToken {
    std::string token;       // 明文 token（仅本次返回给客户端）
    int64_t     expires_at;  // 过期时间（epoch 秒）
};

struct ActiveToken {
    std::string device_id;
    int64_t     expires_at{0};
    int         revoked{0};
};

// 签发新 token：撤销该用户此前所有未撤销 token，再插入一条（事务）。返回明文 token。
drogon::Task<std::optional<IssuedToken>>
issueToken(const std::string& app_id, const std::string& username,
           const std::string& device_id, const std::string& ip,
           const std::string& user_agent, int64_t expires_at_epoch);

// 取该用户当前有效（未撤销且未过期）的会话，用于沿用旧登录的错误码语义。
drogon::Task<std::optional<ActiveToken>>
getActiveToken(const std::string& app_id, const std::string& username);

// 由明文 token 反查用户名（仅当未撤销）。
drogon::Task<std::optional<std::string>>
getUsernameByToken(const std::string& app_id, const std::string& token);

// 校验明文 token 是否有效（存在、未撤销、未过期）。
drogon::Task<bool>
verifyToken(const std::string& app_id, const std::string& token);

} // namespace aegis::tcp
