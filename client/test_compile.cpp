// 编译/用法示例（C++）：握手 + 登录。无 STL/CRT 依赖，仅 Win32 + tweetnacl。
// 编译：cl test_compile.cpp aegis_client.cpp tweetnacl.cpp
#include "aegis_client.h"

int main() {
    aegis_ctx c;
    AegisLoginResp r;
    // 实际部署时填服务端启动时打印的公钥（32 字节）做 pin 校验：
    unsigned char server_pk[AEGIS_PUBKEY] = {0};

    if (aegis_global_init() != 0) return 1;
    // 直接用域名 + 端口连接（getaddrinfo 走 DNS）
    if (aegis_open(&c, "www.lefenbao.com", 9100, 100001, server_pk) != 0) { aegis_global_cleanup(); return 2; }
    if (aegis_login(&c, "TESTKEY-0001", "HWID-ABC", &r) != 0) { aegis_close(&c); aegis_global_cleanup(); return 3; }
    aegis_close(&c);
    aegis_global_cleanup();
    return (int)r.status;
}
