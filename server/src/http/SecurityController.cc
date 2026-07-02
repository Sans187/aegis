#include "http/SecurityController.h"
#include "http/AuthFilter.h"
#include "http/Resp.h"
#include "http/Permissions.h"
#include "security/Password.h"

#include <drogon/drogon.h>

using namespace drogon;
using namespace aegis::http;

namespace aegis {

drogon::Task<> SecurityController::status(HttpRequestPtr req,
                                          std::function<void(const HttpResponsePtr&)> cb) {
    const auto uid = req->getAttributes()->get<std::string>("uid");
    auto db = app().getDbClient();
    auto r = co_await db->execSqlCoro(
        "SELECT level, security_password_hash FROM agents WHERE id=$1 LIMIT 1;", uid);
    Json::Value v;
    v["ok"] = true;
    v["isSuper"] = !r.empty() && r[0]["level"].as<int>() == 1;
    v["set"] = !r.empty() && !r[0]["security_password_hash"].isNull();
    cb(jsonResp(v));
    co_return;
}

drogon::Task<> SecurityController::set(HttpRequestPtr req,
                                       std::function<void(const HttpResponsePtr&)> cb) {
    const auto uid = req->getAttributes()->get<std::string>("uid");
    if (!co_await perm::isSuper(uid)) { cb(err("forbidden", k403Forbidden)); co_return; }

    auto j = req->getJsonObject();
    const std::string loginPw = j ? (*j).get("loginPassword", "").asString() : "";
    const std::string secPw   = j ? (*j).get("securityPassword", "").asString() : "";
    if (secPw.size() < 6) { cb(err("security password too short (min 6)", k400BadRequest)); co_return; }

    auto db = app().getDbClient();
    auto r = co_await db->execSqlCoro("SELECT password_hash FROM agents WHERE id=$1 LIMIT 1;", uid);
    // 设置/修改安全密码必须用登录密码二次确认
    if (r.empty() || !security::verifyPassword(loginPw, r[0]["password_hash"].as<std::string>())) {
        cb(err("login password incorrect", k400BadRequest)); co_return;
    }

    const std::string hash = security::hashPassword(secPw);
    co_await db->execSqlCoro("UPDATE agents SET security_password_hash=$1 WHERE id=$2;", hash, uid);
    cb(okResp());
    co_return;
}

} // namespace aegis
