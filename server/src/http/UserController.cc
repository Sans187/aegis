#include "http/UserController.h"
#include "http/AuthFilter.h"
#include "http/Resp.h"
#include "http/Sql.h"
#include "http/Permissions.h"
#include "security/Password.h"

#include <drogon/drogon.h>
#include <algorithm>
#include <ctime>
#include <unordered_map>
#include <vector>

using namespace drogon;
using namespace aegis::http;

namespace aegis {

// 子树 CTE 片段：以 $2 为根。用于把"子代理过滤"强制放在 SQL 内执行。
static constexpr const char* kSubCte =
    "WITH RECURSIVE sub AS ("
    "  SELECT id FROM agents WHERE id=$2 "
    "  UNION ALL "
    "  SELECT a.id FROM agents a JOIN sub ON a.parent_id = sub.id"
    ") ";

// 后台手动创建账号（账号模式）：用户名 + 密码(哈希) + 时长。
drogon::Task<> UserController::create(HttpRequestPtr req,
                                      std::function<void(const HttpResponsePtr&)> cb) {
    const auto uid = req->getAttributes()->get<std::string>("uid");
    auto j = req->getJsonObject();
    const std::string appId    = j ? (*j).get("app_id", "").asString() : "";
    const std::string username = j ? (*j).get("username", "").asString() : "";
    const std::string password = j ? (*j).get("password", "").asString() : "";
    const long long   hours    = j ? (*j).get("hours", 0).asInt64() : 0;
    const std::string remark   = j ? (*j).get("remark", "").asString() : "";
    if (appId.empty() || username.empty() || password.empty()) {
        cb(err("app_id/username/password required", k400BadRequest)); co_return;
    }
    if (!co_await perm::canAccessApp(uid, appId)) { cb(err("forbidden", k403Forbidden)); co_return; }
    if (!co_await perm::hasPermission(uid, "cardmaking")) { cb(err("forbidden", k403Forbidden)); co_return; }

    auto db = app().getDbClient();
    auto ex = co_await db->execSqlCoro(
        "SELECT 1 FROM users WHERE app_id=$1 AND username=$2 LIMIT 1;", appId, username);
    if (!ex.empty()) { cb(err("username already exists", k409Conflict)); co_return; }

    const long long now = static_cast<long long>(time(nullptr));
    const long long expired_at = now + hours * 3600;
    const std::string hash = security::hashPassword(password);
    co_await db->execSqlCoro(
        "INSERT INTO users(app_id, username, password_hash, created_at, expired_at, "
        "frozen, online, remark, agent_id) VALUES($1,$2,$3,$4,$5,0,0,$6,$7);",
        appId, username, hash, now, expired_at, remark, uid);
    cb(okResp());
    co_return;
}

// 批量搜索用户：粘贴一批卡号，返回"用过这些卡"的用户（限本人子树）。
drogon::Task<> UserController::batch(HttpRequestPtr req,
                                    std::function<void(const HttpResponsePtr&)> cb) {
    const auto uid = req->getAttributes()->get<std::string>("uid");
    auto j = req->getJsonObject();
    const std::string appId = j ? (*j).get("app_id", "").asString() : "";
    const std::string codesText = j ? (*j).get("codes", "").asString() : "";
    if (appId.empty()) { cb(err("app_id required", k400BadRequest)); co_return; }
    if (!co_await perm::canAccessApp(uid, appId)) { cb(err("forbidden", k403Forbidden)); co_return; }

    const auto codes = sql::splitLines(codesText);
    if (codes.empty()) { cb(err("codes required", k400BadRequest)); co_return; }
    const std::string arr = sql::pgTextArray(codes);

    int page = std::max(1, (*j).get("page", 1).asInt());
    int pageSize = std::clamp((*j).get("pageSize", 50).asInt(), 1, 500);
    const int offset = (page - 1) * pageSize;

    const std::string latestType =
        "(recharges -> GREATEST(jsonb_array_length(recharges)-1, 0) ->> 'type_id')";
    const std::string base =
        std::string(" FROM users WHERE app_id=$1 AND agent_id IN (SELECT id FROM sub) "
        "AND EXISTS (SELECT 1 FROM jsonb_array_elements(recharges) e WHERE (e->>'code') = ANY($3::text[]))");
    const std::string tail = " ORDER BY created_at DESC LIMIT " +
        std::to_string(pageSize) + " OFFSET " + std::to_string(offset) + ";";

    try {
        auto db = app().getDbClient();

        // 「全选所有匹配」用：粘贴卡密模式下，只回命中用户的 id（封顶 20000）。
        if ((*j).get("idsOnly", false).asBool()) {
            auto idr = co_await db->execSqlCoro(
                std::string(kSubCte) + "SELECT id" + base + " LIMIT 20000;", appId, uid, arr);
            Json::Value ids(Json::arrayValue);
            for (const auto& r : idr) ids.append(r["id"].as<long long>());
            Json::Value v; v["ok"] = true; v["ids"] = ids; cb(jsonResp(v)); co_return;
        }

        auto cntRs = co_await db->execSqlCoro(
            std::string(kSubCte) + "SELECT COUNT(*) AS cnt" + base + ";", appId, uid, arr);
        const long long total = cntRs[0]["cnt"].as<long long>();
        auto rows = co_await db->execSqlCoro(
            std::string(kSubCte) + "SELECT *, " + latestType + " AS latest_type_id" + base + tail,
            appId, uid, arr);

        std::unordered_map<std::string, std::string> typeCache;
        Json::Value items(Json::arrayValue);
        for (const auto& r : rows) {
            Json::Value u;
            u["id"] = r["id"].as<long long>();
            u["username"] = r["username"].as<std::string>();
            u["machine_code"] = r["machine_code"].isNull() ? "" : r["machine_code"].as<std::string>();
            u["created_at"] = r["created_at"].as<long long>();
            u["expired_at"] = r["expired_at"].as<long long>();
            u["frozen"] = r["frozen"].as<int>();
            u["online"] = r["online"].as<int>();
            u["remark"] = r["remark"].isNull() ? "" : r["remark"].as<std::string>();
            u["extra"] = r["extra"].isNull() ? "" : r["extra"].as<std::string>();
            const std::string tid = r["latest_type_id"].isNull() ? "" : r["latest_type_id"].as<std::string>();
            std::string tname;
            if (!tid.empty()) {
                auto it = typeCache.find(tid);
                if (it == typeCache.end()) {
                    auto tn = co_await db->execSqlCoro("SELECT name FROM card_types WHERE id=$1 LIMIT 1;", tid);
                    it = typeCache.emplace(tid, tn.empty() ? "" : tn[0]["name"].as<std::string>()).first;
                }
                tname = it->second;
            }
            u["card_type"] = tname;
            items.append(u);
        }
        Json::Value v;
        v["ok"] = true; v["total"] = static_cast<Json::Int64>(total);
        v["page"] = page; v["pageSize"] = pageSize; v["items"] = items;
        v["queried"] = static_cast<int>(codes.size());
        cb(jsonResp(v));
    } catch (const std::exception& e) {
        LOG_ERROR << "users.batch: " << e.what();
        cb(err("server error", k500InternalServerError));
    }
    co_return;
}

// 用户详情：全部字段 + 充值历史（限本人子树）。
drogon::Task<> UserController::detail(HttpRequestPtr req,
                                      std::function<void(const HttpResponsePtr&)> cb) {
    const auto uid = req->getAttributes()->get<std::string>("uid");
    const std::string appId = param(req, "app_id");
    const std::string idStr = param(req, "id");
    if (appId.empty() || idStr.empty()) { cb(err("app_id/id required", k400BadRequest)); co_return; }
    if (!co_await perm::canAccessApp(uid, appId)) { cb(err("forbidden", k403Forbidden)); co_return; }
    const long long id = std::atoll(idStr.c_str());

    auto db = app().getDbClient();
    auto r = co_await db->execSqlCoro(
        std::string(
        "WITH RECURSIVE sub AS ("
        "  SELECT id FROM agents WHERE id=$2 "
        "  UNION ALL SELECT a.id FROM agents a JOIN sub ON a.parent_id=sub.id) ") +
        "SELECT id, username, machine_code, rebind_cnt, created_at, expired_at, frozen, online, "
        "ip_address, remark, extra, agent_id, recharges::text AS recharges "
        "FROM users WHERE app_id=$1 AND id=$3 AND agent_id IN (SELECT id FROM sub);",
        appId, uid, id);
    if (r.empty()) { cb(err("not found or forbidden", k404NotFound)); co_return; }
    const auto& row = r[0];

    Json::Value u;
    u["id"] = row["id"].as<long long>();
    u["username"] = row["username"].as<std::string>();
    u["machine_code"] = row["machine_code"].isNull() ? "" : row["machine_code"].as<std::string>();
    u["rebind_cnt"] = row["rebind_cnt"].as<int>();
    u["created_at"] = row["created_at"].as<long long>();
    u["expired_at"] = row["expired_at"].as<long long>();
    u["frozen"] = row["frozen"].as<int>();
    u["online"] = row["online"].as<int>();
    u["ip_address"] = row["ip_address"].isNull() ? "" : row["ip_address"].as<std::string>();
    u["remark"] = row["remark"].isNull() ? "" : row["remark"].as<std::string>();
    u["extra"] = row["extra"].isNull() ? "" : row["extra"].as<std::string>();
    u["agent_id"] = row["agent_id"].isNull() ? "" : row["agent_id"].as<std::string>();

    // 解析充值历史
    Json::Value recharges(Json::arrayValue);
    {
        const auto s = row["recharges"].isNull() ? std::string("[]") : row["recharges"].as<std::string>();
        Json::CharReaderBuilder b; std::string errs; Json::Value tmp;
        std::unique_ptr<Json::CharReader> rd(b.newCharReader());
        if (rd->parse(s.data(), s.data() + s.size(), &tmp, &errs) && tmp.isArray()) recharges = tmp;
    }
    // 给每条充值记录解析出卡种名（type_id -> name，带缓存避免重复查询）
    std::unordered_map<std::string, std::string> typeCache;
    for (Json::ArrayIndex i = 0; i < recharges.size(); ++i) {
        const std::string tid = recharges[i].get("type_id", "").asString();
        if (tid.empty()) { recharges[i]["type_name"] = ""; continue; }
        auto it = typeCache.find(tid);
        if (it == typeCache.end()) {
            auto tn = co_await db->execSqlCoro("SELECT name FROM card_types WHERE id=$1 LIMIT 1;", tid);
            it = typeCache.emplace(tid, tn.empty() ? "" : tn[0]["name"].as<std::string>()).first;
        }
        recharges[i]["type_name"] = it->second;
    }
    u["recharges"] = recharges;
    // 当前卡种 = 最新一条充值/激活记录的卡种
    u["card_type"] = recharges.empty() ? Json::Value("") : recharges[recharges.size() - 1].get("type_name", "");

    // 友好名：所属代理
    if (!u["agent_id"].asString().empty()) {
        auto ar = co_await db->execSqlCoro("SELECT username FROM agents WHERE id=$1 LIMIT 1;", u["agent_id"].asString());
        u["agent_name"] = ar.empty() ? "" : ar[0]["username"].as<std::string>();
    } else u["agent_name"] = "";

    Json::Value v; v["ok"] = true; v["user"] = u;
    cb(jsonResp(v));
    co_return;
}

// 校验 app 访问权 + 目标在子树内，返回 (ok, targetAgent)。
static drogon::Task<std::optional<std::string>>
resolveTargetAgent(const std::string& uid, const std::string& appId,
                   const std::optional<std::string>& wantAgent) {
    if (!co_await perm::canAccessApp(uid, appId)) co_return std::nullopt;
    if (!wantAgent) co_return uid;
    auto allowed = co_await perm::subAgentIds(uid);
    if (std::find(allowed.begin(), allowed.end(), *wantAgent) == allowed.end())
        co_return std::nullopt;   // 越权访问别的代理
    co_return *wantAgent;
}

drogon::Task<> UserController::list(HttpRequestPtr req,
                                    std::function<void(const HttpResponsePtr&)> cb) {
    const auto uid = req->getAttributes()->get<std::string>("uid");
    const std::string appId = param(req, "app_id");
    if (appId.empty()) { cb(err("app_id required", k400BadRequest)); co_return; }

    auto target = co_await resolveTargetAgent(uid, appId, optParam(req, "agent_id"));
    if (!target) { cb(err("forbidden", k403Forbidden)); co_return; }

    const bool selfOnly = param(req, "selfOnly") == "1";

    // ---- 关键词模糊搜索：field 指定单字段（列名白名单化），或 all/空 = 跨全部文本字段 ----
    const std::string field = param(req, "field");
    std::string keyword = param(req, "keyword");
    if (!keyword.empty() && keyword.find('%') == std::string::npos) keyword = "%" + keyword + "%";
    std::string kwClause;
    if (field.empty() || field == "all") {
        kwClause = " AND (NULLIF($3,'') IS NULL OR username LIKE $3 OR machine_code LIKE $3 "
                   "OR ip_address LIKE $3 OR remark LIKE $3 OR extra LIKE $3 OR recharges::text LIKE $3)";
    } else if (field == "code") {
        // 按卡密反查用户：卡号记录在 recharges 数组里（每条含 code/card_id）。
        kwClause = " AND (NULLIF($3,'') IS NULL OR recharges::text LIKE $3)";
    } else {
        const std::string col = sql::isUserSearchField(field) ? field : "username";
        kwClause = " AND (NULLIF($3,'') IS NULL OR " + col + " LIKE $3)";
    }

    // 卡种 = 最新一次充值/激活卡的卡种（recharges 数组最后一条的 type_id）
    const std::string latestType =
        "(recharges -> GREATEST(jsonb_array_length(recharges)-1, 0) ->> 'type_id')";

    // 分页
    int page = std::max(1, std::atoi(param(req, "page", "1").c_str()));
    int pageSize = std::clamp(std::atoi(param(req, "pageSize", "20").c_str()), 1, 50);
    const int offset = (page - 1) * pageSize;

    const std::string now = std::to_string(static_cast<long long>(time(nullptr)));
    const std::string scope = selfOnly ? "agent_id=$2" : "agent_id IN (SELECT id FROM sub)";

    // 所属人过滤：ownerSub=1 按"该所属人的子树"(本人+所有下级)；否则精确匹配本人。
    const bool ownerSub = param(req, "ownerSub") == "1";
    std::string withClause;
    {
        std::vector<std::string> ctes;
        if (!selfOnly)
            ctes.push_back("sub AS (SELECT id FROM agents WHERE id=$2 "
                           "UNION ALL SELECT a.id FROM agents a JOIN sub ON a.parent_id=sub.id)");
        if (ownerSub)
            ctes.push_back("osub AS (SELECT id FROM agents WHERE id=$13 "
                           "UNION ALL SELECT a.id FROM agents a JOIN osub ON a.parent_id=osub.id)");
        if (!ctes.empty()) {
            withClause = "WITH RECURSIVE ";
            for (size_t i = 0; i < ctes.size(); ++i) { if (i) withClause += ", "; withClause += ctes[i]; }
            withClause += " ";
        }
    }
    const std::string ownerClause = ownerSub
        ? " AND (NULLIF($13,'') IS NULL OR agent_id IN (SELECT id FROM osub))"
        : " AND (NULLIF($13,'') IS NULL OR agent_id = $13)";

    const std::string filters = kwClause +
        " AND (NULLIF($4,'')  IS NULL OR frozen = NULLIF($4,'')::int)"
        " AND (NULLIF($5,'')  IS NULL OR online = NULLIF($5,'')::int)"
        " AND (NULLIF($6,'')  IS NULL OR ($6='expired' AND expired_at <= $7::bigint)"
        "                              OR ($6='valid'   AND expired_at >  $7::bigint))"
        " AND (NULLIF($8,'')  IS NULL OR created_at >= NULLIF($8,'')::bigint)"
        " AND (NULLIF($9,'')  IS NULL OR created_at <= NULLIF($9,'')::bigint)"
        " AND (NULLIF($10,'') IS NULL OR expired_at >= NULLIF($10,'')::bigint)"
        " AND (NULLIF($11,'') IS NULL OR expired_at <= NULLIF($11,'')::bigint)"
        " AND (NULLIF($12,'') IS NULL OR " + latestType + " = $12)"
        " AND (NULLIF($14,'') IS NULL OR EXISTS (SELECT 1 FROM jsonb_array_elements(recharges) e "
        "       WHERE (e->>'card_id') IN (SELECT id FROM cards c WHERE c.app_id=$1 AND c.batch_id=$14)))"
        + ownerClause;

    const std::string frozen  = param(req, "frozen");
    const std::string online  = param(req, "online");
    const std::string expMode = param(req, "expireStatus");
    const std::string cStart  = param(req, "createdRangeStart");
    const std::string cEnd    = param(req, "createdRangeEnd");
    const std::string eStart  = param(req, "expireRangeStart");
    const std::string eEnd    = param(req, "expireRangeEnd");
    const std::string typeId  = param(req, "type_id");   // 卡种过滤（最新充值卡的卡种）
    const std::string owner   = param(req, "owner");     // 卡密所属人（users.agent_id 精确匹配，仍受子树限制）
    const std::string batchId = param(req, "batch_id");  // 按批次：用过该制卡批次的卡的用户

    const std::string tail = " ORDER BY created_at DESC LIMIT " +
        std::to_string(pageSize) + " OFFSET " + std::to_string(offset) + ";";

    try {
        auto db = app().getDbClient();
        const std::string base = std::string(" FROM users WHERE app_id=$1 AND ") + scope + filters;

        // 「全选所有匹配」用：只回 id（不分页，封顶 20000），复用同一套筛选条件。
        if (param(req, "idsOnly") == "1") {
            auto idr = co_await db->execSqlCoro(
                withClause + "SELECT id" + base + " ORDER BY created_at DESC LIMIT 20000;",
                appId, target.value(), keyword, frozen, online, expMode, now,
                cStart, cEnd, eStart, eEnd, typeId, owner, batchId);
            Json::Value ids(Json::arrayValue);
            for (const auto& r : idr) ids.append(r["id"].as<long long>());
            Json::Value v; v["ok"] = true; v["ids"] = ids; cb(jsonResp(v)); co_return;
        }

        auto cntRs = co_await db->execSqlCoro(
            withClause + "SELECT COUNT(*) AS cnt" + base + ";",
            appId, target.value(), keyword, frozen, online, expMode, now,
            cStart, cEnd, eStart, eEnd, typeId, owner, batchId);
        const long long total = cntRs[0]["cnt"].as<long long>();

        auto rows = co_await db->execSqlCoro(
            withClause + "SELECT *, " + latestType + " AS latest_type_id" + base + tail,
            appId, target.value(), keyword, frozen, online, expMode, now,
            cStart, cEnd, eStart, eEnd, typeId, owner, batchId);

        // 解析每个用户的"最新卡种"名（type_id -> name，带缓存）
        std::unordered_map<std::string, std::string> typeCache;
        Json::Value items(Json::arrayValue);
        for (const auto& r : rows) {
            Json::Value u;
            u["id"] = r["id"].as<long long>();
            u["username"] = r["username"].as<std::string>();
            u["machine_code"] = r["machine_code"].isNull() ? "" : r["machine_code"].as<std::string>();
            u["rebind_cnt"] = r["rebind_cnt"].as<int>();
            u["created_at"] = r["created_at"].as<long long>();
            u["expired_at"] = r["expired_at"].as<long long>();
            u["frozen"] = r["frozen"].as<int>();
            u["online"] = r["online"].as<int>();
            u["ip_address"] = r["ip_address"].isNull() ? "" : r["ip_address"].as<std::string>();
            u["agent_id"] = r["agent_id"].isNull() ? "" : r["agent_id"].as<std::string>();
            u["remark"] = r["remark"].isNull() ? "" : r["remark"].as<std::string>();
            u["extra"] = r["extra"].isNull() ? "" : r["extra"].as<std::string>();

            const std::string tid = r["latest_type_id"].isNull() ? "" : r["latest_type_id"].as<std::string>();
            std::string tname;
            if (!tid.empty()) {
                auto it = typeCache.find(tid);
                if (it == typeCache.end()) {
                    auto tn = co_await db->execSqlCoro("SELECT name FROM card_types WHERE id=$1 LIMIT 1;", tid);
                    it = typeCache.emplace(tid, tn.empty() ? "" : tn[0]["name"].as<std::string>()).first;
                }
                tname = it->second;
            }
            u["card_type"] = tname;
            items.append(u);
        }

        Json::Value v;
        v["ok"] = true;
        v["total"] = static_cast<Json::Int64>(total);
        v["page"] = page;
        v["pageSize"] = pageSize;
        v["items"] = items;
        cb(jsonResp(v));
    } catch (const std::exception& e) {
        LOG_ERROR << "users.list: " << e.what();
        cb(err("server error", k500InternalServerError));
    }
    co_return;
}

// 单条操作的通用：校验权限键 + app 访问，并把"目标必须在子树内"放进 SQL。
static drogon::Task<bool> guard(const std::string& uid, const std::string& appId,
                                const std::string& permKey) {
    if (!co_await perm::canAccessApp(uid, appId)) co_return false;
    if (!co_await perm::hasPermission(uid, permKey)) co_return false;
    co_return true;
}

drogon::Task<> UserController::freeze(HttpRequestPtr req,
                                      std::function<void(const HttpResponsePtr&)> cb) {
    const auto uid = req->getAttributes()->get<std::string>("uid");
    auto j = req->getJsonObject();
    const std::string appId = j ? (*j).get("app_id", "").asString() : "";
    const long long id = j ? (*j).get("id", 0).asInt64() : 0;
    const int frozen = j ? (*j).get("frozen", 0).asInt() : 0;
    const bool freezeCard = j ? (*j).get("freeze_card", false).asBool() : false;
    if (appId.empty() || id == 0) { cb(err("app_id/id required", k400BadRequest)); co_return; }
    if (!co_await guard(uid, appId, "freezeOp")) { cb(err("forbidden", k403Forbidden)); co_return; }

    auto db = app().getDbClient();
    auto r = co_await db->execSqlCoro(
        std::string(kSubCte) +
        "UPDATE users SET frozen=$3 WHERE app_id=$1 AND id=$4 AND agent_id IN (SELECT id FROM sub);",
        appId, uid, frozen, id);

    // 联动：同时冻结/解冻该用户用过的卡密（取其 recharges 里的 card_id，仍限本人子树）
    if (freezeCard) {
        co_await db->execSqlCoro(
            std::string(kSubCte) +
            "UPDATE cards SET frozen=$3 WHERE app_id=$1 AND maker_id IN (SELECT id FROM sub) "
            "AND id IN (SELECT je ->> 'card_id' FROM users u, jsonb_array_elements(u.recharges) je "
            "           WHERE u.app_id=$1 AND u.id=$4);",
            appId, uid, frozen, id);
    }
    cb(r.affectedRows() ? okResp() : err("not found or forbidden", k404NotFound));
    co_return;
}

drogon::Task<> UserController::extend(HttpRequestPtr req,
                                      std::function<void(const HttpResponsePtr&)> cb) {
    const auto uid = req->getAttributes()->get<std::string>("uid");
    auto j = req->getJsonObject();
    const std::string appId = j ? (*j).get("app_id", "").asString() : "";
    const long long id = j ? (*j).get("id", 0).asInt64() : 0;
    const long long hours = j ? (*j).get("hours", 0).asInt64() : 0;
    if (appId.empty() || id == 0) { cb(err("app_id/id required", k400BadRequest)); co_return; }
    if (!co_await guard(uid, appId, "extenduser")) { cb(err("forbidden", k403Forbidden)); co_return; }

    auto db = app().getDbClient();
    auto r = co_await db->execSqlCoro(
        std::string(kSubCte) +
        "UPDATE users SET expired_at = expired_at + $3 WHERE app_id=$1 AND id=$4 "
        "AND agent_id IN (SELECT id FROM sub);",
        appId, uid, hours * 3600, id);
    cb(r.affectedRows() ? okResp() : err("not found or forbidden", k404NotFound));
    co_return;
}

drogon::Task<> UserController::unbind(HttpRequestPtr req,
                                      std::function<void(const HttpResponsePtr&)> cb) {
    const auto uid = req->getAttributes()->get<std::string>("uid");
    auto j = req->getJsonObject();
    const std::string appId = j ? (*j).get("app_id", "").asString() : "";
    const long long id = j ? (*j).get("id", 0).asInt64() : 0;
    if (appId.empty() || id == 0) { cb(err("app_id/id required", k400BadRequest)); co_return; }
    if (!co_await guard(uid, appId, "unbindOp")) { cb(err("forbidden", k403Forbidden)); co_return; }

    auto db = app().getDbClient();
    auto r = co_await db->execSqlCoro(
        std::string(kSubCte) +
        "UPDATE users SET machine_code=NULL, ip_address=NULL WHERE app_id=$1 AND id=$3 "
        "AND agent_id IN (SELECT id FROM sub);",
        appId, uid, id);
    cb(r.affectedRows() ? okResp() : err("not found or forbidden", k404NotFound));
    co_return;
}

drogon::Task<> UserController::remove(HttpRequestPtr req,
                                      std::function<void(const HttpResponsePtr&)> cb) {
    const auto uid = req->getAttributes()->get<std::string>("uid");
    auto j = req->getJsonObject();
    const std::string appId = j ? (*j).get("app_id", "").asString() : "";
    const long long id = j ? (*j).get("id", 0).asInt64() : 0;
    if (appId.empty() || id == 0) { cb(err("app_id/id required", k400BadRequest)); co_return; }
    if (!co_await guard(uid, appId, "delCardAndUser")) { cb(err("forbidden", k403Forbidden)); co_return; }

    auto db = app().getDbClient();
    auto r = co_await db->execSqlCoro(
        std::string(kSubCte) +
        "DELETE FROM users WHERE app_id=$1 AND id=$3 AND agent_id IN (SELECT id FROM sub);",
        appId, uid, id);
    cb(r.affectedRows() ? okResp() : err("not found or forbidden", k404NotFound));
    co_return;
}

// 批处理用户：对一组 id 执行 加时/冻结/解冻/解绑/删除。每种动作各自校验权限键，
// 目标一律限定在本人子树内（SQL 内强制），单次最多 5000 个。
drogon::Task<> UserController::batchOp(HttpRequestPtr req,
                                       std::function<void(const HttpResponsePtr&)> cb) {
    const auto uid = req->getAttributes()->get<std::string>("uid");
    auto j = req->getJsonObject();
    const std::string appId  = j ? (*j).get("app_id", "").asString() : "";
    const std::string action = j ? (*j).get("action", "").asString() : "";
    const long long hours    = j ? (*j).get("hours", 0).asInt64() : 0;
    if (appId.empty() || action.empty()) { cb(err("app_id/action required", k400BadRequest)); co_return; }

    // 动作 -> 所需权限键
    std::string permKey;
    if (action == "extend") permKey = "extenduser";
    else if (action == "freeze" || action == "unfreeze") permKey = "freezeOp";
    else if (action == "unbind") permKey = "unbindOp";
    else if (action == "delete") permKey = "delCardAndUser";
    else { cb(err("invalid action", k400BadRequest)); co_return; }

    if (!co_await guard(uid, appId, permKey)) { cb(err("forbidden", k403Forbidden)); co_return; }

    std::vector<long long> ids;
    if (j && (*j).isMember("ids") && (*j)["ids"].isArray())
        for (const auto& v : (*j)["ids"]) ids.push_back(v.asInt64());
    if (ids.empty()) { cb(err("ids required", k400BadRequest)); co_return; }
    if (ids.size() > 20000) { cb(err("too many ids (max 20000)", k400BadRequest)); co_return; }
    const std::string arr = sql::pgBigintArray(ids);

    try {
        auto db = app().getDbClient();
        std::string sqlBody;
        if (action == "extend")
            sqlBody = "UPDATE users SET expired_at = expired_at + $3 "
                      "WHERE app_id=$1 AND id = ANY($4::bigint[]) AND agent_id IN (SELECT id FROM sub);";
        else if (action == "freeze")
            sqlBody = "UPDATE users SET frozen=1 "
                      "WHERE app_id=$1 AND id = ANY($4::bigint[]) AND agent_id IN (SELECT id FROM sub);";
        else if (action == "unfreeze")
            sqlBody = "UPDATE users SET frozen=0 "
                      "WHERE app_id=$1 AND id = ANY($4::bigint[]) AND agent_id IN (SELECT id FROM sub);";
        else if (action == "unbind")
            sqlBody = "UPDATE users SET machine_code=NULL, ip_address=NULL "
                      "WHERE app_id=$1 AND id = ANY($4::bigint[]) AND agent_id IN (SELECT id FROM sub);";
        else /* delete */
            sqlBody = "DELETE FROM users "
                      "WHERE app_id=$1 AND id = ANY($4::bigint[]) AND agent_id IN (SELECT id FROM sub);";

        // $3 仅 extend 用到秒数；其它动作 $3 不出现，但占位绑定无害。
        auto r = co_await db->execSqlCoro(std::string(kSubCte) + sqlBody,
                                          appId, uid, hours * 3600, arr);
        Json::Value v; v["ok"] = true; v["affected"] = static_cast<Json::Int64>(r.affectedRows());
        cb(jsonResp(v));
    } catch (const std::exception& e) {
        LOG_ERROR << "users.batchOp: " << e.what();
        cb(err("server error", k500InternalServerError));
    }
    co_return;
}

} // namespace aegis
