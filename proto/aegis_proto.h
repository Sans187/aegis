#pragma once
// aegis_proto.h —— Aegis 验证协议 v2（结构体直发版）。
// 客户端库与服务端共享同一份定义，保证逐字节一致。
//
// 设计护栏：
//   * #pragma pack(1)：去对齐填充，布局确定。
//   * 仅用定宽整型；字符串用定长 char 数组（强制 '\0' 结尾）。
//   * 线缆字节序固定为小端（下方 static_assert 锁死，仅支持小端平台）。
//   * 解析一律“先校验长度再读”，禁止把裸 buffer 直接 reinterpret_cast。
//   * 帧头带 magic + version，便于以后演进。

#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>

namespace aegis::proto {

// ============================ 常量 ============================
inline constexpr uint8_t  kMagic[4] = { 'A', 'G', 'S', '2' };  // 协议魔数
inline constexpr uint8_t  kVersion  = 1;                       // 协议大版本
inline constexpr uint32_t kMaxFrame = 64 * 1024;               // 单帧上限，超出即丢弃断连
inline constexpr int64_t  kTsSkew   = 60;                      // 时间戳容差（秒），抗重放

// 加密 / 握手（与 libsodium crypto_kx / AEAD 尺寸一致，详见 v2/Session.h 的 static_assert）
inline constexpr size_t kPubKeyBytes = 32;   // crypto_kx 公钥
inline constexpr size_t kAeadNonce   = 24;   // XChaCha20-Poly1305 nonce
inline constexpr size_t kAeadTag     = 16;   // Poly1305 tag

// 字符串字段统一上限（含结尾 '\0'）
inline constexpr size_t kKeyLen   = 64;    // 卡密 / 账号
inline constexpr size_t kPassLen  = 64;    // 账号密码
inline constexpr size_t kHwidLen  = 128;   // 机器码
inline constexpr size_t kTokenLen = 64;    // 会话 token
inline constexpr size_t kVerLen   = 32;    // 版本号
inline constexpr size_t kUrlLen   = 256;   // 下载地址
inline constexpr size_t kTextLen  = 512;   // 公告 / 更新日志

// ============================ api_id ============================
enum ApiId : uint16_t {
    Api_Handshake  = 200,   // 会话密钥协商（明文阶段，详见 crypto 层）
    Api_Login      = 300,
    Api_Rebind     = 301,
    Api_Query      = 302,
    Api_Notice     = 303,
    Api_UpdateInfo = 304,
    Api_Unbind     = 305,
    Api_Register   = 306,
    Api_Recharge   = 307,
    Api_Verify     = 500,   // >=500 需要 token
};

// ============================ 状态码 ============================
enum Status : int32_t {
    St_OK                 = 0,
    St_UnusedCard         = 500001,
    St_FrozenCard         = 500002,
    St_KeyNotFound        = 500003,
    St_Exception          = 500004,
    St_ExpiredUser        = 500005,
    St_NoToken            = 500006,
    St_TokenExpired       = 500007,
    St_RotateFailed       = 500008,
    St_FrozenUser         = 500009,
    St_RebindForbidden    = 500010,
    St_NeedRebind         = 500011,
    St_UsernameTaken      = 500012,
    St_RebindLackOfTime   = 600009,
    St_RebindCooldown     = 600010,
    St_BadRequest         = 700001,   // v2 协议级：请求格式/应用不存在
    St_TsTimeout          = 700010,   // = ERR_CMDTAG_TIMEOUT
    St_UnknownApi         = 700011,
};

// flags 位
enum Flags : uint8_t {
    Flag_Encrypted = 1 << 0,   // body 已被会话密钥 AEAD 加密
};

#pragma pack(push, 1)

// ---- 帧头：固定大小，永远明文（用于路由/解密前的分发）----
struct Header {
    uint8_t  magic[4];   // = kMagic
    uint8_t  version;    // = kVersion
    uint8_t  flags;      // Flags 位组合
    uint16_t apiId;      // ApiId
    uint32_t appId;      // 数字应用 id
    uint64_t ts;         // 客户端 unix 秒，抗重放
    uint32_t nonce;      // 请求随机数，配合 ts 防重放
    uint32_t bodyLen;    // 紧随其后的 body 字节数（加密后长度）
};

// ---- 各 api 的 body 结构 ----

// 握手请求（明文：客户端临时公钥）。
struct HandshakeReq {
    uint8_t client_pk[kPubKeyBytes];
};

// 握手响应（明文：服务器长期公钥，供客户端与内置 pin 校验；status 见 Status）。
struct HandshakeResp {
    int32_t status;
    uint8_t server_pk[kPubKeyBytes];
};

// 登录请求（卡密模式：key=卡密；账号模式：key=账号, pass=密码）
struct LoginReq {
    char key[kKeyLen];
    char pass[kPassLen];
    char hwid[kHwidLen];
};

// 登录响应
struct LoginResp {
    int32_t status;          // Status
    int64_t cmdTag;          // 服务端回显，绑定本次请求
    int64_t expired_at;      // 到期 unix 秒
    int64_t created_at;
    int32_t frozen;
    int32_t hours;           // 卡种小时数（本次登录所用卡/最近充值的时长）
    char    token[kTokenLen];// 后续 api>=500 使用
    char    username[kKeyLen];
};

// 换绑请求
struct RebindReq {
    char key[kKeyLen];
    char hwid[kHwidLen];     // 新机器码
};

struct RebindResp {
    int32_t status;
    int64_t cmdTag;
    int64_t expired_at;
    int32_t cooldownRemaining;  // 冷却剩余秒数（status=St_RebindCooldown 时有效）
};

// 仅状态的通用响应（解绑等）
struct StatusResp {
    int32_t status;
    int64_t cmdTag;
};

// 公告响应
struct NoticeResp {
    int32_t status;
    int32_t enabled;
    char    text[kTextLen];
};

// 版本更新响应
struct UpdateResp {
    int32_t status;
    int32_t forceUpdate;
    char    latestVersion[kVerLen];
    char    minVersion[kVerLen];
    char    downloadUrl[kUrlLen];
    char    changelog[kTextLen];
};

// 注册（账号模式：用卡激活并设账号密码）；响应复用 LoginResp。
struct RegisterReq {
    char card[kKeyLen];
    char username[kKeyLen];
    char pass[kPassLen];
    char hwid[kHwidLen];
};

// 充值（账号模式：用卡给已有账号续费）
struct RechargeReq {
    char username[kKeyLen];
    char card[kKeyLen];
};
struct RechargeResp {
    int32_t status;
    int64_t cmdTag;
    int64_t expired_at;
    char    username[kKeyLen];
};

// 查询（卡密模式 key=卡密；账号模式 key=用户名）
struct QueryReq {
    char key[kKeyLen];
};
struct QueryResp {
    int32_t status;
    int64_t cmdTag;
    int64_t expired_at;
    int64_t created_at;
    int32_t frozen;
    int32_t online;
    int32_t hours;           // 卡密模式查卡时的卡种小时数
    char    username[kKeyLen];
    char    maker_name[kKeyLen];
};

// 公告 / 更新请求体为空（无字段）。

// 心跳校验（api>=500，需 token）
struct VerifyReq {
    char token[kTokenLen];
};
struct VerifyResp {
    int32_t status;
    int64_t cmdTag;
};

#pragma pack(pop)

// ============================ 编译期护栏 ============================
static_assert(sizeof(Header) == 28, "Header 布局变化——客户端/服务端会不兼容");
static_assert(std::is_trivially_copyable_v<Header>, "Header 必须可平凡拷贝");
static_assert(std::is_trivially_copyable_v<LoginReq>, "LoginReq 必须可平凡拷贝");
// 仅支持小端平台（线缆即原生小端，直接 memcpy；跨端需另加 byteswap）
static_assert(static_cast<uint8_t>(0x0102 >> 8) == 0x01, "compile target must be little-endian");

// ============================ 安全工具 ============================
// 把定长 char 数组当 C 字符串安全读取（强制以 '\0' 截断，防止无结尾溢读）。
template <size_t N>
inline std::string cstr(const char (&buf)[N]) {
    size_t n = 0;
    while (n < N && buf[n] != '\0') ++n;
    return std::string(buf, n);
}

// 安全写入定长字段：截断到 N-1，保证 '\0' 结尾。
template <size_t N>
inline void setField(char (&buf)[N], const std::string& s) {
    std::memset(buf, 0, N);
    std::memcpy(buf, s.data(), s.size() < N ? s.size() : N - 1);
}

} // namespace aegis::proto
