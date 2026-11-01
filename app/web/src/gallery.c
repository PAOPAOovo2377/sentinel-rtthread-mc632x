/*
 * gallery.c - 证据相册 / 事件日志
 *
 * 见 gallery.h 里"为什么放 RAM""为什么定长"的说明。
 *
 * 线程模型（2026-10-29 改）：
 *   原来全部操作都在 webd 线程里跑，单线程、不加锁。
 *   现在多了一个 evi 工作线程（evi_post 入队 → 它去抓帧），
 *   因为 trip_report() 跑在 IVS 线程里，同步抓帧会把 NN 循环卡住
 *   一两秒（等 cf_busy + 等帧），期间整段越线过程被丢光。
 *   槽的写/删/清统一走 g_lock（临界区只有一次 96KB memcpy，约 0.45ms）。
 *   读侧（evi_item_at / evi_data）不加锁：最坏读到刚被覆盖的槽，
 *   和原来单线程下"浏览器还在下载、槽已被环形复用"是同一类问题。
 */

#include <rtthread.h>
#include <string.h>

#include "cloud_face.h"
#include "gallery.h"
#include "dog.h"

typedef struct
{
    unsigned char buf[EVI_JPEG_MAX];
    evi_item_t    it;
} evi_slot_t;

/* 50 槽 x 96KB ≈ 4.8MB .bss（见 gallery.h 的内存账）。掉电即失，重启后 id 从 1 重新开始。 */
static evi_slot_t g_slot[EVI_SLOTS];
static int        g_next_id = 1;   /* 单调递增，删除不复用 */
static int        g_cursor  = 0;   /* 下一个要写的槽（环形）*/
static int        g_count   = 0;   /* 有效张数 */
static int        g_inited  = 0;
static rt_mutex_t g_lock    = RT_NULL;
/*
 * cf_grab_only() 会把 cloud_face 的 g_want_frame / g_frame_sem 当成单一资源用，
 * 两个线程同时进去会互相把对方的帧"抢走"，结果是两边都超时。
 * 主路径（直接取新鲜帧）不碰它，但流没出图时 webd 手动抓拍和 evi 线程
 * 会同时落到这条路上，所以这里串行化。
 */
static rt_mutex_t g_grab_lock = RT_NULL;

/* ---------------- 自动抓拍队列（越线留证） ---------------- */
/* 深度 4 就够：trip_report 自身有 2 秒静默期，正常最多堆 1~2 个。
 * 满了覆盖最旧的 —— 连着触发时，"最近发生的"永远比"更早发生的"有用。 */
#define EVI_QUEUE 4

static int              g_q_type[EVI_QUEUE];
static int              g_q_head = 0;      /* 出队位置 */
static int              g_q_tail = 0;      /* 入队位置 */
static int              g_q_n    = 0;
static struct rt_semaphore g_q_sem;
static rt_thread_t      g_evi_thread = RT_NULL;
static volatile int     g_evi_run    = 0;
static volatile int     g_snap_ok    = 0;  /* 累计成功张数，`evi` 命令里看 */
static volatile int     g_snap_fail  = 0;

static void evi_worker_entry(void *param);

typedef struct
{
    unsigned up;
    char kind[10];
    char text[40];
} evt_rec_t;

static evt_rec_t g_evt[EVT_MAX];
static int       g_evt_cursor = 0;  /* 下一个写的位置 */
static int       g_evt_count  = 0;

/* ---------------- 事件日志 ---------------- */

void evt_add(const char *kind, const char *text)
{
    evt_rec_t *r;

    /* 两个写者：webd 线程（用户操作、删除、抓拍）和 IVS 线程（越线事件）。
     * 临界区里只有几十字节的 strncpy，关调度不到 10us，不用互斥量。 */
    rt_enter_critical();

    r = &g_evt[g_evt_cursor];
    r->up = (unsigned)(rt_tick_get() / RT_TICK_PER_SECOND);

    if (kind)
    {
        strncpy(r->kind, kind, sizeof(r->kind) - 1);
        r->kind[sizeof(r->kind) - 1] = '\0';
    }
    else
    {
        r->kind[0] = '\0';
    }

    if (text)
    {
        strncpy(r->text, text, sizeof(r->text) - 1);
        r->text[sizeof(r->text) - 1] = '\0';
    }
    else
    {
        r->text[0] = '\0';
    }

    g_evt_cursor = (g_evt_cursor + 1) % EVT_MAX;
    if (g_evt_count < EVT_MAX) g_evt_count++;

    rt_exit_critical();
}

int evt_count(void)
{
    return g_evt_count;
}

int evt_at(int idx, unsigned *up, char *kind, int kcap, char *text, int tcap)
{
    int pos;

    if (idx < 0 || idx >= g_evt_count) return -1;

    /* 最新一条在 cursor-1，往"旧"的方向倒着走。
     * 未写满时 cursor == count，所以 idx < count 时算出来的位置一定有效；
     * 写满后 count == EVT_MAX，环里全是有效记录。不需要再跳过空位。 */
    pos = (g_evt_cursor - 1 - idx) % EVT_MAX;
    if (pos < 0) pos += EVT_MAX;

    if (up) *up = g_evt[pos].up;
    if (kind && kcap > 0)
    {
        strncpy(kind, g_evt[pos].kind, (size_t)(kcap - 1));
        kind[kcap - 1] = '\0';
    }
    if (text && tcap > 0)
    {
        strncpy(text, g_evt[pos].text, (size_t)(tcap - 1));
        text[tcap - 1] = '\0';
    }
    return 0;
}

/* ---------------- JPEG 尺寸 ---------------- */

/* 只认 SOFn 段里的高/宽，解不出图像也没关系：网页上 <img> 自己会按原比例排。
 * 有了尺寸，相册格子才能先用 padding-bottom 占好位置，避免图陆续加载时页面跳动。 */
static void jpeg_dims(const unsigned char *p, int len, int *w, int *h)
{
    int i = 2;

    *w = 0;
    *h = 0;

    if (len < 4 || p[0] != 0xFF || p[1] != 0xD8) return;

    while (i + 9 < len)
    {
        unsigned m = ((unsigned)p[i] << 8) | (unsigned)p[i + 1];
        int seglen;

        if ((m & 0xFF00u) != 0xFF00u)
        {
            i++;
            continue;
        }

        /* 无长度字段的独立标记 */
        if (m == 0xFFD8 || m == 0xFFD9 || (m >= 0xFFD0 && m <= 0xFFD7) || m == 0xFF01)
        {
            i += 2;
            continue;
        }

        seglen = ((int)p[i + 2] << 8) | (int)p[i + 3];
        if (seglen < 2) return;

        if ((m >= 0xFFC0 && m <= 0xFFC3) ||
            (m >= 0xFFC5 && m <= 0xFFC7) ||
            (m >= 0xFFC9 && m <= 0xFFCB) ||
            (m >= 0xFFCD && m <= 0xFFCF))
        {
            *h = ((int)p[i + 5] << 8) | (int)p[i + 6];
            *w = ((int)p[i + 7] << 8) | (int)p[i + 8];
            return;
        }

        i += 2 + seglen;
    }
}

/* ---------------- 相册 ---------------- */

void evi_init(void)
{
    /* 先建锁再清数据：webd restart 会再调一次 evi_init，
     * 那时 evi 线程可能正在往槽里写，裸 memset 3.2MB 会踩坏它。 */
    if (g_lock == RT_NULL)
    {
        g_lock = rt_mutex_create("evi", RT_IPC_FLAG_PRIO);
    }
    if (g_grab_lock == RT_NULL)
    {
        g_grab_lock = rt_mutex_create("evig", RT_IPC_FLAG_PRIO);
    }

    if (g_lock) (void)rt_mutex_take(g_lock, RT_WAITING_FOREVER);

    memset(g_slot, 0, sizeof(g_slot));
    memset(g_evt,  0, sizeof(g_evt));
    g_next_id    = 1;
    g_cursor     = 0;
    g_count      = 0;
    g_evt_cursor = 0;
    g_evt_count  = 0;
    g_inited     = 1;

    if (g_lock) (void)rt_mutex_release(g_lock);

    /* 抓拍线程：只建一次。webd restart 反复调 evi_init 也只会清数据，
     * 不会再多出一个线程（多出来的线程会互相偷队列，抓拍张数对不上）。 */
    if (g_evi_thread == RT_NULL)
    {
        g_q_head = g_q_tail = g_q_n = 0;
        rt_sem_init(&g_q_sem, "evi_q", 0, RT_IPC_FLAG_FIFO);

        g_evi_run    = 1;
        /* 优先级 26：比 webd(25) 低一档。
         * 抓拍永远不着急到需要抢网页的 CPU —— 差几十毫秒也就是画面晚一点点。 */
        g_evi_thread = rt_thread_create("evi",
                                        evi_worker_entry, RT_NULL,
                                        6144, 26, 10);
        if (g_evi_thread != RT_NULL)
        {
            rt_thread_startup(g_evi_thread);
        }
        else
        {
            g_evi_run = 0;
            rt_kprintf("[evi] worker thread create failed (heap?)\n");
        }
    }
}

/*
 * 往队列里丢一个抓拍请求，立刻返回。
 *
 * ★ 这是"越线没抓到图"的核心修复。
 *   原来 trip_report() 同步调 evi_capture()：它要先轮询 cf_busy（最长 1.2s）
 *   再等一帧 JPEG（最长 0.8s，失败还重试一次）。整段跑在 IVS 线程里，
 *   一条越线事件能把 NN 循环卡住近 3 秒 —— 人早就走出画面了，
 *   后续帧全丢，于是"报了一次之后再走就毫无反应"。
 *   现在 IVS 线程只做一次入队（几微秒），抓帧交给 evi 线程。
 */
int evi_post(int type)
{
    if (!g_inited || g_evi_thread == RT_NULL) return -5;

    rt_enter_critical();
    if (g_q_n >= EVI_QUEUE)
    {
        /* 满了丢最旧的：连续触发时"刚发生的那次"才是用户要看的 */
        g_q_head = (g_q_head + 1) % EVI_QUEUE;
        g_q_n--;
    }
    g_q_type[g_q_tail] = type;
    g_q_tail = (g_q_tail + 1) % EVI_QUEUE;
    g_q_n++;
    rt_exit_critical();

    (void)rt_sem_release(&g_q_sem);
    return 0;
}

void evi_stats(int *ok, int *fail, int *pending)
{
    if (ok)      *ok      = g_snap_ok;
    if (fail)    *fail    = g_snap_fail;
    if (pending) *pending = g_q_n;
}

static void evi_worker_entry(void *param)
{
    (void)param;

    while (g_evi_run)
    {
        int type;

        /* sem 超时 500ms 一次，正好当作心跳节拍：即使一直没事件也在报活。
         * 若卡在 cf_grab（等帧/等人脸线程），这里停跳，8s 后判异常。 */
        dog_beat(DOG_EVI);

        if (rt_sem_take(&g_q_sem, rt_tick_from_millisecond(500)) != RT_EOK)
        {
            continue;
        }
        if (!g_evi_run) break;

        rt_enter_critical();
        type     = g_q_type[g_q_head];
        g_q_head = (g_q_head + 1) % EVI_QUEUE;
        if (g_q_n > 0) g_q_n--;
        rt_exit_critical();

        if (evi_capture(type) >= 0) g_snap_ok++;
        else                        g_snap_fail++;
    }
}

/*
 * 取最新一帧 JPEG 的长度（<0 = 没有）。
 *
 * 路径优先级：
 *   1. cloud_face 里刚编好的那一帧（age <= fresh_ms）—— 零等待。
 *      识别线程每抓一次都会更新它，所以正常永远是这条路径。
 *      **它不碰 cf_busy、不碰 vicap**，识别在忙也不影响留证。
 *   2. 没有新鲜帧才退回 cf_grab_only()，也就是让 cloud_face 帮我们等下一帧。
 */
static int evi_grab_frame(const unsigned char **jpg, int fresh_ms)
{
    int age;
    int len = 0;

    age = cf_last_jpeg_age_ms();
    if (age >= 0 && age <= fresh_ms)
    {
        *jpg = cf_last_jpeg(&len);
        if (*jpg && len > 0) return len;
    }

    return -1;
}

int evi_capture(int type)
{
    const unsigned char *jpg = RT_NULL;
    int len = 0;
    int slot;
    int rc;

    if (!g_inited) return -5;

    /*
     * 第一选择：直接拿云台（cloud_face）缓冲区里已经编好的帧。
     *
     * 300ms 的接受窗口对应 5fps MJPEG 的两帧多一点。绊线场景里人正贴着线走，
     * 两帧前的画面就是"越线那一刻"，完全满足留证需要；
     * 反过来，如果坚持"必须是刚刚这一瞬"，就得等下一帧，而等帧正是
     * 原来卡住 IVS 线程的那一段。宁可选 300ms 内的近帧，不选 2 秒后才能拿到的远帧。
     */
    len = evi_grab_frame(&jpg, 300);
    if (len < 0)
    {
        int waited = 0;

        /* 第二次：没有新鲜帧，说明 MJPEG 流没在出图（网页没开摄像头 / 通道没起），
         * 只能主动去要一帧。
         *
         * 等待上限从 1200ms 降到 400ms：
         *   - 抓拍线程独立了，这里等再久也不会堵住网页和 IVS；
         *   - 但等久了抓到的是"事后画面"，对留证没意义。
         *   快速失败 → 上层记一条无图事件，比拖两秒拿一张人已经走掉的图更有用。 */
        if (g_grab_lock) (void)rt_mutex_take(g_grab_lock, RT_WAITING_FOREVER);

        /* 等锁的这段时间里，别的线程可能刚抓到一帧，先看看能不能白拿 */
        len = evi_grab_frame(&jpg, 400);

        while (len < 0 && waited < 400 && cf_busy())
        {
            rt_thread_mdelay(40);
            waited += 40;
            len = evi_grab_frame(&jpg, 400);
        }

        if (len < 0 && !cf_busy())
        {
            rc = cf_grab_only(500);
            if (rc < 0)
            {
                /* 一次补救：常见于刚好被识别线程抽走一帧 */
                rt_thread_mdelay(80);
                rc = cf_grab_only(500);
            }
            if (rc >= 0) len = evi_grab_frame(&jpg, 1500);
        }

        if (g_grab_lock) (void)rt_mutex_release(g_grab_lock);

        if (len < 0)
        {
            /* 失败原因必须打出来：现场"越线没图"只可能是这两种，
             * 不看这一行就只能靠猜是不是模型没认出人。 */
            int e = cf_busy() ? -2 : -3;

            rt_kprintf("[evi] capture failed(%d): %s\n", e,
                       cf_busy() ? "face busy >400ms"
                                 : "no frame (vlcview/MJPEG 没起?)");
            return e;
        }
    }

    if (len > EVI_JPEG_MAX)
    {
        rt_kprintf("[evi] frame %d > EVI_JPEG_MAX %d, dropped\n",
                   len, EVI_JPEG_MAX);
        return -4;
    }

    /* ---- 写槽（临界区只包 memcpy，约 0.3ms） ---- */
    if (g_lock) (void)rt_mutex_take(g_lock, RT_WAITING_FOREVER);

    slot = g_cursor;

    memcpy(g_slot[slot].buf, jpg, (size_t)len);

    g_slot[slot].it.id    = g_next_id++;
    g_slot[slot].it.type  = type;
    g_slot[slot].it.size  = len;
    g_slot[slot].it.up    = (unsigned)(rt_tick_get() / RT_TICK_PER_SECOND);
    g_slot[slot].it.valid = 1;
    jpeg_dims(g_slot[slot].buf, len, &g_slot[slot].it.w, &g_slot[slot].it.h);

    g_cursor = (g_cursor + 1) % EVI_SLOTS;
    if (g_count < EVI_SLOTS) g_count++;

    if (g_lock) (void)rt_mutex_release(g_lock);

    rt_kprintf("[evi] capture id=%d type=%d %dx%d %d bytes\n",
               g_slot[slot].it.id, type,
               g_slot[slot].it.w, g_slot[slot].it.h, len);

    return slot;
}

int evi_count(void)
{
    return g_count;
}

int evi_item_at(int idx, evi_item_t *out)
{
    int i;
    int k;
    int best = -1;
    int last = 0x7FFFFFFF;

    if (!out || idx < 0 || idx >= g_count) return -1;

    /* 有删除之后槽位会有空洞，用 (cursor-1-idx) 算位置会算错，
     * 所以按 id 降序挑第 idx 个（id 单调递增，等于抓拍时间序）。
     * n = EVI_SLOTS = 50，单页最多 20 张 → 最坏 20 x 50 = 1000 次比较，
     * 微秒级，不值得为它引入排序（排序还要额外 200 字节栈）。 */
    for (k = 0; k <= idx; k++)
    {
        best = -1;
        for (i = 0; i < EVI_SLOTS; i++)
        {
            if (!g_slot[i].it.valid)             continue;
            if (g_slot[i].it.id >= last)         continue;
            if (best < 0 || g_slot[i].it.id > g_slot[best].it.id) best = i;
        }
        if (best < 0) return -1;
        last = g_slot[best].it.id;
    }

    *out = g_slot[best].it;
    return 0;
}

int evi_data(int id, const unsigned char **p, int *len)
{
    int i;

    if (id <= 0) return -1;

    for (i = 0; i < EVI_SLOTS; i++)
    {
        if (g_slot[i].it.valid && g_slot[i].it.id == id)
        {
            if (p)   *p   = g_slot[i].buf;
            if (len) *len = g_slot[i].it.size;
            return 0;
        }
    }
    return -1;
}

int evi_delete(int id)
{
    int i;
    int rc = -1;

    if (g_lock) (void)rt_mutex_take(g_lock, RT_WAITING_FOREVER);

    for (i = 0; i < EVI_SLOTS; i++)
    {
        if (g_slot[i].it.valid && g_slot[i].it.id == id)
        {
            /* 只清元数据，不搬 96KB：槽会随环形写入被覆盖。
             * 注意 g_cursor 不回退 —— 回退会让"下一张"写进刚删的槽，
             * 而显示顺序是按 id 排的，看起来会像删错了。 */
            g_slot[i].it.valid = 0;
            if (g_count > 0) g_count--;
            rt_kprintf("[evi] delete id=%d\n", id);
            rc = 0;
            break;
        }
    }

    if (g_lock) (void)rt_mutex_release(g_lock);
    return rc;
}

int evi_clear(void)
{
    int i;
    int n;

    if (!g_inited) return 0;

    if (g_lock) (void)rt_mutex_take(g_lock, RT_WAITING_FOREVER);

    n = g_count;

    /* 只清元数据，不 memset 3.2MB ——
     * 那一次要几毫秒到十几毫秒，webd 是单线程，期间整个页面停住；
     * 而且这些字节下一次 capture 的 memcpy 就会整段覆盖，没有清的必要时。
     * evi_data()/evi_item_at() 全部先看 it.valid，所以清完立刻就是"空相册"。 */
    for (i = 0; i < EVI_SLOTS; i++)
    {
        g_slot[i].it.valid = 0;
        g_slot[i].it.size  = 0;
    }

    /* 环写指针回到起点：全清之后没有"旧图要保护"，从头写最省。
     * id 水位（g_next_id）不回退 —— 网页按 id 排序显示，
     * 回退会让新图的 id 比刚删掉的还小，翻页和下载链接都会错乱。 */
    g_cursor = 0;
    g_count  = 0;

    if (g_lock) (void)rt_mutex_release(g_lock);

    rt_kprintf("[evi] clear all (%d item(s) dropped)\n", n);
    return n;
}
