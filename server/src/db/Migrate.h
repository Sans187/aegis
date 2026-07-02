#pragma once
#include <drogon/orm/DbClient.h>
#include <string>

namespace db {

// 读取并执行 sql/schema.sql（幂等）。statements 以 ';' 分隔逐条执行，
// 因为 PostgreSQL 扩展协议不允许单次多语句。失败抛异常。
void runSchema(const drogon::orm::DbClientPtr& client, const std::string& schemaPath);

// 若 agents 表为空，用 custom_config.bootstrap_admin 创建首个超管（Argon2id 哈希，
// 并置 must_change_password = true）。已存在则跳过。
void bootstrapAdmin(const drogon::orm::DbClientPtr& client);

// 若 apps 表为空，种入 5 个固定应用（100001-100005，对应 TCP app_id）及空设置。
void seedApps(const drogon::orm::DbClientPtr& client);

} // namespace db
