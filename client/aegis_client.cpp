/* aegis_client.cpp —— aegis_client.h 的实现（C++，无 STL/CRT，仅 Win32 + tweetnacl）。 */
#include "aegis_client.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600        /* getaddrinfo 需要 Vista+ */
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>

#include "tweetnacl.h"             /* crypto_box_* */

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "bcrypt.lib")

/* ---- 自带 mem 操作，避免依赖 CRT ---- */
static void a_memcpy(void* d, const void* s, int n){ uint8_t* dd=(uint8_t*)d; const uint8_t* ss=(const uint8_t*)s; for(int i=0;i<n;i++) dd[i]=ss[i]; }
static void a_memset(void* d, int v, int n){ uint8_t* dd=(uint8_t*)d; for(int i=0;i<n;i++) dd[i]=(uint8_t)v; }
static int  a_memcmp(const void* a, const void* b, int n){ const uint8_t* x=(const uint8_t*)a; const uint8_t* y=(const uint8_t*)b; for(int i=0;i<n;i++){ if(x[i]!=y[i]) return x[i]-y[i]; } return 0; }
static int  a_strncpy0(char* d, const char* s, int cap){ int i=0; for(; i<cap-1 && s[i]; i++) d[i]=s[i]; for(int j=i;j<cap;j++) d[j]=0; return i; }

/* ---- TweetNaCl 需要的随机源：用 BCrypt（无 CRT）---- */
void randombytes(unsigned char* p, unsigned long long n){
    BCryptGenRandom(NULL, p, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
}

/* 本 tweetnacl.cpp 注释掉了 crypto_box_keypair；用它导出的 scalarmult_base 补上（标准做法）。 */
static int aegis_box_keypair(unsigned char* pk, unsigned char* sk){
    randombytes(sk, 32);
    return crypto_scalarmult_base(pk, sk);   /* X25519 clamping 在 scalarmult 内部完成 */
}

static uint64_t a_now(void){
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    uint64_t t = ((uint64_t)ft.dwHighDateTime<<32)|ft.dwLowDateTime;
    return (t - 116444736000000000ULL) / 10000000ULL;   /* Win32 文件时间 → unix 秒 */
}

static void put_u32le(uint8_t* p, uint32_t v){ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }
static uint32_t get_u32le(const uint8_t* p){ return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }
static void u16_to_str(uint16_t v, char* out){
    char tmp[6]; int i=0;
    if (v==0){ out[0]='0'; out[1]=0; return; }
    while (v){ tmp[i++]=(char)('0'+(v%10)); v=(uint16_t)(v/10); }
    int j=0; while (i) out[j++]=tmp[--i];
    out[j]=0;
}

/* ---- crypto_box 封装（处理 NaCl ZEROBYTES 填充，与服务端 libsodium _easy 兼容）---- */
#define AEGIS_BUF 2048
static int box_seal(const uint8_t k[32], const uint8_t* msg, int msglen, uint8_t* outWire, int outCap){
    uint8_t mpad[AEGIS_BUF], cpad[AEGIS_BUF];
    int plen = 32 + msglen;
    if (plen > AEGIS_BUF) return -1;
    a_memset(mpad, 0, 32); a_memcpy(mpad+32, msg, msglen);
    uint8_t nonce[AEGIS_NONCE]; randombytes(nonce, AEGIS_NONCE);
    if (crypto_box_afternm(cpad, mpad, (unsigned long long)plen, nonce, k) != 0) return -1;
    int easylen = plen - 16;
    if (AEGIS_NONCE + easylen > outCap) return -1;
    a_memcpy(outWire, nonce, AEGIS_NONCE);
    a_memcpy(outWire+AEGIS_NONCE, cpad+16, easylen);
    return AEGIS_NONCE + easylen;
}
static int box_open(const uint8_t k[32], const uint8_t* wire, int wirelen, uint8_t* outMsg, int outCap){
    if (wirelen < AEGIS_NONCE + AEGIS_MAC) return -1;
    const uint8_t* nonce = wire;
    int easylen = wirelen - AEGIS_NONCE;
    int clen = 16 + easylen;
    uint8_t cpad[AEGIS_BUF], mpad[AEGIS_BUF];
    if (clen > AEGIS_BUF) return -1;
    a_memset(cpad, 0, 16); a_memcpy(cpad+16, wire+AEGIS_NONCE, easylen);
    if (crypto_box_open_afternm(mpad, cpad, (unsigned long long)clen, nonce, k) != 0) return -1;
    int mlen = clen - 32;
    if (mlen > outCap) return -1;
    a_memcpy(outMsg, mpad+32, mlen);
    return mlen;
}

static int sock_send_all(uintptr_t s, const uint8_t* p, int n){
    int off=0; while(off<n){ int r=send((SOCKET)s, (const char*)p+off, n-off, 0); if(r<=0) return -1; off+=r; } return 0;
}
static int sock_recv_all(uintptr_t s, uint8_t* p, int n){
    int off=0; while(off<n){ int r=recv((SOCKET)s, (char*)p+off, n-off, 0); if(r<=0) return -1; off+=r; } return 0;
}

static int send_frame(aegis_ctx* c, uint16_t apiId, int enc, const uint8_t* plain, int plainLen){
    AegisHeader h; a_memset(&h, 0, sizeof(h));
    h.magic[0]=AEGIS_MAGIC0; h.magic[1]=AEGIS_MAGIC1; h.magic[2]=AEGIS_MAGIC2; h.magic[3]=AEGIS_MAGIC3;
    h.version=AEGIS_VERSION; h.flags=(uint8_t)(enc?AEGIS_FLAG_ENC:0);
    h.apiId=apiId; h.appId=c->app_id; h.ts=a_now(); h.nonce=0;

    uint8_t body[AEGIS_BUF]; int bodyLen;
    if (enc){
        uint8_t msg[AEGIS_BUF];
        if ((int)sizeof(h)+plainLen > AEGIS_BUF) return -1;
        h.bodyLen = (uint32_t)(AEGIS_NONCE + AEGIS_MAC + sizeof(h) + plainLen);
        a_memcpy(msg, &h, sizeof(h)); a_memcpy(msg+sizeof(h), plain, plainLen);
        bodyLen = box_seal(c->k, msg, (int)sizeof(h)+plainLen, body, AEGIS_BUF);
        if (bodyLen < 0) return -1;
    } else {
        if (plainLen > AEGIS_BUF) return -1;
        h.bodyLen = (uint32_t)plainLen; a_memcpy(body, plain, plainLen); bodyLen = plainLen;
    }

    uint8_t frame[4+sizeof(AegisHeader)+AEGIS_BUF];
    uint32_t total = (uint32_t)(sizeof(h)+bodyLen);
    put_u32le(frame, total);
    a_memcpy(frame+4, &h, sizeof(h));
    a_memcpy(frame+4+sizeof(h), body, bodyLen);
    return sock_send_all(c->sock, frame, 4+(int)total);
}

static int recv_frame(aegis_ctx* c, AegisHeader* hOut, uint8_t* plainOut, int plainCap){
    uint8_t lp[4];
    if (sock_recv_all(c->sock, lp, 4)) return -1;
    uint32_t total = get_u32le(lp);
    if (total < sizeof(AegisHeader) || total > sizeof(AegisHeader)+AEGIS_BUF) return -1;

    uint8_t buf[sizeof(AegisHeader)+AEGIS_BUF];
    if (sock_recv_all(c->sock, buf, (int)total)) return -1;
    a_memcpy(hOut, buf, sizeof(AegisHeader));
    if (a_memcmp(hOut->magic, "AGS2", 4)!=0 || hOut->version!=AEGIS_VERSION) return -1;

    const uint8_t* body = buf + sizeof(AegisHeader);
    int bodyLen = (int)total - (int)sizeof(AegisHeader);

    if (hOut->flags & AEGIS_FLAG_ENC){
        uint8_t msg[AEGIS_BUF];
        int mlen = box_open(c->k, body, bodyLen, msg, AEGIS_BUF);
        if (mlen < (int)sizeof(AegisHeader)) return -1;
        if (a_memcmp(msg, hOut, sizeof(AegisHeader)) != 0) return -1;   /* 绑定帧头，防篡改 */
        int outLen = mlen - (int)sizeof(AegisHeader);
        if (outLen > plainCap) return -1;
        a_memcpy(plainOut, msg+sizeof(AegisHeader), outLen);
        return outLen;
    }
    if (bodyLen > plainCap) return -1;
    a_memcpy(plainOut, body, bodyLen);
    return bodyLen;
}

/* ---- 公开 API ---- */
int aegis_global_init(void){ WSADATA w; return WSAStartup(MAKEWORD(2,2), &w)==0 ? 0 : -1; }
void aegis_global_cleanup(void){ WSACleanup(); }

int aegis_open(aegis_ctx* c, const char* host, uint16_t port, uint32_t app_id, const uint8_t server_pk[AEGIS_PUBKEY]){
    a_memset(c, 0, sizeof(*c));
    c->app_id = app_id;
    a_memcpy(c->server_pk, server_pk, AEGIS_PUBKEY);

    struct addrinfo hints; a_memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM; hints.ai_protocol = IPPROTO_TCP;
    char portstr[8]; u16_to_str(port, portstr);
    struct addrinfo* res = 0;
    if (getaddrinfo(host, portstr, &hints, &res) != 0) return -1;   /* 走 DNS，支持域名 */

    SOCKET s = INVALID_SOCKET;
    for (struct addrinfo* ai = res; ai; ai = ai->ai_next){
        s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == INVALID_SOCKET) continue;
        if (connect(s, ai->ai_addr, (int)ai->ai_addrlen) == 0) break;
        closesocket(s); s = INVALID_SOCKET;
    }
    freeaddrinfo(res);
    if (s == INVALID_SOCKET) return -1;
    c->sock = (uintptr_t)s;

    /* 握手：临时密钥对 + 发客户端公钥（明文） */
    uint8_t cpk[AEGIS_PUBKEY], csk[32];
    aegis_box_keypair(cpk, csk);
    AegisHandshakeReq req; a_memcpy(req.client_pk, cpk, AEGIS_PUBKEY);
    if (send_frame(c, AEGIS_API_HANDSHAKE, 0, (uint8_t*)&req, sizeof(req))){ aegis_close(c); return -1; }

    AegisHeader h; uint8_t body[AEGIS_BUF];
    int n = recv_frame(c, &h, body, AEGIS_BUF);
    if (n != (int)sizeof(AegisHandshakeResp)){ aegis_close(c); return -1; }
    AegisHandshakeResp* resp = (AegisHandshakeResp*)body;
    if (resp->status != AEGIS_ST_OK){ aegis_close(c); return -1; }
    if (a_memcmp(resp->server_pk, c->server_pk, AEGIS_PUBKEY) != 0){ aegis_close(c); return -1; }  /* pin 校验 */
    if (crypto_box_beforenm(c->k, c->server_pk, csk) != 0){ aegis_close(c); return -1; }
    c->established = 1;
    return 0;
}

int aegis_login(aegis_ctx* c, const char* key, const char* hwid, AegisLoginResp* out){
    if (!c->established) return -1;
    AegisLoginReq req; a_memset(&req, 0, sizeof(req));
    a_strncpy0(req.key, key, AEGIS_KEYLEN);
    a_strncpy0(req.hwid, hwid, AEGIS_HWIDLEN);
    if (send_frame(c, AEGIS_API_LOGIN, 1, (uint8_t*)&req, sizeof(req))) return -1;

    AegisHeader h; uint8_t body[AEGIS_BUF];
    int n = recv_frame(c, &h, body, AEGIS_BUF);
    if (n != (int)sizeof(AegisLoginResp)) return -1;
    a_memcpy(out, body, sizeof(AegisLoginResp));
    return 0;
}

/* ---- 其余 api（请求结构体直发，响应结构体回收）---- */
static int aegis_call(aegis_ctx* c, uint16_t api, const uint8_t* req, int reqLen, void* out, int outLen){
    if (!c->established) return -1;
    if (send_frame(c, api, 1, req, reqLen)) return -1;
    AegisHeader h; uint8_t body[AEGIS_BUF];
    int n = recv_frame(c, &h, body, AEGIS_BUF);
    if (n != outLen) return -1;
    a_memcpy(out, body, outLen);
    return 0;
}

int aegis_register(aegis_ctx* c, const char* card, const char* username, const char* pass, const char* hwid, AegisLoginResp* out){
    AegisRegisterReq req; a_memset(&req, 0, sizeof(req));
    a_strncpy0(req.card, card, AEGIS_KEYLEN); a_strncpy0(req.username, username, AEGIS_KEYLEN);
    a_strncpy0(req.pass, pass, AEGIS_PASSLEN); a_strncpy0(req.hwid, hwid, AEGIS_HWIDLEN);
    return aegis_call(c, AEGIS_API_REGISTER, (uint8_t*)&req, sizeof(req), out, (int)sizeof(*out));
}
int aegis_recharge(aegis_ctx* c, const char* username, const char* card, AegisRechargeResp* out){
    AegisRechargeReq req; a_memset(&req, 0, sizeof(req));
    a_strncpy0(req.username, username, AEGIS_KEYLEN); a_strncpy0(req.card, card, AEGIS_KEYLEN);
    return aegis_call(c, AEGIS_API_RECHARGE, (uint8_t*)&req, sizeof(req), out, (int)sizeof(*out));
}
int aegis_rebind(aegis_ctx* c, const char* key, const char* hwid, AegisRebindResp* out){
    AegisRebindReq req; a_memset(&req, 0, sizeof(req));
    a_strncpy0(req.key, key, AEGIS_KEYLEN); a_strncpy0(req.hwid, hwid, AEGIS_HWIDLEN);
    return aegis_call(c, AEGIS_API_REBIND, (uint8_t*)&req, sizeof(req), out, (int)sizeof(*out));
}
int aegis_unbind(aegis_ctx* c, const char* key, AegisStatusResp* out){
    AegisRebindReq req; a_memset(&req, 0, sizeof(req));   /* 复用：key 鉴权，hwid 留空 */
    a_strncpy0(req.key, key, AEGIS_KEYLEN);
    return aegis_call(c, AEGIS_API_UNBIND, (uint8_t*)&req, sizeof(req), out, (int)sizeof(*out));
}
int aegis_query(aegis_ctx* c, const char* key, AegisQueryResp* out){
    AegisQueryReq req; a_memset(&req, 0, sizeof(req));
    a_strncpy0(req.key, key, AEGIS_KEYLEN);
    return aegis_call(c, AEGIS_API_QUERY, (uint8_t*)&req, sizeof(req), out, (int)sizeof(*out));
}
int aegis_notice(aegis_ctx* c, AegisNoticeResp* out){
    return aegis_call(c, AEGIS_API_NOTICE, (const uint8_t*)"", 0, out, (int)sizeof(*out));
}
int aegis_update(aegis_ctx* c, AegisUpdateResp* out){
    return aegis_call(c, AEGIS_API_UPDATE, (const uint8_t*)"", 0, out, (int)sizeof(*out));
}
int aegis_verify(aegis_ctx* c, const char* token, AegisVerifyResp* out){
    AegisVerifyReq req; a_memset(&req, 0, sizeof(req));
    a_strncpy0(req.token, token, AEGIS_TOKENLEN);
    return aegis_call(c, AEGIS_API_VERIFY, (uint8_t*)&req, sizeof(req), out, (int)sizeof(*out));
}

void aegis_close(aegis_ctx* c){
    if (c->sock){ closesocket((SOCKET)c->sock); c->sock = 0; }
    c->established = 0;
}
