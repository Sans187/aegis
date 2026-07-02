#include "tcp/VerifyApp.h"
#include "tcp/Protocol.h"
#include "tcp/TokenService.h"
#include "tcp/AppSettings.h"
#include "security/Password.h"

#include <algorithm>
#include <ctime>
#include <optional>

using namespace drogon;

namespace aegis::tcp {

static constexpr int kSentinel = 100000;

// token 有效期 = min(now + 设置的 tokenTtlHours, 订阅到期)。
// 短效 token + 客户端续签：降低泄露风险，且冻结/到期能更快生效。
static int64_t tokenExpiry(const Json::Value& settings, int64_t now, int64_t subscriptionExp) {
    int ttlHours = settings["verify"]["tokenTtlHours"].asInt();
    if (ttlHours <= 0) ttlHours = 24;
    const int64_t byTtl = now + static_cast<int64_t>(ttlHours) * 3600;
    return std::min(byTtl, subscriptionExp);
}

static int64_t nowSec() { return static_cast<int64_t>(time(nullptr)); }

// 构建一条充值记录（单元素 JSON 数组，便于 `recharges || $::jsonb` 追加）。
static std::string rechargeRecord(const std::string& cardId, const std::string& code, int hours,
                                  const std::string& typeId, const std::string& makerName, int64_t at) {
    Json::Value o(Json::objectValue), arr(Json::arrayValue);
    o["card_id"] = cardId;
    o["code"] = code;
    o["hours"] = hours;
    o["type_id"] = typeId;     // 卡种 id，详情里解析成卡种名展示
    o["maker_name"] = makerName; // 卡密所属人（制卡人）
    o["at"] = static_cast<Json::Int64>(at);
    arr.append(o);
    Json::StreamWriterBuilder wb; wb["indentation"] = "";
    return Json::writeString(wb, arr);
}

VerifyApp::VerifyApp(int appId, std::unordered_set<int> enabledApis)
    : appId_(appId), appIdStr_(std::to_string(appId)), apis_(std::move(enabledApis)) {}

drogon::Task<std::string> VerifyApp::appMode() {
    auto r = co_await app().getDbClient()->execSqlCoro(
        "SELECT mode FROM apps WHERE id=$1 LIMIT 1;", appIdStr_);
    co_return r.empty() ? std::string("card") : r[0]["mode"].as<std::string>();
}

drogon::Task<std::optional<std::string>> VerifyApp::authUser(const Json::Value& data) {
    const std::string mode = co_await appMode();
    std::string username, cred;
    if (mode == "user") { username = data["username"].asString(); cred = data["password"].asString(); }
    else { cred = data["Key"].asString(); username = makeUsernameFromCard(cred); }

    auto r = co_await app().getDbClient()->execSqlCoro(
        "SELECT password_hash FROM users WHERE app_id=$1 AND username=$2 LIMIT 1;", appIdStr_, username);
    if (r.empty() || r[0]["password_hash"].isNull()) co_return std::nullopt;
    if (!security::verifyPassword(cred, r[0]["password_hash"].as<std::string>())) co_return std::nullopt;
    co_return username;
}

drogon::Task<Json::Value> VerifyApp::apiJson(int apiId, const std::string& ip, Json::Value data) {
    switch (apiId) {
        case api_Login:      co_return co_await doLogin(ip, data);
        case api_Register:   co_return co_await doRegister(ip, data);
        case api_Recharge:   co_return co_await doRecharge(data);
        case api_Query:      co_return co_await doQuery(data);
        case api_Notice:     co_return co_await doNotice();
        case api_UpdateInfo: co_return co_await doUpdateInfo();
        case api_Rebind: {
            auto u = co_await authUser(data);
            if (!u) { Json::Value r; r["status"] = ERR_KEY_NOT_FOUND; co_return r; }
            co_return co_await doRebind(*u, data["hwid"].asString());
        }
        case api_Unbind: {
            auto u = co_await authUser(data);
            if (!u) { Json::Value r; r["status"] = ERR_KEY_NOT_FOUND; co_return r; }
            co_return co_await doUnbind(*u);
        }
        case api_Verify: {
            Json::Value r; r["cmdTag"] = static_cast<Json::Int64>(nowSec());
            const std::string token = data["token"].asString();
            if (!co_await verifyToken(appIdStr_, token)) { r["status"] = ERR_NO_TOKEN; co_return r; }
            auto u = co_await getUsernameByToken(appIdStr_, token);
            if (!u) { r["status"] = ERR_NO_TOKEN; co_return r; }
            auto rr = co_await app().getDbClient()->execSqlCoro(
                "SELECT frozen, expired_at FROM users WHERE app_id=$1 AND username=$2;", appIdStr_, *u);
            if (rr.empty()) { r["status"] = ERR_KEY_NOT_FOUND; co_return r; }
            if (rr[0]["frozen"].as<int>() == 1) { r["status"] = ERR_FROZEN_USER; co_return r; }
            if (rr[0]["expired_at"].as<long long>() < nowSec()) { r["status"] = ERR_EXPIRED_USER; co_return r; }
            r["status"] = ERR_OK; co_return r;
        }
        default: { Json::Value r; r["status"] = ERR_UNKNOWN_API; co_return r; }
    }
}

drogon::Task<Json::Value> VerifyApp::doLogin(const std::string& ip, const Json::Value& data) {
    if (co_await appMode() == "user") co_return co_await doLoginAccount(ip, data);
    co_return co_await doLoginCard(ip, data);
}

drogon::Task<Json::Value> VerifyApp::finishLogin(const drogon::orm::Row& row, const std::string& username,
                                                 const std::string& hwid, const std::string& ip) {
    Json::Value result;
    result["cmdTag"] = static_cast<Json::Int64>(nowSec());
    auto db = app().getDbClient();

    const long long exp_time = row["expired_at"].as<long long>();
    const auto machine_code = row["machine_code"].isNull() ? std::string() : row["machine_code"].as<std::string>();
    const long long now = nowSec();

    if (exp_time < now) {
        result["status"] = ERR_EXPIRED_USER;
        result["expired_at"] = static_cast<Json::Int64>(exp_time);
        result["username"] = username;
        co_return result;
    }
    if (row["frozen"].as<int>() == 1) { result["status"] = ERR_FROZEN_USER; co_return result; }

    const auto settings = co_await readSettings(appIdStr_);
    const bool allow_rebind = settings["rebind"]["allow"].asBool();
    const int forbid = allow_rebind ? ERR_NEED_REBIND : ERR_REBIND_FORBIDDEN;
    if (machine_code.empty()) {
        auto rb = co_await doRebind(username, hwid);
        if (rb["status"].asInt() != ERR_OK) co_return rb;
    } else if (machine_code != hwid) {
        result["status"] = forbid;
        co_return result;
    }

    // 登录即签发新 token（撤销旧的）。token 短效，客户端到期后凭卡/账号重登续签。
    auto issued = co_await issueToken(appIdStr_, username, hwid, ip, "",
                                      tokenExpiry(settings, now, exp_time));
    if (!issued) { result["status"] = ERR_ROTATE_FAILED; co_return result; }

    co_await db->execSqlCoro(
        "UPDATE users SET online=1, ip_address=$1 WHERE app_id=$2 AND username=$3;", ip, appIdStr_, username);

    // 卡种小时数：取最近一次充值/激活记录的 hours
    int cardHours = 0;
    if (!row["recharges"].isNull()) {
        const std::string rs = row["recharges"].as<std::string>();
        Json::Value arr; Json::CharReaderBuilder rb; std::string errs;
        std::unique_ptr<Json::CharReader> rd(rb.newCharReader());
        if (rd->parse(rs.data(), rs.data() + rs.size(), &arr, &errs) && arr.isArray() && !arr.empty())
            cardHours = arr[arr.size() - 1].get("hours", 0).asInt();
    }

    result["status"] = ERR_OK;
    result["token"] = issued->token;
    result["expired_at"] = static_cast<Json::Int64>(exp_time);
    result["username"] = username;
    result["hours"] = cardHours;      // 卡种小时数
    co_return result;
}

drogon::Task<Json::Value> VerifyApp::doLoginCard(const std::string& ip, const Json::Value& data) {
    const std::string key = data["Key"].asString();
    const std::string hwid = data["hwid"].asString();
    const auto username = makeUsernameFromCard(key);

    Json::Value result;
    result["cmdTag"] = static_cast<Json::Int64>(nowSec());
    auto db = app().getDbClient();

    auto r = co_await db->execSqlCoro(
        "SELECT * FROM users WHERE app_id=$1 AND username=$2;", appIdStr_, username);
    if (!r.empty()) {
        // 已激活用户：用完整卡密校验哈希（比旧版仅按派生用户名匹配更严）
        if (r[0]["password_hash"].isNull()
            || !security::verifyPassword(key, r[0]["password_hash"].as<std::string>())) {
            result["status"] = ERR_KEY_NOT_FOUND; co_return result;
        }
        co_return co_await finishLogin(r[0], username, hwid, ip);
    }

    // 未激活：消费卡密创建用户
    auto rs = co_await db->execSqlCoro(
        "SELECT * FROM cards WHERE app_id=$1 AND code=$2;", appIdStr_, key);
    if (rs.empty()) { result["status"] = ERR_KEY_NOT_FOUND; co_return result; }
    const auto& crow = rs[0];
    if (crow["status"].as<std::string>() != "unused") { result["status"] = ERR_UNUSED_CARD; co_return result; }
    if (crow["frozen"].as<int>() == 1) { result["status"] = ERR_FROZEN_CARD; co_return result; }

    const long long now = nowSec();
    const int hours = crow["hours"].isNull() ? 0 : crow["hours"].as<int>();
    const std::string note   = crow["remark"].isNull() ? std::string() : crow["remark"].as<std::string>();
    const std::string maker  = crow["maker_id"].isNull() ? std::string() : crow["maker_id"].as<std::string>();
    const std::string makerName = crow["maker_name"].isNull() ? std::string() : crow["maker_name"].as<std::string>();
    const std::string typeId = crow["type_id"].as<std::string>();
    const std::string cardId = crow["id"].as<std::string>();
    const long long expired_at = now + static_cast<long long>(hours) * 3600;
    const std::string hash = security::hashPassword(key);

    const auto settings = co_await readSettings(appIdStr_);
    auto issued = co_await issueToken(appIdStr_, username, hwid, ip, "",
                                      tokenExpiry(settings, now, expired_at));
    if (!issued) { result["status"] = ERR_ROTATE_FAILED; co_return result; }

    co_await db->execSqlCoro(
        "INSERT INTO users(app_id, username, password_hash, created_at, expired_at, "
        "frozen, online, ip_address, remark, agent_id, machine_code, recharges) "
        "VALUES($1,$2,$3,$4,$5,0,1,$6,$7,$8,$9,$10::jsonb);",
        appIdStr_, username, hash, now, expired_at, ip, note, maker, hwid,
        rechargeRecord(cardId, key, hours, typeId, makerName, now));
    co_await db->execSqlCoro(
        "UPDATE cards SET status='used', used_at=$1, expire_at=$2, used_by=$5 WHERE app_id=$3 AND code=$4;",
        now, expired_at, appIdStr_, key, username);
    // 余额系统：激活即真正消耗——制卡人 consumed += 卡密单价。
    if (!maker.empty())
        co_await db->execSqlCoro("UPDATE agents SET consumed = consumed + $1 WHERE id=$2;",
            crow["price"].isNull() ? 0.0 : crow["price"].as<double>(), maker);

    result["status"] = ERR_OK;
    result["token"] = issued->token;
    result["expired_at"] = static_cast<Json::Int64>(expired_at);
    result["username"] = username;
    result["hours"] = hours;          // 卡种小时数
    co_return result;
}

drogon::Task<Json::Value> VerifyApp::doLoginAccount(const std::string& ip, const Json::Value& data) {
    const std::string username = data["username"].asString();
    const std::string password = data["password"].asString();
    const std::string hwid = data["hwid"].asString();

    Json::Value result;
    result["cmdTag"] = static_cast<Json::Int64>(nowSec());

    auto r = co_await app().getDbClient()->execSqlCoro(
        "SELECT * FROM users WHERE app_id=$1 AND username=$2;", appIdStr_, username);
    if (r.empty()
        || r[0]["password_hash"].isNull()
        || !security::verifyPassword(password, r[0]["password_hash"].as<std::string>())) {
        // 不区分"账号不存在/密码错误"，避免泄露账号是否存在
        result["status"] = ERR_KEY_NOT_FOUND; co_return result;
    }
    co_return co_await finishLogin(r[0], username, hwid, ip);
}

drogon::Task<Json::Value> VerifyApp::doRegister(const std::string& ip, const Json::Value& data) {
    const std::string card = data["card"].asString();
    const std::string username = data["username"].asString();
    const std::string password = data["password"].asString();
    const std::string hwid = data["hwid"].asString();

    Json::Value result;
    result["status"] = kSentinel;
    result["cmdTag"] = static_cast<Json::Int64>(nowSec());
    if (username.empty() || password.empty()) { result["status"] = ERR_KEY_NOT_FOUND; co_return result; }

    auto db = app().getDbClient();
    auto cr = co_await db->execSqlCoro(
        "SELECT * FROM cards WHERE app_id=$1 AND code=$2;", appIdStr_, card);
    if (cr.empty()) { result["status"] = ERR_KEY_NOT_FOUND; co_return result; }
    if (cr[0]["status"].as<std::string>() != "unused") { result["status"] = ERR_UNUSED_CARD; co_return result; }
    if (cr[0]["frozen"].as<int>() == 1) { result["status"] = ERR_FROZEN_CARD; co_return result; }

    auto ur = co_await db->execSqlCoro(
        "SELECT 1 FROM users WHERE app_id=$1 AND username=$2 LIMIT 1;", appIdStr_, username);
    if (!ur.empty()) { result["status"] = ERR_USERNAME_TAKEN; co_return result; }

    const long long now = nowSec();
    const int hours = cr[0]["hours"].isNull() ? 0 : cr[0]["hours"].as<int>();
    const std::string typeId = cr[0]["type_id"].as<std::string>();
    const std::string maker  = cr[0]["maker_id"].isNull() ? std::string() : cr[0]["maker_id"].as<std::string>();
    const std::string makerName = cr[0]["maker_name"].isNull() ? std::string() : cr[0]["maker_name"].as<std::string>();
    const std::string cardId = cr[0]["id"].as<std::string>();
    const long long expired_at = now + static_cast<long long>(hours) * 3600;
    const std::string hash = security::hashPassword(password);

    const auto settings = co_await readSettings(appIdStr_);
    auto issued = co_await issueToken(appIdStr_, username, hwid, ip, "",
                                      tokenExpiry(settings, now, expired_at));
    if (!issued) { result["status"] = ERR_ROTATE_FAILED; co_return result; }

    co_await db->execSqlCoro(
        "INSERT INTO users(app_id, username, password_hash, created_at, expired_at, "
        "frozen, online, ip_address, agent_id, machine_code, recharges) "
        "VALUES($1,$2,$3,$4,$5,0,1,$6,$7,$8,$9::jsonb);",
        appIdStr_, username, hash, now, expired_at, ip, maker, hwid,
        rechargeRecord(cardId, card, hours, typeId, makerName, now));
    co_await db->execSqlCoro(
        "UPDATE cards SET status='used', used_at=$1, expire_at=$2, used_by=$5 WHERE app_id=$3 AND code=$4;",
        now, expired_at, appIdStr_, card, username);
    // 余额系统：激活即真正消耗——制卡人 consumed += 卡密单价。
    if (!maker.empty())
        co_await db->execSqlCoro("UPDATE agents SET consumed = consumed + $1 WHERE id=$2;",
            cr[0]["price"].isNull() ? 0.0 : cr[0]["price"].as<double>(), maker);

    result["status"] = ERR_OK;
    result["token"] = issued->token;
    result["expired_at"] = static_cast<Json::Int64>(expired_at);
    result["username"] = username;
    co_return result;
}

drogon::Task<Json::Value> VerifyApp::doRecharge(const Json::Value& data) {
    Json::Value result;
    result["status"] = kSentinel;
    result["cmdTag"] = static_cast<Json::Int64>(nowSec());

    if (co_await appMode() != "user") { result["status"] = ERR_UNKNOWN_API; co_return result; }

    const std::string username = data["username"].asString();
    const std::string card = data["card"].asString();
    if (username.empty() || card.empty()) { result["status"] = ERR_KEY_NOT_FOUND; co_return result; }

    auto db = app().getDbClient();
    auto cr = co_await db->execSqlCoro("SELECT * FROM cards WHERE app_id=$1 AND code=$2;", appIdStr_, card);
    if (cr.empty()) { result["status"] = ERR_KEY_NOT_FOUND; co_return result; }
    if (cr[0]["status"].as<std::string>() != "unused") { result["status"] = ERR_UNUSED_CARD; co_return result; }
    if (cr[0]["frozen"].as<int>() == 1) { result["status"] = ERR_FROZEN_CARD; co_return result; }

    auto ur = co_await db->execSqlCoro(
        "SELECT 1 FROM users WHERE app_id=$1 AND username=$2 LIMIT 1;", appIdStr_, username);
    if (ur.empty()) { result["status"] = ERR_KEY_NOT_FOUND; co_return result; }

    const long long now = nowSec();
    const int hours = cr[0]["hours"].isNull() ? 0 : cr[0]["hours"].as<int>();
    const std::string cardId = cr[0]["id"].as<std::string>();
    const std::string typeId = cr[0]["type_id"].isNull() ? std::string() : cr[0]["type_id"].as<std::string>();
    const std::string makerName = cr[0]["maker_name"].isNull() ? std::string() : cr[0]["maker_name"].as<std::string>();
    const long long addSec = static_cast<long long>(hours) * 3600;
    const std::string rec = rechargeRecord(cardId, card, hours, typeId, makerName, now);

    // 叠加时长（已过期则从现在算起）并追加充值记录
    auto upd = co_await db->execSqlCoro(
        "UPDATE users SET expired_at = GREATEST(expired_at, $1) + $2, "
        "recharges = recharges || $3::jsonb WHERE app_id=$4 AND username=$5 RETURNING expired_at;",
        now, addSec, rec, appIdStr_, username);
    co_await db->execSqlCoro(
        "UPDATE cards SET status='used', used_at=$1, expire_at=$2, used_by=$5 WHERE app_id=$3 AND code=$4;",
        now, now + addSec, appIdStr_, card, username);
    // 余额系统：激活即真正消耗——制卡人 consumed += 卡密单价。
    {
        const std::string mk = cr[0]["maker_id"].isNull() ? std::string() : cr[0]["maker_id"].as<std::string>();
        if (!mk.empty())
            co_await db->execSqlCoro("UPDATE agents SET consumed = consumed + $1 WHERE id=$2;",
                cr[0]["price"].isNull() ? 0.0 : cr[0]["price"].as<double>(), mk);
    }

    result["status"] = ERR_OK;
    result["username"] = username;
    result["expired_at"] = static_cast<Json::Int64>(upd[0]["expired_at"].as<long long>());
    co_return result;
}

drogon::Task<Json::Value> VerifyApp::doRebind(const std::string& username, const std::string& hwid) {
    Json::Value result;
    result["status"] = kSentinel;
    result["cmdTag"] = static_cast<Json::Int64>(nowSec());

    auto db = app().getDbClient();
    const auto settings = co_await readSettings(appIdStr_);
    const int maxTimes = settings["rebind"]["maxTimes"].asInt();
    const int deductHours = settings["rebind"]["unbindDeductHours"].asInt();
    const int cooldownMin = settings["rebind"]["cooldownMinutes"].asInt();

    auto r = co_await db->execSqlCoro(
        "SELECT * FROM users WHERE app_id=$1 AND username=$2;", appIdStr_, username);
    if (r.empty()) co_return result;

    const auto& row = r[0];
    const auto machine_code = row["machine_code"].isNull() ? std::string() : row["machine_code"].as<std::string>();
    const long long exp_time = row["expired_at"].as<long long>();
    const int rebind_cnt = row["rebind_cnt"].as<int>();
    const long long last_rebind_at = row["last_rebind_at"].isNull() ? 0 : row["last_rebind_at"].as<long long>();
    const long long now = nowSec();

    // 换绑冷却：已绑定机器时(实际换机)，距上次换绑不足冷却时长则拒绝；首次绑定不受限。
    if (!machine_code.empty() && cooldownMin > 0 &&
        now < last_rebind_at + static_cast<long long>(cooldownMin) * 60) {
        result["status"] = ERR_REBIND_COOLDOWN;
        result["cooldownRemaining"] =
            static_cast<Json::Int64>(last_rebind_at + static_cast<long long>(cooldownMin) * 60 - now);
        co_return result;
    }

    long long adjusted = exp_time - static_cast<long long>(deductHours) * 3600;

    if (rebind_cnt >= maxTimes) {
        if (now >= adjusted) { result["status"] = ERR_REBIND_LACK_OF_TIME; co_return result; }
        co_await db->execSqlCoro(
            "UPDATE users SET machine_code=$1, rebind_cnt=$2, expired_at=$3, last_rebind_at=$4 WHERE app_id=$5 AND username=$6;",
            hwid, rebind_cnt + 1, adjusted, now, appIdStr_, username);
    } else {
        adjusted = exp_time;
        const int newCnt = machine_code.empty() ? 0 : rebind_cnt + 1;
        co_await db->execSqlCoro(
            "UPDATE users SET machine_code=$1, rebind_cnt=$2, last_rebind_at=$3 WHERE app_id=$4 AND username=$5;",
            hwid, newCnt, now, appIdStr_, username);
    }
    result["status"] = ERR_OK;
    result["expired_at"] = static_cast<Json::Int64>(adjusted);
    co_return result;
}

drogon::Task<Json::Value> VerifyApp::doUnbind(const std::string& username) {
    Json::Value result;
    result["status"] = kSentinel;
    result["cmdTag"] = static_cast<Json::Int64>(nowSec());
    auto r = co_await app().getDbClient()->execSqlCoro(
        "UPDATE users SET machine_code='' WHERE app_id=$1 AND username=$2;", appIdStr_, username);
    result["status"] = (r.affectedRows() == 0) ? ERR_KEY_NOT_FOUND : ERR_OK;
    co_return result;
}

drogon::Task<Json::Value> VerifyApp::doQuery(const Json::Value& data) {
    const std::string mode = co_await appMode();
    const std::string username = (mode == "user")
        ? data["username"].asString()
        : makeUsernameFromCard(data["Key"].asString());

    Json::Value result;
    result["status"] = ERR_KEY_NOT_FOUND;
    result["cmdTag"] = static_cast<Json::Int64>(nowSec());

    auto db = app().getDbClient();
    auto rs = co_await db->execSqlCoro(
        "SELECT * FROM users WHERE app_id=$1 AND username=$2;", appIdStr_, username);
    if (!rs.empty()) {
        const long long exp_time = rs[0]["expired_at"].as<long long>();
        result["status"] = ERR_OK;
        result["username"] = rs[0]["username"].as<std::string>();
        result["expired_at"] = static_cast<Json::Int64>(exp_time);
        result["created_at"] = static_cast<Json::Int64>(rs[0]["created_at"].as<long long>());
        result["frozen"] = rs[0]["frozen"].as<int>();
        result["online"] = rs[0]["online"].as<int>();
        result["expire"] = exp_time < nowSec();
        co_return result;
    }
    if (mode != "user") {
        auto rc = co_await db->execSqlCoro(
            "SELECT * FROM cards WHERE app_id=$1 AND code=$2;", appIdStr_, data["Key"].asString());
        if (!rc.empty()) {
            result["status"] = ERR_OK;
            result["maker_name"] = rc[0]["maker_name"].isNull() ? "" : rc[0]["maker_name"].as<std::string>();
            result["created_at"] = static_cast<Json::Int64>(rc[0]["created_at"].as<long long>());
            result["frozen"] = rc[0]["frozen"].as<int>();
            result["hours"] = rc[0]["hours"].isNull() ? 0 : rc[0]["hours"].as<int>();
        }
    }
    co_return result;
}

drogon::Task<Json::Value> VerifyApp::doNotice() {
    Json::Value result;
    result["status"] = ERR_OK;
    result["Notice"] = "";
    const auto s = co_await readSettings(appIdStr_);
    if (s["notice"]["enabled"].asBool()) result["Notice"] = s["notice"]["text"].asString();
    co_return result;
}

drogon::Task<Json::Value> VerifyApp::doUpdateInfo() {
    Json::Value result;
    result["status"] = ERR_OK;
    const auto s = co_await readSettings(appIdStr_);
    result["update"] = s["update"];
    result["updatelog"] = s["changelog"]["text"];
    co_return result;
}


} // namespace aegis::tcp
