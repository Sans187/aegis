#include "db/Migrate.h"
#include "security/Password.h"

#include <drogon/drogon.h>
#include <drogon/utils/Utilities.h>

#include <fstream>
#include <sstream>
#include <vector>
#include <stdexcept>

namespace db {

// 去掉 -- 行注释，并按 ';' 切分为独立语句。
// 注意：本 schema 不含字符串字面量内的 ';' 或 '--'，故此简单切分是安全的。
static std::vector<std::string> splitStatements(const std::string& sql) {
    std::string cleaned;
    cleaned.reserve(sql.size());
    std::istringstream in(sql);
    std::string line;
    while (std::getline(in, line)) {
        auto pos = line.find("--");
        if (pos != std::string::npos) line.erase(pos);
        cleaned += line;
        cleaned += '\n';
    }

    std::vector<std::string> stmts;
    std::string cur;
    for (char c : cleaned) {
        if (c == ';') {
            // trim
            size_t a = cur.find_first_not_of(" \t\r\n");
            if (a != std::string::npos) {
                size_t b = cur.find_last_not_of(" \t\r\n");
                stmts.push_back(cur.substr(a, b - a + 1));
            }
            cur.clear();
        } else {
            cur += c;
        }
    }
    return stmts;
}

void runSchema(const drogon::orm::DbClientPtr& client, const std::string& schemaPath) {
    std::ifstream f(schemaPath, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open schema file: " + schemaPath);
    std::stringstream ss;
    ss << f.rdbuf();
    const auto stmts = splitStatements(ss.str());

    for (const auto& s : stmts) {
        try {
            client->execSqlSync(s);
        } catch (const std::exception& e) {
            LOG_ERROR << "schema statement failed: " << e.what() << "\n  SQL: " << s;
            throw;
        }
    }
    LOG_INFO << "schema applied: " << stmts.size() << " statements";
}

void bootstrapAdmin(const drogon::orm::DbClientPtr& client) {
    auto r = client->execSqlSync("SELECT COUNT(*) AS n FROM agents;");
    if (r[0]["n"].as<long long>() > 0) return;  // 已有账号，跳过

    const auto& cc = drogon::app().getCustomConfig();
    std::string user, pass;
    if (!cc.isNull() && cc.isMember("bootstrap_admin")) {
        const auto& a = cc["bootstrap_admin"];
        user = a.get("username", "").asString();
        pass = a.get("password", "").asString();
    }
    if (user.empty() || pass.empty()) {
        LOG_WARN << "bootstrap_admin not configured; no admin created";
        return;
    }

    const std::string hash = security::hashPassword(pass);
    const long long now = trantor::Date::now().secondsSinceEpoch();
    client->execSqlSync(
        "INSERT INTO agents(id, username, password_hash, must_change_password, "
        "nickname, level, parent_id, apps, perms, status, created_at) "
        "VALUES($1,$2,$3,FALSE,$4,1,NULL,$5,$6,'active',$7);",
        std::string("root-1"), user, hash, std::string("admin"),
        std::string("[]"),
        std::string(R"(["batchData","cardmaking","delCardAndUser","allowAddChild","unbindOp","freezeOp","extenduser"])"),
        now);

    LOG_INFO << "bootstrap admin created: " << user;
}

void seedApps(const drogon::orm::DbClientPtr& client) {
    auto r = client->execSqlSync("SELECT COUNT(*) AS n FROM apps;");
    if (r[0]["n"].as<long long>() > 0) return;

    const long long now = trantor::Date::now().secondsSinceEpoch();
    for (int id = 100001; id <= 100005; ++id) {
        const std::string appId = std::to_string(id);
        client->execSqlSync(
            "INSERT INTO apps(id, owner_id, name, mode, created_at) VALUES($1,$2,$3,'card',$4) "
            "ON CONFLICT (id) DO NOTHING;",
            appId, std::string("root-1"), std::string("App ") + appId, now);
        client->execSqlSync(
            "INSERT INTO app_settings(app_id, settings, updated_at) VALUES($1,'{}'::jsonb,$2) "
            "ON CONFLICT (app_id) DO NOTHING;",
            appId, now);
    }
    LOG_INFO << "seeded 5 default apps (100001-100005)";
}

} // namespace db
