#include "tcp/TokenService.h"
#include "security/Crypto.h"

#include <ctime>

using namespace drogon;
using namespace drogon::orm;

namespace aegis::tcp {

static int64_t nowSec() { return static_cast<int64_t>(time(nullptr)); }

// 32 字节 CSPRNG -> 十六进制明文 token（cryptopp 的安全随机源）
static std::string randomTokenHex() {
    return Crypto::Base64Encode(Crypto::generateRandomKey(32)); // 高熵；非 hex 但作为不透明 token 足矣
}

drogon::Task<std::optional<IssuedToken>>
issueToken(const std::string& app_id, const std::string& username,
           const std::string& device_id, const std::string& ip,
           const std::string& user_agent, int64_t expires_at_epoch) {
    try {
        auto db = app().getDbClient();
        const auto now = nowSec();
        const std::string token = randomTokenHex();
        const std::string hash = Crypto::Sha256Hex(token);

        {
            auto trx = co_await db->newTransactionCoro();
            co_await trx->execSqlCoro(
                "UPDATE tokens SET revoked=1 WHERE app_id=$1 AND username=$2 AND revoked=0;",
                app_id, username);
            co_await trx->execSqlCoro(
                "INSERT INTO tokens(app_id, username, token_hash, device_id, first_ip, last_ip, "
                "user_agent, created_at, last_seen_at, expires_at, revoked) "
                "VALUES($1,$2,$3,$4,$5,$5,$6,$7,$7,$8,0);",
                app_id, username, hash, device_id, ip, user_agent, now, expires_at_epoch);
        }
        co_return IssuedToken{token, expires_at_epoch};
    } catch (const std::exception& e) {
        LOG_ERROR << "issueToken: " << e.what();
        co_return std::nullopt;
    }
}

drogon::Task<std::optional<ActiveToken>>
getActiveToken(const std::string& app_id, const std::string& username) {
    try {
        auto db = app().getDbClient();
        auto r = co_await db->execSqlCoro(
            "SELECT device_id, expires_at, revoked FROM tokens "
            "WHERE app_id=$1 AND username=$2 AND revoked=0 AND expires_at>$3 "
            "ORDER BY created_at DESC LIMIT 1;",
            app_id, username, nowSec());
        if (r.empty()) co_return std::nullopt;
        ActiveToken t;
        t.device_id  = r[0]["device_id"].as<std::string>();
        t.expires_at = r[0]["expires_at"].as<int64_t>();
        t.revoked    = r[0]["revoked"].as<int>();
        co_return t;
    } catch (const std::exception& e) {
        LOG_ERROR << "getActiveToken: " << e.what();
        co_return std::nullopt;
    }
}

drogon::Task<std::optional<std::string>>
getUsernameByToken(const std::string& app_id, const std::string& token) {
    try {
        auto db = app().getDbClient();
        auto r = co_await db->execSqlCoro(
            "SELECT username FROM tokens WHERE app_id=$1 AND token_hash=$2 AND revoked=0 LIMIT 1;",
            app_id, Crypto::Sha256Hex(token));
        if (r.empty()) co_return std::nullopt;
        co_return r[0]["username"].as<std::string>();
    } catch (const std::exception& e) {
        LOG_ERROR << "getUsernameByToken: " << e.what();
        co_return std::nullopt;
    }
}

drogon::Task<bool>
verifyToken(const std::string& app_id, const std::string& token) {
    try {
        auto db = app().getDbClient();
        auto r = co_await db->execSqlCoro(
            "SELECT 1 FROM tokens WHERE app_id=$1 AND token_hash=$2 "
            "AND revoked=0 AND expires_at>$3 LIMIT 1;",
            app_id, Crypto::Sha256Hex(token), nowSec());
        co_return !r.empty();
    } catch (const std::exception& e) {
        LOG_ERROR << "verifyToken: " << e.what();
        co_return false;
    }
}

} // namespace aegis::tcp
