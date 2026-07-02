#include "tcp/v2/Session.h"

#include <sodium.h>
#include <drogon/drogon.h>

#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace aegis::tcp::v2 {

// 协议常量必须与 libsodium 实际尺寸一致（NaCl crypto_box，和客户端 TweetNaCl 字节兼容）。
static_assert(aegis::proto::kPubKeyBytes == crypto_box_PUBLICKEYBYTES, "pubkey size mismatch");
static_assert(aegis::proto::kPubKeyBytes == crypto_box_SECRETKEYBYTES, "seckey size mismatch");
static_assert(kSessionKeyBytes == crypto_box_BEFORENMBYTES, "shared key size mismatch");
static_assert(aegis::proto::kAeadNonce == crypto_box_NONCEBYTES, "nonce size mismatch");
static_assert(aegis::proto::kAeadTag   == crypto_box_MACBYTES, "mac size mismatch");

namespace {
constexpr const char* kKeyFile = "config/server_kx.key";   // 64 字节：pk(32) || sk(32)
}

ServerKeys& ServerKeys::instance() {
    static ServerKeys inst;
    return inst;
}

ServerKeys::ServerKeys() {
    if (sodium_init() < 0) throw std::runtime_error("libsodium 初始化失败");

    std::error_code ec;
    if (std::filesystem::exists(kKeyFile, ec)) {
        std::ifstream f(kKeyFile, std::ios::binary);
        char buf[crypto_box_PUBLICKEYBYTES + crypto_box_SECRETKEYBYTES];
        if (f.read(buf, sizeof(buf)) && f.gcount() == sizeof(buf)) {
            std::memcpy(pk_.data(), buf, crypto_box_PUBLICKEYBYTES);
            std::memcpy(sk_.data(), buf + crypto_box_PUBLICKEYBYTES, crypto_box_SECRETKEYBYTES);
            LOG_INFO << "v2 server box key loaded, pub=" << pubHex();
            return;
        }
        LOG_WARN << "v2 server key 文件损坏，重新生成";
    }

    crypto_box_keypair(pk_.data(), sk_.data());
    std::ofstream out(kKeyFile, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(pk_.data()), pk_.size());
    out.write(reinterpret_cast<const char*>(sk_.data()), sk_.size());
    LOG_INFO << "v2 server box key generated, pub=" << pubHex()
             << "（请把此公钥内置到客户端做 pin 校验）";
}

std::string ServerKeys::pubHex() const {
    char hex[crypto_box_PUBLICKEYBYTES * 2 + 1];
    sodium_bin2hex(hex, sizeof(hex), pk_.data(), pk_.size());
    return std::string(hex);
}

bool Session::establish(const uint8_t client_pk[aegis::proto::kPubKeyBytes]) {
    const auto& keys = ServerKeys::instance();
    // 预计算共享密钥 k = box_beforenm(client_pk, server_sk)
    if (crypto_box_beforenm(k_.data(), client_pk, keys.sec().data()) != 0) return false;
    established_ = true;
    return true;
}

bool Session::open(const std::string& wire, const std::string& aad, std::string& out) const {
    if (!established_) return false;
    if (wire.size() < crypto_box_NONCEBYTES + crypto_box_MACBYTES) return false;

    const auto* npub = reinterpret_cast<const unsigned char*>(wire.data());
    const auto* cipher = npub + crypto_box_NONCEBYTES;
    const size_t clen = wire.size() - crypto_box_NONCEBYTES;

    std::string msg;
    msg.resize(clen - crypto_box_MACBYTES);
    if (crypto_box_open_easy_afternm(reinterpret_cast<unsigned char*>(msg.data()),
                                     cipher, clen, npub, k_.data()) != 0) {
        return false;   // 认证失败
    }
    // 校验 aad 前缀（绑定帧头，等效 AAD）
    if (msg.size() < aad.size() || std::memcmp(msg.data(), aad.data(), aad.size()) != 0) return false;
    out.assign(msg.data() + aad.size(), msg.size() - aad.size());
    return true;
}

std::string Session::seal(const std::string& plain, const std::string& aad) const {
    std::string msg;
    msg.reserve(aad.size() + plain.size());
    msg.append(aad);
    msg.append(plain);

    std::string wire;
    wire.resize(crypto_box_NONCEBYTES + msg.size() + crypto_box_MACBYTES);
    auto* npub = reinterpret_cast<unsigned char*>(wire.data());
    randombytes_buf(npub, crypto_box_NONCEBYTES);

    crypto_box_easy_afternm(npub + crypto_box_NONCEBYTES,
                            reinterpret_cast<const unsigned char*>(msg.data()), msg.size(),
                            npub, k_.data());
    return wire;
}

} // namespace aegis::tcp::v2
