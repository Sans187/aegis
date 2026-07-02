#include "http/StatsController.h"
#include "http/AuthFilter.h"
#include "http/Resp.h"
#include "http/Permissions.h"

#include <drogon/drogon.h>
#include <ctime>
#include <cstdio>
#include <unordered_map>
#include <string>
#include <vector>
#include <algorithm>
#include <memory>

using namespace drogon;
using namespace aegis::http;

namespace {
// 解析 agents.card_types（JSON 文本）为对象 {app:{type:price}}（兼容旧数组结构）
Json::Value parseObj(const std::string& s) {
    Json::Value out(Json::objectValue), tmp;
    Json::CharReaderBuilder b; std::string errs;
    std::unique_ptr<Json::CharReader> rd(b.newCharReader());
    if (rd->parse(s.data(), s.data() + s.size(), &tmp, &errs) && tmp.isObject()) out = tmp;
    return out;
}
// 公历 Y-M-D 距 1970-01-01 的天数（Howard Hinnant 算法）
long long daysFromCivil(int y, unsigned m, unsigned d) {
    y -= (m <= 2);
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<long long>(doe) - 719468;
}
} // namespace

namespace aegis {

drogon::Task<> StatsController::overview(HttpRequestPtr req,
                                         std::function<void(const HttpResponsePtr&)> cb) {
    const auto uid = req->getAttributes()->get<std::string>("uid");
    const bool super = co_await perm::isSuper(uid);

    auto db = app().getDbClient();
    // 计数一律按"本人子树"过滤：超管的子树=全部代理，自然统计到全部；
    // 普通代理只统计到自己名下（maker_id/agent_id ∈ 子树）的卡和用户。
    auto rows = co_await db->execSqlCoro(
        "WITH RECURSIVE sub AS ("
        "  SELECT id FROM agents WHERE id=$1 "
        "  UNION ALL SELECT a.id FROM agents a JOIN sub ON a.parent_id=sub.id) "
        "SELECT a.id, a.name, "
        "  (SELECT COUNT(*) FROM cards c WHERE c.app_id=a.id AND c.maker_id IN (SELECT id FROM sub)) AS cards_total, "
        "  (SELECT COUNT(*) FROM cards c WHERE c.app_id=a.id AND c.maker_id IN (SELECT id FROM sub) AND c.status='unused') AS cards_unused, "
        "  (SELECT COUNT(*) FROM users u WHERE u.app_id=a.id AND u.agent_id IN (SELECT id FROM sub)) AS users_total, "
        "  (SELECT COUNT(*) FROM users u WHERE u.app_id=a.id AND u.agent_id IN (SELECT id FROM sub) AND u.online=1) AS users_online "
        "FROM apps a ORDER BY a.id;",
        uid);

    Json::Value items(Json::arrayValue);
    long long tCards = 0, tUnused = 0, tUsers = 0, tOnline = 0;
    int appCount = 0;
    for (const auto& r : rows) {
        const auto appId = r["id"].as<std::string>();
        // 只展示本人可访问的软件（超管全可）
        if (!super && !co_await perm::canAccessApp(uid, appId)) continue;

        const long long ct = r["cards_total"].as<long long>();
        const long long cu = r["cards_unused"].as<long long>();
        const long long ut = r["users_total"].as<long long>();
        const long long uo = r["users_online"].as<long long>();
        tCards += ct; tUnused += cu; tUsers += ut; tOnline += uo; ++appCount;

        Json::Value it;
        it["app_id"] = appId;
        it["name"] = r["name"].as<std::string>();
        it["cards_total"] = ct;
        it["cards_unused"] = cu;
        it["cards_used"] = ct - cu;
        it["users_total"] = ut;
        it["users_online"] = uo;
        items.append(it);
    }

    Json::Value totals;
    totals["apps"] = appCount;
    totals["cards_total"] = static_cast<Json::Int64>(tCards);
    totals["cards_unused"] = static_cast<Json::Int64>(tUnused);
    totals["cards_used"] = static_cast<Json::Int64>(tCards - tUnused);
    totals["users_total"] = static_cast<Json::Int64>(tUsers);
    totals["users_online"] = static_cast<Json::Int64>(tOnline);

    // ============ 今日数据 + 最近 14 天每日趋势（按服务器时区分天）============
    int tzMin = 480;  // 默认 UTC+8
    {
        const auto& cc = app().getCustomConfig();
        if (!cc.isNull() && cc.isMember("tz_offset_minutes")) tzMin = cc["tz_offset_minutes"].asInt();
    }
    const long long offsetSec = static_cast<long long>(tzMin) * 60;
    const long long now = static_cast<long long>(time(nullptr));
    const long long localDayNow = (now + offsetSec) / 86400;       // 本地“天”序号
    const int N = 14;
    const long long sinceDay = localDayNow - (N - 1);
    const long long sinceEpoch = sinceDay * 86400 - offsetSec;     // N 天前本地 0 点
    const long long todayStart = localDayNow * 86400 - offsetSec;  // 今日本地 0 点

    const std::string subCte =
        "WITH RECURSIVE sub AS (SELECT id FROM agents WHERE id=$1 "
        "UNION ALL SELECT a.id FROM agents a JOIN sub ON a.parent_id=sub.id) ";

    Json::Value today;
    {
        auto t = co_await db->execSqlCoro(subCte +
            "SELECT "
            " (SELECT COUNT(*) FROM users u WHERE u.agent_id IN (SELECT id FROM sub) AND u.created_at>=$2) AS reg_today, "
            " (SELECT COUNT(*) FROM cards c WHERE c.maker_id IN (SELECT id FROM sub) AND c.used_at>=$2 "
            "    AND EXISTS (SELECT 1 FROM users u WHERE u.app_id=c.app_id AND u.username=c.used_by AND u.frozen=1)) AS frozen_today, "
            " (SELECT COUNT(*) FROM cards c WHERE c.maker_id IN (SELECT id FROM sub) AND c.used_at>=$2)     AS act_today;",
            uid, todayStart);
        today["registrations"] = static_cast<Json::Int64>(t[0]["reg_today"].as<long long>());
        today["frozen"]        = static_cast<Json::Int64>(t[0]["frozen_today"].as<long long>());
        today["activations"]   = static_cast<Json::Int64>(t[0]["act_today"].as<long long>());
    }

    // ============ 今日销售额（我的实收）============
    // 多级分销定价：cards.price 是“制卡人付给其上级”的价；每级加价。对 viewer(uid) 而言，
    // 下线无论多深的每一笔，我能收到的只是“我给我的直属下级设定的价”。所以把每张卡按
    // “它所在那条直属下级分支根”的定价折算；自己的卡按其制卡价。冻结用户的卡不计入。
    // 1) 直属下级的定价表 priceMap[childId][appId][typeId] = 该下级付我的单价
    std::unordered_map<std::string,
        std::unordered_map<std::string, std::unordered_map<std::string, double>>> priceMap;
    {
        auto dc = co_await db->execSqlCoro("SELECT id, card_types FROM agents WHERE parent_id=$1;", uid);
        for (const auto& r : dc) {
            const auto cid = r["id"].as<std::string>();
            Json::Value ct = parseObj(r["card_types"].as<std::string>());   // {app:{type:price}}
            for (const auto& app : ct.getMemberNames()) {
                const Json::Value& types = ct[app];
                if (!types.isObject()) continue;   // 旧数组结构无价格 → 跳过，后面退回制卡价
                for (const auto& tp : types.getMemberNames())
                    priceMap[cid][app][tp] = types[tp].asDouble();
            }
        }
    }
    // 2) 每个后代 → 其所属“直属下级分支根”
    std::unordered_map<std::string, std::string> branchOf;
    {
        auto bm = co_await db->execSqlCoro(
            "WITH RECURSIVE sub AS ("
            "  SELECT id, id AS branch FROM agents WHERE parent_id=$1 "
            "  UNION ALL SELECT a.id, s.branch FROM agents a JOIN sub s ON a.parent_id=s.id) "
            "SELECT id, branch FROM sub;", uid);
        for (const auto& r : bm) branchOf[r["id"].as<std::string>()] = r["branch"].as<std::string>();
    }
    // 3) 下线今日已激活的卡（排除冻结用户），按 maker/app/type 聚合后逐组折算
    std::unordered_map<std::string, double> salesByAgent;   // agentId -> 我的实收
    double totalSales = 0;
    {
        auto cg = co_await db->execSqlCoro(subCte +
            "SELECT c.maker_id, c.app_id, c.type_id, COUNT(*) AS cnt, COALESCE(SUM(c.price),0) AS sump "
            "FROM cards c WHERE c.maker_id IN (SELECT id FROM sub) AND c.used_at>=$2 "
            "  AND NOT EXISTS (SELECT 1 FROM users u WHERE u.app_id=c.app_id AND u.username=c.used_by AND u.frozen=1) "
            "GROUP BY c.maker_id, c.app_id, c.type_id;",
            uid, todayStart);
        for (const auto& r : cg) {
            const auto maker  = r["maker_id"].as<std::string>();
            const auto appId  = r["app_id"].as<std::string>();
            const auto typeId = r["type_id"].isNull() ? std::string() : r["type_id"].as<std::string>();
            const long long cnt = r["cnt"].as<long long>();
            const double sump   = r["sump"].as<double>();   // 制卡价合计（退回用）
            double val;
            if (maker == uid) {
                val = sump;                                  // 自己的卡：按制卡价
            } else {
                double unit = -1;
                auto bit = branchOf.find(maker);
                if (bit != branchOf.end()) {
                    auto pb = priceMap.find(bit->second);
                    if (pb != priceMap.end()) {
                        auto pa = pb->second.find(appId);
                        if (pa != pb->second.end()) {
                            auto pt = pa->second.find(typeId);
                            if (pt != pa->second.end()) unit = pt->second;
                        }
                    }
                }
                val = (unit >= 0) ? unit * static_cast<double>(cnt) : sump;  // 找不到定价 → 退回制卡价
            }
            salesByAgent[maker] += val;
            totalSales += val;
        }
    }
    today["sales"] = totalSales;
    // 注：旗下代理逐日销售明细（含卡种拆分）改由独立接口 /api/stats/agent-sales 提供（支持选日期）

    // 每日注册 / 每日激活
    std::unordered_map<long long, long long> regMap, actMap;
    {
        auto du = co_await db->execSqlCoro(subCte +
            "SELECT ((u.created_at + $3)/86400) AS d, COUNT(*) AS c FROM users u "
            "WHERE u.agent_id IN (SELECT id FROM sub) AND u.created_at>=$2 GROUP BY d;",
            uid, sinceEpoch, offsetSec);
        for (const auto& r : du) regMap[r["d"].as<long long>()] = r["c"].as<long long>();
        auto da = co_await db->execSqlCoro(subCte +
            "SELECT ((c.used_at + $3)/86400) AS d, COUNT(*) AS c FROM cards c "
            "WHERE c.maker_id IN (SELECT id FROM sub) AND c.used_at IS NOT NULL AND c.used_at>=$2 GROUP BY d;",
            uid, sinceEpoch, offsetSec);
        for (const auto& r : da) actMap[r["d"].as<long long>()] = r["c"].as<long long>();
    }

    Json::Value daily(Json::arrayValue);
    for (long long d = sinceDay; d <= localDayNow; ++d) {
        char buf[16];
        const time_t tt = static_cast<time_t>(d * 86400);   // 该本地天对应日期
        struct tm tmv{};
#ifdef _WIN32
        gmtime_s(&tmv, &tt);
#else
        gmtime_r(&tt, &tmv);
#endif
        std::snprintf(buf, sizeof(buf), "%02d-%02d", tmv.tm_mon + 1, tmv.tm_mday);
        Json::Value o;
        o["date"] = buf;
        auto rit = regMap.find(d); o["registrations"] = static_cast<Json::Int64>(rit == regMap.end() ? 0 : rit->second);
        auto ait = actMap.find(d); o["activations"]   = static_cast<Json::Int64>(ait == actMap.end() ? 0 : ait->second);
        daily.append(o);
    }

    // ============ 近 10 天逐小时激活（曲线图：按天叠加，X 轴=小时）============
    const int HD = 10;
    const long long since10Day = localDayNow - (HD - 1);
    const long long since10Epoch = since10Day * 86400 - offsetSec;
    const long long curHour = ((now + offsetSec) % 86400) / 3600;
    std::unordered_map<long long, long long> hourMap;   // key = day*100 + hour
    {
        auto hr = co_await db->execSqlCoro(subCte +
            "SELECT ((c.used_at + $3)/86400) AS d, (((c.used_at + $3)%86400)/3600) AS h, COUNT(*) AS c "
            "FROM cards c WHERE c.maker_id IN (SELECT id FROM sub) AND c.used_at IS NOT NULL AND c.used_at>=$2 "
            "GROUP BY d, h;",
            uid, since10Epoch, offsetSec);
        for (const auto& r : hr)
            hourMap[r["d"].as<long long>() * 100 + r["h"].as<long long>()] = r["c"].as<long long>();
    }
    Json::Value hourly(Json::arrayValue);
    for (long long d = since10Day; d <= localDayNow; ++d) {
        char buf[16];
        const time_t tt = static_cast<time_t>(d * 86400);
        struct tm tmv{};
#ifdef _WIN32
        gmtime_s(&tmv, &tt);
#else
        gmtime_r(&tt, &tmv);
#endif
        std::snprintf(buf, sizeof(buf), "%02d-%02d", tmv.tm_mon + 1, tmv.tm_mday);
        const bool isToday = (d == localDayNow);
        Json::Value day;
        day["date"] = buf;
        day["today"] = isToday;
        Json::Value hrs(Json::arrayValue);
        const long long lastH = isToday ? curHour : 23;   // 今天只到当前小时
        for (long long h = 0; h <= lastH; ++h) {
            auto it = hourMap.find(d * 100 + h);
            hrs.append(static_cast<Json::Int64>(it == hourMap.end() ? 0 : it->second));
        }
        day["hours"] = hrs;
        hourly.append(day);
    }

    Json::Value v; v["ok"] = true; v["items"] = items; v["totals"] = totals;
    v["today"] = today; v["daily"] = daily; v["hourly"] = hourly;
    cb(jsonResp(v));
    co_return;
}

// ============ 旗下代理某日销售明细（含按卡种销量拆分）============
drogon::Task<> StatsController::agentSales(HttpRequestPtr req,
                                           std::function<void(const HttpResponsePtr&)> cb) {
    const auto uid = req->getAttributes()->get<std::string>("uid");
    auto db = app().getDbClient();

    int tzMin = 480;
    {
        const auto& cc = app().getCustomConfig();
        if (!cc.isNull() && cc.isMember("tz_offset_minutes")) tzMin = cc["tz_offset_minutes"].asInt();
    }
    const long long offsetSec = static_cast<long long>(tzMin) * 60;
    const long long now = static_cast<long long>(time(nullptr));
    long long localDay = (now + offsetSec) / 86400;                    // 默认今天
    {
        const std::string ds = req->getParameter("date");             // 期望 YYYY-MM-DD
        int yy = 0, mm = 0, dd = 0;
        if (ds.size() == 10 && std::sscanf(ds.c_str(), "%d-%d-%d", &yy, &mm, &dd) == 3 &&
            mm >= 1 && mm <= 12 && dd >= 1 && dd <= 31) {
            localDay = daysFromCivil(yy, static_cast<unsigned>(mm), static_cast<unsigned>(dd));
        }
    }
    const long long dayStart = localDay * 86400 - offsetSec;
    const long long dayEnd   = dayStart + 86400;
    char dateBuf[16];
    {
        const time_t tt = static_cast<time_t>(localDay * 86400);
        struct tm tmv{};
#ifdef _WIN32
        gmtime_s(&tmv, &tt);
#else
        gmtime_r(&tt, &tmv);
#endif
        std::snprintf(dateBuf, sizeof(dateBuf), "%04d-%02d-%02d", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
    }

    const std::string subCte =
        "WITH RECURSIVE sub AS (SELECT id FROM agents WHERE id=$1 "
        "UNION ALL SELECT a.id FROM agents a JOIN sub ON a.parent_id=sub.id) ";

    // 1) 直属下级定价表 priceMap[childId][app][type]（= 该下级付我的单价）
    std::unordered_map<std::string,
        std::unordered_map<std::string, std::unordered_map<std::string, double>>> priceMap;
    {
        auto dc = co_await db->execSqlCoro("SELECT id, card_types FROM agents WHERE parent_id=$1;", uid);
        for (const auto& r : dc) {
            const auto cid = r["id"].as<std::string>();
            Json::Value ct = parseObj(r["card_types"].as<std::string>());
            for (const auto& app : ct.getMemberNames()) {
                if (!ct[app].isObject()) continue;
                for (const auto& tp : ct[app].getMemberNames())
                    priceMap[cid][app][tp] = ct[app][tp].asDouble();
            }
        }
    }
    // 2) 每个后代 → 直属下级分支根
    std::unordered_map<std::string, std::string> branchOf;
    {
        auto bm = co_await db->execSqlCoro(
            "WITH RECURSIVE sub AS ("
            "  SELECT id, id AS branch FROM agents WHERE parent_id=$1 "
            "  UNION ALL SELECT a.id, s.branch FROM agents a JOIN sub s ON a.parent_id=s.id) "
            "SELECT id, branch FROM sub;", uid);
        for (const auto& r : bm) branchOf[r["id"].as<std::string>()] = r["branch"].as<std::string>();
    }
    // 3) 当日卡，按 maker/app/type 聚合：总量、净量（排除冻结用户）、制卡单价
    struct AgInfo {
        long long act = 0, frozen = 0;
        double sales = 0;
        // 按卡种拆分：typeId -> {count, sales}
        std::unordered_map<std::string, std::pair<long long, double>> byType;
    };
    std::unordered_map<std::string, AgInfo> ag;
    std::unordered_map<std::string, std::string> typeName;   // typeId -> name（缓存）
    {
        auto cg = co_await db->execSqlCoro(subCte +
            "SELECT c.maker_id, c.app_id, c.type_id, COUNT(*) AS cnt, "
            "  COUNT(*) FILTER (WHERE NOT EXISTS (SELECT 1 FROM users u WHERE u.app_id=c.app_id AND u.username=c.used_by AND u.frozen=1)) AS net_cnt, "
            "  MAX(c.price) AS unit_price "
            "FROM cards c WHERE c.maker_id IN (SELECT id FROM sub) AND c.used_at>=$2 AND c.used_at<$3 "
            "GROUP BY c.maker_id, c.app_id, c.type_id;",
            uid, dayStart, dayEnd);
        for (const auto& r : cg) {
            const auto maker  = r["maker_id"].as<std::string>();
            const auto appId  = r["app_id"].as<std::string>();
            const auto typeId = r["type_id"].isNull() ? std::string() : r["type_id"].as<std::string>();
            const long long cnt     = r["cnt"].as<long long>();
            const long long netCnt  = r["net_cnt"].as<long long>();
            const double unitPrice  = r["unit_price"].isNull() ? 0.0 : r["unit_price"].as<double>();

            // 我对这张卡的实收单价
            double unit;
            if (maker == uid) {
                unit = unitPrice;                       // 自己的卡：制卡价
            } else {
                unit = unitPrice;                       // 默认退回制卡价
                auto bit = branchOf.find(maker);
                if (bit != branchOf.end()) {
                    auto pb = priceMap.find(bit->second);
                    if (pb != priceMap.end()) {
                        auto pa = pb->second.find(appId);
                        if (pa != pb->second.end()) {
                            auto pt = pa->second.find(typeId);
                            if (pt != pa->second.end()) unit = pt->second;
                        }
                    }
                }
            }
            const double groupSales = unit * static_cast<double>(netCnt);

            AgInfo& a = ag[maker];
            a.act    += cnt;
            a.frozen += (cnt - netCnt);
            a.sales  += groupSales;
            auto& bt = a.byType[typeId];
            bt.first  += cnt;            // 销量按激活总数（直观看卖了多少张）
            bt.second += groupSales;     // 该卡种我的实收

            if (!typeId.empty() && !typeName.count(typeId)) {
                auto tn = co_await db->execSqlCoro("SELECT name FROM card_types WHERE id=$1 LIMIT 1;", typeId);
                typeName[typeId] = tn.empty() ? std::string() : tn[0]["name"].as<std::string>();
            }
        }
    }

    // 4) 取所有子树代理（含没有销售的，保证列表完整），组装 + 排序
    struct Row {
        std::string id, username, nickname;
        long long act = 0, frozen = 0;
        double sales = 0;
        std::unordered_map<std::string, std::pair<long long, double>> byType;
    };
    std::vector<Row> rows;
    double totalSales = 0;
    {
        auto al = co_await db->execSqlCoro(subCte +
            "SELECT a.id, a.username, a.nickname FROM agents a WHERE a.id IN (SELECT id FROM sub);", uid);
        for (const auto& r : al) {
            Row row;
            row.id       = r["id"].as<std::string>();
            row.username = r["username"].as<std::string>();
            row.nickname = r["nickname"].isNull() ? std::string() : r["nickname"].as<std::string>();
            auto it = ag.find(row.id);
            if (it != ag.end()) {
                row.act = it->second.act; row.frozen = it->second.frozen; row.sales = it->second.sales;
                row.byType = it->second.byType;
            }
            totalSales += row.sales;
            rows.push_back(std::move(row));
        }
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        if (a.sales != b.sales) return a.sales > b.sales;
        if (a.act   != b.act)   return a.act   > b.act;
        return a.username < b.username;
    });

    Json::Value agents(Json::arrayValue);
    for (const auto& r : rows) {
        Json::Value o;
        o["username"]    = r.username;
        o["nickname"]    = r.nickname;
        o["activations"] = static_cast<Json::Int64>(r.act);
        o["frozen"]      = static_cast<Json::Int64>(r.frozen);
        o["sales"]       = r.sales;
        // 卡种拆分（按销量降序）
        std::vector<std::pair<std::string, std::pair<long long, double>>> tv(r.byType.begin(), r.byType.end());
        std::sort(tv.begin(), tv.end(), [](const auto& x, const auto& y) {
            return x.second.first > y.second.first;
        });
        Json::Value types(Json::arrayValue);
        for (const auto& t : tv) {
            Json::Value to;
            const auto nit = typeName.find(t.first);
            to["name"]  = (nit != typeName.end() && !nit->second.empty()) ? nit->second : std::string("未分类");
            to["count"] = static_cast<Json::Int64>(t.second.first);
            to["sales"] = t.second.second;
            types.append(to);
        }
        o["types"] = types;
        agents.append(o);
    }

    Json::Value v;
    v["ok"] = true; v["date"] = dateBuf; v["total_sales"] = totalSales; v["agents"] = agents;
    cb(jsonResp(v));
    co_return;
}

} // namespace aegis
