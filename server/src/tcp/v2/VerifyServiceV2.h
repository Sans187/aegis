#pragma once
// VerifyServiceV2 —— v2 协议（结构体直发 + 会话握手 + AEAD）的 TCP 服务。
// 本轮聚焦“解析 / 分发”：帧拆分、Header 校验、握手建会话、body 解密、按 api 路由。
// 业务逻辑（登录/换绑/查询…接 DB）将在 dispatch 的各 case 内逐步补齐。
//
// 注意：尚未接入 main.cc，仅独立编译；切换时再注册并选定端口。

#include <trantor/net/TcpServer.h>
#include <trantor/net/EventLoop.h>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <cstdint>

#include "proto/aegis_proto.h"
#include "tcp/v2/Session.h"
#include "tcp/VerifyApp.h"

namespace aegis::tcp::v2 {

class VerifyServiceV2 {
public:
    VerifyServiceV2(trantor::EventLoop* loop, uint16_t port);
    void start();
    void stop();

    void reloadApps();          // 从 apps 表重建注册表
    void addApp(int appId);
    void removeApp(int appId);
    static VerifyServiceV2* instance() { return instance_; }

private:
    std::shared_ptr<VerifyApp> getApp(int appId);

    std::mutex                                          mtx_;
    std::unordered_map<int, std::shared_ptr<VerifyApp>> apps_;
    static VerifyServiceV2*                             instance_;

    void onConnection(const trantor::TcpConnectionPtr& conn);
    void onMessage(const trantor::TcpConnectionPtr& conn, trantor::MsgBuffer* buf);

    // 处理一条完整、已校验的消息（handshake 明文 / 其余需解密）。
    void handleMessage(const trantor::TcpConnectionPtr& conn,
                       const aegis::proto::Header& hdr, const std::string& body);

    // 分发到具体 api（已解密的明文 body）。
    void dispatch(const trantor::TcpConnectionPtr& conn,
                  const aegis::proto::Header& hdr, const std::string& plain);

    // 组帧并加密发送一个响应结构体。
    template <typename Resp>
    void sendResp(const trantor::TcpConnectionPtr& conn, const aegis::proto::Header& reqHdr,
                  const Resp& resp);

    // 业务协程抛异常时的统一兜底（记日志 + 回 St_Exception）。
    void sendErr(const trantor::TcpConnectionPtr& conn, const aegis::proto::Header& reqHdr, const char* what);

    std::unique_ptr<trantor::TcpServer> server_;
};

} // namespace aegis::tcp::v2
