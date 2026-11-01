/*
 * cloud_face.c - 板端抓帧 + 云端人脸比对（PC / 讯飞 双后端，支持运行时登记）
 *
 * 为什么要两条后端：
 *   PC 模式（裸 HTTP，零依赖、不花钱、不需要外网）用来先把"抓帧 → 上传 →
 *   拿下结果"这条链路跑通；讯飞模式再把 URL/签名/解析换掉即可。两者共用
 *   同一块 JPEG 缓冲和同一套结果处理，所以调试 PC 模式得到的经验可直接沿用。
 *
 * 内存设计（这是本模块的核心约束，用户明确要求）：
 *
 *   g_jpeg[64KB]      单帧 JPEG 工作缓冲。PC 模式下它就是请求体本身（零拷贝）；
 *                     讯飞模式下由流式 base64 逐块读取，不产生第二份大缓冲。
 *                     ——如果按"先 memcpy 到 A，再 base64 到 B"的常规写法，
 *                     这里会要 64KB + 88KB = 152KB。现在只花 64KB。
 *   g_ref_pool[128KB] 运行时登记的参考图，2 个槽 x 64KB，存**二进制 JPEG**。
 *                     存二进制而不是 base64，省掉 1/3 膨胀：base64 存法每人要 88KB。
 *                     上传照片登记时，这块也被当作接收暂存区用，
 *                     所以 webd 收 64KB 照片不需要再开缓冲。
 *   g_resp[1KB]       HTTP 响应（讯飞/PC 的返回都在 300 字节内）
 *
 *   合计 ≈ 193KB 静态，工作线程栈 16KB（mbedtls 握手需要）。
 *   全部无堆分配 —— 长时间运行不会产生碎片。
 *   作为参照：本板 RT-Thread 堆有 20MB 以上（0x403684c1 ~ 0x41800000），
 *   193KB 占不到 1%，但静态分配可以保证永不失败。
 */
#include "cloud_face.h"
#include "cf_http.h"
#include "web/src/dog.h"

/* 复用语音模块里已经调通的时间偏移机制：
 * 本 SDK 的系统时间写不进去（set_time 只写 RTC 不刷新 timekeeping），
 * 讯飞签名要求 date 与真实时间偏差 < 300 秒，直接 time() 会 401。
 * xf_time_now_unix() 内部已用服务端 Date 做过偏移补偿，这里直接复用，
 * 不再重写一遍。凭证同理：同一应用的 APIKey/APISecret 对人脸比对通用。 */
#include "xf_http_asr.h"

#include <rtthread.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include <mbedtls/md.h>
#include <mbedtls/base64.h>

#include "libdmc/include/libdmc.h"
#include "dsp/jpeg/include/jpeg_config.h"

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

/* ---------------- 讯飞接口常量 ---------------- */
#define XF_FACE_HOST      "api.xf-yun.com"
#define XF_FACE_PORT      443
#define XF_FACE_URI       "/v1/private/s67c9c78c"

/* 请求体拆成四段，中间的 app_id 与图片 base64 运行时填。
 * ★ 拆成宏是为了能精确算出 Content-Length —— 用 snprintf 估个大概值发出去，
 *   一旦偏大，服务端会按 Content-Length 一直等剩下的字节直到超时（实测表现为
 *   "请求发出去了但 30 秒没响应"）。偏小则请求体被截断、JSON 解析失败。
 *   这里改成 strlen(固定段) + 精确的 base64 长度，一个字节都不差。 */
#define XF_P1_A  "{\"header\":{\"app_id\":\""
#define XF_P1_B  "\",\"status\":3},\"parameter\":{\"s67c9c78c\":{\"service_kind\":\"face_compare\"," \
                 "\"face_compare_result\":{\"encoding\":\"utf8\",\"compress\":\"raw\"," \
                 "\"format\":\"json\"}}},\"payload\":{\"input1\":{\"encoding\":\"jpg\"," \
                 "\"status\":3,\"image\":\""
#define XF_P2    "\"},\"input2\":{\"encoding\":\"jpg\",\"status\":3,\"image\":\""
#define XF_P3    "\"}}}"

/* 流式 base64 分块：输入必须是 3 的倍数，输出 = in/3*4。
 * 输出缓冲不在这里单独开 —— 它直接用 cf_http.c 内部那块 2KB 发送块，
 * 因为 cf_http 只在调用 body 回调时才需要缓冲，两者生命周期完全重叠。 */
#define CF_B64_IN         1536
#define CF_B64_OUT        2048

#define CF_NAME_MAX       32

/* ---------------- 静态缓冲（全部进 .bss，尺寸可查 map） ---------------- */
static uint8_t  g_jpeg[CF_JPEG_MAX];
static char     g_resp[1024];

/* 运行时登记的参考图。用二进制 JPEG 存，比 base64 省 25%。
 * 上传登记时这块就是接收暂存区（见 cf_enroll_begin）。 */
static uint8_t  g_ref_pool[CF_REF_SLOTS][CF_REF_MAX];

/* ---------------- 参考图表 ---------------- */
typedef struct {
    const uint8_t *jpg;      /* 指向 .rodata（编译期底图）或 g_ref_pool 的某一行 */
    int            len;
    int            slot;     /* >=0 : g_ref_pool 的下标；-1 : 编译期底图 */
    char           name[CF_NAME_MAX];
} cf_ref_t;

static cf_ref_t g_ref[1 + CF_REF_SLOTS];
static int      g_ref_cnt = 0;
static int      g_pool_next = 0;   /* 槽位满时的轮转淘汰指针 */

/* ---------------- 运行参数 ---------------- */
/* 默认后端：PC 模式（face_server.py + opencv-contrib LBPH）。
 * 这是 10-27 调通并实测过的那一版：同人 dist 58.6~73.3、异人 133~137、阈值 85。
 * 之所以 11-01 曾短暂改成 XF 又改回来：当时以为是代码问题，实际是 PC 上
 * Python 运行时的 cv2/numpy 被清掉了，服务降级成 engine=none，恒回
 * match=0 score=0.000。代码本来就是好的，装回依赖即可。
 * 想用讯飞云端：msh 敲 face mode xf（重启后回到这里）。 */
static int      g_mode = CF_MODE_PC;

static char     g_pc_host[48] = "192.168.137.1";
static int      g_pc_port     = 5000;
static char     g_pc_path[64] = "/api/face";
static char     g_pc_enroll_path[64] = "/api/enroll";

static const char *g_xf_appid      = XF_APPID;
static const char *g_xf_api_key    = XF_API_KEY;
static const char *g_xf_api_secret = XF_API_SECRET;
static char     g_xf_server_ip[32] = {0};
static float    g_xf_threshold     = 0.67f;

static int      g_cap_grp = 1;      /* 默认 G1 CH1：800x480，网页 1112 那一路 */
static int      g_cap_chn = 1;
static int      g_target_venc = -1; /* 由 get_mjpeg_config() 解析出的 VENC 通道号 */

static int      g_verbose = 0;

/* ---------------- 抓帧状态 ---------------- */
static volatile int g_want_frame = 0;
static volatile int g_frame_len  = 0;
static volatile int g_frame_raw  = 0;  /* 最近一帧的原始长度（含被丢弃的超大帧），
                                        * 只用于报错时告诉用户"该把上限调到多少" */
/* 最近一次真正拷进 g_jpeg 的时刻。用来做"帧复用"：
 * 20fps 的流里等下一帧要 50ms，5fps 要 200ms，看起来不多，
 * 但用户点完按钮到看见结果之间，这一跳是纯等待。
 * 只要手里那帧还没过时，直接用它，抓帧耗时归零。 */
static rt_tick_t    g_frame_tick = 0;
#define CF_FRAME_REUSE_MS  400      /* 超过这个岁数的帧不用，宁可等新的 */

/*
 * ★ 2026-10-29：允许**不经过 cf_grab** 直接取最新帧（见 cf_last_jpeg_age_ms）。
 *   一旦有人这么取，就必须保证"读到的是一整帧"。
 *   回调里的写序原来是 memcpy → g_frame_len → g_frame_tick，
 *   读取方若在 memcpy 中途进来，会拿到"新长度 + 半新半旧的数据"。
 *   加一个写标志：读侧看到 1 就当没帧，退回等帧路径，代价只是一帧延迟。
 *   volatile int 的对齐读写在 Cortex-A 上是原子的，不需要关中断。
 */
static volatile int g_frame_writing = 0;

/* 每成功拷进一帧 +1。cf_grab 用它判断"等到的到底是不是新帧"，
 * 这样就不必再把 g_frame_len 置成 -4 这个哨兵值，
 * 好帧的信息（长度 + 时间戳）能完整保留给旁路取图用。 */
static volatile int g_frame_seq   = 0;
/* 最近一次回调的错误码（-5 空帧 / -6 超上限），0 = 无错。 */
static volatile int g_frame_err   = 0;
static int          g_frame_timeout_ms = 3000;
static rt_sem_t     g_frame_sem  = RT_NULL;
static int          g_subscribed = 0;

/* ---------------- 任务线程 ---------------- */
/* JOB_ENROLL_UP = 网页上传照片登记。
 * 和 JOB_ENROLL（板子自己抓一帧）分开，因为数据来源不同：
 * 这个任务的图已经在 g_ref_pool[g_up_slot] 里了，不需要再抓帧。 */
enum { JOB_NONE = 0, JOB_RECOG, JOB_ENROLL, JOB_ENROLL_UP };

static rt_sem_t     g_job_sem  = RT_NULL;
static rt_sem_t     g_done_sem = RT_NULL;
static rt_thread_t  g_thread   = RT_NULL;
static volatile int g_busy     = 0;   /* 1 = 有任务在跑，防止并发发起 */
/* g_busy 置位的时刻。用来做看门狗：cf_face 线程若卡在 TLS 握手/读响应里，
 * 永远走不到"g_busy = 0"，之后 run/enroll 全部回 BUSY，现场表现就是
 * "人脸登记请求失败"，而且点多少次都一样，只能重启板子。 */
static rt_tick_t    g_busy_tick = 0;
#define CF_BUSY_TIMEOUT_MS  45000    /* 正常最长：抓帧 3s + 讯飞 2 人 32s */
static volatile int g_job_kind = JOB_NONE;
static char         g_job_name[CF_NAME_MAX];
static cf_result_t  g_result;
/* 最近一次作业完成的时刻（tick）。0 = 还没跑过任何作业。
 * 网页靠它区分"刚刚认出来的"和"十分钟前认出来的"——在此之前，卡片会一直
 * 挂着上一次的名字，人早走了还显示"已识别 张三"，是最容易被误读的一种显示。 */
static volatile rt_tick_t g_result_tick = 0;
static cf_result_cb_t g_result_cb = RT_NULL;

/* 照片上传登记的流式上下文（cf_enroll_begin/feed/commit） */
static int  g_up_slot = -1;
static int  g_up_len  = 0;
static char g_up_name[CF_NAME_MAX];

#define CF_LOG(fmt, ...)  do { if (g_verbose) rt_kprintf("[face] " fmt, ##__VA_ARGS__); } while (0)

/* url_encode 定义在下面（讯飞鉴权那段），但 PC 登记要用它编名字 */
static void url_encode(const char *src, char *dst, int dsize);

/* ============================================================
 * 零、参考图表
 * ============================================================ */

/* 按 UTF-8 字符边界截断。
 * 直接砍到 CF_NAME_MAX-1 字节会把一个汉字劈成两半，得到无效 UTF-8：
 * 串口打印是乱码，网页 JSON.parse 也可能因此拿到坏字符串。
 * 名字走网页（URL 编码后最长 CF_NAME_MAX*3 字节）和串口两条路，都会经过这里。 */
static void cf_name_copy(char *dst, const char *src)
{
    int i = 0;

    if (!src) src = "";

    while (i < CF_NAME_MAX - 1 && src[i]) {
        unsigned char c = (unsigned char)src[i];
        int clen = (c < 0x80) ? 1
                 : ((c & 0xE0) == 0xC0) ? 2
                 : ((c & 0xF0) == 0xE0) ? 3
                 : ((c & 0xF8) == 0xF0) ? 4 : 1;   /* 非法首字节按 1 字节处理 */
        int k;

        if (i + clen > CF_NAME_MAX - 1) break;     /* 放不下就整个字符丢掉 */
        for (k = 0; k < clen && src[i + k]; k++) dst[i + k] = src[i + k];
        i += k;
        if (k < clen) break;                       /* 源串提前结束 */
    }
    dst[i] = '\0';
}

static int cf_pool_slot_used(int slot)
{
    int i;

    for (i = 0; i < g_ref_cnt; i++) {
        if (g_ref[i].slot == slot) return i;
    }
    return -1;
}

static int cf_pool_free_slot(void)
{
    int i;

    for (i = 0; i < CF_REF_SLOTS; i++) {
        if (cf_pool_slot_used(i) < 0) return i;
    }
    return -1;
}

/* 把一个参考图放进池子并登记。池子满了就轮转淘汰最老的那个，
 * 这样"再来一个人"永远能成功，不会因为槽位用尽而卡死现场演示。 */
static int cf_ref_add(const char *name, const unsigned char *jpg, int len)
{
    int slot, idx;
    cf_ref_t *e;

    if (!jpg || len <= 0) return -1;
    if (len > CF_REF_MAX) {
        rt_kprintf("[face] ref %d bytes > CF_REF_MAX %d, downscale the photo\n",
                   len, CF_REF_MAX);
        return -2;
    }

    slot = cf_pool_free_slot();
    if (slot < 0) {
        slot = g_pool_next;
        g_pool_next = (slot + 1) % CF_REF_SLOTS;
        idx = cf_pool_slot_used(slot);
        if (idx < 0) return -3;              /* 理论不可达 */
        rt_kprintf("[face] ref slots full, replace '%s' with '%s'\n",
                   g_ref[idx].name, name);
    } else {
        if (g_ref_cnt >= (int)(sizeof(g_ref) / sizeof(g_ref[0]))) return -4;
        idx = g_ref_cnt++;
        g_ref[idx].slot = slot;
    }

    e = &g_ref[idx];
    /* 照片上传登记时 src 就是 g_ref_pool[slot] 本身（暂存区原地转正），
     * 同一地址 memcpy 属于未定义行为，这里跳过。 */
    if (jpg != g_ref_pool[slot]) {
        memcpy(g_ref_pool[slot], jpg, (size_t)len);
    }
    e->jpg  = g_ref_pool[slot];
    e->len  = len;
    cf_name_copy(e->name, name);

    rt_kprintf("[face] enrolled '%s' (%d bytes, slot %d), total %d\n",
               e->name, len, slot, g_ref_cnt);
    return 0;
}

int cf_ref_count(void) { return g_ref_cnt; }

const char *cf_ref_name(int idx)
{
    if (idx < 0 || idx >= g_ref_cnt) return "";
    return g_ref[idx].name;
}

/* 清空运行时参考图。返回清掉的个数；<0 = 有任务在跑，拒绝执行。
 * 必须挡一下：识别期间 cf_upload_xf 正在按 g_ref[i].len 读 g_ref_pool，
 * 这里改写 len 会让它读到半张图（或者读到别人）。 */
int cf_ref_clear(void)
{
    int i, n = 0;

    if (g_busy) {
        rt_kprintf("[face] busy, try again after the running job\n");
        return -1;
    }

    for (i = g_ref_cnt - 1; i >= 0; i--) {
        if (g_ref[i].slot >= 0) {
            g_ref[i].slot = -1;
            g_ref[i].len  = 0;
            g_ref[i].jpg  = RT_NULL;
            g_ref[i].name[0] = '\0';
            n++;
        }
    }

    /* 压缩数组，保留编译期底图（slot == -1 且 jpg 非空） */
    {
        int k = 0;
        for (i = 0; i < g_ref_cnt; i++) {
            if (g_ref[i].jpg != RT_NULL || g_ref[i].slot >= 0) {
                if (k != i) g_ref[k] = g_ref[i];
                k++;
            }
        }
        g_ref_cnt = k;
    }

    g_pool_next = 0;
    if (n) rt_kprintf("[face] cleared %d runtime ref(s), %d left\n", n, g_ref_cnt);
    return n;
}

static void cf_ref_init(void)
{
    if (cf_ref_jpg_len > 0) {
        g_ref[0].jpg  = cf_ref_jpg;
        g_ref[0].len  = cf_ref_jpg_len;
        g_ref[0].slot = -1;
        cf_name_copy(g_ref[0].name, "default");
        g_ref_cnt = 1;
        rt_kprintf("[face] built-in ref: %d bytes (.rodata)\n", cf_ref_jpg_len);
    }
}

/* ============================================================
 * 一、抓帧：订阅 dmc 的 MJPEG 帧
 * ============================================================ */

/* dmc 回调在 VENC 取流线程里同步执行，这里**必须尽快返回**：
 * 只做一次 memcpy（64KB 以内约 0.3ms），base64 与网络全部留给工作线程。
 * 若在这里做编码/发送，会把 MJPEG 推流卡住，网页画面会出现顿挫。 */
static int cf_dmc_cb(int media_chn, int media_type, int media_subtype,
                     unsigned long long frame_pts,
                     unsigned char *frame_data, int frame_len, int end_flag)
{
    (void)media_type; (void)media_subtype; (void)frame_pts; (void)end_flag;

    if (media_chn != g_target_venc) return 0;

    /*
     * ★ 2026-10-29：从"要了才拷"改成"**每帧都拷**"。
     *
     *   原来第一行是 `if (!g_want_frame) return 0;`，也就是只有识别/登记
     *   主动要帧时才更新 g_jpeg。后果是：绊线想留证时，手里那帧可能是一分钟前的，
     *   只能再发一次要帧请求、再等一个帧周期 —— 而等帧正是抓不到图的那一段。
     *   改成无条件缓存最新帧之后，任何时候取图都是"最近 0~200ms 的画面"。
     *
     *   代价：5~15fps 下每秒多做 5~15 次 64KB memcpy ≈ 0.3ms/次，
     *   满载也就 4.5ms/s（0.45% CPU）。换来的是抓拍零等待，值。
     *
     *   错误帧（空/超大）不进缓存，只在有人正等着的时候把错误码交回去。
     */
    g_frame_raw = frame_len;

    if (frame_len <= 0 || frame_len > CF_JPEG_MAX) {
        if (g_want_frame) {
            /* -5 空帧 / -6 超上限，和 cf_ensure_subscribe 的 -2/-3 分开，
             * 否则报错时会一律显示成"mjpeg 没使能"，指向完全错误的方向。 */
            g_frame_err  = (frame_len <= 0) ? -5 : -6;
            g_want_frame = 0;
            if (g_frame_sem) (void)rt_sem_release(g_frame_sem);
        }
        return 0;
    }

    g_frame_writing = 1;
    memcpy(g_jpeg, frame_data, (size_t)frame_len);
    g_frame_len = frame_len;
    g_frame_tick = rt_tick_get();
    g_frame_err = 0;
    g_frame_seq++;
    g_frame_writing = 0;

    if (g_want_frame) {
        g_want_frame = 0;
        if (g_frame_sem) (void)rt_sem_release(g_frame_sem);
    }
    return 0;
}

/* 延迟订阅：cf_init() 可能在媒体管线起来之前被调用，那时
 * get_mjpeg_config() 里还没有有效通道号，订阅会拿到 -1。
 * 因此把订阅推到第一次真正抓帧时。 */
static int cf_ensure_subscribe(void)
{
    ENC_INFO *info;

    if (g_subscribed) return 0;

    info = get_mjpeg_config(g_cap_grp, g_cap_chn);
    if (!info || !info->enable || info->channel <= 0) {
        rt_kprintf("[face] VPU[%d] CHN[%d] MJPEG not enabled (enable=%d chn=%d)\n",
                   g_cap_grp, g_cap_chn, info ? info->enable : -1,
                   info ? info->channel : -1);
        return -2;
    }
    g_target_venc = info->channel;

    /* grpcode 传 -1：libdmc 的过滤是 `grpcode & (1 << (media_chn / 3))`，
     * 而 MJPEG 的 media_chn 是 30/29/28... 这类 VENC 通道号，按组号算位
     * 会得到 bit10 以上。全 1 掩码可以匹配任何通道，通道筛选交给回调自己做。 */
    if (dmc_subscribe("CLOUD_FACE", -1, DMC_MEDIA_TYPE_MJPEG, cf_dmc_cb) != 0) {
        rt_kprintf("[face] dmc_subscribe failed (subscriber table full?)\n");
        return -3;
    }
    g_subscribed = 1;
    rt_kprintf("[face] capture source: VPU[%d] CHN[%d] -> VENC chn %d\n",
               g_cap_grp, g_cap_chn, g_target_venc);
    return 0;
}

static int cf_grab(int timeout_ms)
{
    int rc   = cf_ensure_subscribe();
    int seq0 = 0;

    /* 把订阅失败的真实原因透出去（-2 没使能 / -3 订阅表满），
     * 一律折成 -2 会让"订阅表满"看起来像"通道没开"。 */
    if (rc != 0) return rc;
    if (!g_frame_sem) return -1;

    /*
     * 帧复用：回调现在无条件把每帧拷进 g_jpeg（见 cf_dmc_cb 的说明），
     * 所以这里几乎总能命中 —— 手里那帧最多也就一个帧周期旧。
     * 400ms 的上限只在"流停了"的时候才起作用：那时宁可超时报错，
     * 也不要拿一分钟前的画面去做识别/留证。
     *
     * 这一条对"点了没反应"的观感影响很大：原来每次识别/登记都要
     * 先空等一个帧周期，链路一慢（PC 端忙）就叠加成秒级。
     */
    if (g_frame_len > 0 &&
        (rt_int32_t)(rt_tick_get() - g_frame_tick) <
            (rt_int32_t)rt_tick_from_millisecond(CF_FRAME_REUSE_MS)) {
        return g_frame_len;
    }

    (void)rt_sem_control(g_frame_sem, RT_IPC_CMD_RESET, RT_NULL);

    /*
     * ★ 2026-10-29：不再把 g_frame_len 置成 -4 这个哨兵值。
     *   改成记下 g_frame_seq，回来比对 —— 只有序号涨了才说明真拿到新帧。
     *   好处是等待期间"上一帧好帧"的长度/时间戳一直有效，
     *   绊线旁路取图（cf_last_jpeg_age_ms）任何时候都能拿到可用数据，
     *   不会因为人脸线程正在抓帧就退化成"没有帧"。
     */
    seq0         = g_frame_seq;
    g_frame_err  = 0;
    g_frame_raw  = 0;
    g_want_frame = 1;

    if (rt_sem_take(g_frame_sem, rt_tick_from_millisecond((rt_int32_t)timeout_ms)) != RT_EOK) {
        g_want_frame = 0;
        return -4;
    }

    /* 回调拿到的是错误帧（空帧/超大），序号没涨 */
    if (g_frame_seq == seq0) {
        /* -6 = 该帧超过 CF_JPEG_MAX 被丢弃。这里把**原始大小**打出来，
         * 用户据此就知道上限该调到多少，而不是只拿到一个光秃秃的 -6。 */
        if (g_frame_err == -6)
            rt_kprintf("[face] frame %d > CF_JPEG_MAX %d, raise it in cloud_face.h\n",
                       (int)g_frame_raw, CF_JPEG_MAX);
        return g_frame_err ? g_frame_err : -4;
    }

    return g_frame_len;
}

int cf_grab_only(int frame_timeout_ms)
{
    return cf_grab(frame_timeout_ms);
}

const unsigned char *cf_last_jpeg(int *len)
{
    if (len) *len = g_frame_len;
    return g_jpeg;
}

/*
 * 最近一帧的年龄（毫秒）。<0 = 没有可用帧（还没抓过 / 正在写 / 上次是错误码）。
 *
 * 为什么需要它：evi_capture 原来只能走 cf_grab_only，而 cf_grab 第一步是
 * `if (g_busy) return -6`（这里是被 gallery 的 cf_busy 轮询挡在外面）。
 * 人脸识别一忙，相册就得等到 1.2 秒超时才放弃 —— 越线那一刻的画面早就过去了。
 * 但 g_jpeg 里**本来就有**刚被识别线程拷进来的那一帧，几毫秒前才写好。
 * 有这个接口，旁路取图就能做到零等待，也不再和识别线程抢 vicap 通道。
 */
int cf_last_jpeg_age_ms(void)
{
    if (g_frame_writing)         return -1;
    if (g_frame_len <= 0)        return -1;
    if (g_frame_tick == 0)       return -1;

    return (int)((rt_tick_get() - g_frame_tick) * 1000u / RT_TICK_PER_SECOND);
}

/* 抓帧源预检：只看配置，不订阅、不等帧、不改任何状态。
 * 与 cf_ensure_subscribe() 的前半段判断保持一致，但**不打印、不置 g_subscribed**，
 * 这样网页可以随时调它做健康检查。 */
int cf_check_source(void)
{
    ENC_INFO *info = get_mjpeg_config(g_cap_grp, g_cap_chn);

    if (!info || !info->enable || info->channel <= 0) return -2;
    return 0;
}

void cf_get_capture_channel(int *grp_id, int *chn_id)
{
    if (grp_id) *grp_id = g_cap_grp;
    if (chn_id)  *chn_id  = g_cap_chn;
}

const cf_result_t *cf_last_result(void)
{
    return &g_result;
}

/* 距最近一次作业过了多少毫秒。-1 = 开机还没跑过任何作业。
 * 必须是"作业结束时刻"，不能用 g_busy_tick（那个是开始时刻，
 * 一次 HTTPS 登记可能跑几十秒，会让年龄虚高）。 */
int cf_result_age_ms(void)
{
    if (g_result_tick == 0) return -1;
    return (int)((rt_tick_get() - g_result_tick) * 1000u / RT_TICK_PER_SECOND);
}

int cf_busy(void) { return g_busy; }

/* 手动解锁（网页 /api/face?m=reset）。
 * 返回之前是否 busy —— 网页据此判断"到底是不是真的卡住了"，
 * 因为 was_busy=0 时说明问题不在锁上，得往抓帧通道那边查。 */
int cf_busy_reset(void)
{
    int was = g_busy;

    g_busy = 0;
    /* 半截的上传也要一起丢掉：只清 g_busy 而留着 g_up_slot>=0，
     * 下一次 cf_enroll_begin 会以 -9 失败，看起来像换了另一个毛病。 */
    g_up_slot = -1;
    g_up_len  = 0;
    g_up_name[0] = '\0';
    return was;
}

/* ============================================================
 * 二、极简 JSON 取值（与 xf_http_asr.c 同一套思路，不带 cJSON）
 * ============================================================ */
static void json_get_str(const char *json, const char *key, char *out, int size)
{
    char pat[32];
    const char *p, *q;
    int n;

    if (!out || size <= 0) return;
    out[0] = '\0';

    /* 只匹配到冒号，冒号后的空白单独跳过。
     * 原来直接匹配 "\"%s\":\"" 这种紧凑写法，而 Python 的 json.dumps
     * 默认输出 "name": "zjq"（冒号后有空格），于是 strstr 整串找不到 ->
     * 所有字符串字段（name / msg）全为空。现场表现就是：
     *   matched=1 score=0.616 一切正常，但网页上名字显示"未知"，
     *   而且连服务端的错误原因也一直是 "-"。
     * json_get_int/json_get_permille 早就跳空格了，只有这个函数漏了。 */
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    p = strstr(json, pat);
    if (!p) return;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != '"') return;
    p++;
    q = p;
    while (*q && *q != '"') q++;
    n = (int)(q - p);
    if (n >= size) n = size - 1;
    memcpy(out, p, (size_t)n);
    out[n] = '\0';
}

/* 解析 "0.996186" 这种小数到千分数，避开 newlib-nano 的 %f/atof 不确定性 */
static int json_get_permille(const char *json, const char *key)
{
    char pat[32];
    const char *p;
    int ip = 0, fp = 0, fd = 0;

    snprintf(pat, sizeof(pat), "\"%s\":", key);
    p = strstr(json, pat);
    if (!p) return -1;
    p += strlen(pat);
    while (*p == ' ' || *p == '"') p++;

    while (*p >= '0' && *p <= '9') { ip = ip * 10 + (*p - '0'); p++; }
    if (*p == '.') {
        p++;
        while (*p >= '0' && *p <= '9' && fd < 3) { fp = fp * 10 + (*p - '0'); p++; fd++; }
        while (fd++ < 3) fp *= 10;
    }
    return ip * 1000 + fp;
}

static int json_get_int(const char *json, const char *key)
{
    char pat[32];
    const char *p;

    snprintf(pat, sizeof(pat), "\"%s\":", key);
    p = strstr(json, pat);
    if (!p) return -1;
    p += strlen(pat);
    while (*p == ' ' || *p == '"') p++;
    if (*p == 't' || *p == 'f') return (*p == 't') ? 1 : 0;   /* true/false */
    return atoi(p);
}

/* ============================================================
 * 三、PC 后端：直接 POST 原始 JPEG
 * ============================================================ */
static int cf_upload_pc(int len, cf_result_t *r)
{
    static const char hdr[] = "Content-Type: image/jpeg\r\nX-Device: mc632x\r\n";
    int code;

    r->img_len = len;
    /* vhost 传 NULL：局域网 PC 不看 Host 头，直接用 IP 即可 */
    code = cf_http_post_mem(g_pc_host, RT_NULL, g_pc_port, 0, g_pc_path, hdr,
                            (const char *)g_jpeg, len,
                            g_resp, sizeof(g_resp), 4000);
    r->http_code = code;

    if (code != 200) {
        /* -3 = TCP 连不上。现场 99% 是 PC 端 face_server.py 没启动，
         * 或者 Windows 防火墙把 5000 拦了（两者从板子看都是 ECONNRESET）。
         * 原来这里写 "http -3"，网页上显示出来没人看得懂，
         * 用户只知道"登记失败"，不知道该去启动服务端。 */
        if (code == -3)      snprintf(r->msg, sizeof(r->msg), "no-server tcp");
        else if (code == -2) snprintf(r->msg, sizeof(r->msg), "no-server dns");
        else                 snprintf(r->msg, sizeof(r->msg), "http %d", code);
        CF_LOG("pc upload fail, code=%d\n", code);
        return -1;
    }

    /* 服务端约定（tools/face_server.py）：
     * {"ok":1,"face":1,"matched":1,"name":"zhangsan","score":0.93,"ms":42,"known":3} */
    {
        int ok  = json_get_int(g_resp, "ok");
        int fc  = json_get_int(g_resp, "face");
        int mt  = json_get_int(g_resp, "matched");
        int pm  = json_get_permille(g_resp, "score");
        int kn  = json_get_int(g_resp, "known");
        int ix  = json_get_int(g_resp, "indexed");

        r->ok      = (ok > 0);
        r->matched = (mt > 0);
        r->score   = (pm >= 0) ? ((float)pm / 1000.0f) : 0.0f;
        /* 优先用 indexed（真正进了索引、能被认出来的人数）。
         * known 只是 known/ 目录里的照片张数——登记失败的照片照样落盘，
         * 用 known 会让网页显示"3 人"，实际上一个都认不出。 */
        r->known   = (ix >= 0) ? ix : ((kn > 0) ? kn : 0);
        json_get_str(g_resp, "name", r->name, sizeof(r->name));

        /* 服务端在 ok=1 的时候也可能带 msg，而且这条 msg 才是真因：
         *   {"ok":1,"face":1,"matched":0,"score":0.0,"msg":"detect-only"}
         * 意思是"图收到了、脸也检测到了，但后端没有比对引擎"。
         * 原来的实现在 ok=1 时完全不读 msg，于是网页只剩一句
         * "识别失败"，现场分不清是"认错人"还是"根本没装识别库"。
         * 2026-10-27 现场踩过：PC 上 cv2/face_recognition 一个都没装，
         * 三次登记全部只是存盘，ref 从 1 涨到 3，识别却恒为 score=0.000。 */
        json_get_str(g_resp, "msg", r->msg, sizeof(r->msg));

        if (!r->ok) {
            if (!r->msg[0]) snprintf(r->msg, sizeof(r->msg), "server ok=0");
            return -1;
        }
        if (!fc && !r->msg[0]) snprintf(r->msg, sizeof(r->msg), "no face");
        return 0;
    }
}

/* 把一块 JPEG 发给 PC 登记。jpg 可能是 g_jpeg（抓帧登记）或
 * g_ref_pool[slot]（照片上传登记），两者都不复制。 */
static int cf_enroll_pc(const char *name, const unsigned char *jpg, int len,
                        cf_result_t *r)
{
    /* 224 = 路径 63 + "?name=" 6 + URL 编码后的名字(CF_NAME_MAX 31 字节 x3) + '\0' */
    static char path[224];
    char enc[CF_NAME_MAX * 3 + 8];
    int code;

    url_encode(name, enc, (int)sizeof(enc));
    snprintf(path, sizeof(path), "%s?name=%s", g_pc_enroll_path, enc);

    r->img_len = len;
    code = cf_http_post_mem(g_pc_host, RT_NULL, g_pc_port, 0, path,
                            "Content-Type: image/jpeg\r\nX-Device: mc632x\r\n",
                            (const char *)jpg, len,
                            g_resp, sizeof(g_resp), 6000);
    r->http_code = code;

    if (code != 200) {
        if (code == -3)      snprintf(r->msg, sizeof(r->msg), "no-server tcp");
        else if (code == -2) snprintf(r->msg, sizeof(r->msg), "no-server dns");
        else                 snprintf(r->msg, sizeof(r->msg), "enroll http %d", code);
        return -1;
    }

    r->ok    = (json_get_int(g_resp, "ok") > 0);
    /* 同 cf_upload_pc：人数优先取 indexed，没有这个字段才退回 known */
    r->known = json_get_int(g_resp, "indexed");
    if (r->known < 0) r->known = json_get_int(g_resp, "known");
    if (r->known < 0) r->known = 0;
    if (!r->ok) {
        json_get_str(g_resp, "msg", r->msg, sizeof(r->msg));
        return -1;
    }
    snprintf(r->msg, sizeof(r->msg), "enrolled");
    return 0;
}

/* ============================================================
 * 四、讯飞后端
 * ============================================================ */

/* --- base64 / hmac / urlencode 工具 --- */
static int b64_enc(const unsigned char *src, int slen, char *dst, int dlen)
{
    size_t olen = 0;
    if (mbedtls_base64_encode((unsigned char *)dst, (size_t)dlen, &olen,
                              src, (size_t)slen) != 0)
        return -1;
    dst[olen] = '\0';
    return (int)olen;
}

static int hmac_sha256(const char *key, const char *data, unsigned char out[32])
{
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    mbedtls_md_context_t ctx;
    int rc;

    if (!info) return -1;
    mbedtls_md_init(&ctx);
    if (mbedtls_md_setup(&ctx, info, 1) != 0) { mbedtls_md_free(&ctx); return -1; }
    mbedtls_md_hmac_starts(&ctx, (const unsigned char *)key, strlen(key));
    mbedtls_md_hmac_update(&ctx, (const unsigned char *)data, strlen(data));
    rc = mbedtls_md_hmac_finish(&ctx, out);
    mbedtls_md_free(&ctx);
    return (rc == 0) ? 0 : -1;
}

static void url_encode(const char *src, char *dst, int dsize)
{
    static const char hex[] = "0123456789ABCDEF";
    int i, j = 0;

    for (i = 0; src[i] && j < dsize - 1; i++) {
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
    dst[j] = '\0';
}

static void make_date(char *out, int size)
{
    time_t t = (time_t)xf_time_now_unix();
    struct tm *gmt = gmtime(&t);

    if (!gmt) {
        strncpy(out, "Thu, 01 Jan 1970 00:00:00 GMT", (size_t)(size - 1));
        out[size - 1] = '\0';
        return;
    }
    strftime(out, (size_t)size, "%a, %d %b %Y %H:%M:%S GMT", gmt);
}

/* 讯飞 v2 鉴权：对 "host/date/request-line" 做 HMAC-SHA256，
 * 与语音听写那套完全一致，只有 request-line 从 GET 改成 POST。 */
static int xf_build_path(char *path, int size)
{
    char date[64];
    char sig_origin[192];
    char auth_origin[320];
    unsigned char mac[32];
    char mac_b64[64];
    char auth_b64[384];
    char auth_enc[1152];
    char date_enc[128];

    make_date(date, sizeof(date));

    snprintf(sig_origin, sizeof(sig_origin),
             "host: %s\ndate: %s\nPOST %s HTTP/1.1",
             XF_FACE_HOST, date, XF_FACE_URI);

    if (hmac_sha256(g_xf_api_secret, sig_origin, mac) != 0) return -1;
    if (b64_enc(mac, 32, mac_b64, (int)sizeof(mac_b64)) < 0) return -1;

    snprintf(auth_origin, sizeof(auth_origin),
             "api_key=\"%s\", algorithm=\"hmac-sha256\", "
             "headers=\"host date request-line\", signature=\"%s\"",
             g_xf_api_key, mac_b64);

    if (b64_enc((const unsigned char *)auth_origin, (int)strlen(auth_origin),
                auth_b64, (int)sizeof(auth_b64)) < 0) return -1;

    url_encode(auth_b64, auth_enc, (int)sizeof(auth_enc));
    url_encode(date, date_enc, (int)sizeof(date_enc));

    snprintf(path, (size_t)size, "%s?authorization=%s&host=%s&date=%s",
             XF_FACE_URI, auth_enc, XF_FACE_HOST, date_enc);
    return 0;
}

/* --- 流式 body：前缀 → 现场图 b64 → 中缀 → 参考图 b64 → 后缀 ---
 * 两张图都走"边读边编码"，所以 body 从来不会在内存里完整出现。 */
enum { BS_P1 = 0, BS_IMG, BS_P2, BS_REF, BS_P3, BS_DONE };
static struct {
    int st;
    int off;
} g_bs;

/* P1 段带 app_id，运行时拼一次、之后整块 copy 出去。
 * 固定段一律用 memcpy 而不是 snprintf —— 关键点：
 *   cf_http.c 每次给回调的 buflen = min(2064, 剩余 body 长度)，
 *   收到最后一段时 buflen **正好等于该段长度**（P3 只有 5 字节）。
 *   snprintf 需要留 '\0'，这种情况下会返回"本该写入的长度" == buflen，
 *   再判 n >= buflen 就变成了"每次请求都失败"。
 *   memcpy 不需要终止符，长度精确吻合。 */
static char g_bs_p1[384];
static int  g_bs_p1_len = 0;

/* 本次请求要比对的参考图（二进制） */
static const unsigned char *g_bs_ref = RT_NULL;
static int                  g_bs_ref_len = 0;

/* 图片分块 base64 的公共实现：输入切成 3 的倍数，输出 <= 4/3 倍。
 * ★ 块长必须是 3 的倍数，否则后续块的 base64 会整体错位、解不开。
 *   唯一的例外是最后一块，它可以非 3 倍数，由 mbedtls 自动补 '='
 *   —— 与整段一次性编码的结果完全一致。
 * consumed 回传"这次吃掉了多少输入字节"。不要在外面用输出长度反推输入长度：
 * 末块带 '=' 补位时反推会算错，偏移一旦偏了就解不出来。 */
static int b64_stream_block(const unsigned char *img, int len, int off,
                            char *buf, int buflen, int *consumed)
{
    int left = len - off;
    int max_in, in;
    size_t olen = 0;

    *consumed = 0;
    if (left <= 0) return 0;

    /* (buflen-1)：mbedtls 的 base64_encode 会写 olen 个字符 + 一个 '\0'，
     * 所以要保证 dlen >= olen+1。按 buflen 算的话，buflen 恰好是 4 的倍数时
     * 会要求写满整个缓冲再加终止符，直接返回 BUFFER_TOO_SMALL。 */
    max_in = ((buflen - 1) / 4) * 3;
    if (max_in < 3) return -1;
    in = MIN(left, MIN(CF_B64_IN, max_in));

    if (mbedtls_base64_encode((unsigned char *)buf, (size_t)buflen, &olen,
                              img + off, (size_t)in) != 0)
        return -1;
    *consumed = in;
    return (int)olen;
}

static int xf_body_read(void *user, char *buf, int buflen)
{
    (void)user;

    for (;;) {
        switch (g_bs.st) {
        case BS_P1:
            if (g_bs_p1_len <= 0 || buflen < g_bs_p1_len) return -1;
            memcpy(buf, g_bs_p1, (size_t)g_bs_p1_len);
            g_bs.st  = BS_IMG;
            g_bs.off = 0;
            return g_bs_p1_len;

        case BS_IMG: {
            int n, used = 0;
            if (g_frame_len - g_bs.off <= 0) { g_bs.st = BS_P2; g_bs.off = 0; continue; }
            n = b64_stream_block(g_jpeg, g_frame_len, g_bs.off, buf, buflen, &used);
            if (n <= 0) return -1;
            g_bs.off += used;
            return n;
        }

        case BS_P2: {
            int l = (int)sizeof(XF_P2) - 1;
            if (buflen < l) return -1;
            memcpy(buf, XF_P2, (size_t)l);
            g_bs.st  = BS_REF;
            g_bs.off = 0;
            return l;
        }

        case BS_REF: {
            int n, used = 0;
            if (g_bs_ref_len - g_bs.off <= 0) { g_bs.st = BS_P3; g_bs.off = 0; continue; }
            n = b64_stream_block(g_bs_ref, g_bs_ref_len, g_bs.off, buf, buflen, &used);
            if (n <= 0) return -1;
            g_bs.off += used;
            return n;
        }

        case BS_P3: {
            int l = (int)sizeof(XF_P3) - 1;
            if (buflen < l) return -1;
            memcpy(buf, XF_P3, (size_t)l);
            g_bs.st = BS_DONE;
            return l;
        }

        default:
            return 0;
        }
    }
}

/* 请求体精确长度：P1 段（运行时拼好的那块）+ base64(现场图) + 中缀 + base64(参考图) + 尾段 */
static int xf_body_len(int img_len, int ref_len)
{
    int b64_img = ((img_len + 2) / 3) * 4;
    int b64_ref = ((ref_len + 2) / 3) * 4;

    return g_bs_p1_len
         + b64_img
         + ((int)sizeof(XF_P2) - 1)
         + b64_ref
         + ((int)sizeof(XF_P3) - 1);
}

/* 1:1 比对一次：现场图 g_jpeg[0..len) vs 参考图 ref[0..ref_len) */
static int cf_xf_compare(int len, const unsigned char *ref, int ref_len,
                         cf_result_t *r)
{
    static char path[1536];
    cf_body_t body;
    int code;

    r->img_len = len;

    if (xf_build_path(path, (int)sizeof(path)) != 0) {
        snprintf(r->msg, sizeof(r->msg), "sign fail");
        r->ok = 0;
        return -1;
    }

    /* P1 段带 app_id，每次请求拼一次；拼不下就别发，否则 Content-Length 会算错 */
    g_bs_p1_len = snprintf(g_bs_p1, sizeof(g_bs_p1), "%s%s%s",
                           XF_P1_A, g_xf_appid, XF_P1_B);
    if (g_bs_p1_len <= 0 || g_bs_p1_len >= (int)sizeof(g_bs_p1)) {
        g_bs_p1_len = 0;
        snprintf(r->msg, sizeof(r->msg), "appid too long");
        r->ok = 0;
        return -1;
    }

    g_bs.st      = BS_P1;
    g_bs.off     = 0;
    g_bs_ref     = ref;
    g_bs_ref_len = ref_len;
    body.read = xf_body_read;
    body.user = RT_NULL;

    /* 第一个参数是"连到哪"，第二个是"SNI / Host 用哪个域名"。
     * 预置 IP 直连时两者必须不同：讯飞 LB 靠 SNI 选 vhost，
     * 把 IP 当 SNI 发过去会被直接 RST（ws_client.c TX#1 踩过）。 */
    code = cf_http_post(g_xf_server_ip[0] ? (const char *)g_xf_server_ip : XF_FACE_HOST,
                        XF_FACE_HOST, XF_FACE_PORT, 1, path,
                        "Content-Type: application/json\r\n",
                        xf_body_len(len, ref_len), &body, g_resp, sizeof(g_resp), 15000);
    r->http_code = code;

    if (code != 200) {
        if (code == -3)      snprintf(r->msg, sizeof(r->msg), "no-server tcp");
        else if (code == -4) snprintf(r->msg, sizeof(r->msg), "tls handshake");
        else if (code == -2) snprintf(r->msg, sizeof(r->msg), "no-server dns");
        else                 snprintf(r->msg, sizeof(r->msg), "http %d", code);
        CF_LOG("xf http fail, code=%d\n", code);
        /* 401/403 多半是签名时间不对，提示一下排查方向 */
        if (code == 401 || code == 403)
            rt_kprintf("[face] 鉴权失败：检查 APIKey/APISecret 与系统时间偏差(<300s)\n");
        return -1;
    }

    /* 响应结构：
     * {"header":{"code":0,...},"payload":{"face_compare_result":{"text":"<base64>"}}}
     * text 解码后才是 {"ret":0,"score":0.996} */
    {
        static char tb64[512];
        static unsigned char dec[256];
        size_t dlen = 0;
        int pm;

        json_get_str(g_resp, "text", tb64, (int)sizeof(tb64));
        if (!tb64[0]) {
            json_get_str(g_resp, "message", r->msg, sizeof(r->msg));
            r->ok = 0;
            return -1;
        }
        if (mbedtls_base64_decode(dec, sizeof(dec) - 1, &dlen,
                                  (const unsigned char *)tb64, strlen(tb64)) != 0) {
            snprintf(r->msg, sizeof(r->msg), "decode fail");
            r->ok = 0;
            return -1;
        }
        dec[dlen] = '\0';

        pm = json_get_permille((const char *)dec, "score");
        r->score   = (pm >= 0) ? ((float)pm / 1000.0f) : 0.0f;
        r->ok      = (json_get_int((const char *)dec, "ret") == 0);
        r->matched = r->ok && (pm >= 0) &&
                     ((float)pm / 1000.0f >= g_xf_threshold);

        if (!r->ok) snprintf(r->msg, sizeof(r->msg), "ret!=0");
        else if (!r->matched) snprintf(r->msg, sizeof(r->msg), "score %d.%03d",
                                       pm / 1000, pm % 1000);
        return r->ok ? 0 : -1;
    }
}

/* 讯飞没有人脸库能力，只能"一个人发一次请求"，然后取最高分。
 * 2 个人 = 2 次 HTTPS，所以默认只给 2 个槽：再多，单次识别就要十几秒。
 * 早停条件：分数已经 >= 0.90，继续比对另一个人也不可能翻盘。 */
static int cf_upload_xf(int len, cf_result_t *r)
{
    cf_result_t t;
    float best = -1.0f;
    int   best_i = -1;
    int   i, ok_any = 0, rc = -1;

    r->img_len = len;

    if (g_ref_cnt <= 0) {
        snprintf(r->msg, sizeof(r->msg), "no ref face");
        /* rt_kprintf 内部缓冲只有 128 字节，超长会**连换行一起静默丢弃**，
         * 所以中文提示一律拆成短行打印。 */
        rt_kprintf("[face] 还没有登记任何人\n");
        rt_kprintf("[face] 先 face enroll <名字>，或网页上点\"登记\"\n");
        r->ok = 0;
        return -1;
    }

    for (i = 0; i < g_ref_cnt; i++) {
        memset(&t, 0, sizeof(t));
        rc = cf_xf_compare(len, g_ref[i].jpg, g_ref[i].len, &t);

        if (rc == 0 && t.ok) {
            ok_any = 1;
            if (t.score > best) { best = t.score; best_i = i; }
            CF_LOG("vs '%s' -> %d/1000\n", g_ref[i].name, (int)(t.score * 1000 + 0.5f));
            if (t.matched && t.score >= 0.90f) {
                *r = t;
                cf_name_copy(r->name, g_ref[i].name);
                r->known = g_ref_cnt;
                return 0;
            }
        } else if (i == 0 || !ok_any) {
            *r = t;                     /* 保留失败原因，便于排障 */
        }
    }

    r->known = g_ref_cnt;

    if (best_i >= 0) {
        int pm = (int)(best * 1000.0f + 0.5f);
        r->ok      = 1;
        r->score   = best;
        r->matched = (best >= g_xf_threshold);
        cf_name_copy(r->name, r->matched ? g_ref[best_i].name : "");
        if (!r->matched)
            snprintf(r->msg, sizeof(r->msg), "best %d.%03d < %d",
                     pm / 1000, pm % 1000, (int)(g_xf_threshold * 1000.0f + 0.5f));
        else
            r->msg[0] = '\0';
        return 0;
    }

    return ok_any ? 0 : -1;
}

/* ============================================================
 * 五、任务执行与线程
 * ============================================================ */

/* 把抓帧失败的原因分开写清楚：只看一个负数很难判断该改哪里。 */
static void cf_grab_fail_msg(cf_result_t *r, int len)
{
    switch (len) {
    case -1: snprintf(r->msg, sizeof(r->msg), "sem missing"); break;
    case -2: snprintf(r->msg, sizeof(r->msg), "mjpeg not enabled"); break;
    case -3: snprintf(r->msg, sizeof(r->msg), "dmc subscribe failed"); break;
    case -4: snprintf(r->msg, sizeof(r->msg), "no frame in %d ms",
                      g_frame_timeout_ms); break;
    case -5: snprintf(r->msg, sizeof(r->msg), "empty frame"); break;
    case -6: snprintf(r->msg, sizeof(r->msg), "frame %d > %d",
                      (int)g_frame_raw, CF_JPEG_MAX); break;
    default: snprintf(r->msg, sizeof(r->msg), "grab %d", len); break;
    }
}

static void cf_print_result_line(void)
{
    /* 分数一律换算成千分数再用整数打印：newlib-nano 的 %f 支持不稳定，
     * 且 float 直接截断会把 0.996 打成 0.995。 */
    int pm = (int)(g_result.score * 1000.0f + 0.5f);
    if (pm < 0) pm = 0;

    /* 拆成两行：rt_kprintf 内部缓冲只有 128 字节，一行里塞 msg(48)+name(32)
     * 会超长，而超长是**整条连换行一起静默丢弃**——恰恰把最有用的信息丢了。 */
    rt_kprintf("[face] %s: ok=%d match=%d score=%d.%03d img=%d ms=%d ref=%d\n",
               (g_mode == CF_MODE_PC) ? "pc" : "xf",
               g_result.ok, g_result.matched, pm / 1000, pm % 1000,
               g_result.img_len, g_result.ms, g_result.known);

    if (g_result.msg[0] || g_result.name[0])
        rt_kprintf("       msg=%s name=%s\n",
                   g_result.msg[0] ? g_result.msg : "-",
                   g_result.name[0] ? g_result.name : "-");
}

static void cf_do_recog(void)
{
    int len;

    len = cf_grab(g_frame_timeout_ms);
    if (len <= 0) {
        cf_grab_fail_msg(&g_result, len);
        rt_kprintf("[face] grab frame failed, code=%d\n", len);
        return;
    }

    CF_LOG("got frame %d bytes, mode=%s\n", len, cf_mode_name());

    if (g_mode == CF_MODE_PC) (void)cf_upload_pc(len, &g_result);
    else                      (void)cf_upload_xf(len, &g_result);
}

static void cf_do_enroll(void)
{
    int len;

    len = cf_grab(g_frame_timeout_ms);
    if (len <= 0) {
        cf_grab_fail_msg(&g_result, len);
        rt_kprintf("[face] enroll %s failed: %s\n", g_job_name, g_result.msg);
        return;
    }

    g_result.img_len = len;
    if (g_mode == CF_MODE_PC) {
        (void)cf_enroll_pc(g_job_name, g_jpeg, len, &g_result);
    } else {
        int e = cf_ref_add(g_job_name, g_jpeg, len);

        if (e == 0) {
            g_result.ok    = 1;
            g_result.known = g_ref_cnt;
            snprintf(g_result.msg, sizeof(g_result.msg), "enrolled");
        } else {
            g_result.ok = 0;
            /* 必须带返回码：-1 图片参数不对、-2 超过 CF_REF_MAX(64KB)、
             * -3 池子状态异常、-4 登记表已满。
             * 只写 "ref add failed" 的话，网页上根本分不清是"照片太大"
             * 还是"槽位不够"，用户只会觉得"登记又失败了"。 */
            snprintf(g_result.msg, sizeof(g_result.msg), "ref add failed (%d)", e);
        }
    }
    cf_name_copy(g_result.name, g_job_name);
}

/* 网页上传照片的登记任务。
 *
 * 为什么必须放到这个线程而不是在 webd 里直接做：
 * PC 模式下 cf_enroll_pc() 是一次完整的 TCP 往返（连 192.168.137.1:5000、
 * 发 60KB、等服务端跑完 Haar 检测），而 webd 是单线程的阻塞式实现。
 * 直接在 webd 线程里发，整个网页服务会被堵住 —— 期间所有 /api/state
 * 轮询都排在后面，页面上就是"点了登记，界面卡好几秒才动"。
 * 放到这里，webd 收完图立刻回 OK:uploading，结果由 f_msg 轮询带回来。 */
static void cf_do_enroll_up(void)
{
    int slot = g_up_slot;
    int len  = g_up_len;

    if (slot < 0 || slot >= CF_REF_SLOTS || len <= 0) {
        snprintf(g_result.msg, sizeof(g_result.msg), "upload lost");
        g_up_slot = -1;
        g_up_len  = 0;
        return;
    }

    g_result.img_len = len;
    cf_name_copy(g_result.name, g_up_name);

    if (g_mode == CF_MODE_PC) {
        /* 图在 g_ref_pool[slot] 里，直接当请求体发出去（不再复制一份） */
        (void)cf_enroll_pc(g_up_name, g_ref_pool[slot], len, &g_result);
    } else {
        int e = cf_ref_add(g_up_name, g_ref_pool[slot], len);

        if (e == 0) {
            g_result.ok    = 1;
            g_result.known = g_ref_cnt;
            snprintf(g_result.msg, sizeof(g_result.msg), "enrolled");
        } else {
            g_result.ok = 0;
            snprintf(g_result.msg, sizeof(g_result.msg), "ref add failed (%d)", e);
        }
    }

    g_up_slot = -1;
    g_up_len  = 0;
}

static void cf_do_job(void)
{
    rt_tick_t t0 = rt_tick_get();

    memset(&g_result, 0, sizeof(g_result));
    g_result.ok        = 0;
    g_result.http_code = 0;
    g_result.ms        = 0;

    if (g_job_kind == JOB_ENROLL)         cf_do_enroll();
    else if (g_job_kind == JOB_ENROLL_UP) cf_do_enroll_up();
    else                                  cf_do_recog();

    g_result.ms = (int)((rt_tick_get() - t0) * 1000u / RT_TICK_PER_SECOND);
    g_result_tick = rt_tick_get();

    cf_print_result_line();
    if (g_result_cb) g_result_cb(&g_result);
}

static void cf_thread_entry(void *arg)
{
    (void)arg;

    for (;;) {
        /* 1s 超时只为心跳。原来用 RT_WAITING_FOREVER 的话，没人登记/识别时
         * 这个线程会一直睡，永远不上报，dog 线程 60s 后会把"正常待命"误判成
         * "卡死"然后停喂狗。信号量是计数型的，超时不影响作业语义：
         * 真来了作业，下一轮 take 立刻拿到。 */
        if (rt_sem_take(g_job_sem, rt_tick_from_millisecond(1000)) != RT_EOK) {
            dog_beat(DOG_FACE);
            continue;
        }
        dog_beat(DOG_FACE);
        cf_do_job();
        g_job_kind = JOB_NONE;
        g_busy = 0;
        (void)rt_sem_release(g_done_sem);
    }
}

/* 异步派发：busy 的判定和置位必须在同一处，避免 msh 与网页同时进来。
 *
 * up_owned=1 表示调用者自己正持有 g_up_slot（网页上传登记的交接口），
 * 这时"g_up_slot>=0"不是别人的半截上传，而是本次要处理的数据。 */
static int cf_submit_ex(int kind, const char *name, int up_owned)
{
    if (!g_thread) {
        if (cf_init() != 0) return -5;
    }

    /* ★ 看门狗（2026-10-27）。cf_face 线程正常会在任务结束时置 g_busy=0，
     * 但只要它卡在某次 TLS 握手或读响应里（换网络、DNS 半通、对端不回包），
     * 就永远走不到那一行，此后所有 face 操作都回 BUSY —— 现场看到的就是
     * "登记请求失败"，且重启前再也点不动。超时强行解锁。 */
    if (g_busy) {
        int el = (int)((rt_tick_get() - g_busy_tick) * 1000u / RT_TICK_PER_SECOND);
        if (el >= CF_BUSY_TIMEOUT_MS) {
            rt_kprintf("[face] busy stuck %d ms (>%d), force unlock\n",
                       el, CF_BUSY_TIMEOUT_MS);
            g_busy = 0;
        }
    }

    /* g_up_slot >= 0 表示 webd 正在往池子里流式收一张照片：
     * 它和识别共用 cf_http 的静态连接上下文，撞上会互相踩。 */
    if (g_busy || (g_up_slot >= 0 && !up_owned)) return -6;

    g_busy = 1;
    g_busy_tick = rt_tick_get();
    g_job_kind = kind;
    cf_name_copy(g_job_name, name ? name : "");
    (void)rt_sem_control(g_done_sem, RT_IPC_CMD_RESET, RT_NULL);
    if (rt_sem_release(g_job_sem) != RT_EOK) {
        g_busy = 0;
        return -3;
    }
    return 0;
}

static int cf_submit(int kind, const char *name)
{
    return cf_submit_ex(kind, name, 0);
}

/* ============================================================
 * 六、对外接口
 * ============================================================ */
int cf_init(void)
{
    if (g_thread) return 0;

    cf_ref_init();

    g_frame_sem = rt_sem_create("cf_frm", 0, RT_IPC_FLAG_FIFO);
    g_job_sem   = rt_sem_create("cf_job", 0, RT_IPC_FLAG_FIFO);
    g_done_sem  = rt_sem_create("cf_done", 0, RT_IPC_FLAG_FIFO);
    if (!g_frame_sem || !g_job_sem || !g_done_sem) {
        rt_kprintf("[face] sem create failed\n");
        /* 不能直接 return：g_thread 还是 NULL，下次 cf_submit 会再进来一次，
         * 已经建好的那几个信号量就永远漏掉了。 */
        if (g_frame_sem) { rt_sem_delete(g_frame_sem); g_frame_sem = RT_NULL; }
        if (g_job_sem)   { rt_sem_delete(g_job_sem);   g_job_sem   = RT_NULL; }
        if (g_done_sem)  { rt_sem_delete(g_done_sem);  g_done_sem  = RT_NULL; }
        return -1;
    }

    /* 16KB 栈：mbedtls 握手在握手阶段要占 6~10KB，base64/JSON 再叠一点。
     * 放在独立线程里跑，而不是借用 msh 线程 —— msh 默认栈只有 4KB，
     * 直接在里面发 HTTPS 必爆栈。 */
    g_thread = rt_thread_create("cf_face", cf_thread_entry, RT_NULL, 16 * 1024, 12, 10);
    if (!g_thread) {
        rt_kprintf("[face] thread create failed\n");
        if (g_frame_sem) { rt_sem_delete(g_frame_sem); g_frame_sem = RT_NULL; }
        if (g_job_sem)   { rt_sem_delete(g_job_sem);   g_job_sem   = RT_NULL; }
        if (g_done_sem)  { rt_sem_delete(g_done_sem);  g_done_sem  = RT_NULL; }
        return -2;
    }
    rt_thread_startup(g_thread);

    rt_kprintf("[face] cloud_face ready, mode=%s, ref=%d\n", cf_mode_name(), g_ref_cnt);
    if (g_mode == CF_MODE_XF && g_xf_server_ip[0] == '\0')
    {
        /* 现场网络封 UDP/53，DNS 解析不出 api.xf-yun.com 时，识别会报
         * "no-server tcp"（cf_http_post 返回 -3）。此时在 PC 上
         * ping api.xf-yun.com 取一个 IP，敲 face xfip <ip> 即可跳过 DNS。 */
        rt_kprintf("[face] xf: no preset ip, dns needed\n");
    }
    return 0;
}

void cf_deinit(void)
{
    if (g_thread) {
        rt_thread_delete(g_thread);
        g_thread = RT_NULL;
    }
    if (g_subscribed) {
        (void)dmc_unsubscribe("CLOUD_FACE", DMC_MEDIA_TYPE_MJPEG);
        g_subscribed = 0;
    }
    if (g_frame_sem) { rt_sem_delete(g_frame_sem); g_frame_sem = RT_NULL; }
    if (g_job_sem)   { rt_sem_delete(g_job_sem);   g_job_sem   = RT_NULL; }
    if (g_done_sem)  { rt_sem_delete(g_done_sem);  g_done_sem  = RT_NULL; }
}

/* 同步接口要等多久。
 * 讯飞模式是"一个人发一次请求"（15s 上限），2 个人就是 30s；
 * 原来写死 +20000 会在第 2 个人还没比完就先超时，然后把 g_busy 清掉，
 * 后面的任务就会叠上去。这里按人数算。 */
static int cf_job_timeout_ms(void)
{
    int per_req, n;

    if (g_mode == CF_MODE_XF) {
        per_req = 16000;
        n = (g_ref_cnt > 0) ? g_ref_cnt : 1;
    } else {
        per_req = 8000;
        n = 1;
    }

    return g_frame_timeout_ms + per_req * n + 3000;
}

int cf_run(int frame_timeout_ms, cf_result_t *out)
{
    int ret;

    g_frame_timeout_ms = (frame_timeout_ms > 0) ? frame_timeout_ms : 3000;

    ret = cf_submit(JOB_RECOG, RT_NULL);
    if (ret != 0) {
        if (ret == -6) rt_kprintf("[face] busy: previous job still running\n");
        return ret;
    }

    /* 等结果：抓帧超时 + 上传超时（PC 4s / 讯飞 15s x 人数）+ 余量 */
    if (rt_sem_take(g_done_sem, rt_tick_from_millisecond(cf_job_timeout_ms())) != RT_EOK) {
        rt_kprintf("[face] job timeout\n");
        g_busy = 0;
        return -4;
    }

    if (out) *out = g_result;
    return g_result.ok ? 0 : -5;
}

int cf_run_async(void)
{
    return cf_submit(JOB_RECOG, RT_NULL);
}

int cf_enroll(const char *name, int frame_timeout_ms)
{
    int ret;

    if (!name || !name[0]) return -8;
    g_frame_timeout_ms = (frame_timeout_ms > 0) ? frame_timeout_ms : 3000;

    ret = cf_submit(JOB_ENROLL, name);
    if (ret != 0) {
        if (ret == -6) rt_kprintf("[face] busy: previous job still running\n");
        return ret;
    }

    if (rt_sem_take(g_done_sem, rt_tick_from_millisecond(cf_job_timeout_ms())) != RT_EOK) {
        rt_kprintf("[face] enroll timeout\n");
        g_busy = 0;
        return -4;
    }
    return g_result.ok ? 0 : -5;
}

int cf_enroll_async(const char *name)
{
    if (!name || !name[0]) return -8;
    /*
     * 这里**不做槽位预检**：cf_ref_add() 在池子满时会轮转淘汰最老的那张
     * （见该函数注释），所以"槽满"从来不是这条路径的失败原因。
     * 真正会失败的是 cf_ref_add 内部的长度/参数校验，原因写在
     * g_result.msg 里（"ref add failed (n)"），网页按 msg 显示。
     */
    return cf_submit(JOB_ENROLL, name);
}

int cf_enroll_mem(const char *name, const unsigned char *jpg, int len)
{
    if (!name || !name[0] || !jpg || len <= 0) return -8;

    /* 识别任务正在按 g_ref[i].len 读参考图，这时改写池子会让它读到半张；
     * PC 模式更直接——两边会抢 cf_http.c 里那套静态连接上下文。
     * 正常路径挡得住（cf_submit / cf_enroll_begin 都看了 g_busy），
     * 这里是兜底。 */
    if (g_busy) return -6;

    if (g_mode == CF_MODE_PC) {
        cf_result_t r;
        memset(&r, 0, sizeof(r));
        if (cf_enroll_pc(name, jpg, len, &r) == 0) {
            g_result = r;
            cf_print_result_line();
            return 0;
        }
        g_result = r;
        return -5;
    }

    if (cf_ref_add(name, jpg, len) != 0) return -5;
    memset(&g_result, 0, sizeof(g_result));
    g_result.ok    = 1;
    g_result.known = g_ref_cnt;
    g_result.img_len = len;
    snprintf(g_result.msg, sizeof(g_result.msg), "enrolled");
    cf_name_copy(g_result.name, name);
    cf_print_result_line();     /* 和 PC 模式保持一致，串口上能看到这一笔 */
    return 0;
}

/* ---- 流式登记（webd 收照片用） ---- */

int cf_enroll_begin(const char *name)
{
    int slot;

    if (!name || !name[0]) return -8;
    /* 识别任务正在读 g_ref_pool，此时改写会读出半张图 */
    if (g_busy) return -6;
    if (g_up_slot >= 0) return -9;          /* 上一次没 commit/abort */

    slot = cf_pool_free_slot();
    if (slot < 0) {
        /* 池子满了：不淘汰正在用的人，让用户先 face del all
         * （上传路径无法预知这张图能不能用，盲目淘汰会把好数据冲掉） */
        return -7;
    }

    g_up_slot = slot;
    g_up_len  = 0;
    cf_name_copy(g_up_name, name);
    return 0;
}

int cf_enroll_feed(const unsigned char *p, int n)
{
    if (g_up_slot < 0) return -9;
    if (!p || n <= 0) return -8;
    if (g_up_len + n > CF_REF_MAX) return -1;

    memcpy(g_ref_pool[g_up_slot] + g_up_len, p, (size_t)n);
    g_up_len += n;
    return 0;
}

int cf_enroll_commit(void)
{
    int slot = g_up_slot;
    int len  = g_up_len;
    int rc;

    if (slot < 0) return -9;

    /* 先清掉上一轮的结论。否则这一步失败时，webd 会把*上一次*的 msg
     * 当成这次的原因报给网页（典型：这次其实是 BUSY，页面却显示
     * "照片里没脸"），比没有信息还误导。 */
    g_result.msg[0] = '\0';
    g_result.ok     = 0;

    if (len <= 0) { cf_enroll_abort(); return -8; }
    if (len < 4)  { cf_enroll_abort(); return -10; }

    /* JPEG 头尾各查两个字节，能挡掉"选错文件"这种最常见的手误，
     * 比让云端返回一句看不懂的 400 友好得多。 */
    if (!(g_ref_pool[slot][0] == 0xff && g_ref_pool[slot][1] == 0xd8))
    {
        cf_enroll_abort();
        return -10;
    }
    if (!(g_ref_pool[slot][len - 2] == 0xff && g_ref_pool[slot][len - 1] == 0xd9))
    {
        cf_enroll_abort();
        return -11;
    }

    /*
     * PC 模式：整条链路是一次跨网 HTTP 往返，而 webd 是单线程阻塞实现。
     * 在这里同步发，网页服务会被堵住整整一个往返（服务端跑 Haar 检测
     * 通常 100~200ms，PC 忙或网络抖动时会到秒级），页面上就是
     * "点了登记之后界面很久不动"。改成派发给人脸线程，
     * 立刻返回 1 = 已接收，真正的结果由下一次 /api/state 的 f_msg 给出。
     */
    if (g_mode == CF_MODE_PC)
    {
        rc = cf_submit_ex(JOB_ENROLL_UP, g_up_name, 1);
        if (rc != 0)
        {
            cf_enroll_abort();
            return (rc == -6) ? -6 : -5;
        }
        /* 注意：这里**不能**清 g_up_slot —— 人脸线程马上就要读它。
         * g_busy 已在本函数返回前置 1，别人插不进来，任务结束时由
         * cf_do_enroll_up() 负责释放。 */
        return 1;
    }

    /* 本地（讯飞）模式：只是往内存池里放一张图，微秒级，同步做就行 */
    g_up_slot = -1;
    g_up_len  = 0;

    rc = cf_enroll_mem(g_up_name, g_ref_pool[slot], len);
    if (rc != 0 && g_result.msg[0] == '\0')
    {
        /* 把失败原因留给 webd：它靠这条 msg 给网页一个能照着查的提示 */
        snprintf(g_result.msg, sizeof(g_result.msg), "ref add failed (%d)", rc);
        g_result.ok = 0;
    }
    return rc;
}

void cf_enroll_abort(void)
{
    g_up_slot = -1;
    g_up_len  = 0;
}

int cf_set_mode(int mode)
{
    if (mode != CF_MODE_PC && mode != CF_MODE_XF) return -1;
    g_mode = mode;
    rt_kprintf("[face] mode -> %s\n", cf_mode_name());
    return 0;
}

int cf_get_mode(void) { return g_mode; }

const char *cf_mode_name(void)
{
    return (g_mode == CF_MODE_PC) ? "pc" : "xunfei";
}

void cf_set_pc_target(const char *host, int port, const char *path)
{
    if (host) {
        strncpy(g_pc_host, host, sizeof(g_pc_host) - 1);
        g_pc_host[sizeof(g_pc_host) - 1] = '\0';
    }
    if (port > 0) g_pc_port = port;
    if (path) {
        strncpy(g_pc_path, path, sizeof(g_pc_path) - 1);
        g_pc_path[sizeof(g_pc_path) - 1] = '\0';
    }
    rt_kprintf("[face] pc target: %s:%d%s\n", g_pc_host, g_pc_port, g_pc_path);
}

void cf_set_pc_enroll_path(const char *path)
{
    if (!path || !path[0]) return;
    strncpy(g_pc_enroll_path, path, sizeof(g_pc_enroll_path) - 1);
    g_pc_enroll_path[sizeof(g_pc_enroll_path) - 1] = '\0';
}

void cf_set_xf_credential(const char *appid, const char *api_key, const char *api_secret)
{
    if (appid)      g_xf_appid      = appid;
    if (api_key)    g_xf_api_key    = api_key;
    if (api_secret) g_xf_api_secret = api_secret;
}

void cf_set_xf_server_ip(const char *ip)
{
    if (!ip || !ip[0]) {
        g_xf_server_ip[0] = '\0';
        rt_kprintf("[face] xf server ip cleared (use dns)\n");
        return;
    }
    strncpy(g_xf_server_ip, ip, sizeof(g_xf_server_ip) - 1);
    g_xf_server_ip[sizeof(g_xf_server_ip) - 1] = '\0';
    rt_kprintf("[face] xf server ip: %s (skip dns)\n", g_xf_server_ip);
}

void cf_set_xf_threshold(float th)
{
    if (th > 0.0f && th < 1.0f) {
        g_xf_threshold = th;
        rt_kprintf("[face] xf threshold -> %d/1000\n", (int)(th * 1000.0f));
    }
}

void cf_set_capture_channel(int grp_id, int chn_id)
{
    g_cap_grp = grp_id;
    g_cap_chn = chn_id;

    /* 换通道时把"最近一帧"作废：缓冲区里还留着上一路画面，
     * g_frame_tick 若还在 400ms 保鲜期内，第一次抓帧会直接复用它，
     * 于是"换了通道"看起来完全没生效。 */
    g_frame_len  = 0;
    g_frame_tick = 0;

    if (g_subscribed) {
        (void)dmc_unsubscribe("CLOUD_FACE", DMC_MEDIA_TYPE_MJPEG);
        g_subscribed  = 0;
        g_target_venc = -1;
    }
    rt_kprintf("[face] capture -> VPU[%d] CHN[%d]\n", grp_id, chn_id);
}

void cf_set_result_cb(cf_result_cb_t cb) { g_result_cb = cb; }

void cf_set_verbose(int on) { g_verbose = on ? 1 : 0; }

/* ============================================================
 * 七、msh 命令
 * ============================================================ */
static void face_print_result(const cf_result_t *r)
{
    int pm = (int)(r->score * 1000.0f + 0.5f);
    if (pm < 0) pm = 0;

    rt_kprintf("  ok      : %d\n", r->ok);
    rt_kprintf("  matched : %d\n", r->matched);
    rt_kprintf("  score   : %d.%03d\n", pm / 1000, pm % 1000);
    rt_kprintf("  img_len : %d bytes\n", r->img_len);
    rt_kprintf("  http    : %d\n", r->http_code);
    rt_kprintf("  elapsed : %d ms\n", r->ms);
    rt_kprintf("  refs    : %d\n", r->known);
    rt_kprintf("  msg     : %s\n", r->msg[0] ? r->msg : "-");
    if (r->name[0]) rt_kprintf("  name    : %s\n", r->name);
}

static void face_list_refs(void)
{
    int i;

    if (g_ref_cnt <= 0) {
        rt_kprintf("[face] no one enrolled yet\n");
        return;
    }
    for (i = 0; i < g_ref_cnt; i++) {
        rt_kprintf("  [%d] %-16s %6d bytes  %s\n", i, g_ref[i].name, g_ref[i].len,
                   (g_ref[i].slot < 0) ? "(built-in .rodata)" : "runtime");
    }
}

static void cmd_face(int argc, char **argv)
{
    /* 必须清零：cf_run 忙 / 超时时会在填 out 之前就返回，
     * 不清零的话下面 face_print_result 打出来的是一栈垃圾。 */
    cf_result_t r;

    memset(&r, 0, sizeof(r));

    if (argc < 2) {
        rt_kprintf("usage:\n");
        rt_kprintf("  face run [timeout_ms]   grab one frame and recognize\n");
        rt_kprintf("  face enroll <name>      enroll current frame as <name>\n");
        rt_kprintf("  face refs               list enrolled people\n");
        rt_kprintf("  face del <name|all>     delete runtime refs\n");
        rt_kprintf("  face grab               grab only (self-check)\n");
        rt_kprintf("  face mode [pc|xf]       show / switch backend\n");
        rt_kprintf("  face url <host> [port] [path]   PC target\n");
        rt_kprintf("  face xfip [ip]          xunfei preset ip ('' to clear)\n");
        rt_kprintf("  face th <0.50~0.99>     xunfei match threshold\n");
        rt_kprintf("  face src <grp> <chn>    capture channel (default 1 1)\n");
        rt_kprintf("  face stat               show current state\n");
        return;
    }

    if (!strcmp(argv[1], "run")) {
        int to = (argc >= 3) ? atoi(argv[2]) : 3000;
        rt_kprintf("[face] running (%s) ...\n", cf_mode_name());
        if (cf_run(to, &r) == 0) rt_kprintf("[face] success\n");
        else                     rt_kprintf("[face] failed\n");
        face_print_result(&r);

    } else if (!strcmp(argv[1], "enroll")) {
        if (argc < 3) {
            rt_kprintf("[face] usage: face enroll <name>\n");
            return;
        }
        rt_kprintf("[face] enrolling '%s' (%s) ...\n", argv[2], cf_mode_name());
        if (cf_enroll(argv[2], 3000) == 0) rt_kprintf("[face] enrolled ok\n");
        else                               rt_kprintf("[face] enroll failed\n");
        face_print_result(&g_result);

    } else if (!strcmp(argv[1], "refs")) {
        face_list_refs();

    } else if (!strcmp(argv[1], "del")) {
        if (argc < 3) {
            rt_kprintf("[face] usage: face del <name|all>\n");
            return;
        }
        if (!strcmp(argv[2], "all")) {
            int n = cf_ref_clear();
            if (n < 0) rt_kprintf("[face] busy, try again later\n");
            else       rt_kprintf("[face] removed %d ref(s)\n", n);
        } else {
            rt_kprintf("[face] 只能 face del all（运行时登记的都能清，"
                       "编译期底图靠重编）\n");
        }

    } else if (!strcmp(argv[1], "grab")) {
        int len = cf_grab_only(3000);
        if (len > 0) rt_kprintf("[face] grabbed %d bytes\n", len);
        else         rt_kprintf("[face] grab failed, code=%d\n", len);

    } else if (!strcmp(argv[1], "mode")) {
        if (argc < 3) {
            rt_kprintf("[face] mode = %s\n", cf_mode_name());
        } else if (!strcmp(argv[2], "pc")) {
            (void)cf_set_mode(CF_MODE_PC);
        } else if (!strcmp(argv[2], "xf")) {
            (void)cf_set_mode(CF_MODE_XF);
        } else {
            rt_kprintf("[face] unknown mode: %s\n", argv[2]);
        }

    } else if (!strcmp(argv[1], "url")) {
        if (argc < 3) {
            rt_kprintf("[face] usage: face url <host> [port] [path]\n");
            return;
        }
        cf_set_pc_target(argv[2],
                         (argc >= 4) ? atoi(argv[3]) : 5000,
                         (argc >= 5) ? argv[4] : "/api/face");

    } else if (!strcmp(argv[1], "xfip")) {
        cf_set_xf_server_ip((argc >= 3) ? argv[2] : RT_NULL);

    } else if (!strcmp(argv[1], "th")) {
        if (argc >= 3) {
            int v = atoi(argv[2]);
            cf_set_xf_threshold((float)v / 1000.0f);
        } else {
            rt_kprintf("[face] usage: face th 670   (means 0.670)\n");
        }

    } else if (!strcmp(argv[1], "src")) {
        if (argc >= 4) cf_set_capture_channel(atoi(argv[2]), atoi(argv[3]));
        else           rt_kprintf("[face] usage: face src <grp> <chn>\n");

    } else if (!strcmp(argv[1], "stat")) {
        /* rt_memory_info 的形参是 rt_uint32_t*，不是 rt_size_t*
         * （rt_size_t 是 unsigned long，类型不兼容在 -Werror 下直接报错） */
        rt_uint32_t total = 0, used = 0, max_used = 0;
        rt_memory_info(&total, &used, &max_used);
        rt_kprintf("mode      : %s\n", cf_mode_name());
        rt_kprintf("pc target : %s:%d%s  enroll%s\n",
                   g_pc_host, g_pc_port, g_pc_path, g_pc_enroll_path);
        rt_kprintf("capture   : VPU[%d] CHN[%d] (venc %d)\n",
                   g_cap_grp, g_cap_chn, g_target_venc);
        rt_kprintf("built-in  : %d bytes (.rodata)\n", cf_ref_jpg_len);
        rt_kprintf("enrolled  : %d / %d runtime slots\n", g_ref_cnt, 1 + CF_REF_SLOTS);
        rt_kprintf("buf       : jpeg %dKB + refs %dKB + resp 1KB = %dKB static\n",
                   CF_JPEG_MAX / 1024, (CF_REF_SLOTS * CF_REF_MAX) / 1024,
                   (CF_JPEG_MAX + CF_REF_SLOTS * CF_REF_MAX + 1024) / 1024);
        rt_kprintf("heap      : total %dKB, used %dKB, peak %dKB\n",
                   (int)(total / 1024), (int)(used / 1024), (int)(max_used / 1024));
        face_print_result(&g_result);

    } else {
        rt_kprintf("[face] unknown cmd: %s\n", argv[1]);
    }
}

/*
 * 包一层只为了返回 0。
 *
 * cmd_face 本身是 void，而 MSH 的命令表按 `int (*)(int,char**)` 调用它，
 * 于是 msh_exec() 拿到的是函数末尾寄存器里的**垃圾值**，webd 的 /api/cmd
 * 就把它翻成 `ERR:45` 这类莫名其妙的回复 —— 命令其实执行了，
 * 但网页上看起来像失败了。包一层显式 return 0，语义才对得上。
 */
static int cmd_face_entry(int argc, char **argv)
{
    cmd_face(argc, argv);
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_face_entry, face, cloud face recognition);
