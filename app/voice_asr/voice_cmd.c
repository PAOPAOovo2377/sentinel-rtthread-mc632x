/*
 * voice_cmd.c - 语音控制线程（以太网 + 讯飞 iat WebSocket 一句话识别）
 *
 * 触发：msh 命令 `voice_test` 释放信号量，任务线程执行一次识别。
 * 识别结果经 xf_asr 回调返回，这里做关键词匹配并调用执行桩。
 *
 * 阶段0 命令为桩函数（打印 + 预留 GPIO / 媒体控制接口）。
 * 接真实硬件时，把 cmd_exec() 里的 TODO 换成你的 GPIO / VOU / NPU 控制调用。
 */

#include "voice_cmd.h"
#include "xf_http_asr.h"
#include "audio_capture.h"
#include "ws_client.h"           /* xf_tls 探针需要 wss_connect/wss_close */
#include "dsp/fh_audio_mpi.h"   /* mic_probe 需要 FH_AC_AI_Get_Feature_ChannelMask */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>      /* atoi（mic_test 用） */
#include <time.h>
#include <errno.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>      /* RT-Thread SAL 套接字用 dfs 的 close() 关闭 */
#include <fcntl.h>       /* F_GETFL / O_NONBLOCK（xf_raw 用） */

/* 上电时 board_ntp_sync() 在 start_demo 里后台调用（不阻塞启动）。
 * RTC 已由 rtc_init_buildtime() 用编译时刻兜底，NTP 只做精校。
 * 实现位于 sample_demo/sample_common_demo.c */
extern int board_ntp_sync(void);

/* net_tools.c 里 msh `dns` 命令的实现（set_dns）。
 * 直接 extern 复用它，避免 app 层再 include <lwip/dns.h> 引入网络栈头路径依赖。 */
extern void set_dns(int index, char *dns_server);

#define VOICE_STACK 40960   /* 40KB：mbedtls TLS 握手 + SDK 闭源音频库(fh_acw)需要大栈 */
#define VOICE_PRI   12      /* 低于 NPU/视频任务，避免抢占实时检测 */

static rt_sem_t g_trig = RT_NULL;
static char     g_voice_text[64];   /* 最近一次识别文本，供 UI 读取 */

/* 最近一次识别的失败原因（空串 = 没出错）。
 *
 * 为什么必须存在：改造前所有失败只打串口（"[voice] mic not ready"、
 * "[voice] connect failed (rc=-8)" 之类），网页那边 /api/state 只看到
 * g_voice_state 回到 0 且 g_voice_text 为空，于是显示"待机"——
 * 用户点完按钮等 20 秒，界面从"准备中"跳回"待机"，一句话都没有，
 * 现象就是"语音没返回"。串口日志在现场往往看不到（没接串口线），
 * 所以把原因做成一个网页可读的字段是这条链路的必需件，不是可选项。 */
static char     g_voice_err[48];

/* 语音链路进度：0=空闲 1=等麦收敛 2=录音中 3=上传识别。
 * 网页 /api/state 轮询这个值来显示"准备中/请说话/识别中"，否则点完按钮到
 * 出结果之间要等 15~20 秒毫无反馈，看起来就像没响应。
 * 必须把"等麦"和"录音"分开：麦收敛要 3.5~4.5 秒，这段时间开口说话会被丢掉，
 * 网页提示"请说话"必须等采集真正就绪之后才亮。 */
static volatile int g_voice_state = 0;

/* 本轮录音的目标语音时长（毫秒），给网页画进度条用。
 * 注意它是"语音时长"，不是"从点了按钮算起的时长"——
 * 录音窗口现在从检测到人声才起算，见 audio_capture_record 的阶段 A。 */
static volatile int g_rec_target_ms = 6000;

/* 校时状态：1 = 已被网络时间(NTP/HTTP 头)校准过，或已被讯飞握手验证过，签名可用；
 * 0 = 只有 rtc_init_buildtime 的编译时刻兜底，偏差可能 >300s → 讯飞会 401 */
static volatile int g_time_ok = 0;

/* xf_http_asr.c 在握手成功/服务端 Date 校时成功后置位（它比任何本地校时都可信） */
extern int xf_time_trusted;

static int http_time_sync(void);

/* 打印当前时间（UTC，RFC1123）——用修正后的时间（叠加服务端偏移），
 * 因为本 SDK 系统时间写不进去，time() 本身是错的。 */
static void time_print(const char *tag) {
    time_t now = (time_t)xf_time_now_unix();
    struct tm *g = gmtime(&now);
    if (!g) { rt_kprintf("%s: time unavailable\n", tag); return; }
    char tbuf[48];
    strftime(tbuf, sizeof(tbuf), "%a, %d %b %Y %H:%M:%S GMT", g);
    rt_kprintf("%s: %s\n", tag, tbuf);
}

const char *voice_cmd_last_text(void) {
    return g_voice_text;
}

/* 供网页读取：最近一次识别的失败原因，成功或未跑过时为空串 */
const char *voice_cmd_last_err(void) {
    return g_voice_err;
}

/* 供 webd 读取：0=空闲 1=等麦 2=录音中 3=识别中 */
int voice_cmd_state(void) {
    return g_voice_state;
}

/* 给网页用的细粒度阶段 + 板端录音进度。
 *
 * ★ 为什么 state 不够：录音窗口现在分两段——先"等你开口"（不计时），
 *   再"从人声起点录满 N 秒"。这两段在 state 里都是 2，但界面上必须
 *   显示成两种完全不同的提示，否则用户又会在"正在录"的时候以为
 *   还没开始，白白浪费半个窗口。
 *
 * elapsed_ms 直接取自采集循环里的 g_prog_ms，是**板子的真实进度**；
 * 网页拿它画进度条，显示与实际的偏差最多一个轮询周期，且永远滞后不超前。 */
int voice_cmd_phase(int *elapsed_ms, int *total_ms) {
    int ms = 0;
    int lead = 0;
    int ph;
    int live;

    live = audio_capture_progress(&ms, &lead);
    /* 没在录时进度是上一轮的残值，必须清零。
     * 不清的话：state 已经置 2、record 还没进阶段 A 的那个空档里，
     * 会短暂显示成"录音 x.y 秒"，看起来像窗口已经跑掉了。 */
    if (live != 0) ms = 0;
    (void)lead;     /* 阶段 A 的等待时长目前不给前端用，留着备查 */

    switch (g_voice_state) {
    case 1:
        ph = VOICE_PH_SETTLE;
        break;
    case 2:
        /* ms>0 说明已经检测到人声、正在计时；否则还在等你开口 */
        ph = (ms > 0) ? VOICE_PH_REC : VOICE_PH_ARMED;
        break;
    case 3:
        ph = VOICE_PH_ASR;
        break;
    default:
        /* 空闲但上一轮留下了失败原因 —— 网页据此显示"上次为什么没成" */
        ph = (g_voice_err[0] != '\0') ? VOICE_PH_FAIL : VOICE_PH_IDLE;
        break;
    }

    if (elapsed_ms) *elapsed_ms = (ph == VOICE_PH_REC) ? ms : 0;
    if (total_ms)   *total_ms   = g_rec_target_ms;
    return ph;
}

/* ---------------- 命令执行桩 ---------------- */
static void cmd_exec(const char *text) {
    rt_kprintf("[voice] cmd: %s\n", text);

    if (strstr(text, "报警") || strstr(text, "危险")) {
        /* TODO: 拉高报警 GPIO / 开蜂鸣器 / 屏上红色告警 */
        rt_kprintf("[voice] -> ALARM ON\n");
    } else if (strstr(text, "解除") || strstr(text, "正常")) {
        /* TODO: 清除报警状态 */
        rt_kprintf("[voice] -> ALARM OFF\n");
    } else if (strstr(text, "开灯") || strstr(text, "打开")) {
        /* TODO: 打开工位照明继电器 */
        rt_kprintf("[voice] -> RELAY ON\n");
    } else if (strstr(text, "关灯") || strstr(text, "关闭")) {
        rt_kprintf("[voice] -> RELAY OFF\n");
    } else if (strstr(text, "切换") || strstr(text, "画面")) {
        /* TODO: 切换 VOU 显示的 video group / 通道 */
        rt_kprintf("[voice] -> SWITCH VIEW\n");
    } else if (strstr(text, "检测") || strstr(text, "开始")) {
        rt_kprintf("[voice] -> START MONITOR\n");
    } else {
        rt_kprintf("[voice] -> (no command matched)\n");
    }
}

/* ---------------- ASR 结果回调 ---------------- */
static void asr_cb(const char *text, int final, void *user) {
    /* 讯飞分词结果可能带空格（"打开 灯光"），去掉后再做关键词匹配 */
    char clean[64];
    size_t j = 0;
    for (size_t i = 0; text[i] && j < sizeof(clean) - 1; i++) {
        if (text[i] != ' ' && text[i] != '\t' && text[i] != '\r' && text[i] != '\n')
            clean[j++] = text[i];
    }
    clean[j] = '\0';

    /* 增量结果不打印：串口刷屏会挤占 msh 轮询、加剧"吞字符"，
     * 且分片文本对用户无意义，只打最终结果。 */
    if (final) rt_kprintf("[xf] FINAL: %s\n", clean);

    strncpy(g_voice_text, clean, sizeof(g_voice_text) - 1);
    g_voice_text[sizeof(g_voice_text) - 1] = '\0';

    if (final) cmd_exec(clean);  /* 最终结果才执行命令，避免增量误触发 */
}

/* ---------------- 校时状态 ---------------- */
static void time_sync_worker(void *arg);   /* 定义在文件后部，前向声明 */

/* 同步校时：只走 NTP（约 5 秒有界）。
 * 刻意不在这里自动调 http_time_sync()——它依赖 gethostbyname() + 外网 80 端口，
 * 在校园网 + Windows ICS 环境下实测会把语音线程卡死 7 分钟以上
 *（www.baidu.com 解析长时间无响应）。HTTP 校时仅保留 cmd_httptime 手动触发。
 * 时间不准由讯飞握手 401 响应里的 Date 头自愈兜底，不依赖任何外部服务。 */
static int time_sync_blocking(void)
{
    if (g_time_ok) return 0;
    if (board_ntp_sync() == 0) { g_time_ok = 1; return 0; }
    return -1;
}

/* 后台校时线程（幂等，只创建一次） */
static volatile int g_timesync_started = 0;
static void ensure_timesync_bg(void)
{
    if (g_timesync_started) return;
    g_timesync_started = 1;
    rt_thread_t nt = rt_thread_create("timesync", time_sync_worker, RT_NULL,
                                      4096, 20, 10);
    if (nt) rt_thread_startup(nt);
}

/* ---------------- 讯飞域名后台解析（把 DNS 移出识别关键路径） ----------------
 * 策略：启动时和每次识别前，若缓存的 IP 缺失或超过 10 分钟，就起一个后台
 *       线程去解析域名并回填。识别线程本身永远不调用 gethostbyname/getaddrinfo，
 *       因此 DNS 再怎么慢都卡不住识别。
 *      · 自动解析成功 → xf_asr_set_server_ip()，之后连接 0 次 DNS
 *      · DNS 完全不可用 → 用 msh 命令 xf_ip <ip> 手动指定（PC 上 ping 域名取 IP） */
static char     g_host_ip[32] = {0};
static rt_tick_t g_host_ip_tick = 0;
static volatile int g_resolving = 0;        /* 解析中标志：防重复起线程耗尽堆 */
static rt_tick_t g_resolve_try_tick = 0;
#define HOST_IP_TTL_MS    (10 * 60 * 1000)   /* 10 分钟后重新解析，防止 IP 轮换失效 */
#define HOST_IP_RETRY_MS  (15 * 1000)       /* 解析失败后冷却 15s：太短会频繁起线程，
                                             * 太长则手动改完 dns 还要干等一分钟 */

/* DNS 兜底服务器：DHCP 下发的路由器 DNS 偶发直接失败（实测 192.168.2.1
 * 在板子上返回失败，而同网络里 ping 223.5.5.5 是 4/4 通的）。
 * 只在第一次解析失败时改一次全局 DNS，之后不再重复设置。 */
#define DNS_FALLBACK_0    "223.5.5.5"
#define DNS_FALLBACK_1    "114.114.114.114"
static int g_dns_fallback_done = 0;

/* TCP 探活：确认解析到的 IP 在 443 上真的连得上。
 * 背景：同一域名在不同 DNS 下会返回不同节点（校园网 DNS 给124.172.152.233
 * 可达，ICS DNS 给 114.118.75.203 连不上），必须实测挑能用的那个。 */
static int tcp_probe(const char *ip, int port)
{
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &sa.sin_addr) != 1) return 0;

    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return 0;
    /* 阻塞 connect（这里不设非阻塞）：由 lwIP 的 SYN 重传超时兜底，
     * 且阻塞语义才能得到"真的连上了"的结论。 */
    int ok = (connect(s, (struct sockaddr *)&sa, sizeof(sa)) == 0);
    if (!ok) rt_kprintf("[voice] tcp probe: %s:%d UNREACHABLE (errno %d)\n",
                        ip, port, errno);
    close(s);
    return ok;
}

/* ---------------- xf_raw：裸 TCP 探针（完全脱离 TLS） ----------------
 * 现象是"TCP 建连成功 -> 发出 ClientHello -> 10ms 内 ECONNRESET"。
 * 这一步把 mbedtls 整个摘掉，只问三件事：
 *   1) 建连本身要多久（= 一个 RTT），判断对端离我们多远；
 *   2) 连上之后**什么都不发**，连接能不能活过 3 秒；
 *   3) 发一段任意字节之后，连接还活不活、能收到什么。
 * 判读：
 *   静默就死          -> 与本机发什么无关，网关/代理在拆这条流
 *   发数据才死        -> 中间设备按内容拦
 *   发什么都活着      -> 问题只在 TLS/SNI 那一层
 */

/* errno -> 人话（不用 strerror：本移植 libc 未必提供该符号） */
static const char *raw_err_str(int e)
{
    switch (e) {
    case 0:            return "ok";
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

static int xf_raw(int argc, char **argv)
{
    if (argc < 2) {
        rt_kprintf("usage: xf_raw <ip> [port] [text]\n");
        rt_kprintf("  xf_raw 124.172.152.233          连上后静默 3 秒\n");
        rt_kprintf("  xf_raw 124.172.152.233 443 hi   连上后发 'hi'\n");
        return 0;
    }

    const char *ip   = argv[1];
    int         port = (argc >= 3) ? atoi(argv[2]) : XF_ASR_PORT;
    const char *text = (argc >= 4) ? argv[3] : RT_NULL;

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &sa.sin_addr) != 1) {
        rt_kprintf("[raw] bad ip: %s\n", ip);
        return 0;
    }

    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        rt_kprintf("[raw] socket fail errno=%d\n", errno);
        return 0;
    }

    rt_tick_t t0 = rt_tick_get();
    if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        rt_kprintf("[raw] connect FAIL errno=%d %s (%d ms)\n",
                   errno, raw_err_str(errno),
                   (int)((rt_tick_get() - t0) * 1000 / RT_TICK_PER_SECOND));
        close(s);
        return 0;
    }
    rt_kprintf("[raw] %s:%d connected, SYN-ACK %d ms\n", ip, port,
               (int)((rt_tick_get() - t0) * 1000 / RT_TICK_PER_SECOND));

    int fl = fcntl(s, F_GETFL, 0);
    if (fl >= 0) (void)fcntl(s, F_SETFL, fl | O_NONBLOCK);

    if (text && text[0]) {
        int n = (int)strlen(text);
        int w = (int)send(s, text, (size_t)n, 0);
        rt_kprintf("[raw] sent %d/%d bytes: %s\n", w, n, text);
    } else {
        rt_kprintf("[raw] silent: 不发任何字节，观察 3 秒\n");
    }

    int got = 0;
    char shown[34];
    for (int ms = 0; ms < 3000; ms += 100) {
        unsigned char buf[128];
        int n = (int)recv(s, buf, sizeof(buf), 0);
        if (n > 0) {
            int k = 0;
            for (int i = 0; i < n && i < 32; i++)
                shown[k++] = (buf[i] >= 32 && buf[i] < 127) ? (char)buf[i] : '.';
            shown[k] = '\0';
            got += n;
            rt_kprintf("[raw] rx %d B @%dms: %s\n", n, ms, shown);
            continue;
        }
        if (n == 0) {
            rt_kprintf("[raw] peer FIN @%dms (total rx %d)\n", ms, got);
            close(s);
            return 0;
        }
        if (errno != EWOULDBLOCK && errno != EAGAIN) {
            rt_kprintf("[raw] *** DEAD @%dms errno=%d %s (total rx %d)\n",
                       ms, errno, raw_err_str(errno), got);
            close(s);
            return 0;
        }
        if (ms > 0 && (ms % 1000) == 0)
            rt_kprintf("[raw] alive @%dms\n", ms);
        rt_thread_mdelay(100);
    }
    rt_kprintf("[raw] ALIVE 3s, total rx %d\n", got);
    close(s);
    return 0;
}
MSH_CMD_EXPORT(xf_raw, xf_raw <ip> [port] [text] : raw tcp probe without tls);

/* ---------------- 无 DNS 时的兜底节点 ----------------
 * 这条网络实测对 UDP/53 有限制：dns 设成 192.168.2.1 或 223.5.5.5 都解析失败，
 * 而同一时刻 ping 223.5.5.5、TCP 443 都是通的。可用性不能押在 DNS 上。
 *
 * 讯飞 iat 是 CDN 多节点，TLS 的 SNI 用的是**域名**（ws_client.c 传的是 host，
 * 不是 IP），所以连到哪个节点证书都能通过 —— 只要 TCP 443 通就能握手。
 * 下面是本项目实测连通过 IAT 的节点，逐个 tcp_probe，第一个通的就用。 */
static const char *k_seed_ip[] = {
    "124.172.152.233",   /* 今天两次 101 成功用的节点 */
    "124.172.152.234",
    "123.57.26.93",      /* 另一次成功握手用的节点 */
};

/* 探活并采用一个 IP；成功返回 0 */
static int adopt_ip(const char *ip)
{
    if (!tcp_probe(ip, XF_ASR_PORT)) return -1;

    strncpy(g_host_ip, ip, sizeof(g_host_ip) - 1);
    g_host_ip[sizeof(g_host_ip) - 1] = '\0';
    g_host_ip_tick = rt_tick_get();
    xf_asr_set_server_ip(g_host_ip);
    rt_kprintf("[voice] use ip %s (tcp 443 ok)\n", g_host_ip);
    return 0;
}

static void try_seed_ip(void)
{
    unsigned n = (unsigned)(sizeof(k_seed_ip) / sizeof(k_seed_ip[0]));
    unsigned i;

    for (i = 0; i < n; i++) {
        rt_kprintf("[voice] dns unavailable, try node %u/%u: %s ...\n",
                   i + 1, n, k_seed_ip[i]);
        if (adopt_ip(k_seed_ip[i]) == 0) return;
    }
    rt_kprintf("[voice] all built-in nodes unreachable, use 'xf_ip <ip>'\n");
}

/* 换节点重试：TLS 层的 -0x4c（对端在 ClientHello 后直接关连接）可能是单个
 * 边缘节点/单条链路的问题，换一个内置节点往往就好了。返回 0 表示已切到新节点。 */
static int rotate_seed_ip(void)
{
    unsigned n = (unsigned)(sizeof(k_seed_ip) / sizeof(k_seed_ip[0]));
    unsigned cur = n, k;

    for (k = 0; k < n; k++)
        if (strcmp(k_seed_ip[k], g_host_ip) == 0) { cur = k; break; }

    for (k = 1; k <= n; k++) {
        const char *ip = k_seed_ip[(cur + k) % n];
        if (strcmp(ip, g_host_ip) == 0) continue;
        if (adopt_ip(ip) == 0) {
            rt_kprintf("[voice] failover -> %s\n", g_host_ip);
            return 0;
        }
    }
    rt_kprintf("[voice] no alternative node reachable\n");
    return -1;
}

static void resolve_host_task(void *arg) {
    (void)arg;
    struct hostent *he = gethostbyname(XF_ASR_HOST);

    /* ★ DNS 兜底重试。
     * 现象：开机 dhcpc 拿到的 dns #0 = 192.168.2.1（路由器代理），偶发直接
     *       返回失败，日志是 `[ws] dns fail`，而同一时刻 ping 223.5.5.5 是通的。
     * 处理：失败一次就把全局 DNS 换成公共服务再解析一次；成功过就不再改回，
     *       后续识别都走缓存 IP，不会反复动全局配置。 */
    if (!he && !g_dns_fallback_done) {
        char d0[] = DNS_FALLBACK_0, d1[] = DNS_FALLBACK_1;
        g_dns_fallback_done = 1;
        set_dns(0, d0);
        set_dns(1, d1);
        rt_kprintf("[voice] dns failed, switch to %s / %s and retry...\n", d0, d1);
        he = gethostbyname(XF_ASR_HOST);
        if (he && he->h_addr_list && he->h_addr_list[0])
            rt_kprintf("[voice] dns fallback OK\n");
    }

    if (he && he->h_addr_list && he->h_addr_list[0]) {
        struct in_addr a;
        const char *ipa;
        memcpy(&a, he->h_addr_list[0], sizeof(a));
        ipa = inet_ntoa(a);          /* 返回静态缓冲，立即拷贝 */
        if (ipa) {
            char ipbuf[32];
            snprintf(ipbuf, sizeof(ipbuf), "%s", ipa);
            if (adopt_ip(ipbuf) == 0) {   /* DNS 给的节点 443 通了就直接用 */
                g_resolving = 0;
                return;
            }
            /* DNS 有结果但节点连不上：不缓存，改用内置节点 */
            rt_kprintf("[voice] dns gave unusable ip %s\n", ipbuf);
        } else {
            rt_kprintf("[voice] inet_ntoa failed\n");
        }
    } else {
        rt_kprintf("[voice] host resolve FAILED (dns)\n");
    }

    /* DNS 没给出可用节点 → 走固件内置的种子 IP，完全不依赖 DNS */
    try_seed_ip();
    g_resolving = 0;
}

/* 非阻塞：需要时才起解析线程，且保证同一时刻只有一个在跑 */
static void ensure_host_resolved(void)
{
    rt_tick_t now = rt_tick_get();

    if (g_host_ip[0] &&
        (now - g_host_ip_tick) < rt_tick_from_millisecond(HOST_IP_TTL_MS))
        return;   /* 缓存仍有效 */

    if (g_resolving) return;
    if (g_resolve_try_tick &&
        (now - g_resolve_try_tick) < rt_tick_from_millisecond(HOST_IP_RETRY_MS))
        return;   /* 失败冷却中，避免 DNS 长期不通时反复起线程 */

    g_resolving = 1;
    g_resolve_try_tick = now;

    rt_thread_t t = rt_thread_create("dnsres", resolve_host_task, RT_NULL,
                                     4096, 20, 10);
    if (t) rt_thread_startup(t);
    else {
        g_resolving = 0;
        rt_kprintf("[voice] dns worker create fail\n");
    }
}

/* 手动预置服务器 IP。
 * 注意：MSH_CMD_EXPORT 的第一个参数就是命令名，所以函数名必须与想让用户敲的
 * 名字一致。这里正式命令名取 "xf_ip"，同时保留 "cmd_xf_ip" 兼容别名
 * （早期文档里写的是 cmd_xf_ip，两个都能敲）。 */
static int xf_ip(int argc, char **argv) {
    if (argc < 2) {
        rt_kprintf("[xf] preset ip = %s\n", g_host_ip[0] ? g_host_ip : "(none)");
        return 0;
    }
    if (strcmp(argv[1], "off") == 0) {
        g_host_ip[0] = '\0';
        g_host_ip_tick = 0;
        g_resolve_try_tick = 0;
        xf_asr_set_server_ip(NULL);
        return 0;
    }
    rt_kprintf("[xf] probing %s ...\n", argv[1]);
    if (!tcp_probe(argv[1], XF_ASR_PORT)) {
        rt_kprintf("[xf] %s unreachable, keep previous: %s\n",
                   argv[1], g_host_ip[0] ? g_host_ip : "(none)");
        return -1;
    }
    strncpy(g_host_ip, argv[1], sizeof(g_host_ip) - 1);
    g_host_ip[sizeof(g_host_ip) - 1] = '\0';
    g_host_ip_tick = rt_tick_get();
    g_resolve_try_tick = rt_tick_get();
    xf_asr_set_server_ip(g_host_ip);
    rt_kprintf("[xf] now using %s\n", g_host_ip);
    return 0;
}
MSH_CMD_EXPORT(xf_ip, xf_ip [<ip>|off] : preset xfyun server ip to skip dns);

static int cmd_xf_ip(int argc, char **argv) { return xf_ip(argc, argv); }
MSH_CMD_EXPORT(cmd_xf_ip, alias of xf_ip (same function));

/* ---------------- TLS 探针（只握手，不录音不识别） ----------------
 * 为什么需要它：TLS 失败时 -0x4c 只说明"对端把连接关了"，看不出是
 * ① 板子 TLS 栈发出的 ClientHello 不被接受，还是 ② 这条链路/这个 LB 节点的问题。
 * 有了 xf_tls，就能在同一份固件上换 IP、换 SNI 反复试，不用每次烧机：
 *    xf_tls                            用当前预设 IP + 默认域名
 *    xf_tls 124.172.152.233            指定 IP
 *    xf_tls 124.172.152.233 iat-api.xfyun.cn   指定 IP + SNI
 * 看到 "[ws] tls ok" = 这条组合能握手；此时再失败就是 WS 层/鉴权问题。
 * 注意：握手成功后会继续尝试 WS 握手（path 给 "/"），报 401 属正常。 */
static int xf_tls(int argc, char **argv) {
    const char *ip   = (argc > 1) ? argv[1] : (g_host_ip[0] ? g_host_ip : NULL);
    const char *host = (argc > 2) ? argv[2] : XF_ASR_HOST;

    if (!ip) {
        rt_kprintf("[tls] no ip; usage: xf_tls <ip> [sni]\n");
        return -1;
    }
    rt_kprintf("[tls] probe %s:%d sni=%s\n", ip, XF_ASR_PORT, host);

    wss_ctx_t *c = wss_connect(host, XF_ASR_PORT, "/", "", ip);
    if (c) {
        rt_kprintf("[tls] probe OK (tls handshake passed)\n");
        wss_close(c);
        return 0;
    }
    rt_kprintf("[tls] probe FAIL (依次看上面: local ip -> tcp -> TX#1 -> RX#1)\n");
    return -1;
}
MSH_CMD_EXPORT(xf_tls, xf_tls <ip> [sni] : TLS handshake probe only);

/* ---------------- 执行一次识别 ---------------- */
/* tick 差 -> 毫秒。RT_TICK_PER_SECOND=100 ⇒ 1 tick = 10ms，分辨率就是 10ms，
 * 不要在这里假称 ms 级精度。 */
static int tick_ms(rt_tick_t a, rt_tick_t b) {
    return (int)(((rt_tick_t)(b - a) * 1000u) / RT_TICK_PER_SECOND);
}

static int do_recognize(void) {
    static uint8_t pcm[AUDIO_CAP_BUFMAX];
    /* ★ 时间戳（2026-10-21 现场："识别的时间不好判断"）。
     * 串口日志原来一行时间信息都没有，用户无法知道
     *   · 敲完命令到真正开始收音隔了多久（其实就是等麦就绪）
     *   · 实际录了几秒（固定 5s 还是 VAD 提前收尾）
     *   · 上传/识别各占多少
     * 全部用 tick 差值打出来，四段相加 ≈ 总耗时，便于定位慢在哪一段。 */
    rt_tick_t t_all, t0, t1, t2;

    /* 校时与域名解析都不阻塞识别：
     * ① 后台线程在跑 NTP 与域名解析，识别线程一步也不等；
     * ② 讯飞握手若因时间偏差被 401拒签，xf_http_asr 会用服务端 Date
     *    自愈校时并自动重试（xf_time_trusted 置位后就不再尝试）。 */
    if (!g_time_ok && !xf_time_trusted)
        ensure_timesync_bg();
    ensure_host_resolved();

    g_voice_state = 1;              /* 等麦就绪 + 录音 */
    g_voice_err[0] = '\0';          /* 新一轮开始，先把上一轮的失败原因清掉 */
    t_all = rt_tick_get();

    /* ★ 先等采集真正可用（直流伺服收敛实测 3.5~4.5 秒），再提示说话。
     *
     * 原顺序是"提示 → do_recognize → open"，中间隔着整整一个收敛期：
     * 用户听到提示立刻开口，那几秒的帧全被 audio_capture_read() 当成
     * "未收敛"丢掉，录到的 5 秒里只剩静音。讯飞对纯静音回 status=2、
     * 没有任何 word，于是整轮识别一句话都不打印，看起来像卡死。
     * 现在改成"能录了再提示"，提示与真正的录音窗口对齐。 */
    {
        int wr = audio_capture_wait_ready(12000);
        if (wr != 0) {
            rt_kprintf("[voice] mic not ready (err=%d)\n", wr);
            snprintf(g_voice_err, sizeof(g_voice_err), "mic not ready (%d)", wr);
            g_voice_state = 0;
            return -1;
        }
        t0 = rt_tick_get();
        g_voice_state = 2;          /* 麦已收敛，正式开始录音 */
        rt_kprintf("[voice] >>> SPEAK NOW <<<  (等麦就绪 %d ms)\n",
                   tick_ms(t_all, t0));
    }

    /* 清掉上一轮的文本：asr_cb 只在有结果时才写它，不清会读到陈旧的旧句子 */
    g_voice_text[0] = '\0';

    int len = audio_capture_record(pcm, sizeof(pcm), AUDIO_CAP_SEC);
    if (len == -2) {
        rt_kprintf("[voice] no speech detected, try again\n");
        snprintf(g_voice_err, sizeof(g_voice_err), "no speech (too quiet)");
        g_voice_state = 0;
        return -1;
    }
    if (len <= 0) {
        rt_kprintf("[voice] audio capture failed (err=%d)\n", len);
        snprintf(g_voice_err, sizeof(g_voice_err), "capture failed (%d)", len);
        g_voice_state = 0;
        return -1;
    }
    g_voice_state = 3;              /* 录音结束，进入上传/识别 */
    rt_kprintf("[voice] captured %d bytes pcm, recognizing...\n", len);
    t1 = rt_tick_get();
    rt_kprintf("[voice] 录音会话 %d ms (音频 %d ms)\n",
               tick_ms(t0, t1), (int)((long)len * 1000 / (AUDIO_CAP_RATE * 2)));

    /* 最后一道保险：后台解析还没给出 IP（DNS 被墙/超时）时，同步探一次内置节点。
     * 放在这里是因为录音已经结束，多花一两秒不影响采集窗口；
     * 若放到 do_recognize 开头，tcp_probe 的耗时会被算进"等麦就绪"里。 */
    if (!g_host_ip[0]) {
        rt_kprintf("[voice] no server ip yet, probing built-in nodes...\n");
        try_seed_ip();
    }

    int rc = xf_asr_recognize_once(pcm, len, asr_cb, NULL);

    /* TLS 层失败后换节点再试一次：PCM 已经在内存里，不用重录，
     * 失败的成本只有一次握手（实测失败是"秒断"，不拖时间）。 */
    if (rc == -8 && g_host_ip[0] && g_time_ok && rotate_seed_ip() == 0) {
        rt_kprintf("[voice] retry recognize on node %s ...\n", g_host_ip);
        rc = xf_asr_recognize_once(pcm, len, asr_cb, NULL);
    }

    /* 时间戳收尾：等麦 / 录音 / 识别三段，相加就是用户实际等到的总时长。 */
    t2 = rt_tick_get();
    rt_kprintf("[voice] 识别 %d ms, 全程 %d ms\n", tick_ms(t1, t2), tick_ms(t_all, t2));

    if (rc == -8) {
        /* -8 = 连接阶段就失败（DNS 无结果 / TCP 连不上 / 握手非 101 都会归到这里），
         * 并不等于"凭证错了"。按有没有拿到服务器 IP 分三种提示，否则会把人
         * 误导向 api_key。 */
        if (!g_host_ip[0]) {
            rt_kprintf("[voice] connect failed (rc=-8): no server ip, dns failed\n");
            rt_kprintf("[voice]   fix: dns %s %s  then retry;  or xf_ip <ip>\n",
                       DNS_FALLBACK_0, DNS_FALLBACK_1);
            snprintf(g_voice_err, sizeof(g_voice_err), "no server ip (dns failed)");
        } else if (!g_time_ok && !xf_time_trusted) {
            rt_kprintf("[voice] connect failed (rc=-8): ip=%s, time not trusted\n",
                       g_host_ip);
            /* ★ 时间坑（已实测确认，别再猜）：
             * 本移植 TZ=UTC-8，`date` 的**输入和显示都是北京时间（UTC+8）**：
             *   - 显示：ctime→localtime 加 8h
             *   - 输入：set_datetime→mktime 反向减 8h，写进 RTC 的仍是 UTC 秒
             * 所以 manual 设时**照手机上显示的时间填**即可，不用自己减 8 小时。
             * 真正的坑在另一个地方：rtc_init_buildtime 曾每次上电都把时钟
             * 打回"编译时刻常量"，复位后就会凭空落后几小时（已改为仅在
             * RTC 明显无效时才写）。 */
            rt_kprintf("[voice]   fix: date <现在几点>   (北京时间即可, 例 2026 10 20 20 07 0)\n");
            rt_kprintf("[voice]   then 'date' again: 显示应和你手机一致; 'tcheck' 看 gmtime\n");
            snprintf(g_voice_err, sizeof(g_voice_err), "time not trusted (run 'date')");
        } else {
            rt_kprintf("[voice] connect failed (rc=-8): ip=%s, check api_key/secret/appid\n",
                       g_host_ip);
            rt_kprintf("[voice]   or try another node: xf_ip <ip> ; probe: xf_tls <ip>\n");
            snprintf(g_voice_err, sizeof(g_voice_err), "tls/tcp failed ip=%s", g_host_ip);
        }
    } else if (rc != 0) {
        rt_kprintf("[voice] recognize rc=%d\n", rc);
        snprintf(g_voice_err, sizeof(g_voice_err), "asr rc=%d", rc);
    } else {
        /* 成功路径原来一个字都不打：若服务端只回了个 status=2 而没有词
         * （纯静音时就是这样），整轮识别零输出，现象和"程序卡死"完全一样。
         * 这里必须留痕，否则无法区分"没识别到"和"根本没跑"。 */
        rt_kprintf("[voice] done rc=0, text %d bytes\n",
                   (int)strlen(g_voice_text));
        if (g_voice_text[0] == '\0')
            snprintf(g_voice_err, sizeof(g_voice_err), "empty result (silence?)");
    }
    g_voice_state = 0;
    return rc;
}

/* ---------------- 任务线程 ---------------- */
static void voice_task(void *arg) {
    rt_kprintf("[voice] task started (mic + http iat)\n");
    while (1) {
        rt_sem_take(g_trig, RT_WAITING_FOREVER);
        /* ">>> SPEAK NOW <<<" 不在这里打：采集 open 之后还有 3.5~4.5 秒的
         * 直流伺服收敛期，那段时间的帧会被 audio_capture_read() 全部丢弃。
         * 提示打进 do_recognize() 里、等采集真正就绪之后再打。 */
        do_recognize();
    }
}

/* ---------------- 导出 msh 命令 ---------------- */
/* voice_cmd_start 定义在文件尾部，这里前向声明（懒初始化用） */
int voice_cmd_start(void);

static int voice_test(int argc, char **argv) {
    /* ★ 可选参数 = 固定录音秒数（2026-10-21-s）。
     *   voice_test        -> 沿用当前设置（默认定长 4 秒）
     *   voice_test 5      -> 固定录 5 秒，中途怎么说都不改变结束时刻
     *   voice_test 0      -> 切回 VAD 自动收尾
     * 现场反馈"识别的时间不好判断"，根子就是 VAD 的收尾点不透明：
     * 阈值 30 和人声帧 RMS（20~100）挨得太近，判早判晚全看运气。
     * 固定时长后时序完全可预测，也顺手排除了"说到一半被腰斩"这类失败。 */
    if (argc >= 2) {
        int sec = atoi(argv[1]);
        if (sec < 0) sec = 0;
        if (sec > AUDIO_CAP_SEC) sec = AUDIO_CAP_SEC;
        audio_capture_set_fixed_sec(sec);
        if (sec > 0)
            rt_kprintf("[voice] rec fixed %d s (关闭 VAD 提前收尾)\n", sec);
        else
            rt_kprintf("[voice] rec mode VAD auto (说完了就停)\n");
    } else {
        int cur = audio_capture_get_fixed_sec();
        if (cur > 0)
            rt_kprintf("[voice] rec fixed %d s (默认); 'voice_test 0' 切 VAD\n", cur);
        else
            rt_kprintf("[voice] rec mode VAD auto; 'voice_test 5' 切定长\n");
    }
    /* 进度条的总长度。注意它是"语音时长"，起点是检测到你开口那一刻，
     * 不是点了按钮那一刻——所以网页显示"已录 x/y 秒"永远不会和实际脱节。 */
    {
        int cur = audio_capture_get_fixed_sec();
        g_rec_target_ms = (cur > 0 ? cur : AUDIO_CAP_SEC) * 1000;
    }
    /* 懒初始化：万一 start_demo 没执行到 voice_cmd_start（旧版启动文件/
     * 提前 return 等），第一次敲 vt 就当场把 RTC + 信号量 + 线程拉起来 */
    if (!g_trig) {
        rt_kprintf("[voice] not started yet, lazy init...\n");
        if (voice_cmd_start() != 0) {
            rt_kprintf("[voice] trigger FAILED (see reason above)\n");
            return -1;
        }
    }
    /* 先置状态再 release：网页每 2 秒才轮询一次 /api/state，如果只靠后台
     * 线程进 do_recognize 之后才置 1，点完按钮的头 1~2 秒界面一点变化都没有
     * （还是"待机"），用户会以为按钮没生效——这也是"语音没返回"的一部分。
     * 这里抢在触发瞬间把状态推到 1，下一次轮询立刻能看到"准备中…"。 */
    g_voice_state = 1;
    g_voice_err[0] = '\0';
    rt_kprintf("[voice] trigger: g_trig=%p (recognizing in background)\n", g_trig);
    rt_sem_release(g_trig);
    return 0;
}
/* MSH_CMD_EXPORT 是 2 参数宏，描述里不能出现"圆括号之外"的逗号，
 * 否则会被当成第 3 个参数 -> "passed 3 arguments, but takes just 2"。
 * 这里把逗号换成分号；想写逗号就套一层括号，例如 (a, b)。 */
MSH_CMD_EXPORT(voice_test, voice_test [sec] : one-shot xfyun iat recognize; sec=0 -> VAD);

/* 两个字符的快捷别名：吞字符环境下最好敲 */
static int vt(int argc, char **argv) { return voice_test(argc, argv); }
MSH_CMD_EXPORT(vt, quick alias of voice_test);

/* 手动 SNTP 校时 —— 后台线程执行，命令立即返回，不再卡 msh shell */
static void ntp_worker(void *arg) {
    (void)arg;
    if (board_ntp_sync() == 0) g_time_ok = 1;
}

/* ---------------- HTTP 头校时（手动备用，自动路径已停用） ----------------
 * 原理：向任意 HTTP 服务发 HEAD 请求，响应头第一行就有 RFC1123 GMT 时间。
 * 注意：本函数依赖 gethostbyname()，在校园网/ICS 环境下域名解析可能长时间
 * 无响应，故只供 cmd_httptime 手动触发，不进入自动识别路径。
 * GMT 即 UTC，解析后由 xf_time_set_gmt() 按 UTC 纪元写入。 */
static int http_time_sync(void) {
    /* 两个站点互为备份：任一能返回 Date 头即可 */
    static const char *hosts[] = { "www.baidu.com", "www.qq.com" };
    struct hostent *he = NULL;
    const char *used = NULL;

    for (int i = 0; i < 2 && !he; i++) {
        he = gethostbyname(hosts[i]);
        if (he && he->h_addr_list && he->h_addr_list[0]) used = hosts[i];
        else he = NULL;
    }
    if (!he) {
        rt_kprintf("[time] http sync FAIL: dns\n");
        return -1;
    }

    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { rt_kprintf("[time] http sync FAIL: socket\n"); return -1; }

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(80);
    memcpy(&sa.sin_addr, he->h_addr_list[0], sizeof(struct in_addr));

    if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        rt_kprintf("[time] http sync FAIL: connect %s\n", used);
        close(s);
        return -1;
    }

    /* 收包超时 5s：绝不能让语音线程在 recv 上永久卡死（lwIP SO_RCVTIMEO 单位 ms） */
    int to = 5000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const void *)&to, sizeof(to));

    char req[128];
    int rlen = snprintf(req, sizeof(req),
                        "HEAD / HTTP/1.0\r\nHost: %s\r\nUser-Agent: mc632x\r\n\r\n", used);
    if (send(s, req, (size_t)rlen, 0) < 0) {
        rt_kprintf("[time] http sync FAIL: send\n");
        close(s);
        return -1;
    }

    char buf[1024];
    int total = 0, n;
    while (total < (int)sizeof(buf) - 1 &&
           (n = recv(s, buf + total, sizeof(buf) - 1 - total, 0)) > 0)
        total += n;
    close(s);
    buf[total] = '\0';

    /* "Date: Thu, 15 Oct 2026 08:15:30 GMT"；URL 里的 date= 大小写不同，不会误命中 */
    char *p = strstr(buf, "Date:");
    if (!p) { rt_kprintf("[time] http sync FAIL: no Date hdr\n"); return -1; }

    /* 交给统一时间接口按 UTC 纪元写入（避免 set_date/set_time 的时区歧义） */
    if (xf_time_set_gmt(p + 5) != 0) {
        rt_kprintf("[time] http sync FAIL: parse\n");
        return -1;
    }
    g_time_ok = 1;
    time_print("[time] http sync ok");
    return 0;
}

/* 组合校时 worker：先 NTP（最快），失败再 HTTP 头（最稳） */
static void time_sync_worker(void *arg) {
    (void)arg;
    time_sync_blocking();
}

/* 线程入口包装：rt_thread_create 要求 void(*)(void*)，而 http_time_sync 返回 int */
static void http_time_worker(void *arg) {
    (void)arg;
    http_time_sync();
}

static int cmd_httptime(int argc, char **argv) {
    rt_thread_t t = rt_thread_create("htime", http_time_worker, RT_NULL,
                                     4096, 20, 10);
    if (!t) { rt_kprintf("[time] worker create fail\n"); return -1; }
    rt_thread_startup(t);
    rt_kprintf("[time] syncing via http in background (dns may be slow)...\n");
    return 0;
}
MSH_CMD_EXPORT(cmd_httptime, sync system time via http date header (tcp 80, manual only));

/* 短别名：httptime / ntp_sync，避免用户按文档敲 cmd_ 前缀的长名字 */
static int httptime(int argc, char **argv) { return cmd_httptime(argc, argv); }
MSH_CMD_EXPORT(httptime, alias of cmd_httptime);

static int cmd_ntp_sync(int argc, char **argv) {
    rt_thread_t t = rt_thread_create("ntpw", ntp_worker, RT_NULL, 4096, 20, 10);
    if (!t) {
        rt_kprintf("[ntp] worker create fail (heap low?)\n");
        return -1;
    }
    rt_thread_startup(t);
    rt_kprintf("[ntp] syncing in background...\n");
    return 0;
}
MSH_CMD_EXPORT(cmd_ntp_sync, sync system time via sntp (aliyun 203.107.6.1));

static int ntp_sync(int argc, char **argv) { return cmd_ntp_sync(argc, argv); }
MSH_CMD_EXPORT(ntp_sync, alias of cmd_ntp_sync);

/* ---------------- 时间体检 ----------------
 * 为什么需要：`date` 命令走的是 localtime()/mktime()，带时区换算；
 * 而讯飞签名要的是**纯 UTC**。两者一旦混淆就会出现"date 看着对、签名却 401"，
 * 或者反过来"date 看着差 8 小时、其实是对的"。
 * tcheck 把三个值一次摆出来，不用再猜：
 *   unix    : time() 原始秒数（RTC 里的绝对时间）
 *   gmtime  : 纯 UTC —— 拿手机时间减 8 小时来对，一致就说明 RTC 准
 *   localtm : localtime() —— `date` 显示的就是它，本移植 TZ=UTC-8 → 恒为 +8
 *   签名用  : time() + 服务端偏移，才是真正进 authorization 的时间 */
static int tcheck(int argc, char **argv) {
    (void)argc; (void)argv;

    time_t raw = time(RT_NULL);
    long   eff = xf_time_now_unix();
    struct tm gs, ls;
    char gb[32] = "?", lb[32] = "?";

    memset(&gs, 0, sizeof(gs));
    memset(&ls, 0, sizeof(ls));

    /* ★ 必须用 *_r 版本：本 libc 的 gmtime()/localtime() 返回**同一个静态
     * 缓冲区**，先取 g 再取 l 会把 g 覆盖掉。之前 tcheck 两行显示得一模一样
     * （gmtime 也带 +8），就是这个原因，极易误判成"gmtime 返回北京时间"。 */
    if (gmtime_r(&raw, &gs))   strftime(gb, sizeof(gb), "%m-%d %H:%M:%S", &gs);
    if (localtime_r(&raw, &ls)) strftime(lb, sizeof(lb), "%m-%d %H:%M:%S", &ls);

    rt_kprintf("[time] time()    = %ld\n", (long)raw);
    rt_kprintf("[time] gmtime    = %s   <- 纯 UTC，应等于北京时间-8h（签名要这个）\n", gb);
    rt_kprintf("[time] localtime = %s   <- date 显示的就是它（北京时间）\n", lb);
    rt_kprintf("[time] signed    = %ld  (offset %ld s)\n", eff, eff - (long)raw);
    time_print("[time] signed GMT");
    if (eff != (long)raw)
        rt_kprintf("[time] 警告：偏移量非 0。若你刚用 date 校过时，敲 toffset 0 清掉它\n");
    return 0;
}
MSH_CMD_EXPORT(tcheck, tcheck : show time tone: raw / gmtime / localtime / signed);

/* ---------------- 手动清除/查看时间偏移量 ----------------
 * 用法：toffset          查看
 *       toffset 0        清除（用 date / ntp_sync 校时后务必执行）
 *       toffset <秒>     手动指定（调试用） */
static int toffset(int argc, char **argv) {
    long raw = (long)time(RT_NULL);
    long eff = xf_time_now_unix();
    long off = eff - raw;

    if (argc < 2) {
        rt_kprintf("[time] offset = %ld s (signed = time() + %ld)\n", off, off);
        return 0;
    }
    if (strcmp(argv[1], "0") == 0) {
        xf_time_offset_clear("manual toffset 0");
        if (off == 0) rt_kprintf("[time] offset already 0\n");
        else          rt_kprintf("[time] offset cleared\n");
        return 0;
    }
    {
        long v = strtol(argv[1], RT_NULL, 10);
        /* 借 set_gmt 的反向用法不方便，这里直接给出等效操作：把系统时间拨到
         * time()+v 再清零偏移，语义等价且不动全局静态变量。 */
        xf_time_set_unix(raw + v);
        xf_time_offset_clear("manual toffset");
        rt_kprintf("[time] system clock shifted by %ld s, offset now 0\n", v);
    }
    return 0;
}
MSH_CMD_EXPORT(toffset, toffset [0|<sec>] : show/clear time offset used by signing);

/* ---------------- RTC 编译时刻兜底 ---------------- */
/* 开机把 RTC 设为固件编译时刻：系统从第一秒起就有"合理时间"。
 * 注意：__DATE__/__TIME__ 是 PC 本地时间（UTC+8），而系统时间按 UTC 纪元存储
 * ——直接写入会比真实 UTC 快 8 小时，讯飞签名要求偏差 <300s，必须先减 8 小时。
 * 写入走 xf_time_set_unix()（直接设系统时间），不用 set_date/set_time。
 *
 * ★ 只在 RTC 明显无效时才写。编译时刻是个**常量**，无条件覆盖会导致
 * "明明刚才 date 校好了，复位一下就回到 8 小时前"——本项目真实踩过：
 * 板子 19:47 复位后 date 显示 11:47，签名时间 03:47（正好是编译时刻），
 * 识别必然失败，而现象看着像"date 命令不准"，极难查。
 * RTC 是 32 位秒计数器，软复位/看门狗复位不掉电时它是延续的，
 * 那份时间比编译常量可信得多，所以先看现值再决定写不写。 */
#define RTC_PLAUSIBLE_MIN  1735689600L   /* 2025-01-01 00:00:00 UTC */

static void rtc_init_buildtime(void) {
    static const char *months[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                   "Jul","Aug","Sep","Oct","Nov","Dec"};
    char mon[4] = {0};
    int day = 1, year = 2026, hour = 12, min = 0, sec = 0, mon_i = 0;
    long cur = (long)time(RT_NULL);

    /* RTC 已经有可信时间 -> 保留它（复位不掉电时这才是对的） */
    if (cur > RTC_PLAUSIBLE_MIN && cur > 0) {
        time_print("[rtc] keep existing clock (reset-safe), use 'date' if wrong");
        return;
    }

    sscanf(__DATE__, "%3s %d %d", mon, &day, &year);   /* "Oct 15 2026" */
    sscanf(__TIME__, "%d:%d:%d", &hour, &min, &sec);   /* "16:54:32" */
    for (int i = 0; i < 12; i++)
        if (strcmp(mon, months[i]) == 0) { mon_i = i + 1; break; }
    if (mon_i == 0) return;

    /* PC 本地(UTC+8) -> UTC：减 8 小时 */
    long utc = xf_days_from_civil(year, mon_i, day) * 86400L
             + hour * 3600L + min * 60L + sec - 8 * 3600L;
    xf_time_set_unix(utc);
    time_print("[rtc] build-time clock (RTC was invalid)");
}

/* 对外暴露的薄封装：开机自启要赶在媒体管线之前把时钟兜住。
 * 里面就是 rtc_init_buildtime()，逻辑只有一份，不另写第二套。 */
int voice_cmd_time_bootstrap(void) {
    rtc_init_buildtime();
    return 0;
}

/* ---------------- 启动 ---------------- */
/* 幂等：重复调用直接返回 0（start_demo 开机调一次 + vt 懒初始化兜底，不会重复建线程） */
int voice_cmd_start(void) {
    if (g_trig) return 0;

    rtc_init_buildtime();

    g_trig = rt_sem_create("voice", 0, RT_IPC_FLAG_FIFO);
    if (!g_trig) {
        rt_kprintf("[voice] sem create FAIL (heap low?)\n");
        return -1;
    }

    /* 栈大小逐级降级：SDK 闭源音频库最吃栈，但堆紧张时宁可降栈也要把功能跑起来。
     * 40KB -> 28KB -> 16KB；都不成功才放弃并给出明确原因。 */
    static const rt_uint32_t stacks[] = { VOICE_STACK, 28672, 16384 };
    rt_thread_t t = RT_NULL;
    rt_uint32_t used = 0;
    for (unsigned i = 0; i < sizeof(stacks) / sizeof(stacks[0]); i++) {
        t = rt_thread_create("voice", voice_task, RT_NULL, stacks[i], VOICE_PRI, 20);
        if (t) { used = stacks[i]; break; }
        rt_kprintf("[voice] thread create fail with %d stack, trying smaller...\n",
                   (int)stacks[i]);
    }
    if (!t) {
        rt_kprintf("[voice] thread create FAIL: need %d stack, heap not enough\n",
                   VOICE_STACK);
        rt_sem_delete(g_trig);
        g_trig = RT_NULL;
        return -1;
    }
    rt_thread_startup(t);
    if (used != VOICE_STACK)
        rt_kprintf("[voice] WARN: running with reduced stack %d (mic quality may suffer)\n",
                   (int)used);
    rt_kprintf("[voice] start ok (prio %d, stack %d)\n", VOICE_PRI, (int)used);

    /* 后台 NTP 校时（有界 5s）。识别路径不等待它；
     * 失败也不影响——讯飞 401 会用服务端 Date 自愈。 */
    ensure_timesync_bg();
    /* 后台解析讯飞域名，让识别路径 0 次 DNS 查询 */
    ensure_host_resolved();
    return 0;
}
