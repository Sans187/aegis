#pragma once
// Session —— v2 协议的会话密钥与 AEAD（libsodium）。
//   * ServerKeys：进程级服务器长期 kx 密钥，启动时加载或生成并持久化。
//   * Session：每条 TCP 连接一个，握手后持有 收(rx)/发(tx) 会话密钥；
//     用 XChaCha20-Poly1305 对 body 做加密+完整性，帧头作为 AAD 绑定，防篡改/重放。

#include <array>
#include <cstdint>
#include <string>

#include "proto/aegis_proto.h"

namespace aegis::tcp::v2 {

inline constexpr size_t kSessionKeyBytes = 32;

// 服务器长期密钥（单例）。公钥需内置到客户端做 pin 校验。
class ServerKeys {
public:
    static ServerKeys& instance();

    const std::array<uint8_t, aegis::proto::kPubKeyBytes>& pub() const { return pk_; }
    const std::array<uint8_t, aegis::proto::kPubKeyBytes>& sec() const { return sk_; }
    std::string pubHex() const;   // 打印给客户端内置

private:
    ServerKeys();
    std::array<uint8_t, aegis::proto::kPubKeyBytes> pk_{};
    std::array<uint8_t, aegis::proto::kPubKeyBytes> sk_{};
};

// 每连接会话。
class Session {
public:
    bool established() const { return established_; }

    // 用客户端临时公钥完成服务器侧密钥协商，建立 rx/tx。成功 true。
    bool establish(const uint8_t client_pk[aegis::proto::kPubKeyBytes]);

    // 解密入站 body：wire = [nonce(24)][secretbox]。aad（帧头）以前缀方式绑定，
    // 解密后校验前缀 == aad，不符则失败。成功 true 并写 out（已剥除 aad 前缀）。
    bool open(const std::string& wire, const std::string& aad, std::string& out) const;

    // 加密出站 body：先把 aad 前缀拼到明文头部再 secretbox，返回 [nonce(24)][secretbox]。
    std::string seal(const std::string& plain, const std::string& aad) const;

private:
    bool established_{false};
    std::array<uint8_t, kSessionKeyBytes> k_{};   // crypto_box 预计算共享密钥（双向同一把）
};

} // namespace aegis::tcp::v2
