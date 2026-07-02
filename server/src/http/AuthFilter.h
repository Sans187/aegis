#pragma once
// AuthFilter —— 后台接口鉴权。校验 Authorization: Bearer <token> 对应的会话，
// 通过则把 agent_id 注入 req 属性 "uid"，否则 401。

#include <drogon/HttpFilter.h>

namespace aegis {

class AuthFilter : public drogon::HttpFilter<AuthFilter> {
public:
    void doFilter(const drogon::HttpRequestPtr& req,
                  drogon::FilterCallback&& fcb,
                  drogon::FilterChainCallback&& fccb) override;
};

} // namespace aegis
