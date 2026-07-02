#include "tcp/v2/VerifyServiceV2.h"

#include <drogon/drogon.h>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <unordered_set>

using namespace aegis::proto;
using namespace drogon;

namespace aegis::tcp::v2 {

VerifyServiceV2* VerifyServiceV2::instance_ = nullptr;

static std::unordered_set<int> fullApiSet() {
    return {300, 301, 302, 303, 304, 305, 306, 307, 500};  // login/rebind/query/notice/update/unbind/register/recharge/verify
}

namespace {

int64_t nowSec() { return static_cast<int64_t>(::time(nullptr)); }

uint32_t readU32LE(const char* p) {
    return  (static_cast<uint32_t>(static_cast<uint8_t>(p[0]))      ) |
            (static_cast<uint32_t>(static_cast<uint8_t>(p[1])) <<  8) |
            (static_cast<uint32_t>(static_cast<uint8_t>(p[2])) << 16) |
            (static_cast<uint32_t>(static_cast<uint8_t>(p[3])) << 24);
}

void appendU32LE(std::string& s, uint32_t v) {
    s.push_back(static_cast<char>( v        & 0xff));
    s.push_back(static_cast<char>((v >>  8) & 0xff));
    s.push_back(static_cast<char>((v >> 16) & 0xff));
    s.push_back(static_cast<char>((v >> 24) & 0xff));
}

// 帧上限：长度前缀(4) + Header + body
constexpr uint32_t kMaxTotal = static_cast<uint32_t>(sizeof(Header)) + kMaxFrame;

} // namespace

VerifyServiceV2::VerifyServiceV2(trantor::EventLoop* loop, uint16_t port) {
    trantor::InetAddress addr(port);
    server_ = std::make_unique<trantor::TcpServer>(loop, addr, "AegisVerifyV2");
    instance_ = this;
}

void VerifyServiceV2::reloadApps() {
    std::unordered_map<int, std::shared_ptr<VerifyApp>> fresh;
    try {
        auto r = app().getDbClient()->execSqlSync("SELECT id FROM apps;");
        for (const auto& row : r) {
            try {
                int id = std::stoi(row["id"].as<std::string>());
                fresh[id] = std::make_shared<VerifyApp>(id, fullApiSet());
            } catch (const std::exception&) { /* 跳过非数字 id */ }
        }
    } catch (const std::exception& e) {
        LOG_ERROR << "v2 reloadApps: " << e.what();
        return;
    }
    std::lock_guard<std::mutex> lk(mtx_);
    apps_ = std::move(fresh);
    LOG_INFO << "v2 verify apps loaded: " << apps_.size();
}

void VerifyServiceV2::addApp(int appId) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!apps_.count(appId)) apps_[appId] = std::make_shared<VerifyApp>(appId, fullApiSet());
}

void VerifyServiceV2::removeApp(int appId) {
    std::lock_guard<std::mutex> lk(mtx_);
    apps_.erase(appId);
}

std::shared_ptr<VerifyApp> VerifyServiceV2::getApp(int appId) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = apps_.find(appId);
    return it == apps_.end() ? nullptr : it->second;
}

void VerifyServiceV2::start() {
    ServerKeys::instance();   // 启动即加载/生成密钥并打印公钥
    reloadApps();
    // 启动时无任何连接，清掉上次运行残留的"在线"标记（防进程重启后误显示在线）。
    try { app().getDbClient()->execSqlSync("UPDATE users SET online=0 WHERE online=1;"); }
    catch (const std::exception& e) { LOG_WARN << "v2 startup online reset: " << e.what(); }
    server_->setConnectionCallback([this](const trantor::TcpConnectionPtr& conn) { onConnection(conn); });
    server_->setRecvMessageCallback(
        [this](const trantor::TcpConnectionPtr& conn, trantor::MsgBuffer* buf) { onMessage(conn, buf); });
    server_->start();
    LOG_INFO << "Aegis TCP verify service v2 started";
}

void VerifyServiceV2::stop() { if (server_) server_->stop(); }

void VerifyServiceV2::onConnection(const trantor::TcpConnectionPtr& conn) {
    if (conn->connected()) {
        conn->setContext(std::make_shared<Session>());   // 每连接一个会话
        LOG_INFO << "v2 conn up: " << conn->peerAddr().toIpPort();
        return;
    }
    // 连接断开：把该 IP 上的在线用户置离线（恢复旧协议 onDisconnect 行为）。
    const std::string ip = conn->peerAddr().toIp();
    drogon::async_run([ip]() -> drogon::Task<> {
        try {
            co_await app().getDbClient()->execSqlCoro(
                "UPDATE users SET online=0 WHERE ip_address=$1 AND online=1;", ip);
        } catch (const std::exception& e) { LOG_WARN << "v2 offline-on-disconnect: " << e.what(); }
        co_return;
    });
}


void VerifyServiceV2::onMessage(const trantor::TcpConnectionPtr& conn, trantor::MsgBuffer* buf) {
    while (buf->readableBytes() >= sizeof(uint32_t)) {
        const uint32_t total = readU32LE(buf->peek());
        if (total < sizeof(Header) || total > kMaxTotal) {
            LOG_WARN << "v2 bad frame length " << total << " from " << conn->peerAddr().toIpPort();
            conn->shutdown();
            buf->retrieveAll();
            return;
        }
        if (buf->readableBytes() < sizeof(uint32_t) + total) return;   // 等更多数据

        buf->retrieve(sizeof(uint32_t));

        Header hdr{};
        std::memcpy(&hdr, buf->peek(), sizeof(Header));

        // Header 校验
        if (std::memcmp(hdr.magic, kMagic, sizeof(kMagic)) != 0 || hdr.version != kVersion ||
            hdr.bodyLen != total - sizeof(Header)) {
            LOG_WARN << "v2 bad header from " << conn->peerAddr().toIpPort();
            conn->shutdown();
            buf->retrieveAll();
            return;
        }

        std::string body(buf->peek() + sizeof(Header), hdr.bodyLen);
        buf->retrieve(total);

        handleMessage(conn, hdr, body);
    }
}

void VerifyServiceV2::handleMessage(const trantor::TcpConnectionPtr& conn,
                                    const Header& hdr, const std::string& body) {
    // 注意：不再用客户端时间戳(hdr.ts)做抗重放——客户端机器时钟常年不准，会误挡正常用户。
    // 跨连接重放已由「每连接临时会话密钥」从根上挡住（抓到的密文换个连接用不同密钥解不开）；
    // 连接内重放的精确防护可后续加「会话内递增序列号」，无需依赖墙上时钟。
    auto session = conn->getContext<Session>();
    if (!session) { conn->shutdown(); return; }

    // ---- 握手：明文，建立会话密钥 ----
    if (hdr.apiId == Api_Handshake) {
        if ((hdr.flags & Flag_Encrypted) || body.size() != sizeof(HandshakeReq)) {
            HandshakeResp r{St_BadRequest, {}};
            std::memcpy(r.server_pk, ServerKeys::instance().pub().data(), kPubKeyBytes);
            // 握手响应走明文
            std::string plain(reinterpret_cast<const char*>(&r), sizeof(r));
            Header rh = hdr; std::memcpy(rh.magic, kMagic, 4); rh.version = kVersion;
            rh.flags = 0; rh.ts = static_cast<uint64_t>(nowSec()); rh.bodyLen = static_cast<uint32_t>(plain.size());
            std::string hb(reinterpret_cast<const char*>(&rh), sizeof(rh));
            std::string frame; appendU32LE(frame, static_cast<uint32_t>(hb.size() + plain.size()));
            frame += hb; frame += plain;
            conn->send(frame);
            return;
        }
        HandshakeReq req{};
        std::memcpy(&req, body.data(), sizeof(req));

        HandshakeResp r{};
        r.status = session->establish(req.client_pk) ? St_OK : St_BadRequest;
        std::memcpy(r.server_pk, ServerKeys::instance().pub().data(), kPubKeyBytes);

        std::string plain(reinterpret_cast<const char*>(&r), sizeof(r));
        Header rh{};
        std::memcpy(rh.magic, kMagic, 4);
        rh.version = kVersion; rh.flags = 0; rh.apiId = Api_Handshake; rh.appId = hdr.appId;
        rh.ts = static_cast<uint64_t>(nowSec()); rh.nonce = 0;
        rh.bodyLen = static_cast<uint32_t>(plain.size());
        std::string hb(reinterpret_cast<const char*>(&rh), sizeof(rh));
        std::string frame; appendU32LE(frame, static_cast<uint32_t>(hb.size() + plain.size()));
        frame += hb; frame += plain;
        conn->send(frame);
        LOG_INFO << "v2 handshake "
                 << (r.status == St_OK ? "ok" : "FAILED") << " app=" << hdr.appId;
        return;
    }

    // ---- 其余 api：必须已握手且加密 ----
    if (!session->established() || !(hdr.flags & Flag_Encrypted)) {
        StatusResp r{St_NoToken, nowSec()};
        sendResp(conn, hdr, r);
        return;
    }

    // AAD = 收到的帧头字节，绑定密文，防篡改
    std::string aad(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
    std::string plain;
    if (!session->open(body, aad, plain)) {
        LOG_WARN << "v2 decrypt failed from " << conn->peerAddr().toIpPort();
        conn->shutdown();
        return;
    }
    dispatch(conn, hdr, plain);
}

void VerifyServiceV2::dispatch(const trantor::TcpConnectionPtr& conn,
                               const Header& hdr, const std::string& plain) {
    auto vapp = getApp(hdr.appId);
    if (!vapp) { StatusResp r{St_BadRequest, nowSec()}; sendResp(conn, hdr, r); return; }
    const std::string ip = conn->peerAddr().toIp();
    const Header h = hdr;   // 拷贝供协程使用

    switch (hdr.apiId) {
        case Api_Login:
        case Api_Register: {
            const size_t need = (hdr.apiId == Api_Login) ? sizeof(LoginReq) : sizeof(RegisterReq);
            if (plain.size() != need) { StatusResp r{St_BadRequest, nowSec()}; sendResp(conn, hdr, r); return; }
            std::string card, user, pass, hwid;
            if (hdr.apiId == Api_Login) {
                LoginReq req{}; std::memcpy(&req, plain.data(), sizeof(req));
                card = cstr(req.key); user = cstr(req.key); pass = cstr(req.pass); hwid = cstr(req.hwid);
            } else {
                RegisterReq req{}; std::memcpy(&req, plain.data(), sizeof(req));
                card = cstr(req.card); user = cstr(req.username); pass = cstr(req.pass); hwid = cstr(req.hwid);
            }
            drogon::async_run([this, conn, h, vapp, ip, card, user, pass, hwid]() -> drogon::Task<> {
                try {
                    Json::Value d;
                    d["Key"] = card; d["card"] = card; d["username"] = user; d["password"] = pass; d["hwid"] = hwid;
                    Json::Value r = co_await vapp->apiJson(h.apiId, ip, d);
                    LoginResp resp{};
                    resp.status     = r.get("status", St_KeyNotFound).asInt();
                    resp.cmdTag     = r.get("cmdTag", (Json::Int64)nowSec()).asInt64();
                    resp.expired_at = r.get("expired_at", 0).asInt64();
                    resp.created_at = r.get("created_at", 0).asInt64();
                    resp.frozen     = r.get("frozen", 0).asInt();
                    resp.hours      = r.get("hours", 0).asInt();
                    setField(resp.token, r.get("token", "").asString());
                    setField(resp.username, r.get("username", "").asString());
                    sendResp(conn, h, resp);
                } catch (const std::exception& e) { sendErr(conn, h, e.what()); }
                co_return;
            });
            return;
        }
        case Api_Recharge: {
            if (plain.size() != sizeof(RechargeReq)) { StatusResp r{St_BadRequest, nowSec()}; sendResp(conn, hdr, r); return; }
            RechargeReq req{}; std::memcpy(&req, plain.data(), sizeof(req));
            std::string user = cstr(req.username), card = cstr(req.card);
            drogon::async_run([this, conn, h, vapp, ip, user, card]() -> drogon::Task<> {
                try {
                    Json::Value d; d["username"] = user; d["card"] = card;
                    Json::Value r = co_await vapp->apiJson(h.apiId, ip, d);
                    RechargeResp resp{};
                    resp.status     = r.get("status", St_KeyNotFound).asInt();
                    resp.cmdTag     = r.get("cmdTag", (Json::Int64)nowSec()).asInt64();
                    resp.expired_at = r.get("expired_at", 0).asInt64();
                    setField(resp.username, r.get("username", "").asString());
                    sendResp(conn, h, resp);
                } catch (const std::exception& e) { sendErr(conn, h, e.what()); }
                co_return;
            });
            return;
        }
        case Api_Rebind: {
            if (plain.size() != sizeof(RebindReq)) { StatusResp r{St_BadRequest, nowSec()}; sendResp(conn, hdr, r); return; }
            RebindReq req{}; std::memcpy(&req, plain.data(), sizeof(req));
            std::string key = cstr(req.key), hwid = cstr(req.hwid);
            drogon::async_run([this, conn, h, vapp, ip, key, hwid]() -> drogon::Task<> {
                try {
                    Json::Value d; d["Key"] = key; d["username"] = key; d["hwid"] = hwid;
                    Json::Value r = co_await vapp->apiJson(h.apiId, ip, d);
                    RebindResp resp{};
                    resp.status            = r.get("status", St_KeyNotFound).asInt();
                    resp.cmdTag            = r.get("cmdTag", (Json::Int64)nowSec()).asInt64();
                    resp.expired_at        = r.get("expired_at", 0).asInt64();
                    resp.cooldownRemaining = r.get("cooldownRemaining", 0).asInt();
                    sendResp(conn, h, resp);
                } catch (const std::exception& e) { sendErr(conn, h, e.what()); }
                co_return;
            });
            return;
        }
        case Api_Unbind: {
            if (plain.size() != sizeof(RebindReq)) { StatusResp r{St_BadRequest, nowSec()}; sendResp(conn, hdr, r); return; }
            RebindReq req{}; std::memcpy(&req, plain.data(), sizeof(req));   // 复用 RebindReq：key 鉴权，hwid 忽略
            std::string key = cstr(req.key);
            drogon::async_run([this, conn, h, vapp, ip, key]() -> drogon::Task<> {
                try {
                    Json::Value d; d["Key"] = key; d["username"] = key;
                    Json::Value r = co_await vapp->apiJson(h.apiId, ip, d);
                    StatusResp resp{ r.get("status", St_KeyNotFound).asInt(), (int64_t)nowSec() };
                    sendResp(conn, h, resp);
                } catch (const std::exception& e) { sendErr(conn, h, e.what()); }
                co_return;
            });
            return;
        }
        case Api_Query: {
            if (plain.size() != sizeof(QueryReq)) { StatusResp r{St_BadRequest, nowSec()}; sendResp(conn, hdr, r); return; }
            QueryReq req{}; std::memcpy(&req, plain.data(), sizeof(req));
            std::string key = cstr(req.key);
            drogon::async_run([this, conn, h, vapp, ip, key]() -> drogon::Task<> {
                try {
                    Json::Value d; d["Key"] = key; d["username"] = key;
                    Json::Value r = co_await vapp->apiJson(h.apiId, ip, d);
                    QueryResp resp{};
                    resp.status     = r.get("status", St_KeyNotFound).asInt();
                    resp.cmdTag     = r.get("cmdTag", (Json::Int64)nowSec()).asInt64();
                    resp.expired_at = r.get("expired_at", 0).asInt64();
                    resp.created_at = r.get("created_at", 0).asInt64();
                    resp.frozen     = r.get("frozen", 0).asInt();
                    resp.online     = r.get("online", 0).asInt();
                    resp.hours      = r.get("hours", 0).asInt();
                    setField(resp.username, r.get("username", "").asString());
                    setField(resp.maker_name, r.get("maker_name", "").asString());
                    sendResp(conn, h, resp);
                } catch (const std::exception& e) { sendErr(conn, h, e.what()); }
                co_return;
            });
            return;
        }
        case Api_Notice: {
            drogon::async_run([this, conn, h, vapp, ip]() -> drogon::Task<> {
                try {
                    Json::Value d;
                    Json::Value r = co_await vapp->apiJson(h.apiId, ip, d);
                    NoticeResp resp{};
                    resp.status = r.get("status", St_OK).asInt();
                    std::string txt = r.get("Notice", "").asString();
                    resp.enabled = txt.empty() ? 0 : 1;
                    setField(resp.text, txt);
                    sendResp(conn, h, resp);
                } catch (const std::exception& e) { sendErr(conn, h, e.what()); }
                co_return;
            });
            return;
        }
        case Api_UpdateInfo: {
            drogon::async_run([this, conn, h, vapp, ip]() -> drogon::Task<> {
                try {
                    Json::Value d;
                    Json::Value r = co_await vapp->apiJson(h.apiId, ip, d);
                    UpdateResp resp{};
                    resp.status = r.get("status", St_OK).asInt();
                    const Json::Value u = r.get("update", Json::Value());
                    resp.forceUpdate = u.get("forceUpdate", false).asBool() ? 1 : 0;
                    setField(resp.latestVersion, u.get("latestVersion", "").asString());
                    setField(resp.minVersion, u.get("minVersion", "").asString());
                    setField(resp.downloadUrl, u.get("downloadUrl", "").asString());
                    setField(resp.changelog, r.get("updatelog", "").asString());
                    sendResp(conn, h, resp);
                } catch (const std::exception& e) { sendErr(conn, h, e.what()); }
                co_return;
            });
            return;
        }
        case Api_Verify: {
            if (plain.size() != sizeof(VerifyReq)) { StatusResp r{St_BadRequest, nowSec()}; sendResp(conn, hdr, r); return; }
            VerifyReq req{}; std::memcpy(&req, plain.data(), sizeof(req));
            std::string token = cstr(req.token);
            drogon::async_run([this, conn, h, vapp, ip, token]() -> drogon::Task<> {
                try {
                    Json::Value d; d["token"] = token;
                    Json::Value r = co_await vapp->apiJson(h.apiId, ip, d);
                    VerifyResp resp{ r.get("status", St_NoToken).asInt(), (int64_t)nowSec() };
                    sendResp(conn, h, resp);
                } catch (const std::exception& e) { sendErr(conn, h, e.what()); }
                co_return;
            });
            return;
        }
        default: {
            StatusResp r{St_UnknownApi, nowSec()};
            sendResp(conn, hdr, r);
            return;
        }
    }
}

// 业务协程抛异常时的统一兜底：记日志 + 回 St_Exception，绝不让未捕获异常崩溃服务。
void VerifyServiceV2::sendErr(const trantor::TcpConnectionPtr& conn, const Header& reqHdr, const char* what) {
    LOG_ERROR << "v2 dispatch api=" << reqHdr.apiId << " app=" << reqHdr.appId << " exception: " << what;
    StatusResp er{ St_Exception, nowSec() };
    sendResp(conn, reqHdr, er);
}

template <typename Resp>
void VerifyServiceV2::sendResp(const trantor::TcpConnectionPtr& conn, const Header& reqHdr,
                               const Resp& resp) {
    auto session = conn->getContext<Session>();

    std::string plain(reinterpret_cast<const char*>(&resp), sizeof(Resp));

    Header h{};
    std::memcpy(h.magic, kMagic, 4);
    h.version = kVersion;
    h.flags   = (session && session->established()) ? Flag_Encrypted : 0;
    h.apiId   = reqHdr.apiId;
    h.appId   = reqHdr.appId;
    h.ts      = static_cast<uint64_t>(nowSec());
    h.nonce   = 0;
    h.bodyLen = (h.flags & Flag_Encrypted)
                    ? static_cast<uint32_t>(kAeadNonce + plain.size() + kAeadTag)
                    : static_cast<uint32_t>(plain.size());

    std::string hb(reinterpret_cast<const char*>(&h), sizeof(h));
    std::string finalBody = (h.flags & Flag_Encrypted) ? session->seal(plain, hb) : plain;

    std::string frame;
    appendU32LE(frame, static_cast<uint32_t>(hb.size() + finalBody.size()));
    frame += hb;
    frame += finalBody;
    conn->send(frame);
}

// 显式实例化用到的响应类型
template void VerifyServiceV2::sendResp<LoginResp>(const trantor::TcpConnectionPtr&, const Header&, const LoginResp&);
template void VerifyServiceV2::sendResp<RebindResp>(const trantor::TcpConnectionPtr&, const Header&, const RebindResp&);
template void VerifyServiceV2::sendResp<StatusResp>(const trantor::TcpConnectionPtr&, const Header&, const StatusResp&);
template void VerifyServiceV2::sendResp<RechargeResp>(const trantor::TcpConnectionPtr&, const Header&, const RechargeResp&);
template void VerifyServiceV2::sendResp<QueryResp>(const trantor::TcpConnectionPtr&, const Header&, const QueryResp&);
template void VerifyServiceV2::sendResp<NoticeResp>(const trantor::TcpConnectionPtr&, const Header&, const NoticeResp&);
template void VerifyServiceV2::sendResp<UpdateResp>(const trantor::TcpConnectionPtr&, const Header&, const UpdateResp&);
template void VerifyServiceV2::sendResp<VerifyResp>(const trantor::TcpConnectionPtr&, const Header&, const VerifyResp&);

} // namespace aegis::tcp::v2
