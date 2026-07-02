#include "http/AgentController.h"
#include "http/AuthFilter.h"
#include "http/Resp.h"
#include "http/Permissions.h"
#include "http/Session.h"
#include "security/Password.h"

#include <drogon/drogon.h>
#include <ctime>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <string>
#include <algorithm>

using namespace drogon;
using namespace aegis::http;

namespace aegis {

static Json::Value parseArr(const std::string& s) {
    Json::Value out(Json::arrayValue), tmp;
    Json::CharReaderBuilder b; std::string errs;
    std::unique_ptr<Json::CharReader> rd(b.newCharReader());
    if (rd->parse(s.data(), s.data() + s.size(), &tmp, &errs) && tmp.isArray()) out = tmp;
    return out;
}
static std::string dumpArr(const Json::Value& a) {
    Json::StreamWriterBuilder wb; wb["indentation"] = "";
    return Json::writeString(wb, a.isArray() ? a : Json::Value(Json::arrayValue));
}
// 取 a 与白名单 allow 的交集（child 只能拿到 parent 拥有的）。
static Json::Value intersect(const Json::Value& a, const std::unordered_set<std::string>& allow) {
    Json::Value out(Json::arrayValue);
    if (a.isArray()) for (const auto& x : a)
        if (allow.count(x.asString())) out.append(x.asString());
    return out;
}
static std::string dumpJson(const Json::Value& v) {
    Json::StreamWriterBuilder wb; wb["indentation"] = "";
    return Json::writeString(wb, v);
}
static Json::Value parseObj(const std::string& s) {
    Json::Value out(Json::objectValue), tmp;
    Json::CharReaderBuilder b; std::string errs;
    std::unique_ptr<Json::CharReader> rd(b.newCharReader());
    if (rd->parse(s.data(), s.data() + s.size(), &tmp, &errs) && tmp.isObject()) out = tmp;
    return out;
}

// 取 card_types[app] 的某卡种价格（兼容旧数组结构：数组里没有价格视为 0）。
static bool parentHasType(const Json::Value& parentApp, const std::string& t, double& floorOut) {
    if (parentApp.isObject()) { if (!parentApp.isMember(t)) return false; floorOut = parentApp[t].asDouble(); return true; }
    if (parentApp.isArray())  { for (const auto& x : parentApp) if (x.asString() == t) { floorOut = 0; return true; } return false; }
    return false;
}

// 计算子代的卡种授权 + 定价：结构 {app:{type:price}}。
//   - 父级受限时，子代卡种必须是父级子集；价格不得低于父级（floor）。
//   - 父级不限（超管或该 app 无条目）时，价格底为 0。
//   - 价格一律 >= 0。任何违规 -> 写 err 并返回 false。
static bool computeChildTypes(bool parentSuper, const std::string& parentJson,
                              const Json::Value& childApps, const Json::Value& reqTypes,
                              std::string& outJson, std::string& err) {
    const Json::Value parentObj = parseObj(parentJson);
    Json::Value out(Json::objectValue);
    if (childApps.isArray()) {
        for (const auto& av : childApps) {
            const std::string a = av.asString();
            if (!(reqTypes.isObject() && reqTypes.isMember(a) && reqTypes[a].isObject())) continue;
            const Json::Value& reqApp = reqTypes[a];
            const bool parentUnlimited =
                parentSuper || !(parentObj.isObject() && parentObj.isMember(a));
            const Json::Value parentApp = parentUnlimited ? Json::Value(Json::objectValue) : parentObj[a];

            Json::Value outApp(Json::objectValue);
            for (const auto& t : reqApp.getMemberNames()) {
                double floor = 0;
                if (!parentUnlimited && !parentHasType(parentApp, t, floor)) {
                    err = "卡种超出父级授权范围"; return false;
                }
                double price = reqApp[t].asDouble();
                if (price <= 0) { err = "注册卡价格必须大于 0"; return false; }
                if (price < floor) { err = "注册卡价格不能低于父级"; return false; }
                outApp[t] = price;
            }
            if (!outApp.empty()) out[a] = outApp;
        }
    }
    outJson = dumpJson(out);
    return true;
}

drogon::Task<> AgentController::list(HttpRequestPtr req,
                                     std::function<void(const HttpResponsePtr&)> cb) {
    const auto uid = req->getAttributes()->get<std::string>("uid");
    auto db = app().getDbClient();
    auto rows = co_await db->execSqlCoro(
        "WITH RECURSIVE sub AS ("
        "  SELECT id FROM agents WHERE id=$1 "
        "  UNION ALL SELECT a.id FROM agents a JOIN sub ON a.parent_id=sub.id) "
        "SELECT id, username, nickname, level, parent_id, apps, perms, card_types, status, "
        "       created_at, last_login_at, last_login_ip, balance, consumed, "
        "  (SELECT username FROM agents p WHERE p.id=agents.parent_id) AS parent_username, "
        "  (SELECT nickname FROM agents p WHERE p.id=agents.parent_id) AS parent_nickname, "
        "  COALESCE((SELECT SUM(balance) FROM agents c WHERE c.parent_id=agents.id),0) AS children_alloc, "
        "  COALESCE((SELECT SUM(price)   FROM cards  k WHERE k.maker_id=agents.id AND k.status='unused'),0) AS held "
        "FROM agents WHERE id IN (SELECT id FROM sub) AND id<>$1 "
        "ORDER BY created_at DESC;", uid);

    Json::Value items(Json::arrayValue);
    for (const auto& r : rows) {
        Json::Value a;
        a["id"] = r["id"].as<std::string>();
        a["username"] = r["username"].as<std::string>();
        a["nickname"] = r["nickname"].isNull() ? "" : r["nickname"].as<std::string>();
        a["level"] = r["level"].as<int>();
        a["parent_id"] = r["parent_id"].isNull() ? "" : r["parent_id"].as<std::string>();
        a["parent_username"] = r["parent_username"].isNull() ? "" : r["parent_username"].as<std::string>();
        a["parent_nickname"] = r["parent_nickname"].isNull() ? "" : r["parent_nickname"].as<std::string>();
        a["apps"] = parseArr(r["apps"].as<std::string>());
        a["perms"] = parseArr(r["perms"].as<std::string>());
        a["card_types"] = parseObj(r["card_types"].as<std::string>());
        a["status"] = r["status"].as<std::string>();
        a["created_at"] = r["created_at"].as<long long>();
        a["last_login_at"] = r["last_login_at"].isNull() ? 0 : r["last_login_at"].as<long long>();
        a["last_login_ip"] = r["last_login_ip"].isNull() ? "" : r["last_login_ip"].as<std::string>();
        const double bal = r["balance"].as<double>();
        const double cons = r["consumed"].as<double>();
        const double childAlloc = r["children_alloc"].as<double>();
        const double held = r["held"].as<double>();
        a["balance"] = bal;
        a["consumed"] = cons;
        a["children_alloc"] = childAlloc;
        a["held"] = held;
        a["available"] = bal - childAlloc - held - cons;   // 剩余可分配/可制卡额度
        items.append(a);
    }
    Json::Value v; v["ok"] = true; v["items"] = items;
    cb(jsonResp(v));
    co_return;
}

// 子树代理列表（自己 + 所有下级），供"卡密所属人"等下拉用。
// 递归 CTE 天然只含自己和后代——看不到上级、也看不到同级。
drogon::Task<> AgentController::subtree(HttpRequestPtr req,
                                        std::function<void(const HttpResponsePtr&)> cb) {
    const auto uid = req->getAttributes()->get<std::string>("uid");
    auto db = app().getDbClient();
    auto rows = co_await db->execSqlCoro(
        "WITH RECURSIVE sub AS ("
        "  SELECT id FROM agents WHERE id=$1 "
        "  UNION ALL SELECT a.id FROM agents a JOIN sub ON a.parent_id=sub.id) "
        "SELECT id, username, nickname, level FROM agents "
        "WHERE id IN (SELECT id FROM sub) ORDER BY level, username;", uid);
    Json::Value items(Json::arrayValue);
    for (const auto& r : rows) {
        Json::Value a;
        a["id"] = r["id"].as<std::string>();
        a["username"] = r["username"].as<std::string>();
        a["nickname"] = r["nickname"].isNull() ? "" : r["nickname"].as<std::string>();
        a["self"] = (r["id"].as<std::string>() == uid);
        items.append(a);
    }
    Json::Value v; v["ok"] = true; v["items"] = items;
    cb(aegis::http::jsonResp(v));
    co_return;
}

drogon::Task<> AgentController::create(HttpRequestPtr req,
                                       std::function<void(const HttpResponsePtr&)> cb) {
    const auto uid = req->getAttributes()->get<std::string>("uid");
    if (!co_await perm::hasPermission(uid, "allowAddChild")) {
        cb(err("forbidden", k403Forbidden)); co_return;
    }
    auto j = req->getJsonObject();
    const std::string username = j ? (*j).get("username", "").asString() : "";
    const std::string password = j ? (*j).get("password", "").asString() : "";
    const std::string nickname = j ? (*j).get("nickname", "").asString() : "";
    if (username.empty() || password.size() < 8) {
        cb(err("username required and password >= 8", k400BadRequest)); co_return;
    }

    auto db = app().getDbClient();
    auto pr = co_await db->execSqlCoro(
        "SELECT level, apps, perms, card_types FROM agents WHERE id=$1 LIMIT 1;", uid);
    if (pr.empty()) { cb(err("parent not found", k404NotFound)); co_return; }
    const int parentLevel = pr[0]["level"].as<int>();
    const bool parentSuper = parentLevel == 1;

    // child 的 apps / perms / card_types 都不得超过 parent（超管不受限）。
    Json::Value reqApps  = j->isMember("apps")  ? (*j)["apps"]  : Json::Value(Json::arrayValue);
    Json::Value reqPerms = j->isMember("perms") ? (*j)["perms"] : Json::Value(Json::arrayValue);
    Json::Value reqTypes = j->isMember("card_types") ? (*j)["card_types"] : Json::Value(Json::objectValue);
    Json::Value childApps, childPerms;
    if (parentSuper) {
        childApps = reqApps.isArray() ? reqApps : Json::Value(Json::arrayValue);
        childPerms = reqPerms.isArray() ? reqPerms : Json::Value(Json::arrayValue);
    } else {
        std::unordered_set<std::string> pApps, pPerms;
        for (const auto& x : parseArr(pr[0]["apps"].as<std::string>()))  pApps.insert(x.asString());
        for (const auto& x : parseArr(pr[0]["perms"].as<std::string>())) pPerms.insert(x.asString());
        childApps  = intersect(reqApps,  pApps);
        childPerms = intersect(reqPerms, pPerms);
    }
    std::string childTypes, ctErr;
    if (!computeChildTypes(parentSuper, pr[0]["card_types"].as<std::string>(),
                           childApps, reqTypes, childTypes, ctErr)) {
        cb(err(ctErr, k400BadRequest)); co_return;
    }

    // 余额：创建时设定；共享额度池——不得超过父级可用余额。
    const double balance = j ? (*j).get("balance", 0).asDouble() : 0;
    if (balance < 0) { cb(err("余额不能小于 0", k400BadRequest)); co_return; }
    if (!parentSuper) {
        const auto bi = co_await perm::balanceInfo(uid);
        if (balance > bi.available() + 1e-9) {
            cb(err("余额超过你的可用额度（可用 " + std::to_string(bi.available()) + "）", k400BadRequest));
            co_return;
        }
    }

    const std::string id = drogon::utils::getUuid();
    const std::string hash = security::hashPassword(password);
    const long long now = static_cast<long long>(time(nullptr));

    try {
        co_await db->execSqlCoro(
            "INSERT INTO agents(id, username, password_hash, must_change_password, nickname, "
            "level, parent_id, apps, perms, card_types, status, created_at, balance, consumed) "
            "VALUES($1,$2,$3,FALSE,$4,$5,$6,$7,$8,$9,'active',$10,$11,0);",
            id, username, hash, nickname, parentLevel + 1, uid,
            dumpArr(childApps), dumpArr(childPerms), childTypes, now, balance);
    } catch (const orm::DrogonDbException&) {
        cb(err("username already exists", k409Conflict)); co_return;
    }
    Json::Value v; v["ok"] = true; v["id"] = id;
    cb(jsonResp(v));
    co_return;
}

drogon::Task<> AgentController::update(HttpRequestPtr req,
                                       std::function<void(const HttpResponsePtr&)> cb) {
    const auto uid = req->getAttributes()->get<std::string>("uid");
    if (!co_await perm::hasPermission(uid, "allowAddChild")) {
        cb(err("forbidden", k403Forbidden)); co_return;
    }
    auto j = req->getJsonObject();
    const std::string id = j ? (*j).get("id", "").asString() : "";
    if (id.empty() || id == uid) { cb(err("invalid target", k400BadRequest)); co_return; }

    auto db = app().getDbClient();
    // 编辑者自身的额度作为上限（与建号同一套交集逻辑）
    auto er = co_await db->execSqlCoro(
        "SELECT level, apps, perms, card_types FROM agents WHERE id=$1 LIMIT 1;", uid);
    if (er.empty()) { cb(err("not found", k404NotFound)); co_return; }
    const bool editorSuper = er[0]["level"].as<int>() == 1;

    Json::Value reqApps  = j->isMember("apps")  ? (*j)["apps"]  : Json::Value(Json::arrayValue);
    Json::Value reqPerms = j->isMember("perms") ? (*j)["perms"] : Json::Value(Json::arrayValue);
    Json::Value reqTypes = j->isMember("card_types") ? (*j)["card_types"] : Json::Value(Json::objectValue);
    Json::Value newApps, newPerms;
    if (editorSuper) {
        newApps = reqApps.isArray() ? reqApps : Json::Value(Json::arrayValue);
        newPerms = reqPerms.isArray() ? reqPerms : Json::Value(Json::arrayValue);
    } else {
        std::unordered_set<std::string> eApps, ePerms;
        for (const auto& x : parseArr(er[0]["apps"].as<std::string>()))  eApps.insert(x.asString());
        for (const auto& x : parseArr(er[0]["perms"].as<std::string>())) ePerms.insert(x.asString());
        newApps  = intersect(reqApps,  eApps);
        newPerms = intersect(reqPerms, ePerms);
    }
    std::string newTypes, ctErr;
    if (!computeChildTypes(editorSuper, er[0]["card_types"].as<std::string>(),
                           newApps, reqTypes, newTypes, ctErr)) {
        cb(err(ctErr, k400BadRequest)); co_return;
    }
    const std::string nickname = j ? (*j).get("nickname", "").asString() : "";

    // 事务：主更新 + 提价联动（把目标下级中低于"上级新价"的卡种逐层顶上去，只升不降）
    auto trx = co_await db->newTransactionCoro();

    // 仅能改"自己子树内、且非自己"的代理
    auto r = co_await trx->execSqlCoro(
        "WITH RECURSIVE sub AS ("
        "  SELECT id FROM agents WHERE id=$1 "
        "  UNION ALL SELECT a.id FROM agents a JOIN sub ON a.parent_id=sub.id) "
        "UPDATE agents SET nickname=$3, apps=$4, perms=$5, card_types=$6 "
        "WHERE id=$2 AND id<>$1 AND id IN (SELECT id FROM sub);",
        uid, id, nickname, dumpArr(newApps), dumpArr(newPerms), newTypes);
    if (!r.affectedRows()) {
        cb(err("not found or forbidden", k404NotFound));
        co_return;   // 事务无提交即回滚
    }

    // ---- 提价联动：目标(id)的新价作为其下级的价格下限，逐层向下传播 ----
    int adjusted = co_await cascadePriceFloor(trx, id, newTypes);

    Json::Value v; v["ok"] = true; v["adjusted_children"] = adjusted;
    cb(jsonResp(v));
    co_return;
}

// 把 rootId 的卡种价格作为其整棵子树的价格下限，自上而下传播：
// 每个节点的新价 = max(自身旧价, 其上级新价)；只升不降；只动确实低于下限的下级。
// 返回被调整的下级数量。在传入的事务内执行，保证与主更新原子。
drogon::Task<int> AgentController::cascadePriceFloor(
        std::shared_ptr<drogon::orm::Transaction> trx,
        const std::string& rootId, const std::string& rootTypesJson) {
    // 拉 rootId 的整棵子树（含 parent_id 与 card_types）
    auto sub = co_await trx->execSqlCoro(
        "WITH RECURSIVE t AS ("
        "  SELECT id, parent_id, card_types FROM agents WHERE parent_id=$1 "
        "  UNION ALL SELECT a.id, a.parent_id, a.card_types FROM agents a JOIN t ON a.parent_id=t.id) "
        "SELECT id, parent_id, card_types FROM t;", rootId);

    std::unordered_map<std::string, std::vector<std::string>> children;   // parent -> [child]
    std::unordered_map<std::string, std::string> parentOf;
    std::unordered_map<std::string, Json::Value> typesOf;                 // agent -> {app:{type:price}}
    for (const auto& row : sub) {
        const auto aid = row["id"].as<std::string>();
        const auto pid = row["parent_id"].as<std::string>();
        children[pid].push_back(aid);
        parentOf[aid] = pid;
        typesOf[aid] = parseObj(row["card_types"].as<std::string>());
    }

    // floorOf[agent][app][type] = 该 agent 作为上级传给其子的下限价
    std::unordered_map<std::string,
        std::unordered_map<std::string, std::unordered_map<std::string, double>>> floorOf;
    Json::Value rootTypes = parseObj(rootTypesJson);
    for (const auto& app : rootTypes.getMemberNames()) {
        if (!rootTypes[app].isObject()) continue;
        for (const auto& tp : rootTypes[app].getMemberNames())
            floorOf[rootId][app][tp] = rootTypes[app][tp].asDouble();
    }

    int adjusted = 0;
    std::vector<std::string> queue;
    if (children.count(rootId)) queue = children[rootId];
    for (size_t qi = 0; qi < queue.size(); ++qi) {
        const std::string x = queue[qi];
        const std::string p = parentOf[x];
        Json::Value& xt = typesOf[x];
        bool changed = false;
        auto pf = floorOf.find(p);
        if (pf != floorOf.end()) {
            for (const auto& appPair : pf->second) {
                const std::string& app = appPair.first;
                for (const auto& tpPair : appPair.second) {
                    const std::string& tp = tpPair.first;
                    const double floor = tpPair.second;
                    double carried = floor;   // 默认沿用上级下限（x 不卖该卡种时也要传下去）
                    if (xt.isMember(app) && xt[app].isObject() && xt[app].isMember(tp)) {
                        double xv = xt[app][tp].asDouble();
                        if (xv < floor) { xt[app][tp] = floor; xv = floor; changed = true; }
                        carried = xv;   // x 的(可能被顶高的)价是其子的下限
                    }
                    floorOf[x][app][tp] = carried;
                }
            }
        }
        if (changed) {
            co_await trx->execSqlCoro("UPDATE agents SET card_types=$2 WHERE id=$1;", x, dumpJson(xt));
            ++adjusted;
        }
        auto ch = children.find(x);
        if (ch != children.end()) for (const auto& c : ch->second) queue.push_back(c);
    }
    co_return adjusted;
}

drogon::Task<> AgentController::setStatus(HttpRequestPtr req,
                                          std::function<void(const HttpResponsePtr&)> cb) {
    const auto uid = req->getAttributes()->get<std::string>("uid");
    auto j = req->getJsonObject();
    const std::string id = j ? (*j).get("id", "").asString() : "";
    const std::string status = j ? (*j).get("status", "").asString() : "";
    if (id.empty() || (status != "active" && status != "disabled")) {
        cb(err("id/status invalid", k400BadRequest)); co_return;
    }
    auto db = app().getDbClient();
    auto r = co_await db->execSqlCoro(
        "WITH RECURSIVE sub AS ("
        "  SELECT id FROM agents WHERE id=$1 "
        "  UNION ALL SELECT a.id FROM agents a JOIN sub ON a.parent_id=sub.id) "
        "UPDATE agents SET status=$3 WHERE id=$2 AND id<>$1 AND id IN (SELECT id FROM sub);",
        uid, id, status);
    if (r.affectedRows() == 0) { cb(err("not found or forbidden", k404NotFound)); co_return; }
    if (status == "disabled") co_await aegis::http::revokeAllForAgent(id);  // 禁用即踢下线
    cb(okResp());
    co_return;
}

drogon::Task<> AgentController::resetPassword(HttpRequestPtr req,
                                              std::function<void(const HttpResponsePtr&)> cb) {
    const auto uid = req->getAttributes()->get<std::string>("uid");
    auto j = req->getJsonObject();
    const std::string id = j ? (*j).get("id", "").asString() : "";
    const std::string newP = j ? (*j).get("newPassword", "").asString() : "";
    if (id.empty() || newP.size() < 8) {
        cb(err("id required and newPassword >= 8", k400BadRequest)); co_return;
    }
    const std::string hash = security::hashPassword(newP);
    const long long now = static_cast<long long>(time(nullptr));

    auto db = app().getDbClient();
    auto r = co_await db->execSqlCoro(
        "WITH RECURSIVE sub AS ("
        "  SELECT id FROM agents WHERE id=$1 "
        "  UNION ALL SELECT a.id FROM agents a JOIN sub ON a.parent_id=sub.id) "
        "UPDATE agents SET password_hash=$3, must_change_password=TRUE, password_changed_at=$4 "
        "WHERE id=$2 AND id<>$1 AND id IN (SELECT id FROM sub);",
        uid, id, hash, now);
    if (r.affectedRows() == 0) { cb(err("not found or forbidden", k404NotFound)); co_return; }
    co_await aegis::http::revokeAllForAgent(id);  // 改密后吊销其全部会话
    cb(okResp());
    co_return;
}

// 登录日志：仅超级管理员可查。返回某代理（或全部代理）最近的后台登录记录，
// 含时间 / IP / 浏览器设备(User-Agent)。数据来自每次登录写入的 admin_sessions。
drogon::Task<> AgentController::loginLogs(HttpRequestPtr req,
                                         std::function<void(const HttpResponsePtr&)> cb) {
    const auto uid = req->getAttributes()->get<std::string>("uid");
    if (!co_await perm::isSuper(uid)) { cb(err("forbidden", k403Forbidden)); co_return; }

    const std::string id = aegis::http::param(req, "id");   // 可选：指定某代理；留空=全部
    int limit = std::atoi(aegis::http::param(req, "limit", "100").c_str());
    if (limit < 1) limit = 100; if (limit > 500) limit = 500;

    auto db = app().getDbClient();
    try {
        Json::Value items(Json::arrayValue);
        const std::string tail =
            " ORDER BY s.created_at DESC LIMIT " + std::to_string(limit) + ";";
        auto rows = id.empty()
            ? co_await db->execSqlCoro(
                  "SELECT s.agent_id, a.username, a.nickname, s.ip, s.user_agent, "
                  "       s.created_at, s.expires_at, s.revoked "
                  "FROM admin_sessions s JOIN agents a ON a.id=s.agent_id" + tail)
            : co_await db->execSqlCoro(
                  "SELECT s.agent_id, a.username, a.nickname, s.ip, s.user_agent, "
                  "       s.created_at, s.expires_at, s.revoked "
                  "FROM admin_sessions s JOIN agents a ON a.id=s.agent_id "
                  "WHERE s.agent_id=$1" + tail, id);
        for (const auto& r : rows) {
            Json::Value o;
            o["agent_id"]   = r["agent_id"].as<std::string>();
            o["username"]   = r["username"].as<std::string>();
            o["nickname"]   = r["nickname"].isNull() ? "" : r["nickname"].as<std::string>();
            o["ip"]         = r["ip"].isNull() ? "" : r["ip"].as<std::string>();
            o["user_agent"] = r["user_agent"].isNull() ? "" : r["user_agent"].as<std::string>();
            o["created_at"] = r["created_at"].as<long long>();
            o["expires_at"] = r["expires_at"].as<long long>();
            o["revoked"]    = r["revoked"].as<int>();
            items.append(o);
        }
        Json::Value v; v["ok"] = true; v["items"] = items;
        cb(jsonResp(v));
    } catch (const std::exception& e) {
        LOG_ERROR << "agents.loginLogs: " << e.what();
        cb(err("server error", k500InternalServerError));
    }
    co_return;
}

// 调整某下级代理的余额（共享额度池）。新余额不得低于该代理已占用额度，
// 增加部分不得超过其【直接上级】的可用额度。
drogon::Task<> AgentController::adjustBalance(HttpRequestPtr req,
                                             std::function<void(const HttpResponsePtr&)> cb) {
    const auto uid = req->getAttributes()->get<std::string>("uid");
    if (!co_await perm::hasPermission(uid, "allowAddChild")) { cb(err("forbidden", k403Forbidden)); co_return; }
    auto j = req->getJsonObject();
    const std::string id = j ? (*j).get("id", "").asString() : "";
    const double newBalance = j ? (*j).get("balance", 0).asDouble() : 0;
    if (id.empty() || id == uid) { cb(err("invalid target", k400BadRequest)); co_return; }
    if (newBalance < 0) { cb(err("余额不能小于 0", k400BadRequest)); co_return; }

    // 目标必须在本人子树内
    auto allowed = co_await perm::subAgentIds(uid);
    if (std::find(allowed.begin(), allowed.end(), id) == allowed.end()) {
        cb(err("not found or forbidden", k404NotFound)); co_return;
    }

    auto db = app().getDbClient();
    auto tr = co_await db->execSqlCoro(
        "SELECT parent_id, balance, consumed, "
        "  COALESCE((SELECT SUM(balance) FROM agents c WHERE c.parent_id=agents.id),0) AS children_alloc, "
        "  COALESCE((SELECT SUM(price)   FROM cards  k WHERE k.maker_id=agents.id AND k.status='unused'),0) AS held "
        "FROM agents WHERE id=$1 LIMIT 1;", id);
    if (tr.empty()) { cb(err("not found", k404NotFound)); co_return; }

    const double oldBalance = tr[0]["balance"].as<double>();
    const double committed = tr[0]["children_alloc"].as<double>()
                           + tr[0]["held"].as<double>() + tr[0]["consumed"].as<double>();
    if (newBalance < committed - 1e-9) {
        cb(err("新余额不能低于该代理已占用额度（已分配+未用卡+已消耗 = "
               + std::to_string(committed) + "）", k400BadRequest));
        co_return;
    }
    const double delta = newBalance - oldBalance;
    if (delta > 1e-9) {
        const std::string parentId = tr[0]["parent_id"].isNull() ? "" : tr[0]["parent_id"].as<std::string>();
        if (!parentId.empty()) {
            const auto pbi = co_await perm::balanceInfo(parentId);
            if (!pbi.super && delta > pbi.available() + 1e-9) {
                cb(err("增加额度超过上级可用余额（上级可用 " + std::to_string(pbi.available()) + "）",
                       k400BadRequest));
                co_return;
            }
        }
    }
    co_await db->execSqlCoro("UPDATE agents SET balance=$2 WHERE id=$1;", id, newBalance);
    cb(okResp());
    co_return;
}

// 当前登录代理的余额账目（供制卡页/概览显示）。
drogon::Task<> AgentController::myBalance(HttpRequestPtr req,
                                         std::function<void(const HttpResponsePtr&)> cb) {
    const auto uid = req->getAttributes()->get<std::string>("uid");
    const auto bi = co_await perm::balanceInfo(uid);
    Json::Value v;
    v["ok"] = true; v["super"] = bi.super;
    v["balance"] = bi.balance; v["consumed"] = bi.consumed;
    v["children_alloc"] = bi.childrenAlloc; v["held"] = bi.held;
    v["available"] = bi.available();
    cb(jsonResp(v));
    co_return;
}

} // namespace aegis
