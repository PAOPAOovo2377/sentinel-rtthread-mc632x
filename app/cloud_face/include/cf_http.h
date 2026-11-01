/*
 * cf_http.h - 极简 HTTP/HTTPS 客户端（仅 POST，单向流式发送 body）
 *
 * 为什么不用 SDK 自带的 libcurl：
 *   1) rtconfig.h 里虽有 FH_USING_LIBCURL，但 media_demo.map 中 curl_easy_* 零命中，
 *      说明当前固件从未链接过它，能否在本平台 lwIP 上跑通完全未经验证；
 *   2) 它的缓冲区由内部 malloc 管理，尺寸不可控，与本模块"内存优先"的目标冲突。
 * 所以这里按 voice_asr/ws_client.c 里**已在现场跑通**的 TCP+mbedTLS 套路重写一份，
 * 只保留 HTTP POST 所需的最小功能。
 *
 * 设计要点：
 *   - body 通过回调分块拉取，调用方不需要在内存里拼出完整请求体
 *     （讯飞人脸比对的 body 是「前缀 + 两张图 base64 + 后缀」，整拼要上百 KB）；
 *   - 请求头固定发 Connection: close，靠服务端关闭连接来界定响应边界，
 *     省掉 chunked / Content-Length 的完整解析；
 *   - TLS 走 MBEDTLS_SSL_VERIFY_NONE，与 ws_client 保持一致（本板无 RTC 可信时间，
 *     证书有效期校验会失败；且 MC632X 的 SRAM 放不下完整 CA 链）。
 */
#ifndef CF_HTTP_H
#define CF_HTTP_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * body 读取回调：把请求体按块填进 buf。
 *   返回 >0 : 本次写入 buf 的字节数（必须 <= buflen）
 *   返回  0 : 请求体结束
 *   返回 <0 : 出错，本次请求中止
 * 注意：实现方必须是无状态的"顺序输出"语义，cf_http 只会顺序调用、不回头重读。
 */
typedef int (*cf_body_read_fn)(void *user, char *buf, int buflen);

typedef struct {
    cf_body_read_fn read;
    void           *user;
} cf_body_t;

/*
 * 发起一次 POST。
 *   host        : 实际要连的地址，域名或 IP，如 "api.xf-yun.com" / "192.168.137.1"
 *   vhost       : 逻辑域名，同时用于 TLS SNI 和 Host 请求头。传 NULL 就等于 host。
 *                 ★ 这两个参数必须分开：工程所在网络 DNS 不可靠，讯飞模式常用
 *                   预置 IP 直连（host = "1.x.x.x"），但 LB 靠 **SNI 和 Host 头**
 *                   选 vhost —— 若把 IP 填进 SNI，讯飞会直接 RST，连 TLS alert
 *                   都不发（ws_client.c 的 TX#1 探针就是为这个坑加的）。
 *                   所以直连 IP 时：host = IP，vhost = "api.xf-yun.com"。
 *   port        : 443 / 5000 ...
 *   use_tls     : 1 = HTTPS（TLS），0 = 裸 HTTP（局域网 PC 用这个）
 *   path_query  : 以 '/' 开头，可带 query，如 "/v1/private/s67c9c78c?authorization=..."
 *   extra_hdr   : 额外请求头，每行以 \r\n 结尾；可为 NULL
 *                 Host / Content-Type / Content-Length 由本函数自动补，不要重复给
 *   body_len    : 请求体总字节数（用于填 Content-Length）
 *   body        : 请求体回调
 *   resp        : 响应缓冲（收前 resp_size-1 字节，保证 '\0' 结尾）
 *   resp_size   : 响应缓冲大小，建议 1024
 *   timeout_ms  : 整体超时（连接 + 发送 + 接收）
 * 返回：HTTP 状态码（200/401/403...）；<0 表示传输层失败：
 *       -1 内存, -2 DNS, -3 TCP, -4 TLS 握手, -5 发送, -6 接收, -7 超时, -8 参数
 */
int cf_http_post(const char *host, const char *vhost, int port, int use_tls,
                 const char *path_query, const char *extra_hdr,
                 int body_len, cf_body_t *body,
                 char *resp, int resp_size, int timeout_ms);

/*
 * 需要一个简单的"一次性 body"时用这个：把整块内存当请求体发出去。
 * 内部用 cf_http_post + 一个游标状态，所以只能在同一个对象上串行使用。
 */
typedef struct {
    const char *data;
    int         len;
    int         off;
} cf_body_mem_t;

void cf_body_mem_init(cf_body_mem_t *m, const char *data, int len);
cf_body_t cf_body_mem_as_reader(cf_body_mem_t *m);

/* 便捷：POST 一整块内存（PC 直传 JPEG 走这条）。vhost 传 NULL 即等于 host。 */
int cf_http_post_mem(const char *host, const char *vhost, int port, int use_tls,
                     const char *path_query, const char *extra_hdr,
                     const char *data, int len,
                     char *resp, int resp_size, int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* CF_HTTP_H */
