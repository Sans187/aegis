#include <drogon/drogon.h>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <ctime>
#include <memory>
#include <chrono>
#include <mutex>
#include <unordered_map>

#include "db/Migrate.h"
#include "security/Password.h"
#include "tcp/v2/VerifyServiceV2.h"

using namespace drogon;

static std::unique_ptr<aegis::tcp::v2::VerifyServiceV2> g_verify;

// 启动期致命错误：同时打印到控制台并追加写入 startup-error.log，
// 这样即使是双击运行、窗口一闪而过，也能在 exe 同目录下查到原因。
static void writeStartupError(const std::string& msg) {
    std::cerr << msg << std::endl;
    std::ofstream f("startup-error.log", std::ios::app);
    if (f) {
        std::time_t t = std::time(nullptr);
        std::tm tmv{};
#ifdef _WIN32
        localtime_s(&tmv, &t);
#else
        localtime_r(&t, &tmv);
#endif
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
        f << "[" << buf << "] " << msg << "\n";
    }
}

// ============================ 全局每-IP 请求限流（令牌桶，内存态） ============================
// 路由前拦截：单 IP 突发上限 kCapacity，稳定速率 kRefillPerSec/秒，超限即回 429。
// 远高于正常人工/页面用量，仅用于挡单 IP 的请求洪水（配合 config 里的 max_connections_per_ip）。
namespace {
struct Bucket { double tokens; std::chrono::steady_clock::time_point last; };
std::mutex g_rlMtx;
std::unordered_map<std::string, Bucket> g_buckets;
constexpr double kCapacity     = 60.0;   // 突发桶容量
constexpr double kRefillPerSec = 30.0;   // 每秒补充令牌数
size_t g_rlHits = 0;

// 返回 true=放行，false=超限。
bool rateAllow(const std::string& ip) {
    using namespace std::chrono;
    const auto now = steady_clock::now();
    std::lock_guard<std::mutex> lk(g_rlMtx);

    // 周期性清理 5 分钟未活动的条目，防止 map 无限增长（被大量不同 IP 打时）。
    if ((++g_rlHits & 0x3FF) == 0) {
        for (auto it = g_buckets.begin(); it != g_buckets.end(); ) {
            if (now - it->second.last > minutes(5)) it = g_buckets.erase(it);
            else ++it;
        }
    }

    auto& b = g_buckets[ip];
    if (b.last.time_since_epoch().count() == 0) {
        b.tokens = kCapacity;
    } else {
        const double dt = duration_cast<duration<double>>(now - b.last).count();
        b.tokens = std::min(kCapacity, b.tokens + dt * kRefillPerSec);
    }
    b.last = now;
    if (b.tokens < 1.0) return false;
    b.tokens -= 1.0;
    return true;
}
} // namespace

int main() {
    // 1) 安全库初始化（Argon2id 依赖 libsodium）
    if (!security::initSodium()) {
        std::cerr << "[FATAL] libsodium init failed\n";
        return 1;
    }

    // 2) 加载配置（PostgreSQL 连接池在此创建）
    if (!std::filesystem::exists("config/config.json")) {
        writeStartupError("[FATAL] 找不到 config/config.json。请确保把 exe 同目录下的 "
                          "config / sql / web_dist 三个文件夹一起拷到服务器（工作目录要在 exe 所在目录）。");
        return 1;
    }
    try {
        app().loadConfigFile("config/config.json");
    } catch (const std::exception& e) {
        writeStartupError(std::string("[FATAL] 配置文件加载失败: ") + e.what());
        return 1;
    }

    // 3) 启动阶段：建表 + 引导超管。（TCP 验证子系统将在阶段 3 接入。）
    app().registerBeginningAdvice([] {
        auto client = app().getDbClient();
        try {
            db::runSchema(client, "sql/schema.sql");
            db::bootstrapAdmin(client);
            // 不再播种默认软件：初始化后 apps 表保持为空，由用户自行添加。
        } catch (const std::exception& e) {
            writeStartupError(std::string("[FATAL] 启动建表/连接数据库失败（多半是 PostgreSQL 没装/没启动，"
                              "或 config.json 里的数据库地址/账号/密码不对）: ") + e.what());
            app().quit();
            return;
        }

        // 启动 TCP 验证服务（端口取自 custom_config.tcp_verify_port，默认 9002）
        const auto& cc = app().getCustomConfig();
        uint16_t tcpPort = 9002;
        if (!cc.isNull() && cc.isMember("tcp_verify_port"))
            tcpPort = static_cast<uint16_t>(cc["tcp_verify_port"].asUInt());
        g_verify = std::make_unique<aegis::tcp::v2::VerifyServiceV2>(app().getLoop(), tcpPort);
        g_verify->start();

        LOG_INFO << "Aegis server startup complete (TCP verify v2 on " << tcpPort << ")";
    });

    // 4) 健康检查（阶段 1 用于验证工具链与数据库连通）
    app().registerHandler(
        "/api/health",
        [](const HttpRequestPtr&,
           std::function<void(const HttpResponsePtr&)>&& cb) {
            Json::Value v;
            v["ok"] = true;
            v["service"] = "aegis";
            // 服务器当前时间 + 时区偏移（分钟），前端统一按服务器时区展示/查询
            v["server_time"] = static_cast<Json::Int64>(time(nullptr));
            int tz = 480;  // 默认 UTC+8
            const auto& cc = app().getCustomConfig();
            if (!cc.isNull() && cc.isMember("tz_offset_minutes")) tz = cc["tz_offset_minutes"].asInt();
            v["tz_offset_min"] = tz;
            try {
                auto r = app().getDbClient()->execSqlSync("SELECT 1 AS one;");
                v["db"] = (!r.empty() && r[0]["one"].as<int>() == 1) ? "up" : "unknown";
            } catch (const std::exception& e) {
                v["db"] = std::string("down: ") + e.what();
            }
            cb(HttpResponse::newHttpJsonResponse(v));
        },
        {Get});

    // 5) 全局限流：路由前对每个请求按来源 IP 做令牌桶限速，超限直接 429。
    app().registerPreRoutingAdvice(
        [](const HttpRequestPtr& req,
           AdviceCallback&& stop,
           AdviceChainCallback&& next) {
            if (rateAllow(req->getPeerAddr().toIp())) { next(); return; }
            Json::Value v; v["ok"] = false; v["error"] = "rate limited";
            auto resp = HttpResponse::newHttpJsonResponse(v);
            resp->setStatusCode(k429TooManyRequests);
            stop(resp);
        });

    LOG_INFO << "Aegis listening (see config/config.json)";
    app().run();
    return 0;
}
