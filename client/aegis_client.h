/* aegis_client.h —— Aegis v2 验证客户端（C++ 声明头，无 STL/CRT 依赖，仅 Windows）。
 *
 * 实现见同目录 aegis_client.cpp；加密见 tweetnacl.h/.cpp（公有领域，与服务端 libsodium 字节兼容）。
 * 链接：ws2_32.lib bcrypt.lib（以及编译 tweetnacl.cpp、aegis_client.cpp）。
 * 全程 C++ 链接，结构体直发；无 CRT 编译（/NODEFAULTLIB）时本套不引入 CRT 调用。
 */
#ifndef AEGIS_CLIENT_H
#define AEGIS_CLIENT_H

#include <stdint.h>          /* freestanding：仅定宽整型，不属于 CRT */

/* ============================ 协议常量（必须与服务端 proto/aegis_proto.h 一致）============================ */
enum {
    AEGIS_MAGIC0='A', AEGIS_MAGIC1='G', AEGIS_MAGIC2='S', AEGIS_MAGIC3='2',
    AEGIS_VERSION   = 1,
    AEGIS_PUBKEY    = 32,
    AEGIS_NONCE     = 24,
    AEGIS_MAC       = 16,
    AEGIS_FLAG_ENC  = 1,
    /* api_id */
    AEGIS_API_HANDSHAKE = 200,
    AEGIS_API_LOGIN     = 300,
    AEGIS_API_REBIND    = 301,
    AEGIS_API_QUERY     = 302,
    AEGIS_API_NOTICE    = 303,
    AEGIS_API_UPDATE    = 304,
    AEGIS_API_UNBIND    = 305,
    AEGIS_API_REGISTER  = 306,
    AEGIS_API_RECHARGE  = 307,
    AEGIS_API_VERIFY    = 500,
    /* 字段长度（与服务端 proto 一致） */
    AEGIS_KEYLEN=64, AEGIS_PASSLEN=64, AEGIS_HWIDLEN=128, AEGIS_TOKENLEN=64,
    AEGIS_VERLEN=32, AEGIS_URLLEN=256, AEGIS_TEXTLEN=512
};

/* 状态码（与服务端 proto::Status / Protocol.h ErrCode 一致） */
enum {
    AEGIS_ST_OK                 = 0,
    AEGIS_ST_UNUSED_CARD        = 500001,
    AEGIS_ST_FROZEN_CARD        = 500002,
    AEGIS_ST_KEY_NOT_FOUND      = 500003,
    AEGIS_ST_EXCEPTION          = 500004,
    AEGIS_ST_EXPIRED_USER       = 500005,
    AEGIS_ST_NO_TOKEN           = 500006,
    AEGIS_ST_TOKEN_EXPIRED      = 500007,
    AEGIS_ST_ROTATE_FAILED      = 500008,
    AEGIS_ST_FROZEN_USER        = 500009,
    AEGIS_ST_REBIND_FORBIDDEN   = 500010,
    AEGIS_ST_NEED_REBIND        = 500011,
    AEGIS_ST_USERNAME_TAKEN     = 500012,
    AEGIS_ST_REBIND_LACK_TIME   = 600009,
    AEGIS_ST_REBIND_COOLDOWN    = 600010,
    AEGIS_ST_BADREQ             = 700001,
    AEGIS_ST_TS_TIMEOUT         = 700010,
    AEGIS_ST_UNKNOWN_API        = 700011
};

#pragma pack(push, 1)

typedef struct {
    uint8_t  magic[4];
    uint8_t  version;
    uint8_t  flags;
    uint16_t apiId;
    uint32_t appId;
    uint64_t ts;
    uint32_t nonce;
    uint32_t bodyLen;
} AegisHeader;                                  /* 28 字节 */

typedef struct { uint8_t client_pk[AEGIS_PUBKEY]; } AegisHandshakeReq;
typedef struct { int32_t status; uint8_t server_pk[AEGIS_PUBKEY]; } AegisHandshakeResp;

typedef struct {
    char key[AEGIS_KEYLEN];
    char pass[AEGIS_PASSLEN];
    char hwid[AEGIS_HWIDLEN];
} AegisLoginReq;

typedef struct {
    int32_t status;
    int64_t cmdTag;
    int64_t expired_at;
    int64_t created_at;
    int32_t frozen;
    int32_t hours;           /* 卡种小时数 */
    char    token[AEGIS_TOKENLEN];
    char    username[AEGIS_KEYLEN];
} AegisLoginResp;

/* 换绑 */
typedef struct { char key[AEGIS_KEYLEN]; char hwid[AEGIS_HWIDLEN]; } AegisRebindReq;
typedef struct { int32_t status; int64_t cmdTag; int64_t expired_at; int32_t cooldownRemaining; } AegisRebindResp;

/* 仅状态响应（解绑等） */
typedef struct { int32_t status; int64_t cmdTag; } AegisStatusResp;

/* 注册（账号模式）；响应复用 AegisLoginResp */
typedef struct {
    char card[AEGIS_KEYLEN];
    char username[AEGIS_KEYLEN];
    char pass[AEGIS_PASSLEN];
    char hwid[AEGIS_HWIDLEN];
} AegisRegisterReq;

/* 充值（账号模式） */
typedef struct { char username[AEGIS_KEYLEN]; char card[AEGIS_KEYLEN]; } AegisRechargeReq;
typedef struct { int32_t status; int64_t cmdTag; int64_t expired_at; char username[AEGIS_KEYLEN]; } AegisRechargeResp;

/* 查询 */
typedef struct { char key[AEGIS_KEYLEN]; } AegisQueryReq;
typedef struct {
    int32_t status;
    int64_t cmdTag;
    int64_t expired_at;
    int64_t created_at;
    int32_t frozen;
    int32_t online;
    int32_t hours;
    char    username[AEGIS_KEYLEN];
    char    maker_name[AEGIS_KEYLEN];
} AegisQueryResp;

/* 公告 */
typedef struct { int32_t status; int32_t enabled; char text[AEGIS_TEXTLEN]; } AegisNoticeResp;

/* 版本更新 */
typedef struct {
    int32_t status;
    int32_t forceUpdate;
    char    latestVersion[AEGIS_VERLEN];
    char    minVersion[AEGIS_VERLEN];
    char    downloadUrl[AEGIS_URLLEN];
    char    changelog[AEGIS_TEXTLEN];
} AegisUpdateResp;

/* 心跳校验 */
typedef struct { char token[AEGIS_TOKENLEN]; } AegisVerifyReq;
typedef struct { int32_t status; int64_t cmdTag; } AegisVerifyResp;

#pragma pack(pop)

/* 会话上下文 */
typedef struct {
    uintptr_t sock;                       /* SOCKET */
    uint32_t  app_id;
    int       established;
    uint8_t   k[32];                      /* crypto_box 预计算共享密钥 */
    uint8_t   server_pk[AEGIS_PUBKEY];    /* 内置 pin 的服务器公钥 */
} aegis_ctx;

/* ============================ 公开 API（返回 0 成功，<0 失败）============================ */
int  aegis_global_init(void);             /* WSAStartup，进程启动调一次 */
void aegis_global_cleanup(void);
int  aegis_open(aegis_ctx* c, const char* host, uint16_t port,
                uint32_t app_id, const uint8_t server_pk[AEGIS_PUBKEY]);  /* host 可为域名或 IP；连接+握手 */
int  aegis_login(aegis_ctx* c, const char* key, const char* hwid, AegisLoginResp* out);
int  aegis_register(aegis_ctx* c, const char* card, const char* username, const char* pass, const char* hwid, AegisLoginResp* out);
int  aegis_recharge(aegis_ctx* c, const char* username, const char* card, AegisRechargeResp* out);
int  aegis_rebind(aegis_ctx* c, const char* key, const char* hwid, AegisRebindResp* out);
int  aegis_unbind(aegis_ctx* c, const char* key, AegisStatusResp* out);
int  aegis_query(aegis_ctx* c, const char* key, AegisQueryResp* out);
int  aegis_notice(aegis_ctx* c, AegisNoticeResp* out);
int  aegis_update(aegis_ctx* c, AegisUpdateResp* out);
int  aegis_verify(aegis_ctx* c, const char* token, AegisVerifyResp* out);
void aegis_close(aegis_ctx* c);

#endif /* AEGIS_CLIENT_H */
