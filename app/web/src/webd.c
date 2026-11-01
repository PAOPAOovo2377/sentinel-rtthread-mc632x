/*
 * webd.c - MC632X 极简 Web 控制台
 *
 * 设计目标：在 60KB 剩余 flash / 紧张 RAM 下提供一个可用的网页控制台。
 *
 *   GET  /                 返回内嵌的 gzip 网页（RAM 内不解压，原样发给浏览器）
 *   GET  /api/state        返回设备状态 JSON（含人脸识别结果）
 *   GET  /api/cmd?c=xxx    执行一条 msh 命令（即所有现有命令都能用）
 *   GET  /api/face?m=...   人脸：run / enroll / clear，**异步**，立即返回
 *   POST /api/enroll?n=xx  body 为一整张 JPEG，登记这张照片
 *
 * 内存策略：
 *   - 不调用 malloc，全部静态分配
 *   - 单线程串行处理，一次只服务一个请求（控制台场景足够，视频走 1111 不经过这里）
 *   - 发送缓冲固定 1KB 复用；请求缓冲 1KB
 *   - 上传的 JPEG 64KB **不在这里另开缓冲**，直接流式喂给
 *     cf_enroll_begin/feed/commit，落到 cloud_face 的参考图池里
 *   - 线程栈 8KB
 *
 * 使用：
 *   msh> webd start [port]     启动（默认 8080）
 *   msh> webd stop             停止
 *   msh> webd token <str>      设置一个简单口令（默认无）
 *   msh> webd status           查看状态
 */

#include <rtthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>       /* EAGAIN / EWOULDBLOCK：webd_send_raw 的重试判断 */
#include <stdarg.h>      /* webd_append 的可变参数 */

#include "cloud_face.h"
#include "img_tune.h"
#include "gallery.h"
#include "sysmon.h"
#include "talk.h"
#include "dog.h"

/*
 * 绊线的运行态（det 人数 / 目标中心点 / 线位置）。
 * 实现在 sample_demo/nna_app/src/sample_nna_ivs.c。
 *
 * ★ 必须用 config.h 的宏包住：
 *   把 FH_APP_OPEN_IVS 关掉时 media.mk 不再编译那个文件，
 *   这里的 extern 就变成悬空引用 —— 链接期报 undefined reference，
 *   而错误信息只指向 webd.o，看起来像 webd 自己坏了。
 *   config.h 里是无值定义（#define FH_APP_OPEN_IVS），所以用 #ifdef。
 */
#include "config.h"

/*
 * TCP_NODELAY 不一定有声明。
 *   RT-Thread 走 SAL 时 <netinet/tcp.h> 通常不存在（这个宏在 lwip/sockets.h
 *   里，而该头文件并不一定被 <sys/socket.h> 带进来），直接写名字就是
 *   -Werror 下的 undeclared。lwIP 里它的值固定是 0x01（关掉 Nagle）。
 *   这里补一份自有定义；万一底层不认这个选项，setsockopt 失败也只是回到
 *   "多等一个 ACK 延迟"，不影响功能（返回值本来就不检查）。
 */
#ifndef TCP_NODELAY
#define TCP_NODELAY 0x01
#endif

#ifdef FH_APP_OPEN_IVS
extern int ivs_status(int *det, int *cx, int *line, int *run);
#endif

/* 不直接 include <msh.h>（shell/ 不在默认 include 路径里），手工声明即可 */
extern int msh_exec(char *cmd, rt_size_t length);

/*
 * 语音识别结果。
 * voice_test 只 release 一个信号量就返回，识别在 voice_task 后台线程里跑，
 * 所以 webd 拿不到它的输出（那些 rt_kprintf 只进串口）。
 * 这三个函数由 voice_cmd.c 导出，是网页能看到语音链路状态的唯一通道。
 */
extern const char *voice_cmd_last_text(void);
extern const char *voice_cmd_last_err(void);
extern int voice_cmd_state(void);

/*
 * 阶段码。这里**不 include "voice_cmd.h"**：webd 的 INCS 里没有 voice_asr
 * 目录，为几个常量改构建路径不划算。拷一份定义，改动时两边同步即可。
 * 与 voice_cmd.h 里的 VOICE_PH_* 必须逐值一致。
 */
#define VOICE_PH_SETTLE  1
#define VOICE_PH_ARMED   2
#define VOICE_PH_REC     3
#define VOICE_PH_ASR     4
#define VOICE_PH_FAIL    5
extern int voice_cmd_phase(int *elapsed_ms, int *total_ms);

#define WEBD_PORT_DEF     8080
#define WEBD_REQ_MAX      1024
/*
 * 发送缓冲 2048（原来 1024）。
 *
 * ★ 为什么要扩（2026-10-28 深夜，降延迟的关键一环）
 *   原来每个响应都是"先 send 头、再 send body"两次系统调用。
 *   两次 send 之间，lwIP 会把第一个小包（HTTP 头 200 多字节）先发出去，
 *   等对端 ACK 才发第二个 —— 而 lwIP 的延迟 ACK 默认是几百毫秒级。
 *   一个 /api/state 就这样白等一个 ACK 往返，浏览器上就是"点一下要等半秒"。
 *   扩到 2048 以后，头 + body 能拼成一次 send 发完，
 *   一个往返就能全部吐出去（1500 MTU 以内还是一个包）。
 */
#define WEBD_TX_MAX       2048
/*
 * 栈必须 >= 8192：
 * /api/cmd 走 msh_exec()，等于在 webd 线程里跑 finsh 解释器 + 目标命令，
 * finsh 自身的 shell 线程栈就是 4096，命令再各自压栈 ⇒ 4096 必溢出。
 */
#define WEBD_STACK_SIZE   8192
#define WEBD_PRIO         25
#define WEBD_CMD_MAX      192
#define WEBD_BACKLOG      8

/* 发送阻塞重试：socket 设了 SO_SNDTIMEO=1s，窗口满时 send 会超时返回 -1。
 * 最多重试 20 次 × 20ms = 400ms，够一个 TCP 窗口排空。 */
#define WEBD_SEND_RETRY     20
#define WEBD_SEND_RETRY_MS  20

/* 由 web_index_gz.c 提供，gzip 压缩过的单文件网页 */
extern const unsigned char web_index_gz[];
extern const unsigned int  web_index_gz_len;

static char      g_req[WEBD_REQ_MAX];
static char      g_tx[WEBD_TX_MAX];
/*
 * 响应体必须用独立缓冲。
 * webd_head() 内部会把自己的 HTTP 头 snprintf 进 g_tx，
 * 若响应体也放在 g_tx，头部会把 body 整段覆盖（/api/state 曾因此返回错内容）。
 * 896 字节的账：asr 120 + asr_err 56 + last_cmd 72 + 人脸那批字段约 120
 * + f_src/f_nm/f_msg 约 110 + ip/固定部分约 110 + asr_ph/el/tot 约 30。
 * 留了一百多字余量，超了会走 404 分支而不是截断（截断会让前端 JSON.parse 直接抛错）。
 *
 * 2026-10-28 扩到 1400：新增 cpu / evi_n / 系统版本 / 固件版本（版本串带编译日期，
 * 二十多个字符）。相册列表单独走 /api/gallery，不塞进这里 —— 16 张图的元数据
 * 就要 900 多字，塞进来必然超。
 *
 * ⚠ 这个 1800 同时也是 /api/gallery 和 /api/evt 的容量上限（三个接口共用一块
 *   g_body，因为它们永远不会同时执行）：
 *     /api/gallery  每张 ~57B，**单次最多 lim=20 张**（1140B）—— 相册已经是
 *                   50 槽，靠 off/lim 分页取，一次全塞必然超。
 *     /api/evt      每条 ~42B → 24 条 = 1008B，余量充足。
 *   要改相册张数只改 gallery.h 的 EVI_SLOTS，这里的分页上限不用动。
 *
 * 同日再扩到 1500：加了 i_dev / i_rgn / i_bgn（按路调节）和 talk / t_lv
 * （对讲状态与实时电平）。这几个字段名短但 JSON 的引号冒号很占地方，
 * 十来项就是 120 字节。
 *
 * 2026-10-28 深夜扩到 1800：加了绊线运行态（ivs_n/det/cx/ln）和
 * 相册/事件的"版本号"（gal_t / evt_t）。
 * 后者是为了减少请求数：原来前端每一轮要连发 3 个请求
 * （state → gallery → evt），现在只要 state 里的版本号没变，
 * 就不用去拉另外两个 —— 稳态下每轮只剩 1 个请求。
 */
static char      g_body[1800];
static int       g_listen_fd = -1;
static rt_uint16_t g_port;
static rt_bool_t g_running = RT_FALSE;
static char      g_token[16];
static char      g_last_cmd[64];
static int       g_chn;
static char      g_ip[16];

/* ---------------- 基础发送 ---------------- */

static int webd_send_raw(int fd, const void *buf, int len)
{
    const char *p = (const char *)buf;
    int n;
    int stall = 0;

    while (len > 0)
    {
        n = send(fd, p, (size_t)len, 0);
        if (n > 0)
        {
            p += n;
            len -= n;
            stall = 0;
            continue;
        }

        /*
         * n <= 0 不等于"对端断了"。本 socket 设了 SO_SNDTIMEO=1s，
         * lwIP 在发送窗口满时会让 send() 超时返回 -1（errno=EAGAIN/EWOULDBLOCK）。
         * 原来这里直接 return -1，于是超过一个 TCP 窗口的响应（index.html 的
         * gzip 有十几 KB）会被**截断**，浏览器拿到半截页面/半截 JSON，
         * 表现就是"页面能开但按钮和数据都不动"。
         * 现在区分"暂时发不出去"和"真的断了"：前者短延时重试，后者才放弃。
         */
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
        {
            if (++stall > WEBD_SEND_RETRY)
            {
                return -1;
            }
            rt_thread_mdelay(WEBD_SEND_RETRY_MS);
            continue;
        }

        return -1;
    }

    return 0;
}

/* 运行期改接收超时：收头用短的（挡预连接），收 body 用长的（大文件分段到达） */
static void webd_set_rx_timeout(int fd, int ms)
{
    struct timeval tv;

    tv.tv_sec  = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

static int webd_head(int fd, int code, const char *ctype, int clen, const char *extra)
{
    const char *reason = (code == 200) ? "OK" : "Not Found";
    int n;

    n = snprintf(g_tx, WEBD_TX_MAX,
                 "HTTP/1.1 %d %s\r\n"
                 "Server: mc632x-webd\r\n"
                 "Content-Type: %s\r\n"
                 "Content-Length: %d\r\n"
                 "Cache-Control: no-store\r\n"
                 "Access-Control-Allow-Origin: *\r\n"
                 "%s"
                 "Connection: close\r\n"
                 "\r\n",
                 code, reason, ctype, clen, (extra != RT_NULL) ? extra : "");

    if (n <= 0 || n >= WEBD_TX_MAX)
    {
        return -1;
    }

    return webd_send_raw(fd, g_tx, n);
}

/*
 * 头 + body 合成一次 send。
 *
 * ★ 这是降延迟最有效的一处（2026-10-28 深夜）
 *   原来的写法是 webd_head() 一次 send、webd_send_raw() 再一次 send。
 *   webd_head 那一包只有两百多字节，lwIP 发出去以后进入"等 ACK"状态；
 *   而延迟 ACK 定时器是几百毫秒量级 —— 这段时间 webd 线程就卡在
 *   第二次 send 里（SO_SNDTIMEO 1 秒），整个单线程服务器都被堵住。
 *   一个 /api/state 从"几毫秒"变成"一个 ACK 周期"。
 *
 *   现在只要 body 装得下（头 200 + body 800 < 2048），就 memcpy 到 g_tx
 *   后面一次发出。整个响应落在一个 TCP 段里，没有"等 ACK 再发"的环节。
 *
 *   大响应（index.html 的 gzip 十几 KB）仍然走"先头后体"，
 *   因为塞不进缓冲 —— 也因为它本来就是多段发送，省这一次没有意义。
 */
static int webd_reply(int fd, int code, const char *ctype,
                      const char *extra, const char *body, int blen)
{
    const char *reason = (code == 200) ? "OK" : "Not Found";
    int h;

    h = snprintf(g_tx, WEBD_TX_MAX,
                 "HTTP/1.1 %d %s\r\n"
                 "Server: mc632x-webd\r\n"
                 "Content-Type: %s\r\n"
                 "Content-Length: %d\r\n"
                 "Cache-Control: no-store\r\n"
                 "Access-Control-Allow-Origin: *\r\n"
                 "%s"
                 "Connection: close\r\n"
                 "\r\n",
                 code, reason, ctype, blen, (extra != RT_NULL) ? extra : "");

    if (h <= 0 || h >= WEBD_TX_MAX)
    {
        return -1;
    }

    if (body != RT_NULL && blen > 0 && (h + blen) <= WEBD_TX_MAX)
    {
        memcpy(g_tx + h, body, (size_t)blen);
        return webd_send_raw(fd, g_tx, h + blen);
    }

    if (webd_send_raw(fd, g_tx, h) != 0) return -1;
    if (body != RT_NULL && blen > 0) return webd_send_raw(fd, body, blen);
    return 0;
}

/* ---------------- URL 解码 ---------------- */

static int webd_hex(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void webd_urldecode(char *s)
{
    char *d = s;

    while (*s)
    {
        if (*s == '%' && s[1] && s[2])
        {
            int hi = webd_hex(s[1]);
            int lo = webd_hex(s[2]);
            if (hi >= 0 && lo >= 0)
            {
                *d++ = (char)((hi << 4) | lo);
                s += 3;
                continue;
            }
        }
        else if (*s == '+')
        {
            *d++ = ' ';
            s++;
            continue;
        }

        *d++ = *s++;
    }

    *d = '\0';
}

/* ---------------- 路由处理 ---------------- */

static void webd_do_index(int fd)
{
    if (webd_head(fd, 200, "text/html; charset=utf-8",
                  (int)web_index_gz_len, "Content-Encoding: gzip\r\n") != 0)
    {
        return;
    }

    webd_send_raw(fd, web_index_gz, (int)web_index_gz_len);
}

/*
 * 识别文本和命令都可能带 " 或 \，直接拼进 JSON 会把整个对象搞坏，
 * 浏览器 JSON.parse 抛错后 /api/state 就只剩 catch 里的"离线"。
 * UTF-8 的中文字节都 >= 0x80，不会被这里改写。
 */
static int webd_json_esc(char *dst, int cap, const char *src)
{
    int o = 0;
    unsigned char c;

    if (cap <= 0)
    {
        return 0;
    }

    while (src != RT_NULL && (c = (unsigned char)*src++) != 0)
    {
        if (o >= cap - 2)
        {
            break;
        }

        if (c == '"' || c == '\\')
        {
            dst[o++] = '\\';
            dst[o++] = (char)c;
        }
        else if (c < 0x20)
        {
            dst[o++] = ' ';
        }
        else
        {
            dst[o++] = (char)c;
        }
    }

    dst[o] = '\0';
    return o;
}

static void webd_do_state(int fd)
{
    rt_uint32_t sec = rt_tick_get() / RT_TICK_PER_SECOND;
    const char *txt = voice_cmd_last_text();
    const char *verr = voice_cmd_last_err();
    const cf_result_t *fr = cf_last_result();
    const char *st;
    char asr[120];
    char aerr[56];
    char cmd[72];
    char nm[40];
    char fmsg[56];
    char src[16];
    int grp = 0;
    int chn = 0;
    int n;
    int el = 0;               /* 已录语音毫秒（板端真实进度） */
    int tot = 0;              /* 本轮目标语音毫秒 */
    int ph;                   /* VOICE_PH_* */
    int busy;
    int sc;
    int refs;
    rt_uint32_t mem_tot = 0;  /* 堆总量（字节） */
    rt_uint32_t mem_use = 0;  /* 已用 */
    rt_uint32_t mem_pk  = 0;  /* 历史峰值 */
    img_state_t is = { 0 };  /* ISP 画面参数缓存（img_tune.h），img_tune_get 全量填充 */
    char sver[24];
    char fwver[40];
    int  cpu;
    int  evi_n;
    int  gal_t = 0;           /* 相册最新一张的 id（版本号，见下面注释） */
    int  evt_t = 0;           /* 最新一条事件的开机秒（事件只增不减） */
    /*
     * 绊线运行态。默认值给"没起来"的样子（det=0 / cx=-1），
     * 这样 FH_APP_OPEN_IVS 关掉时网页显示的就是"未运行"，不会误导。
     */
    int  ivs_run = 0;
    int  ivs_det = 0;
    int  ivs_cx  = -1;
    int  ivs_ln  = 0;

    /*
     * 阶段码由 voice_cmd_phase() 给，它比 voice_cmd_state() 细一档：
     * 把"请说话（窗口已开、正在等你开口）"和"正在录（已检测到人声）"
     * 拆成了两个值。以前这两段都叫 rec，网页只能显示一个笼统的"请说话"，
     * 用户看到时窗口往往已经跑掉一两秒 —— 这正是"显示时间与实际不符"。
     */
    ph = voice_cmd_phase(&el, &tot);
    busy = ph;

    /*
     * 取值含义（网页按这个渲染）：
     *   wait  = 等麦收敛（3.5~4.5s，这段时间开口会被丢）
     *   armed = 窗口已开，正在等你开口 —— 此时说话一定录得全
     *   rec   = 已检测到人声，正在计时（配 asr_el / asr_tot 画进度）
     *   asr   = 上传识别中
     *   fail  = 跑完了但没拿到文本，且留下了原因（见 asr_err）
     *   其它  = 识别出来的文本本身
     */
    if (ph == VOICE_PH_SETTLE)
    {
        st = "wait";
    }
    else if (ph == VOICE_PH_ARMED)
    {
        st = "armed";
    }
    else if (ph == VOICE_PH_REC)
    {
        st = "rec";
    }
    else if (ph == VOICE_PH_ASR)
    {
        st = "asr";
    }
    else if (txt != RT_NULL && txt[0] != '\0')
    {
        st = txt;
    }
    else if (ph == VOICE_PH_FAIL)
    {
        /* 跑完了但没拿到文本，并且留下了失败原因。
         * 原来这种情况也归到 "idle"，网页上跟"没点过"长得一模一样，
         * 用户根本分不清"失败了"和"还没开始"。 */
        st = "fail";
    }
    else
    {
        st = "idle";
    }

    /* 失败原因单独一个字段。
     * 把它并进 asr 是不行的：前端要靠 "idle/wait/rec/asr" 这四个固定值
     * 判断是不是过程状态，混进自由文本就再也分不清"在识别"和"识别失败"。 */
    webd_json_esc(asr, sizeof(asr), st);
    webd_json_esc(aerr, sizeof(aerr), (verr != RT_NULL) ? verr : "");
    webd_json_esc(cmd, sizeof(cmd), g_last_cmd);
    webd_json_esc(nm, sizeof(nm), fr ? fr->name : "");
    webd_json_esc(fmsg, sizeof(fmsg), (fr && fr->msg[0]) ? fr->msg : "-");

    /* 抓帧源回显：人脸抓不到帧时，第一件要确认的就是"抓的是哪一路"。
     * 网页上直接看到 GP1CH1 之类，比让人翻串口日志快得多。 */
    cf_get_capture_channel(&grp, &chn);
    snprintf(src, sizeof(src), "GP%dCH%d", grp, chn);

    /* 分数换算成千分数用整数传：JSON 里写浮点要 %f，
     * newlib-nano 的 %f 不稳；前端再除 1000 显示。 */
    sc = fr ? (int)(fr->score * 1000.0f + 0.5f) : 0;
    if (sc < 0) sc = 0;

    /*
     * 已登记人数以谁为准：
     *   xf 模式 → 板端 g_ref_pool，cf_ref_count() 是唯一真相。
     *             （不能用 fr->known：清空之后它还停在旧值，网页会显示"2 人"但实际 0 人）
     *   pc 模式 → 板端没有参考图，人数由 PC 服务端回报。
     */
    refs = (cf_get_mode() == CF_MODE_XF)
         ? cf_ref_count()
         : ((fr && fr->known > 0) ? fr->known : 0);

    rt_memory_info(&mem_tot, &mem_use, &mem_pk);
    img_tune_get(&is);

    cpu   = sysmon_cpu();
    evi_n = evi_count();
    webd_json_esc(sver,  sizeof(sver),  sysmon_os_ver());
    webd_json_esc(fwver, sizeof(fwver), sysmon_fw_ver());

    /*
     * 相册 / 事件日志的"版本号"。
     *
     * 前端原来每轮轮询都固定再发两个请求（/api/gallery、/api/evt），
     * 而大部分时候内容一个字都没变 —— 纯属白占 webd 的单线程。
     * 这里给两个便宜的比较量：
     *   gal_t  最新一张的 id（删除后不复用，所以"拍了/删了"都会变）
     *   evt_t  最新一条事件的开机秒（事件只增不减）
     * 前端发现没变就跳过那两个请求，稳态下每轮只剩 1 个请求。
     */
    {
        evi_item_t it;

        /* 初始化掉：evi_item_at 失败时它不会被写，
         * 后面的 it.id 虽然走不到，但 gcc 的 -Wmaybe-uninitialized 仍可能报警 */
        memset(&it, 0, sizeof(it));
        gal_t = (evi_item_at(0, &it) == 0) ? it.id : 0;
    }
    {
        unsigned eup = 0;
        char ek[12];
        char et[48];

        if (evt_count() > 0 && evt_at(0, &eup, ek, sizeof(ek), et, sizeof(et)) == 0)
        {
            evt_t = (int)eup;
        }
    }

#ifdef FH_APP_OPEN_IVS
    ivs_status(&ivs_det, &ivs_cx, &ivs_ln, &ivs_run);
#endif

    n = snprintf(g_body, sizeof(g_body),
                 "{\"uptime\":\"%u:%02u:%02u\",\"ip\":\"%s\",\"ch\":%d,\"vo\":1,"
                 "\"asr\":\"%s\",\"asr_err\":\"%s\",\"asr_ph\":%d,\"asr_el\":%d,\"asr_tot\":%d,"
                 "\"busy\":%d,\"last_cmd\":\"%s\","
                 "\"mem_tot\":%u,\"mem_use\":%u,\"mem_pk\":%u,\"cpu\":%d,\"evi\":%d,"
                 "\"evi_cap\":%d,"
                 "\"s_ver\":\"%s\",\"fw_ver\":\"%s\","
                 "\"i_ok\":%d,\"i_brt\":%d,\"i_crt\":%d,\"i_sat\":%d,\"i_mir\":%d,\"i_flp\":%d,"
                 "\"i_mode\":%d,\"i_wb\":%d,\"i_dev\":%d,\"i_rgn\":%d,\"i_ggn\":%d,\"i_bgn\":%d,"
                 "\"i_mwb\":%d,"
                 "\"talk\":%d,\"t_lv\":%d,"
                 "\"gal_t\":%d,\"evt_t\":%d,\"evt_n\":%d,"
                 "\"ivs_n\":%d,\"ivs_det\":%d,\"ivs_cx\":%d,\"ivs_ln\":%d,"
                 "\"f_mode\":\"%s\",\"f_ok\":%d,\"f_mt\":%d,\"f_sc\":%d,"
                 "\"f_ms\":%d,\"f_age\":%d,\"f_ref\":%d,\"f_busy\":%d,\"f_src\":\"%s\","
                 "\"f_nm\":\"%s\",\"f_msg\":\"%s\"}",
                 (unsigned)(sec / 3600u),
                 (unsigned)((sec / 60u) % 60u),
                 (unsigned)(sec % 60u),
                 g_ip[0] ? g_ip : "-",
                 g_chn, asr, aerr, ph, el, tot, busy, cmd,
                 (unsigned)(mem_tot / 1024u),
                 (unsigned)(mem_use / 1024u),
                 (unsigned)(mem_pk / 1024u),
                 cpu, evi_n, EVI_SLOTS, sver, fwver,
                 is.ok, is.brt, is.crt, is.sat, is.mir, is.flp,
                 is.mode, is.wb, img_tune_sel_get(), is.rgn, is.ggn, is.bgn,
                 is.mwb,
                 talk_active(), talk_level(),
                 gal_t, evt_t, evt_count(),
                 ivs_run, ivs_det, ivs_cx, ivs_ln,
                 cf_mode_name(),
                 fr ? fr->ok : 0, fr ? fr->matched : 0, sc,
                 fr ? fr->ms : 0,
                 cf_result_age_ms(),
                 refs,
                 cf_busy(),
                 src,
                 nm, fmsg);
    if (n <= 0 || n >= (int)sizeof(g_body))
    {
        /* 装不下就走 404，不要把半截 JSON 发出去 —— 前端 JSON.parse 会直接抛错，
         * 表现成"整个面板都离线"，比明确报错更难查。 */
        webd_head(fd, 404, "text/plain", 0, RT_NULL);
        return;
    }

    webd_reply(fd, 200, "application/json", RT_NULL, g_body, n);
}

static void webd_do_text(int fd, const char *body)
{
    int n = (int)strlen(body);

    /* 一次 send 发完（OK:xxx 这类响应只有几十字节，两次 send 纯属白等一个 ACK） */
    webd_reply(fd, 200, "text/plain; charset=utf-8", RT_NULL, body, n);
}

static void webd_do_cmd(int fd, char *query)
{
    char cmd[WEBD_CMD_MAX];
    char res[32];
    char *c;
    char *k;
    int ret;

    if (query == RT_NULL)
    {
        webd_do_text(fd, "ERR:no-query");
        return;
    }

    /* 口令校验（未设置则跳过） */
    if (g_token[0] != '\0')
    {
        k = strstr(query, "k=");
        if (k == RT_NULL || strncmp(k + 2, g_token, strlen(g_token)) != 0)
        {
            webd_do_text(fd, "ERR:bad-token");
            return;
        }
    }

    c = strstr(query, "c=");
    if (c == RT_NULL)
    {
        webd_do_text(fd, "ERR:no-cmd");
        return;
    }
    c += 2;

    /* 截断到 '&' */
    k = c;
    while (*k && *k != '&')
    {
        k++;
    }
    *k = '\0';

    webd_urldecode(c);

    if (c[0] == '\0' || strlen(c) >= WEBD_CMD_MAX)
    {
        webd_do_text(fd, "ERR:bad-len");
        return;
    }

    /* 记录最近一条，供 /api/state 展示 */
    snprintf(g_last_cmd, sizeof(g_last_cmd), "%s", c);

    /* 切通道这类命令同时更新状态位 */
    if (c[0] == 'c' && c[1] == 'h' && c[2] == ' ')
    {
        g_chn = (c[3] == '1') ? 1 : 0;
    }

    snprintf(cmd, sizeof(cmd), "%s", c);

    rt_kprintf("[webd] exec: %s\n", cmd);

    ret = msh_exec(cmd, (rt_size_t)strlen(cmd));

    if (ret != 0)
    {
        snprintf(res, sizeof(res), "ERR:%d", ret);
        webd_do_text(fd, res);
    }
    else
    {
        webd_do_text(fd, "OK");
    }
}

/* ---------------- 查询参数 / 请求头取值 ---------------- */

/* 大小写不敏感地比较 a 的前 n 字节与 b（b 必须是全小写） */
static int webd_ci_eq(const char *a, const char *b, int n)
{
    int i;

    for (i = 0; i < n; i++)
    {
        char c = a[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != b[i]) return 0;
    }

    return 1;
}

/* 从 query 里取 key=value，value 做 URL 解码。找不到就写空串。 */
static void webd_qget(char *query, const char *key, char *out, int cap)
{
    int klen = (int)strlen(key);
    char *p = query;

    out[0] = '\0';
    if (query == RT_NULL)
    {
        return;
    }

    while (p != RT_NULL && *p)
    {
        /* 只认"行首"或 '&' 后面的 key，避免 m= 命中 cmd= 这类子串 */
        if (webd_ci_eq(p, key, klen) && p[klen] == '=')
        {
            p += klen + 1;
            {
                int n = 0;
                while (p[n] && p[n] != '&' && n < cap - 1)
                {
                    out[n] = p[n];
                    n++;
                }
                out[n] = '\0';
            }
            webd_urldecode(out);
            return;
        }
        p = strchr(p, '&');
        if (p != RT_NULL) p++;
    }
}

/* 取请求头里的整数字段，如 content-length。找不到返回 -1。 */
static int webd_hdr_int(const char *hdr, const char *key_lower)
{
    const char *line = hdr;
    int klen = (int)strlen(key_lower);

    while ((line = strstr(line, "\r\n")) != RT_NULL)
    {
        const char *v;

        line += 2;
        if (line[0] == '\r' || line[0] == '\n')
        {
            break;                       /* 头部结束 */
        }
        if (!webd_ci_eq(line, key_lower, klen) || line[klen] != ':')
        {
            continue;
        }

        v = line + klen + 1;
        while (*v == ' ') v++;
        return atoi(v);
    }

    return -1;
}

/* ---------------- 人脸 ---------------- */

/*
 * GET /api/face?m=run|enroll|clear[&n=名字]
 *
 * 一律**异步**返回再轮询 /api/state：webd 是单线程，
 * 在这里同步等 1~15 秒会把整个控制台（含状态轮询）堵死，
 * 页面看起来像卡死。cf_face 线程跑任务，这里只负责投递。
 */
static void webd_do_face(int fd, char *query)
{
    char m[12];
    /* 名字要先按 URL 编码收下来再解码：一个汉字编成 %XX%XX%XX 占 9 字节，
     * 8 个汉字就 72 字节。按 32 收会把 %XX 从中间截断，
     * 解码后剩半截转义序列，名字直接变乱码。 */
    char nm[96];
    char buf[56];
    int grp = 0;
    int chn = 0;
    int rc;

    webd_qget(query, "m", m, sizeof(m));

    /* 名字先收下来并校验：否则下面抓帧通道的报错会把"没填名字"盖掉 */
    if (!strcmp(m, "enroll"))
    {
        webd_qget(query, "n", nm, sizeof(nm));
        if (!nm[0])
        {
            webd_do_text(fd, "ERR:no-name");
            return;
        }
    }

    /*
     * run / enroll 都要先抓一帧，所以都做同一个前置检查。
     *
     * 为什么必须检查：抓帧通道（MJPEG）是 vlcview 建流时才登记的，
     * get_mjpeg_config() 在那之前返回 channel<=0。此时 cf_run_async()
     * 依然返回 0 —— 任务确实受理了，只是 cf_face 线程里第一件事就是
     * 抓帧失败，结果要等下一轮轮询才出现在 f_msg 里。
     * 网页看到的就是"点了按钮，标签一直是待机"。
     * 现在把这一步提前，失败当场回一个能照着做的原因。
     */
    if (!strcmp(m, "run") || !strcmp(m, "enroll"))
    {
        cf_get_capture_channel(&grp, &chn);
        if (cf_check_source() != 0)
        {
            snprintf(buf, sizeof(buf), "ERR:no-mjpeg GP%dCH%d", grp, chn);
            webd_do_text(fd, buf);
            return;
        }
    }

    if (!strcmp(m, "run"))
    {
        rc = cf_run_async();
        if (rc == 0)       webd_do_text(fd, "OK:started");
        else if (rc == -6) webd_do_text(fd, "BUSY");
        else if (rc == -5) webd_do_text(fd, "ERR:init");   /* cf_init 失败：堆/线程 */
        else if (rc == -3) webd_do_text(fd, "ERR:sem");
        else               webd_do_text(fd, "ERR:run");
    }
    else if (!strcmp(m, "enroll"))
    {
        rc = cf_enroll_async(nm);
        if (rc == 0)       webd_do_text(fd, "OK:enrolling");
        else if (rc == -6) webd_do_text(fd, "BUSY");
        else if (rc == -5) webd_do_text(fd, "ERR:init");
        else if (rc == -7) webd_do_text(fd, "ERR:no-slot");   /* 两个槽都有人了 */
        else if (rc == -3) webd_do_text(fd, "ERR:sem");
        else if (rc == -8) webd_do_text(fd, "ERR:no-name");
        else               webd_do_text(fd, "ERR:enroll");
    }
    else if (!strcmp(m, "clear"))
    {
        int n = cf_ref_clear();
        if (n < 0)
        {
            webd_do_text(fd, "BUSY");      /* 正在识别，别在这时候改参考图 */
            return;
        }
        snprintf(buf, sizeof(buf), "OK:cleared %d", n);
        webd_do_text(fd, buf);
    }
    else if (!strcmp(m, "reset"))
    {
        /*
         * 强制解锁。只有一种情况该用它：上一轮任务卡在 TLS/握手里，
         * g_busy 一直为 1，于是 run 和 enroll 全部回 BUSY，不重启板子就
         * 再也动不了。cf_submit 里另有 45 秒看门狗自动兜底，这里是手动那一路。
         * 返回值 = 之前是否处于 busy，方便判断"到底是不是真的卡了"。
         */
        int was = cf_busy_reset();
        snprintf(buf, sizeof(buf), "OK:reset was_busy=%d", was);
        webd_do_text(fd, buf);
    }
    else
    {
        webd_do_text(fd, "ERR:bad-m");
    }
}

/* ---------------- 画面参数 ---------------- */

/*
 * GET /api/img?k=brt&v=10      亮度     -100 ~ 100
 * GET /api/img?k=crt&v=64      对比度   U2.6，64 = 1.0 倍
 * GET /api/img?k=sat&v=32      饱和度   U3.5，32 = 1.0 倍
 * GET /api/img?k=mir&v=1       水平镜像
 * GET /api/img?k=flp&v=1       垂直翻转
 * GET /api/img?k=rgn&v=-20     红增益偏移 -50~50 %
 * GET /api/img?k=bgn&v=10      蓝增益偏移 -50~50 %
 * GET /api/img?k=reset         恢复 webd start 那一刻的值
 * GET /api/img?k=dump          两路实测参数 + AWB 收敛状态打到串口
 * GET /api/img?k=neutral       把作用路白平衡锁到中性（直接改硬件增益）
 * GET /api/img?k=wauto         退回 AWB 自动
 *
 * neutral / wauto 是 2026-10-28 为"右侧偏色"加的。和 rgn/ggn/bgn 的区别：
 *   滑块走的是 stAwbCompCfg 补偿表，要 bCompEn=1 才算，被驱动忽略时
 *   表现为"拖到底画面一动不动"；
 *   neutral 走 API_ISP_SetAwbGain 直接写硬件增益 + bMwbEn=1 锁住，
 *   改完一定看得见变化，是判断"软件能不能救这台相机"的对照实验。
 *
 * 所有键都可以再带 &dev=0|1|-1 指定作用路：
 *   dev=0 只改 G0（实拍）  dev=1 只改 G1（AI 主路）  dev=-1 两路（默认）
 * 不带 dev 时沿用上一次选的；首次上电是两路。
 *
 * 同步执行，不派发线程：ISP 这几个 Set 都是写寄存器，微秒级。
 * 拖滑块要好用，就必须"发一个请求立刻生效"，绕一圈任务队列反而会积压。
 */
static void webd_do_img(int fd, char *query)
{
    char k[16];
    char vs[12];
    char ds[8];
    char buf[56];
    int v;
    int rc;
    int sel = 0;

    webd_qget(query, "dev", ds, sizeof(ds));
    if (ds[0])
    {
        if (img_tune_sel(atoi(ds)) != 0)
        {
            webd_do_text(fd, "ERR:bad-dev");
            return;
        }
        sel = 1;
    }

    webd_qget(query, "k", k, sizeof(k));
    if (!k[0])
    {
        /* 只切作用路、不带键：回一个确认，网页随后拉 /api/state 拿该路的值 */
        if (sel)
        {
            snprintf(buf, sizeof(buf), "OK:dev=%d", img_tune_sel_get());
            webd_do_text(fd, buf);
        }
        else
        {
            webd_do_text(fd, "ERR:no-key");
        }
        return;
    }

    if (!strcmp(k, "reset"))
    {
        rc = img_tune_reset();
        webd_do_text(fd, (rc == 0) ? "OK:reset" : "ERR:reset");
        return;
    }

    if (!strcmp(k, "dump"))
    {
        /* 输出只看串口；返回 OK 便于网页/脚本确认命令到达 */
        img_tune_dump();
        webd_do_text(fd, "OK:dump");
        return;
    }

    /* 一键中性化 / 退回自动：不需要 v 参数，所以要放在 v 检查之前 */
    if (!strcmp(k, "neutral"))
    {
        rc = img_tune_neutral();
        if (rc == 0)
        {
            evt_add("img", "white balance locked to neutral");
            webd_do_text(fd, "OK:neutral");
        }
        else if (rc == 1)
        {
            /* 本就中性：不是失败。上一版把这种情况报成错误，
             * 用户以为"一键校色失败"，其实什么都没坏。 */
            webd_do_text(fd, "OK:already");
        }
        else if (rc == -1 || rc == -2)
        {
            /* 读不到 AWB 状态/增益：这路 ISP 没起来，或者根本没使能 */
            webd_do_text(fd, "ERR:no-isp");
        }
        else if (rc == -5)
        {
            /* 统计值不在 0.25~4.0：数值本身不可信，不敢拿它当除数 */
            webd_do_text(fd, "ERR:bad-stat");
        }
        else if (rc == -6)
        {
            /* 校完更偏，已经整份回滚。偏色不在白平衡环节，别再点这个按钮 */
            evt_add("img", "neutral rolled back (made it worse)");
            webd_do_text(fd, "ERR:worse");
        }
        else
        {
            snprintf(buf, sizeof(buf), "ERR:neutral%d", rc);
            webd_do_text(fd, buf);
        }
        return;
    }

    if (!strcmp(k, "wauto"))
    {
        rc = img_tune_wb_auto();
        webd_do_text(fd, (rc == 0) ? "OK:wauto" : "ERR:wauto");
        return;
    }

    webd_qget(query, "v", vs, sizeof(vs));
    if (!vs[0])
    {
        webd_do_text(fd, "ERR:no-val");
        return;
    }
    v = atoi(vs);

    /* mode / wb 是"一次改一组"，不能走 img_tune_set 的单键路径 */
    if (!strcmp(k, "mode"))
    {
        rc = img_tune_preset(v);
        if (rc == 0)
        {
            snprintf(buf, sizeof(buf), "OK:mode=%d", v);
            evt_add("img", "image mode changed");
        }
        else
        {
            snprintf(buf, sizeof(buf), "ERR:mode%d", rc);
        }
        webd_do_text(fd, buf);
        return;
    }
    if (!strcmp(k, "wb"))
    {
        rc = img_tune_wb(v);
        if (rc == 0)
        {
            snprintf(buf, sizeof(buf), "OK:wb=%d", v);
            evt_add("img", "white balance changed");
        }
        else
        {
            snprintf(buf, sizeof(buf), "ERR:wb%d", rc);
        }
        webd_do_text(fd, buf);
        return;
    }

    rc = img_tune_set(k, v);
    if (rc == 0)
    {
        snprintf(buf, sizeof(buf), "OK:%s=%d", k, v);
    }
    else if (rc == -4)
    {
        snprintf(buf, sizeof(buf), "ERR:bad-key %s", k);
    }
    else if (rc == -1)
    {
        /* 取原值就失败 —— 基本只有"这路 ISP 还没起来"这一种可能 */
        snprintf(buf, sizeof(buf), "ERR:no-isp");
    }
    else
    {
        snprintf(buf, sizeof(buf), "ERR:set%d", rc);
    }
    webd_do_text(fd, buf);
}

/* ---------------- 证据相册 / 事件 ---------------- */

/* 按需往 g_body 追加，返回新的写入位置。
 * 不用裸 snprintf 的原因：snprintf 返回的是"本该写多少"，
 * 一旦越界 o 就会跳到 cap 之外，后面的减法算 capacity 就变成负数，
 * 转成 size_t 后是个天文数字 —— 那是典型的"看起来正常、偶发踩内存"。 */
static int webd_append(char *buf, int cap, int o, const char *fmt, ...)
{
    va_list ap;
    int r;

    if (o >= cap - 1) return cap - 1;

    va_start(ap, fmt);
    r = vsnprintf(buf + o, (size_t)(cap - o), fmt, ap);
    va_end(ap);

    if (r < 0)            return o;
    if (o + r >= cap)     return cap - 1;
    return o + r;
}

/*
 * GET /api/snap?t=0   抓一帧存进相册
 *   t: 0 手动 / 1 自动 / 2 识别命中 / 3 异常
 *
 * 同步抓帧（最长 800ms）。页面上的"抓拍"按钮点一次等一次，
 * 不派发线程 —— 相册是用户主动动作，不像识别那样要跑几秒。
 */
static void webd_do_snap(int fd, char *query)
{
    char ts[8];
    int t  = EVI_T_MANUAL;
    int rc;

    webd_qget(query, "t", ts, sizeof(ts));
    if (ts[0]) t = atoi(ts);
    if (t < 0 || t > 3) t = EVI_T_MANUAL;

    rc = evi_capture(t);

    if (rc >= 0)
    {
        evt_add("snap", "evidence captured");
        webd_do_text(fd, "OK:snap");
        return;
    }

    if (rc == -2)      webd_do_text(fd, "ERR:busy");
    else if (rc == -4) webd_do_text(fd, "ERR:too-big");
    else if (rc == -5) webd_do_text(fd, "ERR:not-init");
    else               webd_do_text(fd, "ERR:grab");
}

/*
 * GET /api/gallery          列出相册
 * GET /api/gallery?del=N    删除第 N 张
 *
 * up 是抓拍时的**开机秒数**：板上没有 RTC，用墙钟只会得到一个 1970 年的假时间。
 * 网页把它换算成"开机 12:34 抓拍"，是唯一不会撒谎的写法。
 */
static void webd_do_gallery(int fd, char *query)
{
    char ds[10];
    char os[10];
    char ls[10];
    int n;
    int i;
    int c;
    int o = 0;
    int off = 0;
    int lim = 12;
    int top = 0;
    evi_item_t it;

    /* ---- 一键清空：/api/gallery?clear=1 ---- */
    webd_qget(query, "clear", ds, sizeof(ds));
    if (ds[0] && ds[0] != '0')
    {
        c = evi_clear();

        evt_add("del", "gallery cleared");
        webd_do_text(fd, (c > 0) ? "OK:clear" : "OK:empty");
        return;
    }

    webd_qget(query, "del", ds, sizeof(ds));
    if (ds[0])
    {
        if (evi_delete(atoi(ds)) == 0)
        {
            evt_add("del", "evidence removed");
            webd_do_text(fd, "OK:del");
        }
        else
        {
            webd_do_text(fd, "ERR:no-id");
        }
        return;
    }

    /*
     * ---- 分页列出：/api/gallery?off=N&lim=M ----
     * 50 槽一次全塞进 JSON 有 2.8KB，超过 g_body(1800) 会被截断成非法格式，
     * 前端 JSON.parse 直接抛错、页面显示"空" —— 所以必须分页。
     */
    webd_qget(query, "off", os, sizeof(os));
    webd_qget(query, "lim", ls, sizeof(ls));
    if (os[0]) off = atoi(os);
    if (ls[0]) lim = atoi(ls);

    /* 20 张 × 57B = 1140B，是 g_body 能安全装下的上限 */
    if (lim < 1)  lim = 1;
    if (lim > 20) lim = 20;

    n = evi_count();

    if (off < 0) off = 0;
    if (off > n) off = n;

    /* top = 当前最新一张的 id，给前端当"相册版本号"。
     * 不能拿 items[0].id：翻到第 2 页时那不是最新的，
     * 前端会误判"相册变了"，每轮轮询都白拉一次。 */
    if (evi_item_at(0, &it) == 0) top = it.id;

    o = webd_append(g_body, (int)sizeof(g_body), o,
                    "{\"n\":%d,\"cap\":%d,\"off\":%d,\"lim\":%d,\"top\":%d,\"items\":[",
                    n, EVI_SLOTS, off, lim, top);

    for (i = off; i < n && i < off + lim; i++)
    {
        if (evi_item_at(i, &it) != 0) break;

        o = webd_append(g_body, (int)sizeof(g_body), o,
                        "%s{\"id\":%d,\"ty\":%d,\"sz\":%d,\"w\":%d,\"h\":%d,\"up\":%u}",
                        (i > off ? "," : ""), it.id, it.type, it.size, it.w, it.h, it.up);
    }

    o = webd_append(g_body, (int)sizeof(g_body), o, "]}");

    webd_reply(fd, 200, "application/json", RT_NULL, g_body, o);
}

/* GET /api/evi?id=N  取一张原始 JPEG（网页缩略图和大图都直接用它） */
static void webd_do_evi(int fd, char *query)
{
    char ids[10];
    const unsigned char *p = RT_NULL;
    int len = 0;
    int id;

    webd_qget(query, "id", ids, sizeof(ids));
    id = atoi(ids);

    if (id <= 0 || evi_data(id, &p, &len) != 0 || p == RT_NULL || len <= 0)
    {
        webd_head(fd, 404, "text/plain", 0, RT_NULL);
        return;
    }

    /* 不加 extra 头：webd_head 已经带了 Cache-Control: no-store。
     * 照片按 id 不可变，但槽可能被覆盖后 id 递增，no-store 更省心。 */
    if (webd_head(fd, 200, "image/jpeg", len, RT_NULL) != 0) return;
    webd_send_raw(fd, p, len);
}

/* GET /api/evt  事件时间线 */
static void webd_do_evt(int fd)
{
    char kind[12];
    char text[48];
    char kesc[24];
    char tesc[96];
    unsigned up;
    int n;
    int i;
    int o = 0;

    n = evt_count();

    o = webd_append(g_body, (int)sizeof(g_body), o, "{\"n\":%d,\"items\":[", n);

    for (i = 0; i < n; i++)
    {
        if (evt_at(i, &up, kind, sizeof(kind), text, sizeof(text)) != 0) continue;

        webd_json_esc(kesc, sizeof(kesc), kind);
        webd_json_esc(tesc, sizeof(tesc), text);

        o = webd_append(g_body, (int)sizeof(g_body), o,
                        "%s{\"up\":%u,\"k\":\"%s\",\"t\":\"%s\"}",
                        (i ? "," : ""), up, kesc, tesc);
    }

    o = webd_append(g_body, (int)sizeof(g_body), o, "]}");

    webd_reply(fd, 200, "application/json", RT_NULL, g_body, o);
}

/* 把连接里剩下的 body 读掉再关：lwIP 在 close 时若还有未读数据会发 RST，
 * 浏览器侧就变成 ERR_CONNECTION_RESET，错误信息完全指不到真正原因。 */
static void webd_drain(int fd, int n)
{
    char tmp[256];
    int got;

    while (n > 0)
    {
        got = recv(fd, tmp, (size_t)((n < (int)sizeof(tmp)) ? n : (int)sizeof(tmp)), 0);
        if (got <= 0) return;
        n -= got;
    }
}

/*
 * POST /api/enroll?n=名字   body = 一整张 JPEG
 *
 * 浏览器用 fetch(url, {method:'POST', body: blob}) 直接发二进制，
 * 不走 multipart，板端就不用解析边界。
 * 64KB 的照片不在这里开缓冲，边收边喂进 cloud_face 的参考图池。
 */
static void webd_do_enroll_post(int fd, char *query, int got)
{
    char nm[96];
    char *hdr_end;
    char *body;
    int have;
    int clen;
    int left;
    int rc;
    char chunk[512];
    char buf[64];

    webd_qget(query, "n", nm, sizeof(nm));
    clen = webd_hdr_int(g_req, "content-length");

    hdr_end = strstr(g_req, "\r\n\r\n");
    if (hdr_end == RT_NULL || clen <= 0)
    {
        webd_do_text(fd, "ERR:bad-body");
        return;
    }

    body = hdr_end + 4;
    have = got - (int)(body - g_req);
    if (have < 0) have = 0;
    /* 客户端把 body 直接跟在同一段里送过来时，have 可能已超过 Content-Length
     * （理论上不该发生，但真超了会把多出来的字节也喂进参考图，判断长度时会错位） */
    if (have > clen) have = clen;
    left = clen - have;

    /*
     * 有些客户端会先发 Expect: 100-continue 然后等服务器点头。
     * 不回应的话浏览器要等到超时才发 body，表现为"点上传后卡一两秒"。
     * 这里直接补一个 100 Continue，代价是 25 字节。
     *
     * 注意搜索范围：hdr_end 之后就是 JPEG 二进制，里面碰巧含 "Expect:"
     * 的概率虽低，但一旦命中就会在响应中间多塞一段 1xx。
     * 先把头部就地截断，让 strstr 只看见请求头。
     */
    hdr_end[0] = '\0';
    if (strstr(g_req, "Expect:") != RT_NULL || strstr(g_req, "expect:") != RT_NULL)
    {
        static const char cont[] = "HTTP/1.1 100 Continue\r\n\r\n";
        webd_send_raw(fd, cont, (int)sizeof(cont) - 1);
    }

    if (!nm[0])
    {
        webd_drain(fd, left);
        webd_do_text(fd, "ERR:no-name");
        return;
    }

    rc = cf_enroll_begin(nm);
    if (rc != 0)
    {
        webd_drain(fd, left);
        if (rc == -6)      webd_do_text(fd, "ERR:busy");
        else if (rc == -7) webd_do_text(fd, "ERR:no-slot,clear-first");
        else               webd_do_text(fd, "ERR:begin");
        return;
    }

    if (have > 0)
    {
        rc = cf_enroll_feed((const unsigned char *)body, have);
    }

    while (rc == 0 && left > 0)
    {
        int n = recv(fd, chunk, (size_t)((left < (int)sizeof(chunk)) ? left : (int)sizeof(chunk)), 0);
        if (n <= 0)
        {
            rc = -100;                   /* 单独一个码：别和"超上限(-1)"混在一起 */
            break;
        }
        left -= n;
        rc = cf_enroll_feed((const unsigned char *)chunk, n);
    }

    if (rc != 0)
    {
        cf_enroll_abort();
        webd_drain(fd, left);
        webd_do_text(fd, (rc == -1) ? "ERR:too-big" : "ERR:recv");
        return;
    }

    rc = cf_enroll_commit();
    if (rc == 1)
    {
        /*
         * PC 模式：图已收全，转发给人脸线程去发 HTTP。
         * 这里**不能**等 —— 等就等于把上面那些优化全白做了。
         * 回 OK:uploading，网页随即用 fastPoll 去看 f_msg。
         */
        webd_do_text(fd, "OK:uploading");
        rt_kprintf("[webd] enroll '%s' by upload dispatched (%d bytes)\n", nm, clen);
    }
    else if (rc == 0)
    {
        snprintf(buf, sizeof(buf), "OK:enrolled,refs=%d", cf_ref_count());
        webd_do_text(fd, buf);
        rt_kprintf("[webd] enroll '%s' by upload ok, refs=%d\n", nm, cf_ref_count());
    }
    else if (rc == -10)
    {
        webd_do_text(fd, "ERR:not-jpeg");
    }
    else if (rc == -11)
    {
        webd_do_text(fd, "ERR:jpeg-truncated");
    }
    else if (rc == -1)
    {
        webd_do_text(fd, "ERR:too-big");
    }
    else
    {
        /*
         * 关键：把 cloud_face 留下的真实原因带回去。
         * 原来这里只有 "ERR:commit-5" 这种数字码，而 -5 可能意味着
         * "照片里没脸"、"PC 服务没启动"、"图太大"，网页上完全分不出来，
         * 现场只能看到一句"登记失败"。cf_last_result()->msg 里
         * 才是 "no face in photo" / "no-server tcp" 这类能照着查的话。
         */
        const cf_result_t *fr = cf_last_result();

        if (fr != RT_NULL && fr->msg[0] != '\0')
        {
            snprintf(buf, sizeof(buf), "ERR:%s", fr->msg);
            webd_do_text(fd, buf);
            rt_kprintf("[webd] enroll '%s' failed: %s\n", nm, fr->msg);
        }
        else
        {
            snprintf(buf, sizeof(buf), "ERR:commit%d", rc);
            webd_do_text(fd, buf);
        }
    }
}

/*
 * 记下本机地址，供 /api/state 回显。
 * 前端的 `s.ip || location.hostname` 一直是对的写法，但 JSON 里从来没有 ip
 * 字段，所以永远走回退分支。这里用 getsockname 取"本条连接上本机被看到的地址"，
 * 不需要 include lwIP 的 netif，也不会因为多网口而取错。
 */
static void webd_note_local_ip(int fd)
{
    struct sockaddr_in la;
    socklen_t ll = sizeof(la);

    if (getsockname(fd, (struct sockaddr *)&la, &ll) == 0 && la.sin_family == AF_INET)
    {
        const unsigned char *b = (const unsigned char *)&la.sin_addr.s_addr;
        snprintf(g_ip, sizeof(g_ip), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    }
}

/* ---------------- 语音对讲 ---------------- */

static void wav_put16(unsigned char *p, int v)
{
    p[0] = (unsigned char)(v & 0xff);
    p[1] = (unsigned char)((v >> 8) & 0xff);
}

static void wav_put32(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)(v & 0xff);
    p[1] = (unsigned char)((v >> 8) & 0xff);
    p[2] = (unsigned char)((v >> 16) & 0xff);
    p[3] = (unsigned char)((v >> 24) & 0xff);
}

/* 16k / 16bit / 单声道 WAV 头。
 * 浏览器的 <audio> 和 AudioContext.decodeAudioData 都认这个 44 字节 RIFF，
 * 直接给裸 PCM 前端还得自己拼头，没必要。 */
static void wav_hdr(unsigned char *h, int datalen)
{
    const int rate = 16000;
    const int ch   = 1;
    const int bits = 16;

    memcpy(h + 0,  "RIFF", 4);
    wav_put32(h + 4,  (unsigned)(36 + datalen));
    memcpy(h + 8,  "WAVEfmt ", 8);
    wav_put32(h + 16, 16);
    wav_put16(h + 20, 1);                    /* PCM */
    wav_put16(h + 22, ch);
    wav_put32(h + 24, (unsigned)rate);
    wav_put32(h + 28, (unsigned)(rate * ch * bits / 8));
    wav_put16(h + 32, ch * bits / 8);
    wav_put16(h + 34, bits);
    memcpy(h + 36, "data", 4);
    wav_put32(h + 40, (unsigned)datalen);
}

/*
 * GET /api/talk?op=start|stop|status
 *
 * start 会先拒绝"语音识别正在跑"的情况：两者抢同一条采集流，
 * 同时开的结果是两边都只拿到半帧，识别失败 + 对讲断断续续。
 */
static void webd_do_talk(int fd, char *query)
{
    char op[12];
    int rc;
    int n;

    webd_qget(query, "op", op, sizeof(op));

    if (!strcmp(op, "start"))
    {
        rc = talk_start();
        if (rc == 0)       webd_do_text(fd, "OK:start");
        else if (rc == -2) webd_do_text(fd, "OK:running");
        else if (rc == -6) webd_do_text(fd, "ERR:asr-busy");
        else               webd_do_text(fd, "ERR:open");
        return;
    }

    if (!strcmp(op, "stop"))
    {
        talk_stop();
        webd_do_text(fd, "OK:stop");
        return;
    }

    n = snprintf(g_body, sizeof(g_body), "{\"on\":%d,\"rdy\":%d,\"lv\":%d}",
                 talk_active(), talk_ready(), talk_level());
    webd_reply(fd, 200, "application/json", RT_NULL, g_body, n);
}

/*
 * GET /api/mic?sec=3   取最近 N 秒板端麦克风的声音（WAV）
 *
 * 直接从对讲环形缓冲里切，不等采集：从缓冲里取 0 毫秒，
 * 而 audio_capture_record() 要先等 4.5 秒直流收敛。
 */
static void webd_do_mic(int fd, char *query)
{
    unsigned char hdr[44];
    const unsigned char *pcm = RT_NULL;
    char ss[8];
    int sec = 3;
    int n;

    webd_qget(query, "sec", ss, sizeof(ss));
    if (ss[0]) sec = atoi(ss);
    if (sec < 1) sec = 1;
    if (sec > 6) sec = 6;

    if (!talk_active())
    {
        webd_do_text(fd, "ERR:not-active");
        return;
    }

    n = talk_pcm(sec, &pcm);
    if (n <= 0 || pcm == RT_NULL)
    {
        /* 刚 start：直流伺服还在收敛，数据还没进缓冲 */
        webd_do_text(fd, "ERR:warmup");
        return;
    }

    wav_hdr(hdr, n);

    /*
     * HTTP 头 + 44 字节 WAV 头拼成一次 send（原来是三次：http 头 / wav 头 / pcm）。
     * 中间两次都是几十字节的小包，每个都要等一个 ACK 才轮到下一个，
     * 音频是"点了就要响"的东西，不该把时间花在这种地方。
     */
    {
        int h = snprintf(g_tx, WEBD_TX_MAX,
                         "HTTP/1.1 200 OK\r\n"
                         "Server: mc632x-webd\r\n"
                         "Content-Type: audio/wav\r\n"
                         "Content-Length: %d\r\n"
                         "Cache-Control: no-store\r\n"
                         "Access-Control-Allow-Origin: *\r\n"
                         "Connection: close\r\n"
                         "\r\n", 44 + n);

        if (h <= 0 || h + 44 > WEBD_TX_MAX) return;

        memcpy(g_tx + h, hdr, 44);
        if (webd_send_raw(fd, g_tx, h + 44) != 0) return;
        webd_send_raw(fd, pcm, n);
    }
}

/*
 * POST /api/talkdown   body = 裸 PCM（16k/16bit/mono），播到 LINE_OUT
 *
 * 上限 24KB ≈ 0.75 秒。浏览器按 0.4 秒一块推，间隔里 webd 还能响应轮询。
 * 太大块会把单线程的 webd 堵住，页面上的内存/CPU 数字就会停住不动。
 */
static void webd_do_talkdown(int fd, int got)
{
    static unsigned char buf[24576];
    char *hdr_end;
    char *body;
    int clen;
    int have;
    int left;
    int off;
    int n;
    int rc;

    clen    = webd_hdr_int(g_req, "content-length");
    hdr_end = strstr(g_req, "\r\n\r\n");

    if (hdr_end == RT_NULL || clen <= 0)
    {
        webd_do_text(fd, "ERR:bad-body");
        return;
    }
    if (clen > (int)sizeof(buf))
    {
        webd_drain(fd, clen);
        webd_do_text(fd, "ERR:too-big");
        return;
    }

    body = hdr_end + 4;
    have = got - (int)(body - g_req);
    if (have < 0)  have = 0;
    if (have > clen) have = clen;

    memcpy(buf, body, (size_t)have);
    off  = have;
    left = clen - have;

    while (left > 0)
    {
        n = recv(fd, buf + off, (size_t)((left < 4096) ? left : 4096), 0);
        if (n <= 0)
        {
            webd_do_text(fd, "ERR:recv");
            return;
        }
        off  += n;
        left -= n;
    }

    if (!talk_active())
    {
        webd_do_text(fd, "ERR:not-active");
        return;
    }

    /*
     * ★ 入队就返回，不在这里等播放完成（2026-10-28 深夜改）
     *   talk_play() 是按实时节奏睡的：24KB 要睡 750ms。webd 是单线程，
     *   这 750ms 里所有轮询、点击、相册请求全卡住 —— 一开对讲页面就"死"了。
     *   现在只做一次 memcpy，播放交给 talk.c 里那条常驻线程。
     */
    rc = talk_push_down(buf, off);

    if (rc >= 0)       webd_do_text(fd, "OK:played");
    else if (rc == -3) webd_do_text(fd, "ERR:not-active");
    else if (rc == -2) webd_do_text(fd, "ERR:full");
    else               webd_do_text(fd, "ERR:play");
}

static int webd_handle(int cfd)
{
    char *path;
    char *query;
    char *sp;
    int n;
    int got;
    int is_post = 0;
    char pathbuf[96];
    char qbuf[WEBD_CMD_MAX];

    webd_note_local_ip(cfd);

    /*
     * 必须把整个请求头读完（直到 \r\n\r\n）。
     * lwIP 在 close() 时若接收缓冲还有未读数据，会直接发 RST 而不是 FIN，
     * 浏览器侧表现为 ERR_CONNECTION_RESET / 拒绝连接。
     * 带 Cookie 的请求头经常超过 512 字节，只 recv 一次就会踩这个坑。
     */
    got = 0;
    while (got < WEBD_REQ_MAX - 1)
    {
        n = recv(cfd, g_req + got, WEBD_REQ_MAX - 1 - got, 0);
        if (n <= 0)
        {
            break;
        }
        got += n;
        g_req[got] = '\0';

        /* 收到完整的头部结束标记就可以停了（POST 的 body 由各 handler 自己接着收） */
        if (strstr(g_req, "\r\n\r\n") != RT_NULL)
        {
            break;
        }
        /* 只有头没带 Content-Length 的 GET，读到空行即完整 */
        if (strstr(g_req, "\n\n") != RT_NULL)
        {
            break;
        }
    }

    if (got <= 0)
    {
        return -1;
    }
    g_req[got] = '\0';

    if (strncmp(g_req, "GET ", 4) == 0)
    {
        path = g_req + 4;
    }
    else if (strncmp(g_req, "POST ", 5) == 0)
    {
        path  = g_req + 5;
        is_post = 1;
    }
    else
    {
        webd_do_text(cfd, "ERR:method");
        return 0;
    }

    sp = strchr(path, ' ');
    if (sp == RT_NULL)
    {
        return -1;
    }

    /*
     * ★ 2026-10-27 修的根因：**不要就地往 g_req 里写 '\0'**。
     *
     * 原来这里是 `*sp = '\0'; query = strchr(path,'?'); *query = '\0';`，
     * 直接把请求行切断。可 g_req 里同一个缓冲区后面还有请求头和 body，
     * 而 strstr() 遇到第一个 '\0' 就停 —— 于是 webd_do_enroll_post() 里的
     *   hdr_end = strstr(g_req, "\r\n\r\n");      -> NULL
     *   clen    = webd_hdr_int(g_req, "content-length");  -> -1
     * 恒为真，所有 POST 都回 ERR:bad-body；更要命的是它在 drain body 之前
     * 就返回，lwIP 关连接时还有未读数据就发 RST，浏览器连那句 bad-body
     * 都收不到，只看到"上传失败"。
     *
     * 现场表现就是：网页上选照片 → 上传 → 失败，而且怎么试都一样。
     * 板子自己抓帧登记（GET /api/face?m=enroll）不受影响，所以看起来像
     * "只有 PC 本地照片不行"。
     *
     * 现在把 path / query 拷到独立缓冲，g_req 保持原样。
     */
    {
        int plen = (int)(sp - path);
        char *qm = strchr(path, '?');

        if (plen >= (int)sizeof(pathbuf)) plen = (int)sizeof(pathbuf) - 1;
        memcpy(pathbuf, path, (size_t)plen);
        pathbuf[plen] = '\0';

        qbuf[0] = '\0';
        if (qm != RT_NULL && qm < sp)
        {
            int qn = (int)(sp - qm - 1);
            if (qn >= (int)sizeof(qbuf)) qn = (int)sizeof(qbuf) - 1;
            memcpy(qbuf, qm + 1, (size_t)qn);
            qbuf[qn] = '\0';
            pathbuf[qm - path] = '\0';      /* 路径在 '?' 处截断 */
        }

        path  = pathbuf;
        query = qbuf;
    }

    if (is_post)
    {
        /* body 可能是 64KB 的 JPEG，分段到达；收头用的 250ms 对它太紧 */
        webd_set_rx_timeout(cfd, 1000);

        if (strcmp(path, "/api/enroll") == 0)
        {
            webd_do_enroll_post(cfd, query, got);
        }
        else if (strcmp(path, "/api/talkdown") == 0)
        {
            webd_do_talkdown(cfd, got);
        }
        else
        {
            /* 不认识的 POST 也要把 body 读干净，否则 close 时发 RST */
            webd_drain(cfd, webd_hdr_int(g_req, "content-length"));
            webd_head(cfd, 404, "text/plain", 0, RT_NULL);
        }
        return 0;
    }

    if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0)
    {
        webd_do_index(cfd);
    }
    else if (strcmp(path, "/api/state") == 0)
    {
        webd_do_state(cfd);
    }
    else if (strcmp(path, "/api/cmd") == 0)
    {
        webd_do_cmd(cfd, query);
    }
    else if (strcmp(path, "/api/face") == 0)
    {
        webd_do_face(cfd, query);
    }
    else if (strcmp(path, "/api/img") == 0)
    {
        webd_do_img(cfd, query);
    }
    else if (strcmp(path, "/api/snap") == 0)
    {
        webd_do_snap(cfd, query);
    }
    else if (strcmp(path, "/api/gallery") == 0)
    {
        webd_do_gallery(cfd, query);
    }
    else if (strcmp(path, "/api/evi") == 0)
    {
        webd_do_evi(cfd, query);
    }
    else if (strcmp(path, "/api/evt") == 0)
    {
        webd_do_evt(cfd);
    }
    else if (strcmp(path, "/api/talk") == 0)
    {
        webd_do_talk(cfd, query);
    }
    else if (strcmp(path, "/api/mic") == 0)
    {
        webd_do_mic(cfd, query);
    }
    else if (strcmp(path, "/ping") == 0)
    {
        /* 最小存活探针：curl http://<ip>:8080/ping 应返回 pong */
        webd_do_text(cfd, "pong");
    }
    else if (strcmp(path, "/favicon.ico") == 0)
    {
        webd_head(cfd, 404, "text/plain", 0, RT_NULL);
    }
    else
    {
        webd_head(cfd, 404, "text/plain", 0, RT_NULL);
    }

    return 0;
}

/* ---------------- 服务线程 ---------------- */

static void webd_entry(void *param)
{
    struct sockaddr_in addr;
    struct sockaddr_in peer;
    socklen_t plen;
    struct timeval tv;
    int on = 1;
    int cfd;

    (void)param;

    g_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen_fd < 0)
    {
        rt_kprintf("[webd] socket failed\n");
        g_running = RT_FALSE;
        return;
    }

    setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    /* 让 accept 每秒超时一次，否则 webd stop 会卡在 accept 上出不来 */
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(g_listen_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(g_port);

    if (bind(g_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0)
    {
        rt_kprintf("[webd] bind %d failed\n", g_port);
        goto exit;
    }

    if (listen(g_listen_fd, WEBD_BACKLOG) != 0)
    {
        rt_kprintf("[webd] listen failed\n");
        goto exit;
    }

    rt_kprintf("[webd] listening on %d\n", g_port);

    while (g_running)
    {
        /* accept 设了 1s 超时（见下面 setsockopt 的注释），
         * 所以这里每来一轮就报一次活，空闲时也不会被判死。 */
        dog_beat(DOG_WEB);

        plen = sizeof(peer);
        cfd = accept(g_listen_fd, (struct sockaddr *)&peer, &plen);
        if (cfd < 0)
        {
            rt_thread_mdelay(20);
            continue;
        }

        /*
         * ★ 关掉 Nagle（TCP_NODELAY）。
         *   默认 Nagle 会把"小包"攒着等对端 ACK 再发。webd 的响应本来就小
         *   （几百字节），于是一个往返能解决的请求被拆成两三个，
         *   每个之间都要等一个延迟 ACK 周期。浏览器上表现为"点一下半天没反应"。
         *   局域网带宽根本不是瓶颈，这里要的是延迟。
         */
        setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));

        /*
         * ★ 收请求头只等 250ms（原来 1 秒）。
         *   浏览器会"预连接"：提前建好 TCP 但不立刻发请求（预加载、keep-alive
         *   预热都会这么干）。这些连不耗时地挤在 backlog 里，accept 出来以后
         *   recv 干等 —— 原来一次等满 1 秒，期间**所有**别的请求都排在后面，
         *   整个页面就卡住了。这正是"很多请求延迟都很高"的另一个来源。
         *   局域网里请求头本来就在毫秒级到达，250ms 富余得多。
         *   POST 的 body 阶段会在 webd_handle 里单独调回 1s。
         */
        tv.tv_sec = 0;
        tv.tv_usec = 250 * 1000;
        setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        tv.tv_sec = 1;
        tv.tv_usec = 0;
        setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        webd_handle(cfd);

        close(cfd);
    }

exit:
    if (g_listen_fd >= 0)
    {
        close(g_listen_fd);
        g_listen_fd = -1;
    }

    g_running = RT_FALSE;
    rt_kprintf("[webd] stopped\n");
}

/* ---------------- 启动 ---------------- */

/*
 * 真正把 webd 拉起来的唯一入口。msh 的 `webd start` 和开机自启
 * (startup/boot_auto.c → webd_start_auto())都走这里：
 * 两份实现会漂移 —— 自启那份忘了 img_tune_init()，"恢复默认"就没基线了。
 *
 * 返回 0 成功；-1 已在跑；-2 线程创建失败。
 */
static int webd_spawn(rt_uint16_t port)
{
    rt_thread_t tid;

    if (g_running)
    {
        rt_kprintf("[webd] already running on %d\n", g_port);
        return -1;
    }

    g_port = port;

    /* 趁现在抓一份 ISP 出厂值，"恢复默认"才有东西可恢复。
     * 放在这里而不是 /api/img 首次调用时：滑块第一次动之前抓到的
     * 才是真正的原始值，否则"恢复默认"等于恢复成用户刚调的那档。 */
    img_tune_init();

    /* 相册与占用统计：两者都是纯静态资源，init 只是清零 + 注册钩子，
     * 放在这里是为了"webd start 一次就全好了"，不用额外的 msh 命令。 */
    evi_init();
    sysmon_init();
    evt_add("boot", "webd start");

    g_running = RT_TRUE;
    tid = rt_thread_create("webd", webd_entry, RT_NULL,
                           WEBD_STACK_SIZE, WEBD_PRIO, 10);
    if (tid == RT_NULL)
    {
        g_running = RT_FALSE;
        rt_kprintf("[webd] create thread failed\n");
        return -2;
    }
    rt_thread_startup(tid);
    return 0;
}

/* 开机自启入口（默认端口）。跟手敲 `webd start` 完全等价。 */
int webd_start_auto(void)
{
    return webd_spawn(WEBD_PORT_DEF);
}

/* ---------------- msh 命令 ---------------- */

static void webd(int argc, char **argv)
{
    if (argc < 2)
    {
        rt_kprintf("usage:\n");
        rt_kprintf("  webd start [port]   start web console (default 8080)\n");
        rt_kprintf("  webd stop           stop\n");
        rt_kprintf("  webd token <str>    set access token\n");
        rt_kprintf("  webd status         show status\n");
        return;
    }

    if (strcmp(argv[1], "start") == 0)
    {
        rt_uint16_t port = WEBD_PORT_DEF;

        if (argc >= 3) port = (rt_uint16_t)atoi(argv[2]);

        webd_spawn(port);
    }
    else if (strcmp(argv[1], "stop") == 0)
    {
        if (!g_running)
        {
            rt_kprintf("[webd] not running\n");
            return;
        }
        g_running = RT_FALSE;
    }
    else if (strcmp(argv[1], "token") == 0)
    {
        if (argc < 3 || strlen(argv[2]) >= sizeof(g_token))
        {
            rt_kprintf("[webd] token must be 1..%d chars\n", (int)sizeof(g_token) - 1);
            return;
        }
        snprintf(g_token, sizeof(g_token), "%s", argv[2]);
        rt_kprintf("[webd] token set\n");
    }
    else if (strcmp(argv[1], "status") == 0)
    {
        rt_kprintf("[webd] %s, port=%d, token=%s\n",
                   g_running ? "running" : "stopped", g_port,
                   g_token[0] ? "on" : "off");
    }
    else
    {
        rt_kprintf("[webd] unknown: %s\n", argv[1]);
    }
}

MSH_CMD_EXPORT(webd, webd start|stop|token|status : web console);
