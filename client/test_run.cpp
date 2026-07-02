// 端到端实测：连远端 v2 服务,握手 + 登录,打印结果。
#include <cstdio>
#include "aegis_client.h"

int main(int argc, char** argv) {
    const char* card = argc > 1 ? argv[1] : "TESTCARD-0001";
    const char* hwid = argc > 2 ? argv[2] : "HWID-ABC-123";
    // 服务端启动时生成/打印的公钥（bd8276...5b6d）
    unsigned char spk[32] = {
        0xbd,0x82,0x76,0xd7,0x6f,0xc7,0x6f,0xd6,0xc1,0x35,0x84,0x15,0x9d,0xef,0xca,0x7d,
        0x88,0xd5,0x69,0x95,0x66,0x04,0x5d,0xc7,0xc4,0x23,0x3b,0xca,0xdf,0x01,0x5b,0x6d
    };

    if (aegis_global_init() != 0) { printf("global_init FAIL\n"); return 1; }

    aegis_ctx c;
    int ro = aegis_open(&c, "127.0.0.1", 9002, 100001, spk);
    printf("aegis_open (handshake) = %d  %s\n", ro, ro==0 ? "OK" : "FAIL");
    if (ro != 0) { aegis_global_cleanup(); return 2; }

    AegisLoginResp r;
    printf("login card='%s' hwid='%s'\n", card, hwid);
    int rl = aegis_login(&c, card, hwid, &r);
    printf("aegis_login = %d\n", rl);
    printf("  status     = %d\n", (int)r.status);
    printf("  hours      = %d\n", (int)r.hours);
    printf("  expired_at = %lld\n", (long long)r.expired_at);
    printf("  token      = '%s'\n", r.token);

    aegis_close(&c);
    aegis_global_cleanup();
    return 0;
}
