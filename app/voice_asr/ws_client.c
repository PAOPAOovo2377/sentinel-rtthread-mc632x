/*
 * ws_client.c - 极简 WSS 客户端（lwIP socket + mbedtls 2.x + 手写 WS 帧）
 *
 * 仅实现对接讯飞 iat 所需的最小子集：
 *   - TCP 连接（getaddrinfo 解析域名）
 *   - mbedtls TLS 客户端握手（开发期跳过证书校验）
 *   - RFC6455 WS 握手（Sec-WebSocket-Key）
 *   - 发送带掩码的文本帧；接收并解析文本帧（含分片/ping/pong/close）
 */

#include "ws_client.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>       /* F_GETFL / O_NONBLOCK：不依赖 SO_RCVTIMEO 做超时 */
#include <unistd.h>      /* close() 来自 dfs，不在 socket.h 中 */
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#include <mbedtls/ssl.h>
#include <mbedtls/net_sockets.h>   /* MBEDTLS_ERR_NET_SEND_FAILED / RECV_FAILED 等错误码 */
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/base64.h>

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

/* ---------------- 连接上下文 ---------------- */
struct wss_ctx {
    int fd;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config  conf;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    unsigned char rx[8192];   /* WS 帧重组缓冲 */
    int rxlen;
};

/* ---------------- errno -> 人话 ----------------
 * 不用 strerror()：本移植的 libc 未必提供该符号（链不上就整包编不过），
 * 而且 libc 的错误码表与 lwIP 的 errno 段对应关系并不保证一致。
 * 这里只映射网络路径上真会遇到的几个。 */
static const char *ws_err_str(int e) {
    switch (e) {
    case 0:            return "ok";
    case EINPROGRESS:  return "EINPROGRESS(非阻塞连接进行中)";
    case EALREADY:     return "EALREADY";
    case EISCONN:      return "EISCONN";
    case ECONNRESET:   return "ECONNRESET(收到RST)";
    case ECONNREFUSED: return "ECONNREFUSED(端口没监听)";
    case ETIMEDOUT:    return "ETIMEDOUT";
    case EHOSTUNREACH: return "EHOSTUNREACH(主机不可达)";
    case ENETUNREACH:  return "ENETUNREACH(没有路由)";
    case ENETDOWN:     return "ENETDOWN(网卡down)";
    case EPIPE:        return "EPIPE";
    case ENOTCONN:     return "ENOTCONN";
    case EWOULDBLOCK:  return "EWOULDBLOCK";
    default:           return "?";
    }
}

/* ---------------- mbedtls <-> lwIP socket 桥 ----------------
 * socket 在 wss_connect 里设为非阻塞；这里用轮询实现超时。
 * 不依赖 SO_RCVTIMEO —— 该选项在本 SDK 的 lwIP 上未必生效，一旦失效
 * recv() 会无限阻塞，TLS 握手表现为"打印完 connect 就再无任何输出"。 */
#define WS_IO_TIMEOUT_MS 5000

/* ---------------- 调试：把 TLS 记录原样打出来 ----------------
 * 讯飞 LB 用 SNI 选 vhost：SNI 匹配不到就直接掐 TCP（无 alert），
 * mbedtls 侧只剩 -0x4c = MBEDTLS_ERR_NET_RECV_FAILED，看不出真相。
 * 所以在收发回调上抓首包：
 *   TX#1 = ClientHello（能直接看到有没有带 iat-api.xfyun.cn）
 *   RX#1 = 服务端的第一个记录（0x16 握手 / 0x15 警报）
 * rt_kprintf 不支持宽度修饰，hex 自己拼。 */
static const char *g_hello_host = NULL;   /* wss_connect 设置的 SNI 目标 */
static int g_tx_cnt = 0;
static int g_rx_cnt = 0;

static void ws_hexdump(const char *tag, const unsigned char *b, int len) {
    static const char hx[] = "0123456789abcdef";
    char line[16 * 3 + 1];
    for (int i = 0; i < len; i += 16) {
        int n = 0;
        for (int j = 0; j < 16 && i + j < len; j++) {
            unsigned v = b[i + j];
            line[n++] = hx[(v >> 4) & 0xf];
            line[n++] = hx[v & 0xf];
            line[n++] = ' ';
        }
        line[n] = '\0';
        rt_kprintf("%s %d: %s\n", tag, i, line);
    }
}

static void ws_dump_tx1(const unsigned char *b, int len) {
    int n = len > 512 ? 512 : len;   /* 典型 ClientHello 约 180~260 字节，留足余量 */
    rt_kprintf("[ws] TX#1 len=%d type=%x record_ver=%x%x rec_len=%d dump=%d\n",
               len, b[0], b[1], b[2], (b[3] << 8) | b[4], n);
    ws_hexdump("[ws] TX", b, n);
    if (g_hello_host && g_hello_host[0]) {
        int hl = (int)strlen(g_hello_host), at = -1;
        for (int i = 0; i + hl <= len; i++) {
            if (memcmp(b + i, g_hello_host, (size_t)hl) == 0) { at = i; break; }
        }
        if (at >= 0)
            rt_kprintf("[ws] SNI \"%s\" present at %d\n", g_hello_host, at);
        else
            rt_kprintf("[ws] SNI \"%s\" MISSING from ClientHello!\n", g_hello_host);
    }
}

static void ws_dump_rx1(const unsigned char *b, int len) {
    int n = len > 32 ? 32 : len;
    rt_kprintf("[ws] RX#1 len=%d type=%x ver=%x%x\n", len, b[0], b[1], b[2]);
    ws_hexdump("[ws] RX", b, n);
}

static int ws_net_send(void *ctx, const unsigned char *buf, size_t len) {
    int fd = *(int *)ctx;
    const unsigned char *p = buf;
    size_t left = len;
    int waited = 0;

    if (g_tx_cnt == 0 && len >= 3) ws_dump_tx1(buf, (int)len);
    else if (g_tx_cnt < 6) rt_kprintf("[ws] TX#%d len=%d\n", g_tx_cnt + 1, (int)len);
    g_tx_cnt++;

    while (left > 0) {
        int n = send(fd, p, left, 0);
        if (n > 0) {
            p += n;
            left -= (size_t)n;
            waited = 0;
            continue;
        }
        if (n < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)) {
            if (waited >= WS_IO_TIMEOUT_MS) return MBEDTLS_ERR_NET_SEND_FAILED;
            rt_thread_mdelay(10);
            waited += 10;
            continue;
        }
        return MBEDTLS_ERR_NET_SEND_FAILED;
    }
    return (int)len;
}

static int ws_net_recv(void *ctx, unsigned char *buf, size_t len) {
    int fd = *(int *)ctx;
    int waited = 0;

    for (;;) {
        int n = recv(fd, buf, len, 0);
        if (n > 0) {
            if (g_rx_cnt == 0) {
                if (n >= 3) ws_dump_rx1(buf, n);
                else rt_kprintf("[ws] RX#1 len=%d (too short to decode)\n", n);
            }
            g_rx_cnt++;
            return n;
        }
        if (n == 0) {
            rt_kprintf("[ws] RX EOF: peer FIN/closed (waited %d ms)\n", waited);
            return MBEDTLS_ERR_NET_RECV_FAILED;    /* 对端关闭 */
        }

        if (errno == EWOULDBLOCK || errno == EAGAIN) {
            if (waited >= WS_IO_TIMEOUT_MS) return MBEDTLS_ERR_SSL_WANT_READ;
            rt_thread_mdelay(10);
            waited += 10;
            continue;
        }
        /* 其余 errno（ECONNRESET/EPIPE/ENOTCONN…）= 连接真断了，
         * 必须立刻返回错误；否则 mbedtls 会当成"暂时无数据"无限重试。 */
        {
            int e = errno;
            rt_kprintf("[ws] RX errno=%d %s after %d ms\n", e, ws_err_str(e), waited);
        }
        return MBEDTLS_ERR_NET_RECV_FAILED;
    }
}

/* 非安全敏感随机源（掩码 / Sec-WebSocket-Key 用，够用即可） */
static void ws_rand(unsigned char *out, int n) {
    static unsigned long s = 0x12345678UL;
    for (int i = 0; i < n; i++) {
        s = s * 1103515245UL + 12345UL;
        out[i] = (unsigned char)(s >> 16);
    }
}

/* ---------------- 记录服务端 Date 头（401 时用来校时） ---------------- */
static char g_srv_date[64];

static void capture_server_date(const char *hbuf) {
    g_srv_date[0] = '\0';
    /* Date 头一定在状态行之后，故从 "\nDate: " 开始找，避免误命中 URL 里的 date= */
    const char *p = strstr(hbuf, "\nDate: ");
    if (!p) p = strstr(hbuf, "\ndate: ");
    if (!p) return;
    p += 7;                       /* 跳过 "\nDate: " */
    int i = 0;
    while (p[i] && p[i] != '\r' && p[i] != '\n' && i < (int)sizeof(g_srv_date) - 1) {
        g_srv_date[i] = p[i];
        i++;
    }
    g_srv_date[i] = '\0';
}

const char *wss_last_server_date(void) {
    return g_srv_date;
}

/* ---------------- 链路体检 ----------------
 * connect() 失败时最常见的根因不是服务器，而是本机根本没拿到 IP：
 * 每次烧录/断电复位后 DHCP 客户端不会自动重新拉起，ifconfig 显示 0.0.0.0，
 * 此时 connect() 会"立刻"失败（不是超时），很容易被误判成"服务器连不上"。
 *
 * 取本机 IP 的办法：对 UDP 套接字 connect 一个公网地址 —— UDP 的 connect
 * 只做路由选择、**不发任何报文**，随后 getsockname 就能拿到路由选中的源地址。
 * 这样不需要 <lwip/netif.h>（app 的 include 路径里没有 lwip，只有 compat），
 * 避免为了打印一行 IP 去改 makefile。 */
static int ws_local_ip(char *out, int len) {
    struct sockaddr_in d, l;
    socklen_t ll = sizeof(l);
    int s;
    int ok = -1;

    out[0] = '\0';
    s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) { snprintf(out, len, "?"); return -1; }

    memset(&d, 0, sizeof(d));
    d.sin_family = AF_INET;
    d.sin_port   = htons(53);
    inet_pton(AF_INET, "223.5.5.5", &d.sin_addr);

    if (connect(s, (struct sockaddr *)&d, sizeof(d)) != 0) {
        int e = errno;
        snprintf(out, len, "none(%d:%s)", e, ws_err_str(e));
    } else {
        memset(&l, 0, sizeof(l));
        if (getsockname(s, (struct sockaddr *)&l, &ll) != 0 ||
            inet_ntop(AF_INET, &l.sin_addr, out, (socklen_t)len) == NULL) {
            snprintf(out, len, "?");
        } else {
            ok = 0;
        }
    }
    close(s);
    return ok;
}

/* ---------------- 建立连接 + TLS + WS 握手 ---------------- */
wss_ctx_t *wss_connect(const char *host, int port, const char *path_query,
                       const char *date, const char *server_ip) {
    /* 每次握手都是新的一次：先清空缓存的 Date，
     * 否则本次若在 DNS/TCP 阶段就失败，上层会误用过期的旧时间做校准。 */
    g_srv_date[0] = '\0';
    g_tx_cnt = 0;
    g_rx_cnt = 0;
    g_hello_host = host;   /* 供 TX#1 的 SNI 校验用 */

    wss_ctx_t *c = rt_malloc(sizeof(wss_ctx_t));
    if (!c) return NULL;
    memset(c, 0, sizeof(*c));
    c->fd = -1;

    /* 1) 目标地址：优先用预设 IP（inet_pton，0 次 DNS 查询）。
     *    识别路径上完全避免 gethostbyname —— 校园网/ICS 环境下 DNS 可能长
     *    时间无响应，会把语音线程卡死（实测百度域名卡 7 分钟以上）。 */
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons((uint16_t)port);
    int use_ip = 0;

    if (server_ip && server_ip[0] && inet_pton(AF_INET, server_ip, &sa.sin_addr) == 1) {
        use_ip = 1;
    } else {
        struct addrinfo hints, *res = NULL;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family   = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        char sport[8];
        snprintf(sport, sizeof(sport), "%d", port);
        if (getaddrinfo(host, sport, &hints, &res) != 0 || !res || !res->ai_addr) {
            rt_kprintf("[ws] dns fail: %s\n", host);
            if (res) freeaddrinfo(res);
            rt_free(c);
            return NULL;
        }
        memcpy(&sa, res->ai_addr, sizeof(sa));
        freeaddrinfo(res);
    }
    rt_kprintf("[ws] connect %s:%d sni=%s (%s)\n",
               use_ip ? server_ip : host, port, host,
               use_ip ? "preset ip, no dns" : "resolved by dns");

    c->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (c->fd < 0) { rt_free(c); return NULL; }

    /* 本机源地址：只作记录，**不再据此报错**。
     * 这个探针是"UDP connect + getsockname"，本 SDK 的 lwIP 在 UDP connect
     * 时不给 socket 绑定源地址，getsockname 恒返回 0.0.0.0 —— 而同一时刻
     * TCP connect 是成功的（现场日志：local ip=0.0.0.0 下一行就 tcp connected）。
     * 因此把判据改成"TCP 真的失败时"再提示去查 IP，见下面的错误分支。 */
    char lip[40];
    ws_local_ip(lip, (int)sizeof(lip));
    rt_kprintf("[ws] local ip=%s\n", lip);

    /* 关键顺序：先在阻塞模式下 connect，成功后再切非阻塞。
     * 非阻塞 socket 的 connect() 会立刻返回 -1/EINPROGRESS（正在连接中），
     * 若按返回值判失败就会把正常连接误杀——这正是 tcp_probe（阻塞，能连通）
     * 与 wss_connect（旧代码，非阻塞）结论矛盾的原因。 */
    /* 记录 TCP 建连耗时 = 一个 SYN/SYN-ACK 的 RTT（tick=10ms，精度够用）。
     * 这个数字和"[ws] RX errno=104 after N ms"放在一起就能判定 RST 的来源：
     * 若 SYN-ACK 要 30ms，而 ClientHello 发出后 10ms 就收到 RST，
     * 那 RST 在物理上不可能来自对端 —— 只能是本机/网关生成的。 */
    errno = 0;
    rt_tick_t t_conn = rt_tick_get();
    if (connect(c->fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        /* 先把 errno 取下来：后面任何一次库调用（含 rt_kprintf 之外的）都可能
         * 覆盖它，避免打印出与本行无关的二手错误码。 */
        int e = errno;
        int so = -1;
        socklen_t sl = sizeof(so);
        (void)getsockopt(c->fd, SOL_SOCKET, SO_ERROR, (void *)&so, &sl);
        rt_kprintf("[ws] tcp connect fail (errno=%d %s, SO_ERROR=%d, local ip=%s)\n",
                   e, ws_err_str(e), so, lip);
        if (e == EINPROGRESS || e == EALREADY) {
            rt_kprintf("[ws]   连接仍在进行中：本 SDK 的 socket 可能是非阻塞的\n");
        } else if (e == ENETUNREACH || e == EHOSTUNREACH || e == ENETDOWN) {
            rt_kprintf("[ws]   路由不可达：先 ifconfig 看有没有 IP，没有就 dhcpc start e0\n");
        } else if (e == ECONNREFUSED) {
            rt_kprintf("[ws]   对端 RST：IP 上的 443 端口没在监听（节点下线）\n");
        }
        close(c->fd); rt_free(c);
        return NULL;
    }

    /* 连上之后才设非阻塞：后续 IO 超时由 ws_net_send/recv 轮询控制，
     * 不依赖 SO_RCVTIMEO（该选项在本 SDK 的 lwIP 上不可靠）。 */
    rt_uint32_t syn_ticks = rt_tick_get() - t_conn;
    int fl = fcntl(c->fd, F_GETFL, 0);
    if (fl >= 0 && fcntl(c->fd, F_SETFL, fl | O_NONBLOCK) < 0)
        rt_kprintf("[ws] WARN: set O_NONBLOCK failed\n");
    else
        rt_kprintf("[ws] tcp connected (SYN-ACK %d ms)\n",
                   (int)(syn_ticks * 1000 / RT_TICK_PER_SECOND));

    /* 2) TLS */
    mbedtls_entropy_init(&c->entropy);
    mbedtls_ctr_drbg_init(&c->ctr_drbg);
    const char pers[] = "mc632x_ws";
    if (mbedtls_ctr_drbg_seed(&c->ctr_drbg, mbedtls_entropy_func, &c->entropy,
                              (const unsigned char *)pers, sizeof(pers) - 1) != 0)
        goto tls_fail;
    mbedtls_ssl_config_init(&c->conf);
    if (mbedtls_ssl_config_defaults(&c->conf, MBEDTLS_SSL_IS_CLIENT,
            MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT) != 0)
        goto tls_fail;
    mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_NONE);  /* 开发期跳过证书校验 */
    mbedtls_ssl_conf_rng(&c->conf, mbedtls_ctr_drbg_random, &c->ctr_drbg);
    if (mbedtls_ssl_setup(&c->ssl, &c->conf) != 0) goto tls_fail;
    mbedtls_ssl_set_hostname(&c->ssl, host);
    mbedtls_ssl_set_bio(&c->ssl, &c->fd, ws_net_send, ws_net_recv, NULL);

    int hr;
    /* TLS 握手带 15s 硬超时 + 逐秒进度日志：
     * 若看到 [ws] tls step N ... 一直涨却握手失败，说明服务器侧无响应；
     * 若立刻报 -0x7680 等，则是协议/证书协商问题。 */
    rt_tick_t tls_end = rt_tick_get() + rt_tick_from_millisecond(15000);
    rt_tick_t last_log = rt_tick_get();
    int rounds = 0;
    do {
        hr = mbedtls_ssl_handshake(&c->ssl);
        if (hr == MBEDTLS_ERR_SSL_WANT_READ || hr == MBEDTLS_ERR_SSL_WANT_WRITE) {
            if (rt_tick_get() > tls_end) {
                rt_kprintf("[ws] tls handshake TIMEOUT (>15s), last=0x%x\n",
                           (unsigned int)(-hr));
                hr = MBEDTLS_ERR_SSL_TIMEOUT;
                break;
            }
            rt_thread_mdelay(10);
            if (++rounds % 100 == 0 &&   /* 每秒一条 */
                rt_tick_get() - last_log >= rt_tick_from_millisecond(1000)) {
                last_log = rt_tick_get();
                rt_kprintf("[ws] tls step %d, waiting... (last=0x%x)\n",
                           rounds / 100, (unsigned int)(-hr));
            }
        }
    } while (hr == MBEDTLS_ERR_SSL_WANT_READ || hr == MBEDTLS_ERR_SSL_WANT_WRITE);
    if (hr != 0) {
        rt_kprintf("[ws] tls handshake fail -0x%x\n", (unsigned int)(-hr));
        switch (-hr) {
        case 0x4c:
            rt_kprintf("[ws]   = NET_RECV_FAILED: 服务端在 ClientHello 之后主动关闭\n"
                       "[ws]   常见原因: SNI 与 LB 的 vhost 不匹配（见上面 TX#1）\n");
            break;
        case 0x4e:
            rt_kprintf("[ws]   = NET_SEND_FAILED: 发不出去（链路/窗口问题）\n");
            break;
        case 0x2700:
            rt_kprintf("[ws]   = X509 证书校验失败（本工程已 VERIFY_NONE，不应出现）\n");
            break;
        case 0x6800:
            rt_kprintf("[ws]   = SSL_TIMEOUT: 15s 内握手没走完（丢包/被限速）\n");
            break;
        case 0x7780:
            rt_kprintf("[ws]   = FATAL_ALERT: 服务端回了 alert，多半是协议版本/套件不匹配\n");
            break;
        default:
            break;
        }
        goto tls_fail;
    }
    rt_kprintf("[ws] tls ok (%s)\n",
               mbedtls_ssl_get_version(&c->ssl) ? mbedtls_ssl_get_version(&c->ssl) : "?");

    /* 3) WS 握手 */
    unsigned char key[16]; ws_rand(key, 16);
    char key_b64[32]; size_t kol = 0;
    mbedtls_base64_encode((unsigned char *)key_b64, sizeof(key_b64), &kol, key, 16);
    key_b64[kol] = 0;

    char req[1536];   /* path 含长鉴权串，留足余量 */
    int rl = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Date: %s\r\n"          /* 讯飞签名 headers 含 date，头与 query 必须一致 */
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n",
        path_query, host, date ? date : "", key_b64);

    int sent = 0;
    rt_tick_t send_end = rt_tick_get() + rt_tick_from_millisecond(10000);
    while (sent < rl) {
        int n = mbedtls_ssl_write(&c->ssl, (const unsigned char *)req + sent, rl - sent);
        if (n > 0) sent += n;
        else if (n == MBEDTLS_ERR_SSL_WANT_WRITE || n == MBEDTLS_ERR_SSL_WANT_READ) {
            if (rt_tick_get() > send_end) {
                rt_kprintf("[ws] handshake send TIMEOUT\n");
                goto tls_fail;
            }
            rt_thread_mdelay(10);
        }
        else { rt_kprintf("[ws] handshake send fail 0x%x\n", (unsigned int)(-n)); goto tls_fail; }
    }

    /* 读握手响应直到 \r\n\r\n，再多读几轮把 body（错误原因 JSON）也收进来。
     * socket 为非阻塞，mbedtls_ssl_read 会立即返回 WANT_READ，
     * 因此这里必须自己 sleep 重试（原来靠阻塞 socket 才等得到数据）。 */
    char hbuf[1024]; int hlen = 0;
    rt_tick_t hs_end = rt_tick_get() + rt_tick_from_millisecond(15000);
    while (hlen < (int)sizeof(hbuf) - 1) {
        int n = mbedtls_ssl_read(&c->ssl, (unsigned char *)hbuf + hlen,
                                 sizeof(hbuf) - 1 - hlen);
        if (n > 0) {
            hlen += n;
            if (strstr(hbuf, "\r\n\r\n")) {
                /* 头已收全：轮询等待 body——讯飞的 401 原因（签名错误 / IP 白名单 /
                 * appid 无权限）就在 body 里，必须等它到齐才能诊断。 */
                rt_tick_t bend = rt_tick_get() + rt_tick_from_millisecond(1500);
                while (hlen < (int)sizeof(hbuf) - 1 && rt_tick_get() < bend) {
                    int m = mbedtls_ssl_read(&c->ssl, (unsigned char *)hbuf + hlen,
                                             sizeof(hbuf) - 1 - hlen);
                    if (m > 0) { hlen += m; continue; }
                    if (m == MBEDTLS_ERR_SSL_WANT_READ ||
                        m == MBEDTLS_ERR_SSL_WANT_WRITE) {
                        rt_thread_mdelay(10);
                        continue;
                    }
                    break;
                }
                break;
            }
        }
        else if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) {
            if (rt_tick_get() > hs_end) {
                rt_kprintf("[ws] handshake read TIMEOUT (>15s)\n");
                goto tls_fail;
            }
            rt_thread_mdelay(10);
        }
        else {
            rt_kprintf("[ws] handshake read fail 0x%x\n", (unsigned int)(-n));
            goto tls_fail;
        }
    }
    hbuf[hlen] = 0;
    capture_server_date(hbuf);   /* 401 响应里也带真实时间，留给上层校时重试 */

    /* 只打状态行（到第一个 CRLF 为止），body 单独打一行，便于看清 */
    {
        char *eol = strstr(hbuf, "\r\n");
        if (eol) *eol = '\0';
        rt_kprintf("[ws] handshake resp: %s\n", hbuf);
        if (eol) *eol = '\r';   /* 还原（eol 可能为 NULL，必须判空，否则野指针写入） */
    }
    /* body：讯飞在 401/403 时会在此给出具体原因（签名错误 / IP 白名单 / 无权限） */
    {
        char *body = strstr(hbuf, "\r\n\r\n");
        if (body && body[4]) {
            body += 4;
            char *nl = strpbrk(body, "\r\n");
            if (nl) *nl = '\0';
            rt_kprintf("[ws] body: %s\n", body);
            if (nl) *nl = '\r';
        }
    }
    if (!strstr(hbuf, "101") || strncmp(hbuf, "HTTP/1.", 7) != 0) {
        rt_kprintf("[ws] handshake FAILED (need 101); check api_key/api_secret/appid/ip-white-list\n");
        goto tls_fail;
    }
    rt_kprintf("[ws] connected\n");
    return c;

tls_fail:
    if (c->fd >= 0) close(c->fd);
    mbedtls_ssl_free(&c->ssl);
    mbedtls_ssl_config_free(&c->conf);
    mbedtls_ctr_drbg_free(&c->ctr_drbg);
    mbedtls_entropy_free(&c->entropy);
    rt_free(c);
    return NULL;
}

/* ---------------- 发送（带掩码文本帧） ---------------- */
static int wss_send_frame(wss_ctx_t *c, int opcode, const unsigned char *data, int len) {
    unsigned char hdr[14];
    int hl = 0;
    hdr[0] = (unsigned char)(0x80 | (opcode & 0x0f));   /* FIN + opcode */
    if (len < 126) {
        hdr[1] = (unsigned char)(0x80 | len); hl = 2;
    } else if (len < 65536) {
        hdr[1] = 0x80 | 126; hdr[2] = (len >> 8) & 0xff; hdr[3] = len & 0xff; hl = 4;
    } else {
        hdr[1] = 0x80 | 127;
        for (int i = 0; i < 8; i++) hdr[2 + i] = (unsigned char)((len >> (8 * (7 - i))) & 0xff);
        hl = 10;
    }
    unsigned char mask[4]; ws_rand(mask, 4);
    memcpy(hdr + hl, mask, 4); hl += 4;

    unsigned char *out = rt_malloc(hl + len);
    if (!out) return -1;
    memcpy(out, hdr, hl);
    for (int i = 0; i < len; i++) out[hl + i] = data[i] ^ mask[i & 3];

    int total = hl + len, sent = 0;
    rt_tick_t end = rt_tick_get() + rt_tick_from_millisecond(15000);
    while (sent < total) {
        int n = mbedtls_ssl_write(&c->ssl, out + sent, total - sent);
        if (n > 0) { sent += n; continue; }
        if (n == MBEDTLS_ERR_SSL_WANT_WRITE || n == MBEDTLS_ERR_SSL_WANT_READ) {
            if (rt_tick_get() > end) { rt_free(out); return -1; }
            rt_thread_mdelay(10);
            continue;
        }
        rt_free(out);
        return -1;
    }
    rt_free(out);
    return 0;
}

int wss_send_text(wss_ctx_t *c, const char *data, int len) {
    return wss_send_frame(c, 0x1, (const unsigned char *)data, len);
}

/* ---------------- 接收（解析文本帧，处理分片/控制帧） ---------------- */
/* 返回：1=得到完整文本消息；0=连接关闭；-3=控制帧已处理(继续)；-1=需更多数据；-2=溢出 */
static int ws_parse(wss_ctx_t *c, char *buf, int buflen, int *total, int *closed) {
    if (c->rxlen < 2) return -1;
    unsigned char *p = c->rx;
    int b0 = p[0], b1 = p[1];
    int fin  = b0 & 0x80;
    int opcode = b0 & 0x0f;
    int mask = b1 & 0x80;
    int len  = b1 & 0x7f;
    int off = 2;
    if (len == 126) { if (c->rxlen < 4) return -1; len = (p[2] << 8) | p[3]; off = 4; }
    else if (len == 127) {
        if (c->rxlen < 10) return -1;
        unsigned long long ll = 0;
        for (int i = 0; i < 8; i++) ll = (ll << 8) | p[2 + i];
        if (ll > (unsigned long long)sizeof(c->rx)) return -2;   /* 超出重组缓冲 */
        len = (int)ll;
        off = 10;
    }
    unsigned char m[4] = {0};
    if (mask) { if (c->rxlen < off + 4) return -1; memcpy(m, p + off, 4); off += 4; }
    if (c->rxlen < off + len) return -1;   /* 帧未收全 */

    unsigned char *pl = p + off;
    if (mask) for (int i = 0; i < len; i++) pl[i] ^= m[i & 3];

    /* 消费本帧 */
    memmove(c->rx, p + off + len, c->rxlen - off - len);
    c->rxlen -= off + len;

    if (opcode == 0x8) { *closed = 1; return 0; }                 /* close */
    if (opcode == 0x9) { wss_send_frame(c, 0x8A, pl, len); return -3; }  /* ping→pong */
    if (opcode == 0xA) { return -3; }                             /* pong, 忽略 */

    /* text(1)/binary(2)/continuation(0) 累加到 buf */
    int copyn = len;
    if (*total + copyn > buflen) copyn = buflen - *total;
    if (copyn > 0) memcpy(buf + *total, pl, copyn);
    *total += len;
    if (fin) return 1;
    return -1;   /* 分片，等后续帧 */
}

int wss_recv_text(wss_ctx_t *c, char *buf, int buflen) {
    int total = 0, closed = 0;
    rt_tick_t end = rt_tick_get() + rt_tick_from_millisecond(20000);
    while (1) {
        int r = ws_parse(c, buf, buflen, &total, &closed);
        if (closed) return 0;
        if (r == 1) return (total >= buflen) ? buflen - 1 : total;   /* 钳制到 buflen-1，给调用方留 '\0' 位 */
        if (r == -3) continue;
        if (r == -2) return -2;
        /* r == -1 需要更多数据 */
        if (rt_tick_get() > end) { rt_kprintf("[ws] recv timeout\n"); return -1; }
        if (c->rxlen >= (int)sizeof(c->rx)) return -2;

        int n = mbedtls_ssl_read(&c->ssl, c->rx + c->rxlen, sizeof(c->rx) - c->rxlen);
        if (n > 0) { c->rxlen += n; continue; }
        if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) {
            /* 非阻塞 socket：没数据是常态，短暂等一下再问，别空转 */
            rt_thread_mdelay(10);
            continue;
        }
        rt_kprintf("[ws] ssl read err 0x%x\n", (unsigned int)(-n));
        return -1;   /* 真断连 */
    }
}

/* ---------------- 关闭 ---------------- */
void wss_close(wss_ctx_t *c) {
    if (!c) return;
    wss_send_frame(c, 0x8, (const unsigned char *)"", 0);   /* close 帧 */
    mbedtls_ssl_close_notify(&c->ssl);
    if (c->fd >= 0) close(c->fd);
    mbedtls_ssl_free(&c->ssl);
    mbedtls_ssl_config_free(&c->conf);
    mbedtls_ctr_drbg_free(&c->ctr_drbg);
    mbedtls_entropy_free(&c->entropy);
    rt_free(c);
}
