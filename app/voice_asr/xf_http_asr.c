/*
 * xf_http_asr.c - 讯飞语音听写(iat) WebSocket 客户端
 *
 * 硬件平台：MC632X（RT-Thread）。传输层使用 ws_client（lwIP + mbedtls 手写 WS），
 * 因为 SDK 自带 libcurl 仅 7.67，不支持 WebSocket，而讯飞"语音听写"只有 wss 接口。
 *
 * 协议要点（均经 PC 端 e2e 实测确认）：
 *  1. 鉴权：HMAC-SHA256 对 "host/date/GET /v2/iat HTTP/1.1" 签名，base64 后作为
 *     query 参数 authorization；date 必须 RFC1123 且与 HTTP Date 头一致。
 *  2. 音频：16k/16bit/单声道 PCM，每帧 1280 字节(40ms)，帧间 mdelay 40ms。
 *     **实测服务端在收到音频前不返回任何响应，且后续帧无需携带 sid**，
 *     故必须"连续发完所有帧 + status=2 收尾"，然后统一收取响应。
 *  3. 响应：data.result 是**嵌套 JSON 对象**（不是 base64），结构为
 *     {"pgs":"apd|rpl","rg":[起,止],"ws":[{"cw":[{"w":"词"}]}]}；
 *     pgs=apd 追加，pgs=rpl 为动态修正（rg 起点之后的旧词须被 ws 覆盖）。
 *  4. 系统时间：该 SDK 写不进系统时间，改用 g_time_offset 偏移补偿。
 */

#include "xf_http_asr.h"
#include "ws_client.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include <mbedtls/md.h>
#include <mbedtls/base64.h>

/* RTC 写时间接口（实现位于 SDK 的 rtc 驱动；sample_common_demo.c 同样 extern） */
extern rt_err_t set_date(rt_uint32_t year, rt_uint32_t month, rt_uint32_t day);
extern rt_err_t set_time(rt_uint32_t hour, rt_uint32_t minute, rt_uint32_t second);
/* kernel/libc/time/clock_time.c：把 RTC 计数重新灌进 timekeeping。
 * RT_USING_TIMEKEEPING=y 时 time() 读的是 timekeeping，写完 RTC 必须调它。 */
extern int clock_time_system_init(void);

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

/* ---------------- 凭证（可被 xf_asr_set_credential 覆盖） ---------------- */
static const char *g_appid      = XF_APPID;
static const char *g_api_key    = XF_API_KEY;
static const char *g_api_secret = XF_API_SECRET;
static const char *g_host       = XF_ASR_HOST;
static const char *g_uri        = "/v2/iat";

/* 预置服务器 IP：非空则连接时完全跳过 DNS（见 ws_connect 的 server_ip 参数） */
static char g_server_ip[32] = {0};

void xf_asr_set_server_ip(const char *ip) {
    if (!ip || !ip[0]) {
        g_server_ip[0] = '\0';
        rt_kprintf("[xf] server ip cleared, using dns\n");
        return;
    }
    snprintf(g_server_ip, sizeof(g_server_ip), "%s", ip);
    rt_kprintf("[xf] server ip preset: %s (skip dns)\n", g_server_ip);
}

const char *xf_asr_get_server_ip(void) {
    return g_server_ip;
}

/* ---------------- 时间偏移补偿（本 SDK 的核心对策） ----------------
 * 实测：该 SDK 的系统时间写不进去——set_system_time() 可能不生效、set_time()
 * 只写 RTC 硬件不刷新系统时间、极端情况下 time() 还会"冻结"不随 tick 前进。
 * 与其和它纠缠，不如**不依赖系统时间**：
 *   1) 首次握手必然 401，但响应头会带回讯飞真实 GMT 时间；
 *   2) 由该时间算出偏移量 g_time_offset = server_unix - time()；
 *   3) 生成签名 date 时用 time() + g_time_offset，即"修正后的系统时间"。
 * 这样无论系统时间能否写入、是否冻结，签名时间都正确 → 重试必定握手成功。
 * 同时仍会尽力写系统时间（写成功则 date 命令显示也正确，属于附加收益）。 */
static long g_time_offset = 0;      /* 服务器时间 - 本地 time() */

/* ---------------- 僵尸偏移自愈 ----------------
 * g_time_offset 只在"系统时钟没被重新对过"的前提下成立。一旦有人用 `date`
 * 把 RTC 拨到正确时间（或 NTP 校时），偏移量本身不会跟着变，于是签名时间
 * 会被**再减一次** 8 小时 —— 本项目真实踩过：date 显示 11:47（已正确），
 * 签名却发 03:47，识别必然失败，而且现象看着像"date 不准"，极易误导。
 * 判据：把墙钟增量与单调 tick 增量作差，差值超过容差即判定"时钟被人拨过"，
 * 立即丢弃偏移量。tick 是 32 位，减法天然回绕安全。 */
#define XF_STEP_TOL_S  60           /* 单次调用间隔内允许的墙钟/单调偏差 */

static time_t   g_seen_t    = 0;    /* 上次观察到的 time() */
static rt_tick_t g_seen_tick = 0;   /* 上次观察到的单调 tick */

void xf_time_offset_clear(const char *why) {
    if (g_time_offset == 0) return;
    rt_kprintf("[xf] drop time offset %ld s (%s)\n", g_time_offset, why ? why : "");
    g_time_offset = 0;
}

long xf_time_now_unix(void) {
    time_t   t  = time(RT_NULL);
    rt_tick_t tk = rt_tick_get();

    if (g_seen_tick != 0 && g_time_offset != 0) {
        long wall = (long)(t - g_seen_t);
        long mono = (long)((rt_tick_t)(tk - g_seen_tick) / RT_TICK_PER_SECOND);
        long step = wall - mono;
        if (step > XF_STEP_TOL_S || step < -XF_STEP_TOL_S)
            xf_time_offset_clear("system clock was reset");
    }
    g_seen_t    = t;
    g_seen_tick = tk;
    return (long)t + g_time_offset;
}

/* 时间可信标志（见 xf_http_asr.h）：握手通过或用服务端 Date 校时成功后置 1 */
int xf_time_trusted = 0;

void xf_asr_set_credential(const char *appid, const char *api_key, const char *api_secret) {
    if (appid)      g_appid = appid;
    if (api_key)    g_api_key = api_key;
    if (api_secret) g_api_secret = api_secret;
}

/* ---------------- base64（mbedtls） ---------------- */
static int my_base64_encode(const uint8_t *src, int slen, char *dst, int dlen) {
    size_t olen = 0;
    int rc = mbedtls_base64_encode((unsigned char *)dst, (size_t)dlen, &olen, src, (size_t)slen);
    if (rc != 0) return -1;
    dst[olen] = '\0';
    return (int)olen;
}

/* ---------------- HMAC-SHA256（mbedtls 2.x） ---------------- */
static int hmac_sha256(const char *key, const char *data, uint8_t out[32]) {
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!info) return -1;
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    if (mbedtls_md_setup(&ctx, info, 1) != 0) { mbedtls_md_free(&ctx); return -1; }
    mbedtls_md_hmac_starts(&ctx, (const unsigned char *)key, strlen(key));
    mbedtls_md_hmac_update(&ctx, (const unsigned char *)data, strlen(data));
    mbedtls_md_hmac_finish(&ctx, out);
    mbedtls_md_free(&ctx);
    return 0;
}

/* ---------------- 生成 RFC1123 GMT 时间 ---------------- */
static void make_date(char *out, int size) {
    time_t t = (time_t)xf_time_now_unix();   /* 叠加服务端时间偏移 */
    struct tm *gmt = gmtime(&t);
    if (!gmt) {
        strncpy(out, "Thu, 01 Jan 1970 00:00:00 GMT", size - 1);
        out[size - 1] = '\0';
        return;
    }
    strftime(out, size, "%a, %d %b %Y %H:%M:%S GMT", gmt);
}

/* ---------------- 构造 authorization_origin（base64 前） ---------------- */
static int build_auth(const char *date, char *auth_buf, int size) {
    char sig_origin[256];
    snprintf(sig_origin, sizeof(sig_origin),
             "host: %s\ndate: %s\nGET %s HTTP/1.1", g_host, date, g_uri);

    uint8_t mac[32];
    if (hmac_sha256(g_api_secret, sig_origin, mac) != 0) return -1;

    char mac_b64[64];
    if (my_base64_encode(mac, 32, mac_b64, sizeof(mac_b64)) < 0) return -1;

    snprintf(auth_buf, size,
             "api_key=\"%s\", algorithm=\"hmac-sha256\", headers=\"host date request-line\", signature=\"%s\"",
             g_api_key, mac_b64);
    return 0;
}

/* ---------------- URL 编码（query 参数需转义 base64 中的 + / = 等） ---------------- */
static void url_encode(const char *src, char *dst, int dsize) {
    static const char hex[] = "0123456789ABCDEF";
    int j = 0;
    for (int i = 0; src[i] && j < dsize - 1; i++) {
        unsigned char c = (unsigned char)src[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            dst[j++] = (char)c;
        } else {
            if (j + 3 >= dsize) break;
            dst[j++] = '%';
            dst[j++] = hex[(c >> 4) & 0xf];
            dst[j++] = hex[c & 0xf];
        }
    }
    dst[j] = 0;
}

/* ---------------- 极简 JSON 取值 ---------------- */
static void json_get_str(const char *json, const char *key, char *out, int size) {
    out[0] = '\0';
    char pat[32];
    snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    const char *p = strstr(json, pat);
    if (!p) return;
    p += strlen(pat);
    const char *q = p;
    while (*q && *q != '"') q++;
    int n = (int)(q - p);
    if (n >= size) n = size - 1;
    memcpy(out, p, n);
    out[n] = '\0';
}

/* 取整数：兼容 "k":123 与 "k":"123"（讯飞 code/rp/sid 常带引号）、允许空格 */
static int json_get_int(const char *json, const char *key) {
    char pat[32];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(json, pat);
    if (!p) return -1;
    p += strlen(pat);
    while (*p == ' ' || *p == '"' || *p == ':') p++;
    return atoi(p);
}

/* 精确取 data.status（响应里 message.status 也在前面，直接 strstr 会取错） */
static int json_get_data_status(const char *json) {
    const char *d = strstr(json, "\"data\":");
    if (!d) return -1;
    return json_get_int(d, "status");
}

/* ---------------- 识别文本聚合 ----------------
 * 实测（PC 端 e2e 验证）：讯飞 iat 返回的 data.result 是**嵌套 JSON 对象**，
 * 不是 base64 字符串：
 *   {"data":{"result":{"pgs":"rpl","rg":[1,2],"ws":[{"cw":[{"w":"打开"}]},...]},
 *            "status":1}}
 * 语义：pgs=apd 追加新词；pgs=rpl 表示动态修正，rg=[起,止] 指出被修正的词区间，
 * 此时 ws 是"从 rg 起点到句尾"的完整内容，须覆盖而非追加。
 * 例：ws=[打,开,灯] →(rg=[1,2])→ [打开,灯光]；再 apd 追加标点 ","。
 */
#define ASR_MAX_WORDS 40
#define ASR_WORD_MAX  32      /* 一个词最多 10 个汉字（UTF-8 3 字节/字） */

static char g_words[ASR_MAX_WORDS][ASR_WORD_MAX];
static int  g_wcount;

static void asr_words_reset(void) {
    g_wcount = 0;
}

static void asr_words_join(char *out, int size) {
    int n = 0;
    if (size <= 0) return;
    out[0] = '\0';
    for (int i = 0; i < g_wcount; i++) {
        int l = (int)strlen(g_words[i]);
        if (n + l + 1 >= size) break;
        memcpy(out + n, g_words[i], (size_t)l);
        n += l;
        out[n] = '\0';
    }
}

/* ---------------- 长串分段打印 ----------------
 * ★ 2026-10-21 现场定位：`[xf] body: {...}` 永远只打到一百多字节就断在
 * 半截（`..."ls":tr` 后直接接下一行），一度让人以为响应本身是残缺的。
 *
 * 根因不在解析，在打印：RT-Thread 的 rt_kprintf 内部只有一块
 * `RT_CONSOLEBUF_SIZE`（默认 128）的静态缓冲，vsnprintf 之后按长度
 * 一次性写到控制台，**超出部分被静默丢弃，连结尾的 \n 也一起丢**。
 * 所以任何超过约 100 字节的字符串都必须自己分片，否则永远看不全。
 *
 * 每片 96 字节：`"[xf] body[0] "` 前缀 13 字节 + 96 + '\n' = 110 < 128。 */
static void kprint_chunked(const char *tag, const char *s) {
    int n, off = 0, idx = 0;
    char piece[97];

    if (!s) return;
    n = (int)strlen(s);
    if (n == 0) { rt_kprintf("[xf] %s: <empty>\n", tag); return; }
    do {
        int c = n - off;
        if (c > 96) c = 96;
        memcpy(piece, s + off, (size_t)c);
        piece[c] = '\0';
        rt_kprintf("[xf] %s[%d] %s\n", tag, idx, piece);
        off += c;
        idx++;
    } while (off < n && idx < 10);          /* 上限 10 片 = 960 字节 */
}

/* 把合并后的词表逐条打出来（长度 + 内容）。
 * 用途：直接区分"服务端给的是空词 "w":"" "与"解析没取到字"——
 * 前者 words 计数会 +1，但拼出来的文本长度是 0，正好是这次踩的坑。 */
static void asr_words_dump(void) {
    int i;
    rt_kprintf("[xf] words=%d\n", g_wcount);
    for (i = 0; i < g_wcount && i < 8; i++) {
        rt_kprintf("[xf]   w[%d] len=%d '%s'\n",
                   i, (int)strlen(g_words[i]), g_words[i]);
    }
}

/* 从响应里提取 ws 词表并按 pgs/rg 合并。返回 0 = 本帧有识别内容
 *
 * ★ 2026-10-21 重写。旧版要求先定位 "result" 且其值必须是对象，再做花括号
 * 配对求出对象结束位置，任何一处对不上就整帧 return -1。
 *
 * 实测这一帧（现场日志）：
 *   {"code":0,"message":"success","sid":"iat000...","data":{"result":{"ws":
 *    [{"bg":0,"cw":[{"sc":0,"w":"..."}]}],...},"status":2}}
 * 服务端**明明回了 ws/cw/文字**，但旧版在配对/边界判断上失败，
 * 一个字都没进 g_words，对外仍然表现为 words=0、识别为空。
 *
 * 现在改成只依赖 `"w":"` 这一个模式：不管外层括号长什么样、JSON 有没有被
 * 截断，只要出现 `"w":"..."` 就取出来。该模式在讯飞响应里只出现在
 * ws -> cw 内，不会误命中 sid / status / sc。 */
static int asr_merge_result(const char *resp) {
    /* rg:[a,b] → 动态修正起点（1-based → 0-based） */
    int rg_start = -1;
    const char *g = strstr(resp, "\"rg\":[");
    if (g) {
        int a = atoi(g + 6);                 /* "rg":[ 正好 6 个字符 */
        if (a >= 1) rg_start = a - 1;
    }

    /* 收集本帧 ws 里的所有 "w":"..." */
    static char tmp[ASR_MAX_WORDS][ASR_WORD_MAX];
    int cnt = 0;
    const char *r = resp;

    while (cnt < ASR_MAX_WORDS) {
        const char *e;
        int l;

        r = strstr(r, "\"w\":\"");
        if (!r) break;
        r += 5;                              /* 跳过 "w":" */

        e = r;
        while (*e && *e != '"') {            /* 容忍 \" 转义 */
            if (*e == '\\' && e[1]) e++;
            e++;
        }
        if (!*e) break;                      /* 引号没闭合：放弃这一条 */

        l = (int)(e - r);
        /* ★ 空词必须丢掉，不能计数（2026-10-21 现场踩坑）。
         * 讯飞开 dla/wpgs 后会回 `{"cw":[{"sc":0,"w":""}]}` 这种占位空词。
         * 旧地把 l=0 也当成一条有效词存进 g_words，于是对外打出
         * `words=1`（看着像"识别出了一个词"），实际拼出来的文本是
         * 空串 → 回调不触发 → `[voice] done rc=0, text 0 bytes`，
         * 且日志里既没有 `[xf] FINAL:` 也没有 cmd_exec 的输出，
         * 极易误导成"网络通了但服务器不认"。 */
        if (l > 0) {
            if (l > ASR_WORD_MAX - 1) l = ASR_WORD_MAX - 1;
            memcpy(tmp[cnt], r, (size_t)l);
            tmp[cnt][l] = '\0';
            cnt++;
        }
        r = e + 1;
    }

    if (cnt == 0) return -1;

    if (rg_start >= 0) {
        /* 动态修正：丢弃 rg 起点之后的旧词，用 ws 覆盖 */
        if (rg_start > g_wcount) rg_start = g_wcount;
        g_wcount = rg_start;
        for (int i = 0; i < cnt && g_wcount < ASR_MAX_WORDS; i++) {
            strncpy(g_words[g_wcount], tmp[i], ASR_WORD_MAX - 1);
            g_words[g_wcount][ASR_WORD_MAX - 1] = '\0';
            g_wcount++;
        }
    } else {
        /* 普通追加 */
        for (int i = 0; i < cnt && g_wcount < ASR_MAX_WORDS; i++) {
            strncpy(g_words[g_wcount], tmp[i], ASR_WORD_MAX - 1);
            g_words[g_wcount][ASR_WORD_MAX - 1] = '\0';
            g_wcount++;
        }
    }
    return 0;
}

/* ---------------- 时间工具：统一按 UTC 纪元写系统时间 ----------------
 * 本 SDK 的 set_time() 只写 RTC 硬件、不刷新系统时间（只有 set_date 的年月日
 * 生效），因此改用 RT-Thread 的 set_system_time() 直接写 Unix UTC 秒。
 * 用弱符号声明：万一某版 SDK 未导出它，运行时会判空跳过并回退旧接口，
 * 避免链接期直接失败。 */
RT_WEAK void set_system_time(time_t time);

long xf_days_from_civil(int y, int m, int d) {
    y -= (m <= 2);
    long era = (y >= 0 ? y : y - 399) / 400;
    int  yoe = y - era * 400;
    int  doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int  doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

/* 该 SDK 的时间写入行为不一致（不同链接/配置下 set_system_time 可能不生效，
 * 而 set_time() 实测只写 RTC 硬件、不刷新系统时间）。故采用
 * "设置 -> 回读校验 -> 逐级回退" 的自愈策略，确保时间一定能设对。 */
static int g_sysfn_broken = 0;       /* set_system_time 确认无效后不再尝试 */
static int g_time_write_broken = 0;  /* 系统时间整体写不进后不再尝试，交由偏移补偿 */

void xf_time_set_unix(long unix_sec) {
    if (g_time_write_broken) return; /* 已确认写不进去，别再让识别路径白等 */
    time_t t0 = time(RT_NULL);
    int ok = 0;

    /* ★ 策略1（首选）：直接写 RTC 计数器。
     * 驱动契约是 `rt_uint32_t *`（UTC 秒）：
     *   fh_rtc_v2.c:686  fh_rtc_settime(rtc, unsigned int *time)
     *                    { return fh_rtc_set_counter(rtc, *time); }
     * 写完之后必须调 clock_time_system_init() 把新值同步进 timekeeping，
     * 否则 time() 仍读旧值。
     *
     * 【曾经写错的地方，务必别再犯】这里原来传的是 `struct tm *`：
     *   rt_device_control(rtc, SET_TIME, (void *)g);
     * 驱动按 `unsigned int *` 解引用 → 取到的是 tm_sec（结构体第一个字段），
     * RTC 计数器被写成"当前秒数" → time() 变成 0~59 秒，即 1970-01-01。
     * 现象就是 `date` 显示 "Thu Jan 1 08:00:19 1970"，非常迷惑。 */
    {
        rt_device_t rtc = rt_device_find("rtc");
        if (rtc) {
            rt_uint32_t sec = (rt_uint32_t)unix_sec;
            if (rt_device_control(rtc, RT_DEVICE_CTRL_RTC_SET_TIME, &sec) == RT_EOK) {
                clock_time_system_init();          /* RTC -> timekeeping */
                ok = ((long)time(RT_NULL) - unix_sec <= 2 &&
                      (long)time(RT_NULL) - unix_sec >= -2);
            }
            if (!ok) rt_kprintf("[time] rtc write ineffective (ret=%d)\n", (int)ok);
        } else {
            rt_kprintf("[time] no 'rtc' device found\n");
        }
    }

    /* 策略2：RT-Thread set_system_time（若链接进来且策略1 没成） */
    if (!ok && !g_sysfn_broken && set_system_time) {
        set_system_time((time_t)unix_sec);
        ok = ((long)time(RT_NULL) == unix_sec);
        if (!ok) {
            g_sysfn_broken = 1;
            rt_kprintf("[time] set_system_time ineffective\n");
        }
    } else if (!set_system_time) {
        g_sysfn_broken = 1;
    }

    /* 注意：不再用 set_date()/set_time() 这条路。
     * 它们内部走 mktime()，而本移植 TZ=UTC-8（mktime 会减 8h），
     * 传 UTC 字段进去会被再减一次，反而写歪；且实测它们只写 RTC
     * 不刷新系统时间，写对了 time() 也不动。 */

    time_t t1 = time(RT_NULL);
    rt_kprintf("[time] set %ld -> want %ld, now %ld : %s\n",
               (long)t0, unix_sec, (long)t1, ok ? "OK" : "FAIL");
    if (ok) {
        /* 系统时间已经写对了，偏移量必须清零 —— 否则 time() 已是服务器时间，
         * 再叠加一次 (server - 旧local) 就等于把误差算两遍。 */
        xf_time_offset_clear("system clock written successfully");
        return;
    }

    /* 首次失败才花 1.2 秒确认时钟是否会自然前进；若也不动说明 tick 冻结。
     * 两种情况都不影响识别——偏移量补偿已保证签名时间正确。
     * 因此失败一次即永久放弃写系统时间，不让识别路径反复白等。 */
    rt_thread_mdelay(1200);
    if (time(RT_NULL) == t1)
        rt_kprintf("[time] WARNING: clock frozen (tick stopped) - offset mode still works\n");
    else
        rt_kprintf("[time] clock alive but write failed - using offset mode\n");
    g_time_write_broken = 1;
}

int xf_time_set_gmt(const char *gmt) {
    static const char *mon_tbl[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                    "Jul","Aug","Sep","Oct","Nov","Dec"};
    char wd[8] = {0}, mon[4] = {0};
    int day = 0, year = 0, hh = 0, mm = 0, ss = 0, mon_i = 0;

    if (!gmt || !gmt[0]) return -1;
    if (sscanf(gmt, "%7s %d %3s %d %d:%d:%d",
               wd, &day, mon, &year, &hh, &mm, &ss) != 7) return -1;
    (void)wd;
    for (int i = 0; i < 12; i++)
        if (strcmp(mon, mon_tbl[i]) == 0) { mon_i = i + 1; break; }
    if (mon_i == 0 || day < 1 || day > 31 || year < 2000) return -1;

    long server = xf_days_from_civil(year, mon_i, day) * 86400L
                + hh * 3600L + mm * 60L + ss;
    long local = (long)time(RT_NULL);

    /* ① 关键：记录偏移量——签名时间不再依赖系统时间能否被写入 */
    g_time_offset = server - local;
    rt_kprintf("[xf] time offset = %ld s (local %ld -> server %ld)\n",
               g_time_offset, local, server);

    /* ② 尽力写系统时间（写成功则 date 命令显示也正确，属附加收益） */
    xf_time_set_unix(server);
    return 0;
}

/* ---------------- 用服务端返回的 Date 头校准系统时间 ----------------
 * 401 拒签时讯飞仍会在响应头带回真实 GMT 时间。用它校时后重新签名重试，
 * 识别链路便不再依赖 NTP(UDP/123) 或 HTTP(TCP/80) 校时服务。 */
static int apply_server_date(const char *s) {
    time_t before = time(RT_NULL);
    if (xf_time_set_gmt(s) != 0) return -1;
    xf_time_trusted = 1;
    rt_kprintf("[xf] clock corrected from server: %s\n", s);
    rt_kprintf("[xf] system time %ld (drift %ld s), corrected time %ld (offset %ld)\n",
               (long)time(RT_NULL), (long)(time(RT_NULL) - before),
               xf_time_now_unix(), g_time_offset);
    return 0;
}

/* ---------------- 生成 date + authorization + path，连接讯飞 ----------------
 * attempt 0 失败且拿到了服务端时间时，调用方会校时后再走一次。 */
static wss_ctx_t *xf_connect(char *date, int date_size) {
    make_date(date, date_size);

    char auth_origin[256];
    if (build_auth(date, auth_origin, sizeof(auth_origin)) != 0) {
        rt_kprintf("[xf] signature build fail\n");
        return RT_NULL;
    }
    /* auth_origin ≈160 字符 → base64 ≈216 字符 + '\0'，故 auth_b64 需 ≥256 */
    char auth_b64[256]; size_t al = 0;
    if (mbedtls_base64_encode((unsigned char *)auth_b64, sizeof(auth_b64), &al,
                               (unsigned char *)auth_origin, strlen(auth_origin)) != 0) {
        rt_kprintf("[xf] auth base64 fail\n");
        return RT_NULL;
    }
    auth_b64[al] = 0;

    /* url_encode 把 + / = 等按 %XX 展开：实测 ~212 字符，理论最坏 212*3=636，
     * 取 704 留足余量（-Werror 下截断即错误，宁可多给） */
    char auth_enc[704], date_enc[128];
    url_encode(auth_b64, auth_enc, sizeof(auth_enc));
    url_encode(date, date_enc, sizeof(date_enc));

    /* path = /v2/iat?authorization=<~230>&date=<45>&host=<16> + 固定，故需 ≥768 */
    char path[768];
    snprintf(path, sizeof(path), "/v2/iat?authorization=%s&date=%s&host=%s",
             auth_enc, date_enc, g_host);

    /* date 头与 query 中的 date 必须是同一个串（都参与签名校验） */
    return wss_connect(g_host, XF_ASR_PORT, path, date,
                       g_server_ip[0] ? g_server_ip : NULL);
}

/* ---------------- 主入口：一次性识别（逐帧 WS 发送） ---------------- */
int xf_asr_recognize_once(const uint8_t *pcm, int total, xf_asr_result_cb cb, void *user) {
    char date[64];
    wss_ctx_t *ws = RT_NULL;
    /* 分段计时（现场诉求："识别的时间不好判断"）。三段：
     *   connect = TCP+TLS+WS 握手（受 RTT 影响）
     *   send    = 125 帧按 40ms 节奏推完，理论值 = total/32 ms
     *   recv    = 发完收尾帧到拿到 status=2
     * 正常值是 connect 几百 ms、send 约等于音频时长、recv 1~3 秒。
     * 若 recv 明显偏大，是服务端在等 VAD 判静音（vad_eos=10000）。 */
    rt_tick_t t_begin = rt_tick_get();
    rt_tick_t t_conn = t_begin, t_sent = t_begin, t_done = t_begin;

    /* 最多三次：首次若因时间偏差被 401，用服务端 Date 校准（记录偏移量）后重试。
     * 正常情况下第 2 次就 101；第 3 次用于容忍偶发网络抖动。 */
    for (int attempt = 0; attempt < 3; attempt++) {
        ws = xf_connect(date, sizeof(date));
        if (ws) break;

        if (attempt < 2 && apply_server_date(wss_last_server_date()) == 0) {
            rt_kprintf("[xf] time skewed, retry handshake with corrected time...\n");
            continue;
        }
        rt_kprintf("[xf] wss connect fail (our date: %s, unix %ld)\n",
                   date, xf_time_now_unix());
        return -8;
    }
    if (!ws) return -8;
    xf_time_trusted = 1;   /* 服务器接受了签名 → date 偏差在容许范围内 */
    t_conn = rt_tick_get();

    char *full_text = rt_malloc(1024);
    if (!full_text) { wss_close(ws); return -3; }
    full_text[0] = '\0';

    /* 讯飞官方推荐每帧 1280 字节 = 40ms @16k/16bit */
    const int CHUNK = 1280;

    asr_words_reset();

    /* ============ 阶段 1：连续发送所有音频帧（不等响应）============
     * 实测：讯飞服务端在收到音频前**不返回任何响应**（PC 端 e2e 验证为
     * "NO immediate response"），若沿用"发一收一"会在 wss_recv_text 上
     * 阻塞到 20s 超时而失败。TCP 全双工，服务端的响应会暂存在接收窗口，
     * 不影响我们继续发送。所有帧发完后再统一收取。 */
    static char b64[2600];    /* 1280 字节 -> base64 约 1708 字符 */
    static char body[2600];
    int offset = 0, frame = 0, sent_all = 0, rc = 0;
    rt_tick_t t_send0 = rt_tick_get();   /* 发送节奏基准 */
    while (!sent_all) {
        int status  = (frame == 0) ? 0 : 1;
        int clen    = MIN(CHUNK, total - offset);
        const uint8_t *chunk = pcm + offset;
        offset += clen;
        if (offset >= total) sent_all = 1;

        if (my_base64_encode(chunk, clen, b64, sizeof(b64)) < 0) { rc = -5; break; }
        if (frame == 0) {
            snprintf(body, sizeof(body),
                "{\"common\":{\"app_id\":\"%s\"},"
                "\"business\":{\"language\":\"zh_cn\",\"domain\":\"iat\",\"accent\":\"mandarin\","
                "\"vad_eos\":10000,\"dwa\":\"wpgs\"},"
                "\"data\":{\"status\":%d,\"format\":\"audio/L16;rate=16000\","
                "\"encoding\":\"raw\",\"audio\":\"%s\"}}",
                g_appid, status, b64);
        } else {
            /* 后续帧不带 sid：服务端第一帧不回响应，我们拿不到 sid（实测如此） */
            snprintf(body, sizeof(body),
                "{\"data\":{\"status\":%d,\"format\":\"audio/L16;rate=16000\","
                "\"encoding\":\"raw\",\"audio\":\"%s\"}}",
                status, b64);
        }
        if (wss_send_text(ws, body, strlen(body)) != 0) { rc = -9; break; }

        /* ★ 按绝对时间对齐，而不是固定 mdelay(40)。
         *
         * 讯飞要求音频按实时速率上传（每帧 1280B ≈ 40ms）。原来每帧
         * `mdelay(40)` 之后再叠加 TLS 加密 + TCP 发送的耗时，实际每帧
         * 要 50ms 上下；125 帧跑下来整体慢了 1 秒多，服务端会判定
         * "发送过慢"（历史日志里出现过 `server read msg timeout`）。
         * 改成"第 N 帧应于 t0 + N*40ms 发出"，用差值补齐，节奏恒定。 */
        frame++;
        {
            rt_tick_t want = (rt_tick_t)(((uint32_t)frame * 40u * RT_TICK_PER_SECOND) / 1000u);
            rt_tick_t now  = rt_tick_get() - t_send0;
            if (want > now) {
                uint32_t dms = (uint32_t)(((uint32_t)(want - now) * 1000u) / RT_TICK_PER_SECOND);
                if (dms > 0) rt_thread_mdelay(dms);
            }
        }
        if (frame > 400) break;  /* 安全上限 */
    }

    /* 发送完毕留痕：帧数 × 1280 应等于录音字节数。若对不上，说明中途
     * 丢帧或 total 传错，此时讨论"识别为空"没有意义。 */
    t_sent = rt_tick_get();
    rt_kprintf("[xf] sent %d frames, %d bytes%s\n",
               frame, offset, (rc == 0) ? "" : " (send error)");

    /* ============ 阶段 2：发收尾帧（status=2, audio 为空）============ */
    if (rc == 0) {
        snprintf(body, sizeof(body),
            "{\"data\":{\"status\":2,\"format\":\"audio/L16;rate=16000\","
            "\"encoding\":\"raw\",\"audio\":\"\"}}");
        if (wss_send_text(ws, body, strlen(body)) != 0) rc = -9;
    }

    /* ============ 阶段 3：统一收取响应，直到 data.status=2 ============ */
    /* resp 放静态区：8KB 若放栈上会占掉 40KB 线程栈的 20%，且与 mbedtls
         * 握手/SSL 读写的栈需求叠加，嵌入式上容易踩栈。识别是单线程串行的，
         * 用静态区没有并发问题。 */
    static char resp[8192];
    {
        char text[512];
        int guard = 0;
        while (rc == 0 && guard++ < 600) {
            int rn = wss_recv_text(ws, resp, sizeof(resp));
            if (rn <= 0) { rt_kprintf("[xf] recv end rn=%d\n", rn); rc = -6; break; }
            if (rn > (int)sizeof(resp) - 1) rn = (int)sizeof(resp) - 1;
            resp[rn] = '\0';

            int code = json_get_int(resp, "code");
            if (code > 0) {          /* 成功响应无 code 字段，json_get_int 返回 -1 */
                char desc[128];
                json_get_str(resp, "desc", desc, sizeof(desc));
                rt_kprintf("[xf] api error code=%d desc=%s\n", code, desc);
                rc = -7;
                break;
            }

            /* ★ 顺序修正（2026-10-21）：先合并、再打印。
             * 之前 `words=` 打在 asr_merge_result() 之前，永远是 0 ——
             * 现场日志里那句 `code=0 status=2 words=0` 完全不能用来判断
             * "服务端没回字"还是"解析没取到字"，白耗费了一轮排查。 */
            int merged = asr_merge_result(resp);
            if (merged == 0) {
                asr_words_join(text, sizeof(text));
                if (text[0] && cb) cb(text, 0, user);
            }

            {
                int st = json_get_data_status(resp);
                if (guard <= 2 || st == 2 || merged == 0) {
                    /* 打**完整**原文：必须分段，rt_kprintf 的 128 字节静态
                     * 缓冲会把长串截断（注释见 kprint_chunked）。 */
                    rt_kprintf("[xf] resp#%d len=%d code=%d status=%d add=%d words=%d\n",
                               guard, rn, code, st, (merged == 0) ? 1 : 0, g_wcount);
                    kprint_chunked("body", resp);
                    asr_words_dump();
                }
            }

            if (json_get_data_status(resp) == 2) break;   /* 最终结果 */
        }

        /* 空结果留痕：纯静音时讯飞会回 code=0 / status=2、但一个 word 都没有。
         * 打一眼原始响应，能直接区分"音频里确实没人声"与"JSON 没解析出来"。
         * resp 是 static，作用域到此为止，所以断言必须放在这个块里。 */
        if (rc == 0 && g_wcount == 0) {
            rt_kprintf("[xf] EMPTY result: no non-empty word in response\n");
            kprint_chunked("last", resp);
        }
    }

    /* 把拼好的整句打出来再交付，避免"回调没触发"时完全没有线索。
     * 正常应当与 asr_cb 里的 `[xf] FINAL:` 一一对应。 */
    t_done = rt_tick_get();
    rt_kprintf("[xf] timing: connect %d ms, send %d ms, recv %d ms, total %d ms\n",
               (int)(((t_conn  - t_begin) * 1000u) / RT_TICK_PER_SECOND),
               (int)(((t_sent  - t_conn ) * 1000u) / RT_TICK_PER_SECOND),
               (int)(((t_done  - t_sent ) * 1000u) / RT_TICK_PER_SECOND),
               (int)(((t_done  - t_begin) * 1000u) / RT_TICK_PER_SECOND));
    asr_words_join(full_text, 1024);
    rt_kprintf("[xf] result: %d words, %d bytes\n",
               g_wcount, (int)strlen(full_text));
    if (cb && full_text[0]) cb(full_text, 1, user);
    else if (!full_text[0])
        rt_kprintf("[xf] result empty -> callback skipped\n");

    rt_free(full_text);
    wss_close(ws);
    return rc;
}
