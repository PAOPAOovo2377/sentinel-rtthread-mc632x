#ifndef WS_CLIENT_H
#define WS_CLIENT_H

/*
 * ws_client.h - 极简 WSS(WebSocket over TLS) 客户端
 *
 * 用途：在 MC632X（RT-Thread + lwIP + mbedtls）上对接讯飞语音听写(iat)的
 *       WebSocket 接口。SDK 自带的 libcurl 是 7.67（不支持 WebSocket），
 *       故这里直接基于 lwIP socket + mbedtls 手写 TLS 与 WS 帧。
 *
 * 约束：单连接、一次性使用（与"一句话识别"语义匹配）。
 */
#include <rtthread.h>

typedef struct wss_ctx wss_ctx_t;

/*
 * 建立 WSS 连接并完成 WS 握手。
 *  host       : 域名，如 "iat-api.xfyun.cn"（用于 TLS SNI 与 Date 头）
 *  port       : 通常为 443
 *  path_query : 含鉴权参数的请求路径，如 "/v2/iat?authorization=...&date=...&host=..."
 *  date       : RFC1123 GMT 时间串。讯飞要求签名 headers 含 "host date request-line"，
 *               因此 HTTP 请求头必须同时带 Date（与 query 里的 date 保持一致）。
 *  server_ip  : 可选的服务器 IP（非空则直接 inet_pton 连接，0 次 DNS 查询）。
 *               用于把 DNS 从连接关键路径上彻底摘掉。
 *  返回非 NULL 表示成功；失败返回 NULL（串口有 [ws] 错误打印）
 */
wss_ctx_t *wss_connect(const char *host, int port, const char *path_query,
                       const char *date, const char *server_ip);

/* 发送一帧文本（自动加客户端掩码），返回 0 成功 */
int wss_send_text(wss_ctx_t *c, const char *data, int len);

/*
 * 接收下一帧完整文本消息到 buf（自动处理分片/ping/pong/close）。
 *  返回 >0 : 消息字节数
 *  返回  0 : 对端已关闭
 *  返回 <0 : 超时/错误
 */
int wss_recv_text(wss_ctx_t *c, char *buf, int buflen);

/* 关闭：发 close 帧 + 释放 mbedtls + 关闭 socket */
void wss_close(wss_ctx_t *c);

/*
 * 取最近一次握手响应里的服务端 Date 头（RFC1123 GMT，如
 * "Thu, 15 Oct 2026 08:08:34 GMT"）；没有则返回空串。
 * 用途：401 拒签时服务器仍会带回真实时间，用它校准系统时间后重试握手，
 *       从而摆脱对 NTP(UDP/123) 和 HTTP(TCP/80) 校时服务的依赖。
 */
const char *wss_last_server_date(void);

#endif /* WS_CLIENT_H */
