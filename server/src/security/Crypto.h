#pragma once
// Crypto.h —— 通用加密原语（Base64 / 随机 / SHA-256）。
// 旧 v1 协议的 RC4/RSA（含硬编码密钥）已随协议一并移除；会话加密见 tcp/v2/Session.*（libsodium）。

#include <string>

#include <cryptopp/osrng.h>
#include <cryptopp/base64.h>
#include <cryptopp/filters.h>
#include <cryptopp/hex.h>
#include <cryptopp/sha.h>

namespace Crypto
{
    using namespace CryptoPP;

    // ================= Base64 =================
    inline std::string Base64Encode(const std::string& in) {
        std::string out;
        StringSource ss(in, true, new Base64Encoder(new StringSink(out), false));
        return out;
    }
    inline std::string Base64Decode(const std::string& in) {
        std::string out;
        StringSource ss(in, true, new Base64Decoder(new StringSink(out)));
        return out;
    }

    // ================= 高熵随机（token / 卡密随机体）=================
    inline std::string generateRandomKey(size_t len) {
        AutoSeededRandomPool rng;
        std::string key(len, '\0');
        rng.GenerateBlock(reinterpret_cast<CryptoPP::byte*>(key.data()), len);
        return key;
    }

    // ================= SHA-256（十六进制，用于 token 落库）=================
    inline std::string Sha256Hex(const std::string& in) {
        std::string digest, hex;
        SHA256 hash;
        StringSource(in, true, new HashFilter(hash, new StringSink(digest)));
        StringSource(digest, true, new HexEncoder(new StringSink(hex), false /*lowercase*/));
        return hex;
    }
}
