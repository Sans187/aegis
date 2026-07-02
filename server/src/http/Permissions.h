#pragma once
// Permissions —— 代理树与权限判定（PostgreSQL）。
// 核心安全点：列表/操作一律服务端强制按"子代理子树"过滤，不依赖前端字段。

#include <drogon/drogon.h>
#include <string>
#include <vector>

namespace aegis::perm {

// 取该代理自身 + 所有下级（递归 CTE，一条 SQL）。
drogon::Task<std::vector<std::string>> subAgentIds(std::string agentId);

// 该代理是否拥有某权限键（level==1 超管直接放行）。
drogon::Task<bool> hasPermission(std::string agentId, std::string permKey);

// 代理等级（找不到返回 -1）。
drogon::Task<int> agentLevel(std::string agentId);

// 该代理是否可访问某 app（超管全可；否则需在其 apps 列表内或为该 app owner）。
// 用于"读"场景（看列表、看设置）。
drogon::Task<bool> canAccessApp(std::string agentId, std::string appId);

// 该代理是否可"管理"某 app 的全局设置（仅超管或该 app 属主）。
// 用于"写全局配置"场景，比 canAccessApp 更严，防止下级篡改影响全 app 的设置。
drogon::Task<bool> canManageApp(std::string agentId, std::string appId);

// 是否超级管理员（level==1）。全局区/敏感操作的总闸。
drogon::Task<bool> isSuper(std::string agentId);

// 校验该代理的安全密码（二级密码）。未设置则返回 false。
drogon::Task<bool> checkSecurityPassword(std::string agentId, std::string password);

// 余额账目（共享额度池模型）。
//   available = balance - 已分配给子代的额度 - 未用卡占用(held) - 已激活消耗(consumed)
// 超管视为无限额度。
struct BalanceInfo {
    bool   super{false};
    double balance{0};
    double consumed{0};
    double childrenAlloc{0};   // 直接子代 balance 之和
    double held{0};            // 本人未使用卡密的 price 之和
    double available() const { return super ? 1e18 : balance - childrenAlloc - held - consumed; }
};
drogon::Task<BalanceInfo> balanceInfo(std::string agentId);

} // namespace aegis::perm
