#include "http/CardController.h"
#include "http/AuthFilter.h"
#include "http/Resp.h"
#include "http/Sql.h"
#include "http/Permissions.h"
#include "security/Crypto.h"

#include <drogon/drogon.h>
#include <algorithm>
#include <cstdio>
#include <ctime>
#include <unordered_map>

using namespace drogon;
using namespace aegis::http;

namespace aegis {

// 制卡人子树 CTE（$2=uid 为根），把"只能看/操作自己造的卡"放进 SQL 强制执行。
static constexpr const char* kSubCte =
    "WITH RECURSIVE sub AS ("
    "  SELECT id FROM agents WHERE id=$2 "
    "  UNION ALL SELECT a.id FROM agents a JOIN sub ON a.parent_id=sub.id) ";

static drogon::Task<bool> guard(const std::string& uid, const std::string& appId,
                                const std::string& permKey) {
    if (!co_await perm::canAccessApp(uid, appId)) co_return false;
    if (!co_await perm::hasPermission(uid, permKey)) co_return false;
    co_return true;
}

static std::string randCode(const std::string& prefix) {
    std::string raw = Crypto::generateRandomKey(16), hex;   // 16 字节 → 32 位 hex（128 位熵，绝不重复）
    CryptoPP::StringSource(raw, true,
        new CryptoPP::HexEncoder(new CryptoPP::StringSink(hex), true /*uppercase*/));
    return prefix + hex;
}

// ============================ 卡种 ============================

drogon::Task<> CardController::listTypes(HttpRequestPtr req,
                                         std::function<void(const HttpResponsePtr&)> cb) {
    const auto uid = req->getAttributes()->get<std::string>("uid");
    const std::string appId = param(req, "app_id");
    if (appId.empty()) { cb(err("app_id required", k400BadRequest)); co_return; }
    if (!co_await perm::canAccessApp(uid, appId)) { cb(err("forbidden", k403Forbidden)); co_return; }

    std::optional<std::string> q = optParam(req, "q");
    if (q) q = "%" + *q + "%";

    auto db = app().getDbClient();
    auto rows = co_await db->execSqlCoro(
        "SELECT id, name, hours, prefix, created_at FROM card_types "
        "WHERE app_id=$1 AND ($2::text IS NULL OR name LIKE $2 OR prefix LIKE $2) "
        "ORDER BY created_at DESC;", appId, q);

    Json::Value items(Json::arrayValue);
    for (const auto& r : rows) {
        Json::Value t;
        t["id"] = r["id"].as<std::string>();
        t["name"] = r["name"].as<std::string>();
        t["hours"] = r["hours"].as<int>();
        t["prefix"] = r["prefix"].as<std::string>();
        t["created_at"] = r["created_at"].as<long long>();
        items.append(t);
    }
    Json::Value v; v["ok"] = true; v["items"] = items;
    cb(jsonResp(v));
    co_return;
}

drogon::Task<> CardController::createType(HttpRequestPtr req,
                                          std::function<void(const HttpResponsePtr&)> cb) {
    const auto uid = req->getAttributes()->get<std::string>("uid");
    auto j = req->getJsonObject();
    const std::string appId  = j ? (*j).get("app_id", "").asString() : "";
    const std::string name   = j ? (*j).get("name", "").asString() : "";
    const int hours          = j ? (*j).get("hours", 0).asInt() : 0;
    const std::string prefix = j ? (*j).get("prefix", "").asString() : "";
    if (appId.empty() || name.empty() || prefix.empty()) {
        cb(err("app_id/name/prefix required", k400BadRequest)); co_return;
    }
    if (prefix.size() > 4) {   // 卡密前缀最大 4 位
        cb(err("prefix too long (max 4)", k400BadRequest)); co_return;
    }
    // 卡种属于"软件配置"，仅软件属主/超管可管理
    if (!co_await perm::canManageApp(uid, appId)) { cb(err("forbidden: app owner only", k403Forbidden)); co_return; }

    const std::string id = drogon::utils::getUuid();
    const long long now = static_cast<long long>(time(nullptr));
    auto db = app().getDbClient();
    try {
        co_await db->execSqlCoro(
            "INSERT INTO card_types(id, app_id, name, hours, prefix, created_at) "
            "VALUES($1,$2,$3,$4,$5,$6);", id, appId, name, hours, prefix, now);
    } catch (const orm::DrogonDbException&) {
        cb(err("prefix already exists in this app", k409Conflict)); co_return;
    }
    Json::Value v; v["ok"] = true; v["id"] = id;
    cb(jsonResp(v));
    co_return;
}

drogon::Task<> CardController::deleteType(HttpRequestPtr req,
                                          std::function<void(const HttpResponsePtr&)> cb) {
    const auto uid = req->getAttributes()->get<std::string>("uid");
    auto j = req->getJsonObject();
    const std::string appId = j ? (*j).get("app_id", "").asString() : "";
    const std::string id    = j ? (*j).get("id", "").asString() : "";
    const std::string secPw = j ? (*j).get("security_password", "").asString() : "";
    if (appId.empty() || id.empty()) { cb(err("app_id/id required", k400BadRequest)); co_return; }
    if (!co_await perm::canManageApp(uid, appId)) { cb(err("forbidden: app owner only", k403Forbidden)); co_return; }
    // 删卡种会连带清掉该卡种下所有卡密，属破坏性操作，需安全密码确认
    if (!co_await perm::checkSecurityPassword(uid, secPw)) {
        cb(err("security password incorrect or not set", k403Forbidden)); co_return;
    }

    auto db = app().getDbClient();
    long long removedCards = 0;
    try {
        auto trx = co_await db->newTransactionCoro();
        auto rc = co_await trx->execSqlCoro(
            "DELETE FROM cards WHERE app_id=$1 AND type_id=$2;", appId, id);
        removedCards = rc.affectedRows();
        co_await trx->execSqlCoro(
            "DELETE FROM card_batches WHERE app_id=$1 AND type_id=$2;", appId, id);
        co_await trx->execSqlCoro(
            "DELETE FROM card_types WHERE app_id=$1 AND id=$2;", appId, id);
    } catch (const std::exception& e) {
        LOG_ERROR << "deleteType: " << e.what();
        cb(err("delete failed", k500InternalServerError)); co_return;
    }
    Json::Value v; v["ok"] = true; v["removed_cards"] = static_cast<Json::Int64>(removedCards);
    cb(jsonResp(v));
    co_return;
}

// ============================ 制卡 ============================

drogon::Task<> CardController::make(HttpRequestPtr req,
                                    std::function<void(const HttpResponsePtr&)> cb) {
    const auto uid = req->getAttributes()->get<std::string>("uid");
    auto j = req->getJsonObject();
    const std::string appId  = j ? (*j).get("app_id", "").asString() : "";
    const std::string typeId = j ? (*j).get("type_id", "").asString() : "";
    int count                = j ? (*j).get("count", 0).asInt() : 0;
    const std::string note   = j ? (*j).get("note", "").asString() : "";
    const std::string remark = j ? (*j).get("remark", "").asString() : "";
    if (appId.empty() || typeId.empty()) { cb(err("app_id/type_id required", k400BadRequest)); co_return; }
    if (!co_await guard(uid, appId, "cardmaking")) { cb(err("forbidden", k403Forbidden)); co_return; }
    count = std::clamp(count, 1, 2000);

    auto db = app().getDbClient();
    auto tr = co_await db->execSqlCoro(
        "SELECT hours, prefix FROM card_types WHERE app_id=$1 AND id=$2 LIMIT 1;", appId, typeId);
    if (tr.empty()) { cb(err("card type not found", k404NotFound)); co_return; }

    // 卡种授权 + 定价：非超管必须被授权该卡种且有价格；价格用于余额占用。
    double unitPrice = 0;
    auto agr = co_await db->execSqlCoro(
        "SELECT level, card_types FROM agents WHERE id=$1 LIMIT 1;", uid);
    const bool maker_super = !agr.empty() && agr[0]["level"].as<int>() == 1;
    if (!agr.empty() && !maker_super) {
        Json::Value ct;
        Json::CharReaderBuilder b; std::string errs;
        const auto s = agr[0]["card_types"].as<std::string>();
        std::unique_ptr<Json::CharReader> rd(b.newCharReader());
        rd->parse(s.data(), s.data() + s.size(), &ct, &errs);
        bool ok = false;
        if (ct.isObject() && ct.isMember(appId)) {
            const Json::Value& ca = ct[appId];
            if (ca.isObject() && ca.isMember(typeId)) { unitPrice = ca[typeId].asDouble(); ok = true; }
            else if (ca.isArray()) { for (const auto& t : ca) if (t.asString() == typeId) { ok = true; break; } }
        }
        if (!ok) { cb(err("你无权制作该卡种（未授权或未设置注册卡价格）", k403Forbidden)); co_return; }

        // 余额检查：制卡占用 = 单价 × 数量，不得超过可用余额。
        const auto bi = co_await perm::balanceInfo(uid);
        const double need = unitPrice * count;
        if (need > bi.available() + 1e-9) {
            char buf[160];
            std::snprintf(buf, sizeof(buf), "余额不足：本次需占用 %.2f，当前可用 %.2f", need, bi.available());
            cb(err(buf, k400BadRequest)); co_return;
        }
    }
    const int hours = tr[0]["hours"].as<int>();
    const std::string prefix = tr[0]["prefix"].as<std::string>();

    auto ar = co_await db->execSqlCoro("SELECT username FROM agents WHERE id=$1 LIMIT 1;", uid);
    const std::string makerName = ar.empty() ? uid : ar[0]["username"].as<std::string>();
    const std::string makerIp = req->getPeerAddr().toIp();

    const std::string batchId = drogon::utils::getUuid();
    const long long now = static_cast<long long>(time(nullptr));

    try {
        auto trx = co_await db->newTransactionCoro();
        co_await trx->execSqlCoro(
            "INSERT INTO card_batches(id, app_id, type_id, count, note, maker_id, maker_name, maker_ip, created_at) "
            "VALUES($1,$2,$3,$4,$5,$6,$7,$8,$9);",
            batchId, appId, typeId, count, note, uid, makerName, makerIp, now);

        Json::Value codes(Json::arrayValue);
        for (int i = 0; i < count; ++i) {
            const std::string code = randCode(prefix);
            const std::string id = drogon::utils::getUuid();
            co_await trx->execSqlCoro(
                "INSERT INTO cards(id, app_id, type_id, code, status, remark, frozen, created_at, "
                "hours, maker_id, maker_name, batch_id, price) "
                "VALUES($1,$2,$3,$4,'unused',$5,0,$6,$7,$8,$9,$10,$11);",
                id, appId, typeId, code, remark, now, hours, uid, makerName, batchId, unitPrice);
            codes.append(code);
        }
        Json::Value v; v["ok"] = true; v["batch_id"] = batchId; v["count"] = count; v["codes"] = codes;
        cb(jsonResp(v));
    } catch (const std::exception& e) {
        LOG_ERROR << "cards.make: " << e.what();
        cb(err("make failed (retry)", k500InternalServerError));
    }
    co_return;
}

// ============================ 卡密列表/操作 ============================

drogon::Task<> CardController::list(HttpRequestPtr req,
                                    std::function<void(const HttpResponsePtr&)> cb) {
    const auto uid = req->getAttributes()->get<std::string>("uid");
    const std::string appId = param(req, "app_id");
    if (appId.empty()) { cb(err("app_id required", k400BadRequest)); co_return; }
    if (!co_await perm::canAccessApp(uid, appId)) { cb(err("forbidden", k403Forbidden)); co_return; }

    // 关键词模糊搜索：field 指定单字段(白名单)，或 all/空 = 跨全部文本字段
    const std::string field = param(req, "field");
    std::string keyword = param(req, "keyword");
    if (!keyword.empty() && keyword.find('%') == std::string::npos) keyword = "%" + keyword + "%";
    std::string kwClause;
    if (field.empty() || field == "all") {
        kwClause = " AND (NULLIF($3,'') IS NULL OR code LIKE $3 OR remark LIKE $3 "
                   "OR batch_id LIKE $3 OR maker_name LIKE $3)";
    } else {
        const std::string col = sql::isCardSearchField(field) ? field : "code";
        kwClause = " AND (NULLIF($3,'') IS NULL OR " + col + " LIKE $3)";
    }

    int page = std::max(1, std::atoi(param(req, "page", "1").c_str()));
    int pageSize = std::clamp(std::atoi(param(req, "pageSize", "20").c_str()), 1, 50);
    const int offset = (page - 1) * pageSize;

    const std::string status  = param(req, "status");
    const std::string frozen  = param(req, "frozen");
    const std::string typeId  = param(req, "type_id");
    const std::string batchId = param(req, "batch_id");
    const std::string cStart  = param(req, "createdRangeStart");
    const std::string cEnd    = param(req, "createdRangeEnd");
    const std::string uStart  = param(req, "usedRangeStart");
    const std::string uEnd    = param(req, "usedRangeEnd");
    const std::string owner   = param(req, "owner");     // 制卡人(maker_id)，仍受子树限制

    // 制卡人过滤：ownerSub=1 按"该制卡人的子树"(本人+下级)，否则精确匹配本人
    const bool ownerSub = param(req, "ownerSub") == "1";
    std::string withClause = "WITH RECURSIVE sub AS (SELECT id FROM agents WHERE id=$2 "
                             "UNION ALL SELECT a.id FROM agents a JOIN sub ON a.parent_id=sub.id)";
    if (ownerSub)
        withClause += ", osub AS (SELECT id FROM agents WHERE id=$10 "
                      "UNION ALL SELECT a.id FROM agents a JOIN osub ON a.parent_id=osub.id)";
    withClause += " ";
    const std::string ownerClause = ownerSub
        ? " AND (NULLIF($10,'') IS NULL OR maker_id IN (SELECT id FROM osub))"
        : " AND (NULLIF($10,'') IS NULL OR maker_id = $10)";

    const std::string base =
        std::string(" FROM cards WHERE app_id=$1 AND maker_id IN (SELECT id FROM sub)") + kwClause +
        " AND (NULLIF($4,'') IS NULL OR status=$4)"
        " AND (NULLIF($5,'') IS NULL OR frozen=NULLIF($5,'')::int)"
        " AND (NULLIF($6,'') IS NULL OR type_id=$6)"
        " AND (NULLIF($7,'') IS NULL OR batch_id=$7)"
        " AND (NULLIF($8,'') IS NULL OR created_at>=NULLIF($8,'')::bigint)"
        " AND (NULLIF($9,'') IS NULL OR created_at<=NULLIF($9,'')::bigint)" + ownerClause +
        " AND (NULLIF($11,'') IS NULL OR used_at>=NULLIF($11,'')::bigint)"
        " AND (NULLIF($12,'') IS NULL OR used_at<=NULLIF($12,'')::bigint)";

    // LIMIT/OFFSET 内联（已是校验过的整数，无注入风险）
    const std::string tail = " ORDER BY created_at DESC LIMIT " +
        std::to_string(pageSize) + " OFFSET " + std::to_string(offset) + ";";

    try {
        auto db = app().getDbClient();

        // 「全选所有匹配」用：只回 id（不分页，封顶 20000），复用同一套筛选条件。
        if (param(req, "idsOnly") == "1") {
            auto idr = co_await db->execSqlCoro(
                withClause + "SELECT id" + base + " ORDER BY created_at DESC LIMIT 20000;",
                appId, uid, keyword, status, frozen, typeId, batchId, cStart, cEnd, owner, uStart, uEnd);
            Json::Value ids(Json::arrayValue);
            for (const auto& r : idr) ids.append(r["id"].as<std::string>());
            Json::Value v; v["ok"] = true; v["ids"] = ids; cb(jsonResp(v)); co_return;
        }

        auto cntRs = co_await db->execSqlCoro(
            withClause + "SELECT COUNT(*) AS cnt" + base + ";",
            appId, uid, keyword, status, frozen, typeId, batchId, cStart, cEnd, owner, uStart, uEnd);
        const long long total = cntRs[0]["cnt"].as<long long>();

        auto rows = co_await db->execSqlCoro(
            withClause + "SELECT *" + base + tail,
            appId, uid, keyword, status, frozen, typeId, batchId, cStart, cEnd, owner, uStart, uEnd);

        std::unordered_map<std::string, std::string> typeCache;
        Json::Value items(Json::arrayValue);
        for (const auto& r : rows) {
            Json::Value c;
            c["id"] = r["id"].as<std::string>();
            c["code"] = r["code"].as<std::string>();
            c["type_id"] = r["type_id"].as<std::string>();
            c["status"] = r["status"].as<std::string>();
            c["frozen"] = r["frozen"].as<int>();
            c["remark"] = r["remark"].isNull() ? "" : r["remark"].as<std::string>();
            c["hours"] = r["hours"].isNull() ? 0 : r["hours"].as<int>();
            c["created_at"] = r["created_at"].as<long long>();
            c["used_at"] = r["used_at"].isNull() ? 0 : r["used_at"].as<long long>();
            c["used_by"] = r["used_by"].isNull() ? "" : r["used_by"].as<std::string>();
            c["expire_at"] = r["expire_at"].isNull() ? 0 : r["expire_at"].as<long long>();
            c["maker_name"] = r["maker_name"].isNull() ? "" : r["maker_name"].as<std::string>();
            c["batch_id"] = r["batch_id"].isNull() ? "" : r["batch_id"].as<std::string>();

            const std::string tid = r["type_id"].isNull() ? "" : r["type_id"].as<std::string>();
            std::string tname;
            if (!tid.empty()) {
                auto it = typeCache.find(tid);
                if (it == typeCache.end()) {
                    auto tn = co_await db->execSqlCoro("SELECT name FROM card_types WHERE id=$1 LIMIT 1;", tid);
                    it = typeCache.emplace(tid, tn.empty() ? "" : tn[0]["name"].as<std::string>()).first;
                }
                tname = it->second;
            }
            c["card_type"] = tname;
            items.append(c);
        }
        Json::Value v;
        v["ok"] = true; v["total"] = static_cast<Json::Int64>(total);
        v["page"] = page; v["pageSize"] = pageSize; v["items"] = items;
        cb(jsonResp(v));
    } catch (const std::exception& e) {
        LOG_ERROR << "cards.list: " << e.what();
        cb(err("server error", k500InternalServerError));
    }
    co_return;
}

// 批量搜索卡密：粘贴一批卡号(每行一个)，精确匹配返回全部命中（仍限本人子树）。
drogon::Task<> CardController::batch(HttpRequestPtr req,
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

    const std::string base =
        " FROM cards WHERE app_id=$1 AND maker_id IN (SELECT id FROM sub) AND code = ANY($3::text[])";
    const std::string tail = " ORDER BY created_at DESC LIMIT " +
        std::to_string(pageSize) + " OFFSET " + std::to_string(offset) + ";";

    try {
        auto db = app().getDbClient();

        // 「全选所有匹配」用：粘贴卡号模式下，只回命中卡密的 id（封顶 20000）。
        if ((*j).get("idsOnly", false).asBool()) {
            auto idr = co_await db->execSqlCoro(
                std::string(kSubCte) + "SELECT id" + base + " LIMIT 20000;", appId, uid, arr);
            Json::Value ids(Json::arrayValue);
            for (const auto& r : idr) ids.append(r["id"].as<std::string>());
            Json::Value v; v["ok"] = true; v["ids"] = ids; cb(jsonResp(v)); co_return;
        }

        auto cntRs = co_await db->execSqlCoro(
            std::string(kSubCte) + "SELECT COUNT(*) AS cnt" + base + ";", appId, uid, arr);
        const long long total = cntRs[0]["cnt"].as<long long>();
        auto rows = co_await db->execSqlCoro(
            std::string(kSubCte) + "SELECT *" + base + tail, appId, uid, arr);

        std::unordered_map<std::string, std::string> typeCache;
        Json::Value items(Json::arrayValue);
        for (const auto& r : rows) {
            Json::Value c;
            c["id"] = r["id"].as<std::string>();
            c["code"] = r["code"].as<std::string>();
            c["status"] = r["status"].as<std::string>();
            c["frozen"] = r["frozen"].as<int>();
            c["remark"] = r["remark"].isNull() ? "" : r["remark"].as<std::string>();
            c["created_at"] = r["created_at"].as<long long>();
            c["used_at"] = r["used_at"].isNull() ? 0 : r["used_at"].as<long long>();
            c["maker_name"] = r["maker_name"].isNull() ? "" : r["maker_name"].as<std::string>();
            c["batch_id"] = r["batch_id"].isNull() ? "" : r["batch_id"].as<std::string>();
            const std::string tid = r["type_id"].isNull() ? "" : r["type_id"].as<std::string>();
            std::string tname;
            if (!tid.empty()) {
                auto it = typeCache.find(tid);
                if (it == typeCache.end()) {
                    auto tn = co_await db->execSqlCoro("SELECT name FROM card_types WHERE id=$1 LIMIT 1;", tid);
                    it = typeCache.emplace(tid, tn.empty() ? "" : tn[0]["name"].as<std::string>()).first;
                }
                tname = it->second;
            }
            c["card_type"] = tname;
            items.append(c);
        }
        Json::Value v;
        v["ok"] = true; v["total"] = static_cast<Json::Int64>(total);
        v["page"] = page; v["pageSize"] = pageSize; v["items"] = items;
        v["queried"] = static_cast<int>(codes.size());
        cb(jsonResp(v));
    } catch (const std::exception& e) {
        LOG_ERROR << "cards.batch: " << e.what();
        cb(err("server error", k500InternalServerError));
    }
    co_return;
}

// 制卡批次列表（含本人子树代理；按 批次/所属人/时间 搜索）。
drogon::Task<> CardController::listBatches(HttpRequestPtr req,
                                          std::function<void(const HttpResponsePtr&)> cb) {
    const auto uid = req->getAttributes()->get<std::string>("uid");
    const std::string appId = param(req, "app_id");
    if (appId.empty()) { cb(err("app_id required", k400BadRequest)); co_return; }
    if (!co_await perm::canAccessApp(uid, appId)) { cb(err("forbidden", k403Forbidden)); co_return; }
    const bool super = co_await perm::isSuper(uid);   // 仅超管可见制卡IP

    std::string kw = param(req, "keyword");   // 批次：id 或 备注 模糊
    if (!kw.empty() && kw.find('%') == std::string::npos) kw = "%" + kw + "%";
    const std::string owner = param(req, "owner");
    const std::string cStart = param(req, "createdRangeStart");
    const std::string cEnd   = param(req, "createdRangeEnd");

    int page = std::max(1, std::atoi(param(req, "page", "1").c_str()));
    int pageSize = std::clamp(std::atoi(param(req, "pageSize", "2").c_str()), 1, 50);  // 默认最近 2 条
    const int offset = (page - 1) * pageSize;

    // 范围：self=仅自己(默认) / all=本人子树全部代理 / agent=指定代理(配合 owner)
    const std::string scope = param(req, "scope", "self");
    const bool ownerSub = param(req, "ownerSub") == "1";
    std::string withClause = "WITH RECURSIVE sub AS (SELECT id FROM agents WHERE id=$2 "
                             "UNION ALL SELECT a.id FROM agents a JOIN sub ON a.parent_id=sub.id)";
    if (ownerSub)
        withClause += ", osub AS (SELECT id FROM agents WHERE id=$4 "
                      "UNION ALL SELECT a.id FROM agents a JOIN osub ON a.parent_id=osub.id)";
    withClause += " ";
    // 指定代理时按 owner 过滤；owner 必须落在本人子树内（下方 makerScope 用 sub 兜底保证不越权）。
    const std::string ownerClause = ownerSub
        ? " AND (NULLIF($4,'') IS NULL OR maker_id IN (SELECT id FROM osub))"
        : " AND (NULLIF($4,'') IS NULL OR maker_id = $4)";

    // 默认仅自己；all/agent 都限定在本人子树内。
    const std::string makerScope = (scope == "all" || scope == "agent")
        ? "maker_id IN (SELECT id FROM sub)"
        : "maker_id = $2";

    const std::string base =
        std::string(" FROM card_batches WHERE app_id=$1 AND ") + makerScope +
        " AND (NULLIF($3,'') IS NULL OR id LIKE $3 OR note LIKE $3)" + ownerClause +
        " AND (NULLIF($5,'') IS NULL OR created_at >= NULLIF($5,'')::bigint)"
        " AND (NULLIF($6,'') IS NULL OR created_at <= NULLIF($6,'')::bigint)";
    const std::string tail = " ORDER BY created_at DESC LIMIT " +
        std::to_string(pageSize) + " OFFSET " + std::to_string(offset) + ";";

    try {
        auto db = app().getDbClient();
        auto cntRs = co_await db->execSqlCoro(
            withClause + "SELECT COUNT(*) AS cnt" + base + ";",
            appId, uid, kw, owner, cStart, cEnd);
        const long long total = cntRs[0]["cnt"].as<long long>();

        auto rows = co_await db->execSqlCoro(
            withClause + "SELECT *, (SELECT name FROM card_types WHERE id=card_batches.type_id) AS type_name"
            + base + tail,
            appId, uid, kw, owner, cStart, cEnd);

        Json::Value items(Json::arrayValue);
        for (const auto& r : rows) {
            Json::Value b;
            b["id"] = r["id"].as<std::string>();
            b["type_name"] = r["type_name"].isNull() ? "" : r["type_name"].as<std::string>();
            b["count"] = r["count"].as<int>();
            b["note"] = r["note"].isNull() ? "" : r["note"].as<std::string>();
            b["maker_name"] = r["maker_name"].isNull() ? "" : r["maker_name"].as<std::string>();
            if (super) b["maker_ip"] = r["maker_ip"].isNull() ? "" : r["maker_ip"].as<std::string>();
            b["created_at"] = r["created_at"].as<long long>();
            items.append(b);
        }
        Json::Value v;
        v["ok"] = true; v["total"] = static_cast<Json::Int64>(total);
        v["page"] = page; v["pageSize"] = pageSize; v["items"] = items;
        cb(jsonResp(v));
    } catch (const std::exception& e) {
        LOG_ERROR << "cards.listBatches: " << e.what();
        cb(err("server error", k500InternalServerError));
    }
    co_return;
}

drogon::Task<> CardController::freeze(HttpRequestPtr req,
                                     std::function<void(const HttpResponsePtr&)> cb) {
    const auto uid = req->getAttributes()->get<std::string>("uid");
    auto j = req->getJsonObject();
    const std::string appId = j ? (*j).get("app_id", "").asString() : "";
    const std::string id    = j ? (*j).get("id", "").asString() : "";
    const int frozen        = j ? (*j).get("frozen", 0).asInt() : 0;
    const bool freezeUser   = j ? (*j).get("freeze_user", false).asBool() : false;
    if (appId.empty() || id.empty()) { cb(err("app_id/id required", k400BadRequest)); co_return; }
    if (!co_await guard(uid, appId, "freezeOp")) { cb(err("forbidden", k403Forbidden)); co_return; }

    auto db = app().getDbClient();
    auto r = co_await db->execSqlCoro(
        std::string(kSubCte) +
        "UPDATE cards SET frozen=$3 WHERE app_id=$1 AND id=$4 AND maker_id IN (SELECT id FROM sub);",
        appId, uid, frozen, id);

    // 联动：同时冻结/解冻用过这张卡的用户（按 recharges 里的 card_id 命中，仍限本人子树）
    if (freezeUser) {
        co_await db->execSqlCoro(
            std::string(kSubCte) +
            "UPDATE users SET frozen=$3 WHERE app_id=$1 AND agent_id IN (SELECT id FROM sub) "
            "AND recharges @> jsonb_build_array(jsonb_build_object('card_id', $4::text));",
            appId, uid, frozen, id);
    }
    cb(r.affectedRows() ? okResp() : err("not found or forbidden", k404NotFound));
    co_return;
}

drogon::Task<> CardController::remove(HttpRequestPtr req,
                                     std::function<void(const HttpResponsePtr&)> cb) {
    const auto uid = req->getAttributes()->get<std::string>("uid");
    auto j = req->getJsonObject();
    const std::string appId = j ? (*j).get("app_id", "").asString() : "";
    const std::string id    = j ? (*j).get("id", "").asString() : "";
    if (appId.empty() || id.empty()) { cb(err("app_id/id required", k400BadRequest)); co_return; }
    if (!co_await perm::canAccessApp(uid, appId)) { cb(err("forbidden", k403Forbidden)); co_return; }

    // 未使用的卡密：任何人（限本人子树）都可删；已使用的卡密：需『删除卡/用户』权限。
    const bool canDelUsed = co_await perm::hasPermission(uid, "delCardAndUser");
    const std::string statusGuard = canDelUsed ? "" : " AND status='unused'";

    auto db = app().getDbClient();
    auto r = co_await db->execSqlCoro(
        std::string(kSubCte) +
        "DELETE FROM cards WHERE app_id=$1 AND id=$3 AND maker_id IN (SELECT id FROM sub)" + statusGuard + ";",
        appId, uid, id);
    if (r.affectedRows()) cb(okResp());
    else if (!canDelUsed) cb(err("已使用的卡密需要『删除卡/用户』权限才能删除", k403Forbidden));
    else cb(err("not found or forbidden", k404NotFound));
    co_return;
}

// 批处理卡密：对一组卡 id 执行 冻结/解冻/删除。删除时可选 delete_used_users：
// 连带删除"用过这些卡"的用户（需 delCardAndUser 权限，删除卡本身已要求该权限）。
// 目标一律限定在本人子树内，单次最多 5000 个。
drogon::Task<> CardController::batchOp(HttpRequestPtr req,
                                      std::function<void(const HttpResponsePtr&)> cb) {
    const auto uid = req->getAttributes()->get<std::string>("uid");
    auto j = req->getJsonObject();
    const std::string appId  = j ? (*j).get("app_id", "").asString() : "";
    const std::string action = j ? (*j).get("action", "").asString() : "";
    const bool delUsers      = j ? (*j).get("delete_used_users", false).asBool() : false;
    if (appId.empty() || action.empty()) { cb(err("app_id/action required", k400BadRequest)); co_return; }

    // 冻结/解冻需 freezeOp；删除：未使用的卡密任何人可删，已使用的需 delCardAndUser。
    if (action == "freeze" || action == "unfreeze") {
        if (!co_await guard(uid, appId, "freezeOp")) { cb(err("forbidden", k403Forbidden)); co_return; }
    } else if (action == "delete") {
        if (!co_await perm::canAccessApp(uid, appId)) { cb(err("forbidden", k403Forbidden)); co_return; }
    } else { cb(err("invalid action", k400BadRequest)); co_return; }

    std::vector<std::string> ids;
    if (j && (*j).isMember("ids") && (*j)["ids"].isArray())
        for (const auto& v : (*j)["ids"]) ids.push_back(v.asString());
    if (ids.empty()) { cb(err("ids required", k400BadRequest)); co_return; }
    if (ids.size() > 20000) { cb(err("too many ids (max 20000)", k400BadRequest)); co_return; }
    const std::string arr = sql::pgTextArray(ids);

    try {
        auto db = app().getDbClient();
        long long affectedUsers = 0;

        if (action == "delete") {
            // 无『删除卡/用户』权限：只删未使用的卡密，且不连带删用户。
            const bool canDelUsed = co_await perm::hasPermission(uid, "delCardAndUser");
            const std::string statusGuard = canDelUsed ? "" : " AND status='unused'";

            // 先按需删用过这些卡的用户（需权限，限本人子树）
            if (delUsers && canDelUsed) {
                auto ur = co_await db->execSqlCoro(
                    std::string(kSubCte) +
                    "DELETE FROM users u WHERE u.app_id=$1 AND u.agent_id IN (SELECT id FROM sub) "
                    "AND EXISTS (SELECT 1 FROM jsonb_array_elements(u.recharges) e "
                    "            WHERE (e->>'card_id') = ANY($3::text[]));",
                    appId, uid, arr);
                affectedUsers = ur.affectedRows();
            }
            auto r = co_await db->execSqlCoro(
                std::string(kSubCte) +
                "DELETE FROM cards WHERE app_id=$1 AND id = ANY($3::text[]) "
                "AND maker_id IN (SELECT id FROM sub)" + statusGuard + ";",
                appId, uid, arr);
            Json::Value v; v["ok"] = true;
            v["affected"] = static_cast<Json::Int64>(r.affectedRows());
            v["affected_users"] = static_cast<Json::Int64>(affectedUsers);
            cb(jsonResp(v));
        } else {
            const int frozen = (action == "freeze") ? 1 : 0;
            auto r = co_await db->execSqlCoro(
                std::string(kSubCte) +
                "UPDATE cards SET frozen=$3 WHERE app_id=$1 AND id = ANY($4::text[]) "
                "AND maker_id IN (SELECT id FROM sub);",
                appId, uid, frozen, arr);
            Json::Value v; v["ok"] = true; v["affected"] = static_cast<Json::Int64>(r.affectedRows());
            cb(jsonResp(v));
        }
    } catch (const std::exception& e) {
        LOG_ERROR << "cards.batchOp: " << e.what();
        cb(err("server error", k500InternalServerError));
    }
    co_return;
}

} // namespace aegis
