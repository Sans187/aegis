#pragma once
// Protocol.h —— TCP 验证协议的 api_id、错误码、用户名派生（v2 业务层共用）。
// v1 的帧/加密工具（VERIFYSYSTEM 帧、RC4/RSA）已随旧协议移除。
// 线缆帧定义见 proto/aegis_proto.h；会话加密见 tcp/v2/Session.*（libsodium）。

#include <string>

namespace aegis::tcp {

// ---- api_id ----
enum ApiId : int {
    api_Login      = 300,
    api_Rebind     = 301,
    api_Query      = 302,
    api_Notice     = 303,
    api_UpdateInfo = 304,
    api_Unbind     = 305,
    api_Register   = 306,   // 账号模式：用卡激活并设置账号密码
    api_Recharge   = 307,   // 账号模式：用卡给已有账号充值续费
    api_Verify     = 500,   // api>=500 需要 token
};

// ---- 错误码 ----
enum ErrCode : int {
    ERR_OK                       = 0,
    ERR_UNUSED_CARD              = 500001,
    ERR_FROZEN_CARD              = 500002,
    ERR_KEY_NOT_FOUND            = 500003,
    ERR_EXCEPTION                = 500004,
    ERR_EXPIRED_USER             = 500005,
    ERR_NO_TOKEN                 = 500006,
    ERR_TOKEN_EXPIRED            = 500007,
    ERR_ROTATE_FAILED            = 500008,
    ERR_FROZEN_USER              = 500009,
    ERR_REBIND_FORBIDDEN         = 500010,
    ERR_NEED_REBIND              = 500011,
    ERR_USERNAME_TAKEN           = 500012,   // 账号模式：注册用户名已被占用
    ERR_REBIND_LACK_OF_TIME      = 600009,
    ERR_REBIND_COOLDOWN          = 600010,   // 换绑冷却未到，暂不允许换绑
    ERR_CMDTAG_TIMEOUT           = 700010,
    ERR_UNKNOWN_API              = 700011,
};

// ---- 用户名派生 ----
// 卡密 = 前缀 + 32 位随机体。用户名 = 前缀 + "_" + 随机体前 6 位 + 随机体后 8 位。
inline std::string makeUsernameFromCard(const std::string& card) {
    constexpr size_t kBody = 32;                       // 随机体固定 32 位
    if (card.size() <= kBody) return card;             // 异常/旧格式兜底
    const std::string prefix = card.substr(0, card.size() - kBody);
    const std::string body   = card.substr(card.size() - kBody);
    return prefix + "_" + body.substr(0, 6) + body.substr(kBody - 8);
}

} // namespace aegis::tcp
