#pragma once
// 后台会话（管理员/代理）。token 随机生成，落库只存 SHA-256(token)。

#include <drogon/drogon.h>
#include <string>
#include <optional>
#include <cstdint>

namespace aegis::http {

// 新建会话，返回明文 token（仅此一次）。
drogon::Task<std::string>
createSession(std::string agentId, std::string ip, std::string ua, int64_t ttlSeconds);

// 校验明文 token：未撤销且未过期则返回 agent_id。
drogon::Task<std::optional<std::string>>
validateSession(std::string token);

// 撤销单个会话。
drogon::Task<void> revokeSession(std::string token);

// 撤销某代理的全部会话（改密 / 风控用）。
drogon::Task<void> revokeAllForAgent(std::string agentId);

} // namespace aegis::http
