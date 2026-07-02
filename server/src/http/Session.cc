#include "http/Session.h"
#include "security/Crypto.h"

#include <ctime>

using namespace drogon;

namespace aegis::http {

static int64_t nowSec() { return static_cast<int64_t>(time(nullptr)); }

drogon::Task<std::string>
createSession(std::string agentId, std::string ip, std::string ua, int64_t ttlSeconds) {
    auto db = app().getDbClient();
    const std::string token = Crypto::Base64Encode(Crypto::generateRandomKey(32));
    const std::string hash  = Crypto::Sha256Hex(token);
    const int64_t now = nowSec();
    co_await db->execSqlCoro(
        "INSERT INTO admin_sessions(token_hash, agent_id, ip, user_agent, created_at, expires_at, revoked) "
        "VALUES($1,$2,$3,$4,$5,$6,0);",
        hash, agentId, ip, ua, now, now + ttlSeconds);
    co_return token;
}

drogon::Task<std::optional<std::string>>
validateSession(std::string token) {
    if (token.empty()) co_return std::nullopt;
    auto db = app().getDbClient();
    // JOIN agents 并强制 status='active'：账号被禁用即时全局失效（纵深防御，
    // 即使某条会话漏撤销，禁用账号也无法通过鉴权）。
    auto r = co_await db->execSqlCoro(
        "SELECT s.agent_id FROM admin_sessions s "
        "JOIN agents a ON a.id = s.agent_id "
        "WHERE s.token_hash=$1 AND s.revoked=0 AND s.expires_at>$2 AND a.status='active' LIMIT 1;",
        Crypto::Sha256Hex(token), nowSec());
    if (r.empty()) co_return std::nullopt;
    co_return r[0]["agent_id"].as<std::string>();
}

drogon::Task<void> revokeSession(std::string token) {
    auto db = app().getDbClient();
    co_await db->execSqlCoro(
        "UPDATE admin_sessions SET revoked=1 WHERE token_hash=$1;", Crypto::Sha256Hex(token));
    co_return;
}

drogon::Task<void> revokeAllForAgent(std::string agentId) {
    auto db = app().getDbClient();
    co_await db->execSqlCoro(
        "UPDATE admin_sessions SET revoked=1 WHERE agent_id=$1 AND revoked=0;", agentId);
    co_return;
}

} // namespace aegis::http
