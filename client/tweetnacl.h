#ifndef TWEETNACL_H
#define TWEETNACL_H
/* 与本目录 tweetnacl.c（原始短名版，公有领域）匹配的精简公开头。
 * 仅声明 + 尺寸常量；加密实现全部在 tweetnacl.c。
 * 注意：randombytes 由使用方提供（本项目在 aegis_client.h 的实现段用 BCrypt 定义）。 */

#define crypto_box_PUBLICKEYBYTES 32
#define crypto_box_SECRETKEYBYTES 32
#define crypto_box_BEFORENMBYTES  32
#define crypto_box_NONCEBYTES     24
#define crypto_box_ZEROBYTES      32
#define crypto_box_BOXZEROBYTES   16
#define crypto_box_MACBYTES       16   /* ZEROBYTES - BOXZEROBYTES */

#define crypto_secretbox_KEYBYTES      32
#define crypto_secretbox_NONCEBYTES    24
#define crypto_secretbox_ZEROBYTES     32
#define crypto_secretbox_BOXZEROBYTES  16

#define crypto_scalarmult_BYTES        32
#define crypto_scalarmult_SCALARBYTES  32

extern int crypto_box_keypair(unsigned char *pk, unsigned char *sk);
extern int crypto_box_beforenm(unsigned char *k, const unsigned char *pk, const unsigned char *sk);
extern int crypto_box_afternm(unsigned char *c, const unsigned char *m, unsigned long long d,
                              const unsigned char *n, const unsigned char *k);
extern int crypto_box_open_afternm(unsigned char *m, const unsigned char *c, unsigned long long d,
                                   const unsigned char *n, const unsigned char *k);
extern int crypto_box(unsigned char *c, const unsigned char *m, unsigned long long d,
                      const unsigned char *n, const unsigned char *pk, const unsigned char *sk);
extern int crypto_box_open(unsigned char *m, const unsigned char *c, unsigned long long d,
                           const unsigned char *n, const unsigned char *pk, const unsigned char *sk);

extern int crypto_secretbox(unsigned char *c, const unsigned char *m, unsigned long long d,
                            const unsigned char *n, const unsigned char *k);
extern int crypto_secretbox_open(unsigned char *m, const unsigned char *c, unsigned long long d,
                                 const unsigned char *n, const unsigned char *k);

extern int crypto_scalarmult(unsigned char *q, const unsigned char *n, const unsigned char *p);
extern int crypto_scalarmult_base(unsigned char *q, const unsigned char *n);

/* 由使用方提供的随机源 */
extern void randombytes(unsigned char *, unsigned long long);

#endif /* TWEETNACL_H */
