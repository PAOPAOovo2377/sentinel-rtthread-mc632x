#ifndef XF_HTTP_ASR_H
#define XF_HTTP_ASR_H

#include <rtthread.h>

/*
 * 讯飞语音听写 WebAPI（iat）WebSocket(wss) 客户端 —— 非流式 / 一句话识别
 * 依赖：ws_client（lwIP socket + mbedtls 手写 WS，SDK 自带 libcurl 仅 7.67 不支持 WS）
 * 适用场景：以太网接入，录一小段 16k/16bit/单声道 PCM → 逐帧 WS 发送 → 拿回识别文本
 *
 * 注意：这是"一句话识别"路径，不是流式 iat。一次识别把音频切片逐帧经 WS 发送，
 *       会话靠 sid 串联，最后一帧 status=2 收尾。延迟 300ms~2s，对语音控制足够。
 */

/* ============ 讯飞凭证（本仓库不含真实密钥，请自行填写）============
 * 到讯飞开放平台控制台「语音听写 → WebAPI → 接口认证信息」复制三项填入下方，
 * 也可以用编译宏覆盖：-DXF_APPID=\"xxx\" -DXF_API_KEY=\"xxx\" -DXF_API_SECRET=\"xxx\"
 * 三项必须一字不差，抄错会握手返回 401 "apikey not found"。
 * 泄露后请到控制台「重置密钥」并同步更新此处。*/
#ifndef XF_APPID
#define XF_APPID      "YOUR_APPID"
#endif
#ifndef XF_API_KEY
#define XF_API_KEY    "YOUR_API_KEY"
#endif
#ifndef XF_API_SECRET
#define XF_API_SECRET "YOUR_API_SECRET"
#endif

/* 讯飞 iat 要求的音频格式（必须与采集端一致） */
#define XF_AUDIO_RATE    16000   /* 采样率 16k */
#define XF_AUDIO_CHN     1       /* 单声道 */
#define XF_AUDIO_BITS    16      /* 16bit */

/*
 * 识别结果回调。
 *  text  : 当前已拼接的识别文本
 *  final : 1 = 最终结果（会话已结束）；0 = 中间增量结果
 *  user  : 透传指针
 */
typedef void (*xf_asr_result_cb)(const char *text, int final, void *user);

typedef struct {
    const char *appid;
    const char *api_key;
    const char *api_secret;
    const char *host;        /* 默认 iat-api.xfyun.cn */
    const char *uri;         /* 默认 /v2/iat */
    xf_asr_result_cb cb;
    void *user;
} xf_asr_cfg_t;

/* 覆盖宏默认值，运行时设置凭证（推荐） */
void xf_asr_set_credential(const char *appid, const char *api_key, const char *api_secret);

/*
 * 一次性识别整段 PCM（16k/16bit/单声道 raw，不带 WAV 头）。
 *  pcm  : 音频数据指针
 *  len  : 字节数
 *  cb   : 结果回调（可 NULL，仅打印）
 *  user : 透传指针
 * 返回 0 成功，<0 错误（网络 -6 / 签名 -2 / 内存 -3 / API 错误 -7 等）
 *
 * 时间可信标志：握手成功（101）或用服务端 Date 校时成功后置 1。
 * 上层据此跳过耗时的 NTP/HTTP 校时尝试。
 */
extern int xf_time_trusted;

/* ---------------- 时间工具（统一按 UTC 纪元写系统时间） ----------------
 * 背景：本 SDK 的 set_time() 只写 RTC 硬件、不刷新系统时间（实测：上电
 * 1970-01-01 08:01:19，设 2026-10-15 08:54:32 后 date 仍显示
 * 2026-10-15 08:01:28 —— 只有日期生效）。故一律改用 set_system_time()
 * 直接写 Unix UTC 秒，避免 localtime/mktime 的时区歧义。 */
/* 讯飞语音听写服务域名与端口（预置 IP 机制需要域名常量） */
#define XF_ASR_HOST "iat-api.xfyun.cn"
#define XF_ASR_PORT 443

/* ---------------- 预置服务器 IP（把 DNS 从识别关键路径摘掉） ----------------
 * 传入空串/NULL 表示恢复为域名解析；传入合法 IP 则连接时跳过 DNS 查询。
 * 典型用法：后台线程解析一次域名后回填，或直接 msh 命令 xf_ip 1.2.3.4 手动指定。 */
void xf_asr_set_server_ip(const char *ip);
const char *xf_asr_get_server_ip(void);

long xf_days_from_civil(int y, int m, int d);   /* 公历 -> 自 1970-01-01 的天数 */
void xf_time_set_unix(long unix_sec);           /* 按 Unix UTC 秒设置系统时间 */
int  xf_time_set_gmt(const char *gmt);          /* "Thu, 15 Oct 2026 08:20:00 GMT" -> 设置 */

/* 修正后的当前时间（= time() + 服务端时间偏移）。
 * 本 SDK 系统时间写不进去，故签名与打印一律用它，不直接用 time()。 */
long xf_time_now_unix(void);

/* 丢弃当前时间偏移量。系统时钟被 `date` / NTP 重新对过之后必须调用，
 * 否则偏移量会被重复叠加（表现为签名时间凭空差 8 小时）。 */
void xf_time_offset_clear(const char *why);

int xf_asr_recognize_once(const uint8_t *pcm, int len, xf_asr_result_cb cb, void *user);

#endif /* XF_HTTP_ASR_H */
