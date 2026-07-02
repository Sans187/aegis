#pragma once
// AppSettings —— 应用维度的设置（换绑策略、公告、更新、验证参数）。
// 默认值与旧 appstore::defaultSettings 保持一致，保证行为兼容。

#include <drogon/drogon.h>
#include <json/json.h>
#include <memory>
#include <string>

namespace aegis::tcp {

inline Json::Value defaultSettings() {
    Json::Value s;
    s["rebind"]["allow"]              = true;
    s["rebind"]["cooldownMinutes"]    = 0;
    s["rebind"]["maxTimes"]           = 0;
    s["rebind"]["unbindDeductHours"]  = 0;

    s["notice"]["enabled"]            = false;
    s["notice"]["text"]               = "";

    s["update"]["latestVersion"]      = "";
    s["update"]["forceUpdate"]        = false;
    s["update"]["minVersion"]         = "";
    s["update"]["downloadUrl"]        = "";

    s["changelog"]["text"]            = "";

    s["verify"]["tokenTtlHours"]      = 24;
    return s;
}

// 缺字段用默认值递归补齐（兼容旧版本 settings）。
inline void mergeDefaults(Json::Value& out, const Json::Value& def) {
    if (def.isNull()) return;
    if (def.isObject()) {
        if (!out.isObject()) { out = def; return; }
        for (auto it = def.begin(); it != def.end(); ++it) {
            const std::string k = it.name();
            const Json::Value& dv = def[k];
            if (out[k].isNull()) { out[k] = dv; continue; }
            if (dv.isObject()) {
                if (!out[k].isObject()) out[k] = dv;
                else mergeDefaults(out[k], dv);
            } else if (out[k].type() != dv.type()) {
                out[k] = dv;
            }
        }
        return;
    }
    if (out.isNull() || out.type() != def.type()) out = def;
}

// 异步读取设置（app_settings.settings 为 JSONB），缺失或解析失败回退默认值。
inline drogon::Task<Json::Value> readSettings(const std::string& app_id) {
    try {
        auto db = drogon::app().getDbClient();
        auto r = co_await db->execSqlCoro(
            "SELECT settings::text AS settings FROM app_settings WHERE app_id=$1;", app_id);
        if (r.empty()) co_return defaultSettings();

        Json::Value out;
        Json::CharReaderBuilder b;
        std::string errs;
        const auto s = r[0]["settings"].as<std::string>();
        std::unique_ptr<Json::CharReader> rd(b.newCharReader());
        if (!rd->parse(s.data(), s.data() + s.size(), &out, &errs))
            co_return defaultSettings();

        Json::Value def = defaultSettings();
        mergeDefaults(out, def);
        co_return out;
    } catch (const std::exception& e) {
        LOG_WARN << "readSettings(" << app_id << "): " << e.what();
        co_return defaultSettings();
    }
}

} // namespace aegis::tcp
