/*
 * cf_http.c - 极简 HTTP/HTTPS POST 客户端（lwIP + mbedtls 2.x）
 *
 * 内存策略（本文件是"注意内存"的第一现场）：
 *   1) 三个工作缓冲（发送块 / 请求头 / 响应体）全部是函数内 static，
 *      不占线程栈、不进堆，尺寸在编译期就能在 map 里查得到；
 *   2) mbedtls 上下文（ssl/config/entropy/ctr_drbg）也是 static 并复用 ——
 *      mbedtls_ssl_setup() 内部会再 malloc 十几到几十 KB，若每次请求都
 *      init/free 一遍，长时间跑会把堆打出碎片，这正是嵌入式上最常见的
 *      "跑一天就 OOM"的成因。本模块串行发请求，静态复用是安全的；
 *   3) body 由回调分块拉取（默认 2KB 一块），调用方无需拼出完整请求体。
 */
#include "cf_http.h"

#include <rtthread.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#include <mbedtls/ssl.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

#define CF_DBG               0
/* 2048 是 body 回调常见的对齐块；再留 16 字节，避免"base64 输出正好
 * 等于缓冲长度"时某些实现因为需要写结尾符而判 BUFFER_TOO_SMALL。 */
#define CF_BODY_CHUNK        2064      /* 每次从 body 回调拉取的字节数 */
#define CF_HDR_MAX           1536      /* 请求头（讯飞带 authorization 约 300 字符） */
#define CF_IO_TIMEOUT_MS     10000     /* 单次 socket 读写等待上限 */

#if CF_DBG
#define CF_LOG(fmt, ...)     rt_kprintf("[cf] " fmt, ##__VA_ARGS__)
#else
#define CF_LOG(fmt, ...)     do { } while (0)
#endif

/* ---------------- 连接上下文（静态复用） ---------------- */
typedef struct {
    int fd;
    int use_tls;
    mbedtls_ssl_context      ssl;
    mbedtls_ssl_config       conf;
    mbedtls_entropy_context  entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
} cf_conn_t;

static cf_conn_t        s_conn;
static char             s_tx[CF_BODY_CHUNK];
static char             s_hdr[CF_HDR_MAX];

/* ---------------- mbedtls BIO：非阻塞 socket 的轮询封装 ----------------
 * 不能直接把 fd 交给 mbedtls_ssl_set_bio(default)，因为本 SDK 的 lwIP
 * 在非阻塞模式下返回 -1/EWOULDBLOCK，mbedtls 会当成致命错误。
 * 这里把它翻译成 WANT_READ/WANT_WRITE，让 mbedtls 自己重试。 */
static int cf_net_send(void *ctx, const unsigned char *buf, size_t len)
{
    int fd = *(int *)ctx;
    const unsigned char *p = buf;
    size_t left = len;
    int waited = 0;

    while (left > 0) {
        int n = send(fd, p, left, 0);
        if (n > 0) {
            p += n;
            left -= (size_t)n;
            waited = 0;
            continue;
        }
        if (n < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)) {
            if (waited >= CF_IO_TIMEOUT_MS)
                return MBEDTLS_ERR_SSL_TIMEOUT;
            rt_thread_mdelay(10);
            waited += 10;
            continue;
        }
        return MBEDTLS_ERR_NET_SEND_FAILED;
    }
    return (int)len;
}

static int cf_net_recv(void *ctx, unsigned char *buf, size_t len)
{
    int fd = *(int *)ctx;
    int waited = 0;

    for (;;) {
        int n = recv(fd, buf, len, 0);
        if (n > 0)   return n;
        if (n == 0)  return MBEDTLS_ERR_NET_CONN_RESET;

        if (errno == EWOULDBLOCK || errno == EAGAIN) {
            if (waited >= CF_IO_TIMEOUT_MS)
                return MBEDTLS_ERR_SSL_TIMEOUT;
            rt_thread_mdelay(10);
            waited += 10;
            continue;
        }
        return MBEDTLS_ERR_NET_RECV_FAILED;
    }
}

/* ---------------- TCP 建连 ---------------- */
static int cf_tcp_connect(const char *host, int port)
{
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons((uint16_t)port);

    /* 允许 host 直接写 IP：省掉一次 DNS（本工程所在网络 DNS 不可靠，
     * 语音模块也是靠预置 IP 才稳定下来的） */
    if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) {
        struct addrinfo hints, *res = NULL;
        char sport[8];
        memset(&hints, 0, sizeof(hints));
        hints.ai_family   = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        snprintf(sport, sizeof(sport), "%d", port);
        if (getaddrinfo(host, sport, &hints, &res) != 0 || !res || !res->ai_addr) {
            if (res) freeaddrinfo(res);
            rt_kprintf("[cf] dns fail: %s\n", host);
            return -2;
        }
        memcpy(&sa, res->ai_addr, sizeof(sa));
        freeaddrinfo(res);
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -3;

    /* 先阻塞 connect、成功后再切非阻塞 —— 顺序不能反，否则 connect 会
     * 返回 -1/EINPROGRESS 被误判为失败（ws_client 里踩过这个坑）。 */
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        int e = errno;
        rt_kprintf("[cf] tcp connect %s:%d fail (errno=%d)\n", host, port, e);
        close(fd);
        return -3;
    }

    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) (void)fcntl(fd, F_SETFL, fl | O_NONBLOCK);

    return fd;
}

/* ---------------- TLS 握手 ---------------- */
static int cf_tls_handshake(cf_conn_t *c, const char *host)
{
    static const char pers[] = "mc632x_cf";

    mbedtls_ssl_init(&c->ssl);
    mbedtls_ssl_config_init(&c->conf);
    mbedtls_entropy_init(&c->entropy);
    mbedtls_ctr_drbg_init(&c->ctr_drbg);

    if (mbedtls_ctr_drbg_seed(&c->ctr_drbg, mbedtls_entropy_func, &c->entropy,
                              (const unsigned char *)pers, sizeof(pers) - 1) != 0)
        return -4;

    if (mbedtls_ssl_config_defaults(&c->conf, MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0)
        return -4;

    /* 与 ws_client 一致：本板无可信 RTC 时间，证书有效期校验必失败 */
    mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&c->conf, mbedtls_ctr_drbg_random, &c->ctr_drbg);

    if (mbedtls_ssl_setup(&c->ssl, &c->conf) != 0) return -4;

    mbedtls_ssl_set_hostname(&c->ssl, host);      /* SNI，讯飞的 LB 依赖它 */
    mbedtls_ssl_set_bio(&c->ssl, &c->fd, cf_net_send, cf_net_recv, NULL);

    int hr;
    do {
        hr = mbedtls_ssl_handshake(&c->ssl);
        if (hr == MBEDTLS_ERR_SSL_WANT_READ || hr == MBEDTLS_ERR_SSL_WANT_WRITE)
            rt_thread_mdelay(10);
    } while (hr == MBEDTLS_ERR_SSL_WANT_READ || hr == MBEDTLS_ERR_SSL_WANT_WRITE);

    if (hr != 0) {
        rt_kprintf("[cf] tls handshake fail -0x%x\n", (unsigned int)(-hr));
        return -4;
    }
    return 0;
}

/* ---------------- 统一的写/读（透明处理 TLS） ---------------- */
static int cf_write_all(cf_conn_t *c, const char *buf, int len)
{
    int off = 0;
    while (off < len) {
        int n;
        if (c->use_tls) {
            n = mbedtls_ssl_write(&c->ssl, (const unsigned char *)buf + off, (size_t)(len - off));
            if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) {
                rt_thread_mdelay(10);
                continue;
            }
        } else {
            n = send(c->fd, buf + off, (size_t)(len - off), 0);
            if (n < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)) {
                rt_thread_mdelay(10);
                continue;
            }
        }
        if (n <= 0) return -5;
        off += n;
    }
    return 0;
}

static int cf_read_some(cf_conn_t *c, char *buf, int len)
{
    for (;;) {
        int n;
        if (c->use_tls) {
            n = mbedtls_ssl_read(&c->ssl, (unsigned char *)buf, (size_t)len);
            if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) {
                rt_thread_mdelay(10);
                continue;
            }
            if (n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || n == 0) return 0;
            if (n == MBEDTLS_ERR_NET_CONN_RESET) return 0;
        } else {
            n = recv(c->fd, buf, (size_t)len, 0);
            if (n < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)) {
                rt_thread_mdelay(10);
                continue;
            }
            if (n == 0) return 0;
        }
        if (n < 0) return -6;
        return n;
    }
}

/* ---------------- body 便捷封装 ---------------- */
static int mem_read_cb(void *user, char *buf, int buflen)
{
    cf_body_mem_t *m = (cf_body_mem_t *)user;
    int left = m->len - m->off;
    int n;

    if (left <= 0) return 0;
    n = MIN(left, buflen);
    memcpy(buf, m->data + m->off, (size_t)n);
    m->off += n;
    return n;
}

void cf_body_mem_init(cf_body_mem_t *m, const char *data, int len)
{
    m->data = data;
    m->len  = len;
    m->off  = 0;
}

cf_body_t cf_body_mem_as_reader(cf_body_mem_t *m)
{
    cf_body_t b;
    b.read = mem_read_cb;
    b.user = m;
    return b;
}

/* ---------------- 主入口 ---------------- */
int cf_http_post(const char *host, const char *vhost, int port, int use_tls,
                 const char *path_query, const char *extra_hdr,
                 int body_len, cf_body_t *body,
                 char *resp, int resp_size, int timeout_ms)
{
    cf_conn_t *c = &s_conn;
    int ret = 0;
    int sent = 0;
    int total = 0;
    rt_tick_t t_end;

    if (!host || !host[0] || !path_query || !resp || resp_size < 64 ||
        body_len < 0 || !body || !body->read)
        return -8;

    /* vhost 只影响 SNI 和 Host 头，不参与建连。
     * 直连预置 IP 时必须给对，否则讯飞 LB 会直接 RST（见 cf_http.h 注释）。 */
    if (!vhost || !vhost[0]) vhost = host;

    if (resp_size > 0) resp[0] = '\0';

    memset(c, 0, sizeof(*c));
    c->fd = -1;
    c->use_tls = use_tls;

    c->fd = cf_tcp_connect(host, port);
    if (c->fd < 0) return c->fd;

    if (use_tls) {
        ret = cf_tls_handshake(c, vhost);
        if (ret != 0) goto out;
    }

    /* 请求头：Host 必带；Content-Length 由 body_len 给出，服务端据此判断
     * 请求体边界，因此本条路径不依赖 chunked 编码支持。 */
    {
        int hl = snprintf(s_hdr, sizeof(s_hdr),
                          "POST %s HTTP/1.1\r\n"
                          "Host: %s\r\n"
                          "Content-Length: %d\r\n"
                          "Connection: close\r\n"
                          "Accept: */*\r\n"
                          "%s"
                          "\r\n",
                          path_query, vhost, body_len,
                          extra_hdr ? extra_hdr : "");
        if (hl <= 0 || hl >= (int)sizeof(s_hdr)) { ret = -8; goto out; }
        ret = cf_write_all(c, s_hdr, hl);
        if (ret != 0) goto out;
    }

    /* 请求体：按块拉取，全程只占一个 CF_BODY_CHUNK 的缓冲 */
    while (sent < body_len) {
        int want = MIN(CF_BODY_CHUNK, body_len - sent);
        int n = body->read(body->user, s_tx, want);

        if (n < 0) { ret = -5; goto out; }
        /* 回调提前结束 ⇒ 实际 body 比 Content-Length 短。此时若继续发，
         * 服务端会一直等剩下的字节直到超时，不如立刻失败并回报。 */
        if (n == 0) { ret = -5; goto out; }
        if (n > body_len - sent) n = body_len - sent;

        ret = cf_write_all(c, s_tx, n);
        if (ret != 0) goto out;
        sent += n;
    }

    /* 响应：读到对端关闭或缓冲满。发过 Connection: close，服务端会主动关，
     * 所以不需要解析 chunked，也不需要等 Content-Length 收够。 */
    t_end = rt_tick_get() + rt_tick_from_millisecond((rt_int32_t)timeout_ms);
    while (total < resp_size - 1) {
        int n = cf_read_some(c, resp + total, resp_size - 1 - total);
        if (n < 0) {
            /* 已经收到东西就别把结果丢掉：不少服务端发完响应直接 RST，
             * 此时 recv 返回 ECONNRESET，但正文其实收全了。
             * 让下面的状态行解析去决定成败（解析不出来自然判 -6）。 */
            if (total <= 0) { ret = -6; break; }
            break;
        }
        if (n == 0) break;                       /* 正常结束 */
        total += n;
        /* 用有符号差值比较，rt_tick 回绕时也不会误判 */
        if ((rt_int32_t)(rt_tick_get() - t_end) >= 0) break;
    }
    resp[total] = '\0';
    if (total == 0 && ret == 0) ret = -7;

    /* 从状态行取 HTTP 码，如 "HTTP/1.1 200 OK" */
    if (ret == 0) {
        int code = 0;
        if (sscanf(resp, "HTTP/%*d.%*d %d", &code) == 1 && code > 0)
            ret = code;
        else
            ret = -6;
    }

out:
    if (use_tls) {
        (void)mbedtls_ssl_close_notify(&c->ssl);
        mbedtls_ssl_free(&c->ssl);
        mbedtls_ssl_config_free(&c->conf);
        mbedtls_ctr_drbg_free(&c->ctr_drbg);
        mbedtls_entropy_free(&c->entropy);
    }
    if (c->fd >= 0) close(c->fd);
    c->fd = -1;

    return ret;
}

int cf_http_post_mem(const char *host, const char *vhost, int port, int use_tls,
                     const char *path_query, const char *extra_hdr,
                     const char *data, int len,
                     char *resp, int resp_size, int timeout_ms)
{
    cf_body_mem_t m;
    cf_body_t b;

    cf_body_mem_init(&m, data, len);
    b = cf_body_mem_as_reader(&m);
    return cf_http_post(host, vhost, port, use_tls, path_query, extra_hdr,
                        len, &b, resp, resp_size, timeout_ms);
}
