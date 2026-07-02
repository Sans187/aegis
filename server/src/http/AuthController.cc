#include "http/AuthController.h"
#include "http/AuthFilter.h"
#include "http/Session.h"
#include "security/Password.h"

#include <drogon/drogon.h>
#include <chrono>
#include <mutex>
#include <unordered_map>

using namespace drogon;

namespace aegis {

static constexpr int64_t kSessionTtl = 12 * 3600;     // 后台会话 12 小时
static constexpr int     kMaxFails     = 5;           // 单 IP 锁定阈值
static constexpr int     kMaxFailsUser = 20;          // 单用户名锁定阈值（高于 IP，避免被人为锁死受害账号）
static constexpr auto     kWindow      = std::chrono::minutes(15);

// ---------- 登录失败锁定（按 IP + 按用户名，内存态）----------
// 双维度：同 IP 连续失败 5 次锁 15 分钟（挡单机爆破）；同 *用户名* 连续失败 5 次
// 也锁 15 分钟（挡换 IP 的分布式爆破打同一账号）。任一维度命中即拒绝。
namespace {
struct FailState { std::chrono::steady_clock::time_point first; int count{0};
                   std::chrono::steady_clock::time_point lockedUntil{}; };
std::mutex g_mtx;
std::unordered_map<std::string, FailState> g_failIp;
std::unordered_map<std::string, FailState> g_failUser;

// 清理早已不在窗口内、且未处于锁定的过期条目，防止 map 无限增长。
void sweepLocked(std::unordered_map<std::string, FailState>& m,
                 std::chrono::steady_clock::time_point now) {
    for (auto it = m.begin(); it != m.end(); ) {
        if (now > it->second.lockedUntil && now - it->second.first > kWindow) it = m.erase(it);
        else ++it;
    }
}
bool lockedIn(std::unordered_map<std::string, FailState>& m, const std::string& key,
              std::chrono::steady_clock::time_point now) {
    auto it = m.find(key);
    return it != m.end() && now < it->second.lockedUntil;
}
void bumpFail(std::unordered_map<std::string, FailState>& m, const std::string& key,
              std::chrono::steady_clock::time_point now, int threshold) {
    auto& s = m[key];
    if (s.count == 0 || now - s.first > kWindow) { s.first = now; s.count = 0; }
    if (++s.count >= threshold) s.lockedUntil = now + kWindow;
}

// 任一维度锁定 → 拦截。
bool isLocked(const std::string& ip, const std::string& user) {
    std::lock_guard<std::mutex> lk(g_mtx);
    auto now = std::chrono::steady_clock::now();
    return lockedIn(g_failIp, ip, now) || (!user.empty() && lockedIn(g_failUser, user, now));
}
void recordFailure(const std::string& ip, const std::string& user) {
    std::lock_guard<std::mutex> lk(g_mtx);
    auto now = std::chrono::steady_clock::now();
    bumpFail(g_failIp, ip, now, kMaxFails);
    if (!user.empty()) bumpFail(g_failUser, user, now, kMaxFailsUser);
    sweepLocked(g_failIp, now);
    sweepLocked(g_failUser, now);
}
void clearFailures(const std::string& ip, const std::string& user) {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_failIp.erase(ip);
    if (!user.empty()) g_failUser.erase(user);
}
} // namespace

static HttpResponsePtr jsonResp(const Json::Value& v, HttpStatusCode code = k200OK) {
    auto r = HttpResponse::newHttpJsonResponse(v);
    r->setStatusCode(code);
    return r;
}

static Json::Value parsePerms(const std::string& s) {
    Json::Value out(Json::arrayValue);
    Json::CharReaderBuilder b;
    std::string errs;
    std::unique_ptr<Json::CharReader> rd(b.newCharReader());
    Json::Value tmp;
    if (rd->parse(s.data(), s.data() + s.size(), &tmp, &errs) && tmp.isArray()) out = tmp;
    return out;
}
static Json::Value parseObj(const std::string& s) {
    Json::Value out(Json::objectValue), tmp;
    Json::CharReaderBuilder b;
    std::string errs;
    std::unique_ptr<Json::CharReader> rd(b.newCharReader());
    if (rd->parse(s.data(), s.data() + s.size(), &tmp, &errs) && tmp.isObject()) out = tmp;
    return out;
}

drogon::Task<> AuthController::login(HttpRequestPtr req,
                                     std::function<void(const HttpResponsePtr&)> cb) {
    const std::string ip = req->getPeerAddr().toIp();

    auto json = req->getJsonObject();
    const std::string u = json ? (*json).get("username", "").asString() : "";
    const std::string p = json ? (*json).get("password", "").asString() : "";
    if (u.empty() || p.empty()) {
        Json::Value v; v["ok"] = false; v["error"] = "username/password required";
        cb(jsonResp(v, k400BadRequest));
        co_return;
    }

    if (isLocked(ip, u)) {
        Json::Value v; v["ok"] = false; v["error"] = "too many attempts, try later";
        cb(jsonResp(v, k429TooManyRequests));
        co_return;
    }

    try {
        auto db = app().getDbClient();
        auto r = co_await db->execSqlCoro(
            "SELECT id, password_hash, must_change_password, nickname, level, perms, apps, card_types, status "
            "FROM agents WHERE username=$1 LIMIT 1;", u);

        const bool credOk = !r.empty()
                     && security::verifyPassword(p, r[0]["password_hash"].as<std::string>());
        if (!credOk) {
            recordFailure(ip, u);
            Json::Value v; v["ok"] = false; v["error"] = "invalid username or password";
            cb(jsonResp(v, k401Unauthorized));
            co_return;
        }
        // 凭据正确但账号被禁用：明确提示，不计入失败锁定，避免被通用提示掩盖。
        if (r[0]["status"].as<std::string>() != "active") {
            Json::Value v; v["ok"] = false; v["error"] = "账号已被禁用，请联系管理员";
            cb(jsonResp(v, k403Forbidden));
            co_return;
        }
        clearFailures(ip, u);

        const auto id = r[0]["id"].as<std::string>();
        co_await db->execSqlCoro(
            "UPDATE agents SET last_login_ip=$1, last_login_ua=$2, last_login_at=$3 WHERE id=$4;",
            ip, req->getHeader("user-agent"),
            static_cast<int64_t>(trantor::Date::now().secondsSinceEpoch()), id);

        const auto token = co_await aegis::http::createSession(id, ip, req->getHeader("user-agent"), kSessionTtl);

        Json::Value agent;
        agent["id"] = id;
        agent["username"] = u;
        agent["nickname"] = r[0]["nickname"].isNull() ? "" : r[0]["nickname"].as<std::string>();
        agent["level"] = r[0]["level"].as<int>();
        agent["perms"] = parsePerms(r[0]["perms"].as<std::string>());
        agent["apps"] = parsePerms(r[0]["apps"].as<std::string>());
        agent["card_types"] = parseObj(r[0]["card_types"].as<std::string>());

        Json::Value v;
        v["ok"] = true;
        v["token"] = token;
        v["must_change_password"] = r[0]["must_change_password"].as<bool>();
        v["agent"] = agent;
        cb(jsonResp(v));
    } catch (const std::exception& e) {
        LOG_ERROR << "login: " << e.what();
        Json::Value v; v["ok"] = false; v["error"] = "server error";
        cb(jsonResp(v, k500InternalServerError));
    }
    co_return;
}

drogon::Task<> AuthController::logout(HttpRequestPtr req,
                                      std::function<void(const HttpResponsePtr&)> cb) {
    const std::string& h = req->getHeader("authorization");
    const std::string prefix = "Bearer ";
    if (h.size() > prefix.size() && h.compare(0, prefix.size(), prefix) == 0)
        co_await aegis::http::revokeSession(h.substr(prefix.size()));
    Json::Value v; v["ok"] = true;
    cb(jsonResp(v));
    co_return;
}

drogon::Task<> AuthController::me(HttpRequestPtr req,
                                  std::function<void(const HttpResponsePtr&)> cb) {
    const auto uid = req->getAttributes()->get<std::string>("uid");
    auto db = app().getDbClient();
    auto r = co_await db->execSqlCoro(
        "SELECT id, username, nickname, level, perms, apps, card_types, must_change_password "
        "FROM agents WHERE id=$1 LIMIT 1;", uid);
    if (r.empty()) {
        Json::Value v; v["ok"] = false; v["error"] = "not found";
        cb(jsonResp(v, k404NotFound));
        co_return;
    }
    Json::Value agent;
    agent["id"] = r[0]["id"].as<std::string>();
    agent["username"] = r[0]["username"].as<std::string>();
    agent["nickname"] = r[0]["nickname"].isNull() ? "" : r[0]["nickname"].as<std::string>();
    agent["level"] = r[0]["level"].as<int>();
    agent["perms"] = parsePerms(r[0]["perms"].as<std::string>());
    agent["apps"] = parsePerms(r[0]["apps"].as<std::string>());
    agent["card_types"] = parseObj(r[0]["card_types"].as<std::string>());
    agent["must_change_password"] = r[0]["must_change_password"].as<bool>();
    Json::Value v; v["ok"] = true; v["agent"] = agent;
    cb(jsonResp(v));
    co_return;
}

drogon::Task<> AuthController::changePassword(HttpRequestPtr req,
                                              std::function<void(const HttpResponsePtr&)> cb) {
    const auto uid = req->getAttributes()->get<std::string>("uid");
    auto json = req->getJsonObject();
    const std::string oldP = json ? (*json).get("oldPassword", "").asString() : "";
    const std::string newP = json ? (*json).get("newPassword", "").asString() : "";

    if (newP.size() < 8) {
        Json::Value v; v["ok"] = false; v["error"] = "new password too short (min 8)";
        cb(jsonResp(v, k400BadRequest));
        co_return;
    }

    try {
        auto db = app().getDbClient();
        auto r = co_await db->execSqlCoro("SELECT password_hash FROM agents WHERE id=$1 LIMIT 1;", uid);
        if (r.empty() || !security::verifyPassword(oldP, r[0]["password_hash"].as<std::string>())) {
            Json::Value v; v["ok"] = false; v["error"] = "old password incorrect";
            cb(jsonResp(v, k400BadRequest));
            co_return;
        }

        const std::string newHash = security::hashPassword(newP);
        co_await db->execSqlCoro(
            "UPDATE agents SET password_hash=$1, must_change_password=FALSE, password_changed_at=$2 WHERE id=$3;",
            newHash, static_cast<int64_t>(trantor::Date::now().secondsSinceEpoch()), uid);

        // 改密后吊销全部旧会话，再发一个新会话让当前端无缝继续。
        co_await aegis::http::revokeAllForAgent(uid);
        const auto token = co_await aegis::http::createSession(
            uid, req->getPeerAddr().toIp(), req->getHeader("user-agent"), kSessionTtl);

        Json::Value v; v["ok"] = true; v["token"] = token;
        cb(jsonResp(v));
    } catch (const std::exception& e) {
        LOG_ERROR << "changePassword: " << e.what();
        Json::Value v; v["ok"] = false; v["error"] = "server error";
        cb(jsonResp(v, k500InternalServerError));
    }
    co_return;
}

} // namespace aegis
