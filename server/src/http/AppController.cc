#include "http/AppController.h"
#include "http/AuthFilter.h"
#include "http/Resp.h"
#include "http/Permissions.h"
#include "tcp/AppSettings.h"
#include "tcp/v2/VerifyServiceV2.h"

#include <drogon/drogon.h>
#include <ctime>

using namespace drogon;
using namespace aegis::http;

namespace aegis {

// 超管 + 安全密码 双闸（敏感的全局操作专用）。返回错误响应或 nullptr(通过)。
static drogon::Task<HttpResponsePtr> superAndSecPw(const std::string& uid, const std::string& secPw) {
    if (!co_await perm::isSuper(uid)) co_return err("forbidden: super admin only", k403Forbidden);
    if (!co_await perm::checkSecurityPassword(uid, secPw))
        co_return err("security password incorrect or not set", k403Forbidden);
    co_return nullptr;
}

drogon::Task<> AppController::list(HttpRequestPtr req,
                                   std::function<void(const HttpResponsePtr&)> cb) {
    const auto uid = req->getAttributes()->get<std::string>("uid");
    auto db = app().getDbClient();
    auto rows = co_await db->execSqlCoro(
        "SELECT id, owner_id, name, mode, created_at FROM apps ORDER BY id;");

    Json::Value items(Json::arrayValue);
    for (const auto& r : rows) {
        const auto id = r["id"].as<std::string>();
        if (!co_await perm::canAccessApp(uid, id)) continue;  // 只列可访问的（仅 5 行，开销可忽略）
        Json::Value a;
        a["id"] = id;
        a["owner_id"] = r["owner_id"].as<std::string>();
        a["name"] = r["name"].as<std::string>();
        a["mode"] = r["mode"].as<std::string>();
        a["created_at"] = r["created_at"].as<long long>();
        items.append(a);
    }
    Json::Value v; v["ok"] = true; v["items"] = items;
    cb(jsonResp(v));
    co_return;
}

drogon::Task<> AppController::getSettings(HttpRequestPtr req,
                                          std::function<void(const HttpResponsePtr&)> cb) {
    const auto uid = req->getAttributes()->get<std::string>("uid");
    const std::string appId = param(req, "app_id");
    if (appId.empty()) { cb(err("app_id required", k400BadRequest)); co_return; }
    if (!co_await perm::canAccessApp(uid, appId)) { cb(err("forbidden", k403Forbidden)); co_return; }

    auto settings = co_await aegis::tcp::readSettings(appId);  // 已合并默认字段
    Json::Value v; v["ok"] = true; v["app_id"] = appId; v["settings"] = settings;
    cb(jsonResp(v));
    co_return;
}

drogon::Task<> AppController::updateSettings(HttpRequestPtr req,
                                             std::function<void(const HttpResponsePtr&)> cb) {
    const auto uid = req->getAttributes()->get<std::string>("uid");
    auto j = req->getJsonObject();
    const std::string appId = j ? (*j).get("app_id", "").asString() : "";
    if (!j || appId.empty() || !(*j).isMember("settings")) {
        cb(err("app_id/settings required", k400BadRequest)); co_return;
    }
    // 改全局设置更严：仅超管或该 app 属主，防止被分配该 app 的下级篡改全 app 配置。
    if (!co_await perm::canManageApp(uid, appId)) { cb(err("forbidden", k403Forbidden)); co_return; }

    Json::StreamWriterBuilder wb; wb["indentation"] = "";
    const std::string settingsStr = Json::writeString(wb, (*j)["settings"]);
    const long long now = static_cast<long long>(time(nullptr));

    auto db = app().getDbClient();
    co_await db->execSqlCoro(
        "INSERT INTO app_settings(app_id, settings, updated_at) VALUES($1,$2::jsonb,$3) "
        "ON CONFLICT (app_id) DO UPDATE SET settings=excluded.settings, updated_at=excluded.updated_at;",
        appId, settingsStr, now);
    cb(okResp());
    co_return;
}

drogon::Task<> AppController::create(HttpRequestPtr req,
                                     std::function<void(const HttpResponsePtr&)> cb) {
    const auto uid = req->getAttributes()->get<std::string>("uid");
    auto j = req->getJsonObject();
    const std::string name   = j ? (*j).get("name", "").asString() : "";
    const std::string mode   = j ? (*j).get("mode", "card").asString() : "card";
    const std::string secPw  = j ? (*j).get("security_password", "").asString() : "";
    if (name.empty()) { cb(err("name required", k400BadRequest)); co_return; }
    if (mode != "card" && mode != "user") { cb(err("mode must be card/user", k400BadRequest)); co_return; }
    if (auto e = co_await superAndSecPw(uid, secPw)) { cb(e); co_return; }

    auto db = app().getDbClient();
    // app_id 自动分配：当前最大数字 id + 1（从 100001 起）
    auto mx = co_await db->execSqlCoro(
        "SELECT COALESCE(MAX(CAST(id AS INTEGER)), 100000) + 1 AS nid FROM apps;");
    const std::string newId = std::to_string(mx[0]["nid"].as<long long>());
    const long long now = static_cast<long long>(time(nullptr));

    try {
        auto trx = co_await db->newTransactionCoro();
        co_await trx->execSqlCoro(
            "INSERT INTO apps(id, owner_id, name, mode, created_at) VALUES($1,$2,$3,$4,$5);",
            newId, uid, name, mode, now);
        co_await trx->execSqlCoro(
            "INSERT INTO app_settings(app_id, settings, updated_at) VALUES($1,'{}'::jsonb,$2);",
            newId, now);
    } catch (const std::exception& e) {
        LOG_ERROR << "apps.create: " << e.what();
        cb(err("create failed", k500InternalServerError)); co_return;
    }

    // TCP 验证服务实时纳入新软件
    if (auto* vs = tcp::v2::VerifyServiceV2::instance()) vs->addApp(std::stoi(newId));

    Json::Value v; v["ok"] = true; v["id"] = newId;
    cb(jsonResp(v));
    co_return;
}

drogon::Task<> AppController::remove(HttpRequestPtr req,
                                     std::function<void(const HttpResponsePtr&)> cb) {
    const auto uid = req->getAttributes()->get<std::string>("uid");
    auto j = req->getJsonObject();
    const std::string appId = j ? (*j).get("app_id", "").asString() : "";
    const std::string secPw = j ? (*j).get("security_password", "").asString() : "";
    if (appId.empty()) { cb(err("app_id required", k400BadRequest)); co_return; }
    if (auto e = co_await superAndSecPw(uid, secPw)) { cb(e); co_return; }

    auto db = app().getDbClient();
    try {
        // 级联删除该软件的全部数据（统一表，按 app_id 清理）
        auto trx = co_await db->newTransactionCoro();
        co_await trx->execSqlCoro("DELETE FROM tokens       WHERE app_id=$1;", appId);
        co_await trx->execSqlCoro("DELETE FROM users        WHERE app_id=$1;", appId);
        co_await trx->execSqlCoro("DELETE FROM cards        WHERE app_id=$1;", appId);
        co_await trx->execSqlCoro("DELETE FROM card_batches WHERE app_id=$1;", appId);
        co_await trx->execSqlCoro("DELETE FROM card_types   WHERE app_id=$1;", appId);
        co_await trx->execSqlCoro("DELETE FROM app_settings WHERE app_id=$1;", appId);
        co_await trx->execSqlCoro("DELETE FROM apps         WHERE id=$1;", appId);
    } catch (const std::exception& e) {
        LOG_ERROR << "apps.delete: " << e.what();
        cb(err("delete failed", k500InternalServerError)); co_return;
    }

    if (auto* vs = tcp::v2::VerifyServiceV2::instance()) {
        try { vs->removeApp(std::stoi(appId)); } catch (...) {}
    }
    cb(okResp());
    co_return;
}

drogon::Task<> AppController::rename(HttpRequestPtr req,
                                     std::function<void(const HttpResponsePtr&)> cb) {
    const auto uid = req->getAttributes()->get<std::string>("uid");
    if (!co_await perm::isSuper(uid)) { cb(err("forbidden", k403Forbidden)); co_return; }
    auto j = req->getJsonObject();
    const std::string appId = j ? (*j).get("app_id", "").asString() : "";
    const std::string name  = j ? (*j).get("name", "").asString() : "";
    if (appId.empty() || name.empty()) { cb(err("app_id/name required", k400BadRequest)); co_return; }

    auto db = app().getDbClient();
    auto r = co_await db->execSqlCoro("UPDATE apps SET name=$1 WHERE id=$2;", name, appId);
    cb(r.affectedRows() ? okResp() : err("not found", k404NotFound));
    co_return;
}

} // namespace aegis
