/*
 * talk.c - 网页与板端之间的语音对讲（半双工）
 *
 * 结构：一条常驻线程 -> audio_capture_read() -> 环形缓冲 -> /api/mic
 *                                        └-> AO SendFrame <- /api/talkdown
 *
 * ★ 环形缓冲为什么要 2 倍长（384KB）
 *   网页要的是"最近 N 秒连续 PCM"，而环形缓冲在回绕点会断成两截。
 *   如果每次请求都拷贝到临时区，一次最多拷 96KB，webd 那 8KB 栈根本放不下。
 *   所以写入时同时写 [i] 和 [i + RING] 两份 —— 任意不超过 RING 的窗口
 *   都落在连续地址上，talk_pcm() 可以直接给出指针，零拷贝。
 *   代价是 .bss 多 192KB；本板堆有 20MB，这点内存换掉一次 memcpy 值得。
 *
 * ★ 与 ASR 互斥
 *   audio_capture 是全局单例（一份 g_ai_enabled），talk 常开时 voice_cmd
 *   再调 open 会直接返回 0 但读不到自己的帧。所以 talk_start 先看
 *   voice_cmd_state()，识别没结束就拒绝，让网页提示用户等两秒。
 */

#include <rtthread.h>
#include <string.h>

#include "voice_asr/audio_capture.h"
#include "voice_asr/voice_cmd.h"

#include "talk.h"
#include "dog.h"          /* 心跳上报：线程卡死由 dog 线程 + 硬件狗兜底 */

#define TALK_RATE     16000
#define TALK_BPS      2
#define TALK_SEC_MAX  6
#define TALK_RING     (TALK_RATE * TALK_BPS * TALK_SEC_MAX)   /* 192KB */

#define TALK_TICK2MS(t) ((int)(((rt_uint32_t)(t) * 1000u) / RT_TICK_PER_SECOND))

static uint8_t   g_ring[TALK_RING * 2];
static volatile int g_wr;        /* 0 .. TALK_RING-1 */
static volatile int g_total;     /* 累计写入字节，用于判断缓冲还没填满的情况 */
static volatile int g_run;
static volatile int g_dead;
static volatile int g_ready;
static volatile int g_level;
static rt_thread_t  g_tid = RT_NULL;

/* 线程栈只有 2KB，帧缓冲放静态 */
static uint8_t   g_frame[AUDIO_PERIOD_SAMPLES * 2];

/*
 * 下行队列（网页 -> 喇叭）。
 *
 * 32KB ≈ 1 秒。网页每 ~0.37 秒推 12KB，留 3 倍余量吸收网络抖动。
 * 读写各用一个指针，**不做 g_n += / -= 这种读改写** —— 那样写线程和
 * 播放线程会互相覆盖对方的计数，队列长度会漂，最后要么提前丢音、
 * 要么越界。单独维护 wr / rd，各自只改自己那一个，天然无锁。
 */
#define TALK_DQ_SIZE   (32 * 1024)
static uint8_t   g_dq[TALK_DQ_SIZE];
static volatile int g_dq_wr;
static volatile int g_dq_rd;
static volatile int g_dq_run;
static rt_thread_t  g_dq_tid = RT_NULL;
static uint8_t   g_dq_frame[AUDIO_PERIOD_SAMPLES * 2];

static void talk_entry(void *arg)
{
    int fail = 0;

    (void)arg;

    while (g_run)
    {
        int n;
        int i;

        /* 每读一帧（约 20ms）报一次活。audio_capture_read 本身按帧阻塞，
         * 一旦 ACODEC 卡住不再返回，这里就停跳，dog 线程 4s 后判定异常。 */
        dog_beat(DOG_TALK);

        n = audio_capture_read(g_frame, (int)sizeof(g_frame));

        if (n > 0)
        {
            int wr = g_wr;

            /* 双写：保证任意 6 秒窗口在地址上连续。
             * 注意 pos 必须先对 RING 取模再 +RING —— 直接写 wr+i+RING 在
             * wr 接近末尾时会越过 2*RING 的数组边界（一帧 2048 字节，
             * 越过 2000 多字节，砸到后面的静态变量上，症状极难查）。 */
            for (i = 0; i < n; i++)
            {
                uint8_t v = g_frame[i];
                int pos = wr + i;

                if (pos >= TALK_RING) pos -= TALK_RING;
                g_ring[pos]             = v;
                g_ring[pos + TALK_RING] = v;
            }

            wr += n;
            if (wr >= TALK_RING) wr -= TALK_RING;
            g_wr = wr;

            if (g_total < TALK_RING) g_total += n;

            /* 电平条：rms 实测人声 20~780、底噪 1~5，取 2000 作满刻度 */
            {
                rt_uint32_t rms = audio_rms(g_frame, n);
                int lv = (int)(rms / 20u);
                if (lv > 100) lv = 100;
                g_level = lv;
            }

            if (!g_ready)
            {
                g_ready = 1;
                rt_kprintf("[talk] mic ready\n");
            }

            fail = 0;
        }
        else if (n < 0)
        {
            /* 收敛期返回 0 是正常的（见 audio_capture.h），负值才是错。
             * 连续错 100 次（约 6 秒）就退出，避免线程空转把 CPU 占满。 */
            if (++fail > 100)
            {
                rt_kprintf("[talk] read fail x%d, stop\n", fail);
                break;
            }
            rt_thread_mdelay(20);
        }
        /* n == 0：直流伺服还在收敛，继续读就是了，不加延时（read 本身按帧阻塞） */
    }

    g_run  = 0;
    g_ready = 0;
    g_level = 0;
    g_dead = 1;
}

/* ---------------- 下行播放（独立线程，webd 不再被阻塞） ---------------- */

static int talk_dq_used(void)
{
    int u = g_dq_wr - g_dq_rd;

    if (u < 0) u += TALK_DQ_SIZE;
    return u;
}

/* 从队列里取最多 cap 字节。只有播放线程调用。 */
static int talk_pop_down(uint8_t *dst, int cap)
{
    int i;
    int n;
    int rd;

    if (g_dq_rd == g_dq_wr) return 0;

    n = g_dq_wr - g_dq_rd;
    if (n < 0) n += TALK_DQ_SIZE;
    if (n > cap) n = cap;

    rd = g_dq_rd;
    for (i = 0; i < n; i++)
    {
        if (rd >= TALK_DQ_SIZE) rd = 0;
        dst[i] = g_dq[rd++];
    }
    g_dq_rd = rd;

    return n;
}

/*
 * 播放线程。
 *
 * audio_capture_play_raw() 是按实时节奏睡的（每 2048B / 64ms），
 * 所以这条线程的大部分时间都在 mdelay —— 它天生适合承载这个阻塞。
 * webd 那边只做一次 memcpy 就返回，页面不再被语音卡住。
 */
static void talk_down_entry(void *arg)
{
    (void)arg;

    while (g_dq_run)
    {
        int n = talk_pop_down(g_dq_frame, (int)sizeof(g_dq_frame));

        if (n > 0)
        {
            audio_capture_play_raw(g_dq_frame, n);
        }
        else
        {
            /* 队列空：短睡。用 5ms 而不是 20ms，
             * 否则每块开头都会多出 20ms 的静音，连起来像"卡顿"。 */
            rt_thread_mdelay(5);
        }
    }

    g_dq_run = 0;
}

int talk_push_down(const unsigned char *pcm, int len)
{
    int i;
    int wr;
    int room;

    if (pcm == RT_NULL || len <= 0) return -1;
    if (!g_dq_run) return -3;

    /* 留一个字节的空位，保证 wr != rd 时能区分"满"和"空" */
    room = TALK_DQ_SIZE - 1 - talk_dq_used();
    if (len > room) len = room;
    if (len <= 0) return -2;    /* 满了：丢掉这一段，保持已经排队的连续 */

    wr = g_dq_wr;
    for (i = 0; i < len; i++)
    {
        if (wr >= TALK_DQ_SIZE) wr = 0;
        g_dq[wr++] = pcm[i];
    }
    g_dq_wr = wr;

    return len;
}

int talk_down_pending(void)
{
    if (!g_dq_run) return 0;
    return talk_dq_used();
}

int talk_start(void)
{
    int st;

    if (g_run) return -2;

    /* 识别在跑时不给开：抢同一条采集流，两边都会拿到半帧数据 */
    st = voice_cmd_state();
    if (st != 0)
    {
        rt_kprintf("[talk] voice_cmd busy (state=%d), refuse\n", st);
        return -6;
    }

    if (audio_capture_open() != 0)
    {
        rt_kprintf("[talk] audio open fail\n");
        return -1;
    }

    /* AO 要跟着 AI 一起使能，否则 LINE_OUT 没有输出（见 AUDIO_ENABLE_AO_BIAS） */
    memset((void *)g_ring, 0, sizeof(g_ring));
    g_wr    = 0;
    g_total = 0;
    g_ready = 0;
    g_level = 0;
    g_dead  = 0;
    g_run   = 1;

    /* 下行队列复位 */
    g_dq_rd  = 0;
    g_dq_wr  = 0;
    g_dq_run = 1;

    /* 栈给 4KB：audio_capture_read 内部有 FH_AC_AI_FRAME_S 这种带多路
     * 指针表的结构体压在栈上，2KB 太紧（voice_cmd 那边也是 4KB 起步）。 */
    g_tid = rt_thread_create("talk", talk_entry, RT_NULL, 4096, 12, 10);
    if (g_tid == RT_NULL)
    {
        g_run    = 0;
        g_dq_run = 0;
        audio_capture_close();
        return -1;
    }
    rt_thread_startup(g_tid);

    /*
     * 下行播放线程。优先级 13（比采集的 12 低一档）：
     * 采集丢帧会让上行断音，播放晚几毫秒没人听得出来。
     */
    g_dq_tid = rt_thread_create("tkdown", talk_down_entry, RT_NULL, 4096, 13, 10);
    if (g_dq_tid == RT_NULL)
    {
        /* 没有下行线程不影响上行（听现场照旧可用），所以只提示不失败 */
        g_dq_run = 0;
        rt_kprintf("[talk] downlink thread failed (只听不说)\n");
    }
    else
    {
        rt_thread_startup(g_dq_tid);
    }

    rt_kprintf("[talk] started (warmup ~5s)\n");
    return 0;
}

void talk_stop(void)
{
    int waited = 0;

    if (!g_run && g_dead) return;

    g_run = 0;

    /*
     * 下行线程要先停：它可能正卡在 audio_capture_play_raw 的 mdelay 里
     * （最长一个 2048 字节块 = 64ms），等它退干净再关 AO，
     * 否则会对着一个已经关掉的流发帧。
     */
    g_dq_run = 0;
    {
        int w = 0;

        /* 线程退出时会自己把 g_dq_run 清 0；这里等它清，最多 400ms */
        while (g_dq_run && w < 400)
        {
            rt_thread_mdelay(10);
            w += 10;
        }
        g_dq_tid = RT_NULL;
    }

    /* 线程可能正卡在 GetFrame 里（最长 AUDIO_FRAME_WAIT_MS = 260ms），
     * 等它自己退出再关音频，否则 close 之后线程还会去读一个已关的流。 */
    while (!g_dead && waited < 1000)
    {
        rt_thread_mdelay(20);
        waited += 20;
    }

    g_tid = RT_NULL;
    audio_capture_close();
    rt_kprintf("[talk] stopped (%dms)\n", waited);
}

int talk_active(void)
{
    return g_run ? 1 : 0;
}

int talk_ready(void)
{
    return g_ready ? 1 : 0;
}

int talk_level(void)
{
    return g_level;
}

int talk_pcm(int sec, const unsigned char **p)
{
    int want;
    int avail;
    int start;

    if (!p) return 0;

    if (sec < 1) sec = 1;
    if (sec > TALK_SEC_MAX) sec = TALK_SEC_MAX;

    avail = g_total;
    if (avail > TALK_RING) avail = TALK_RING;

    want = sec * TALK_RATE * TALK_BPS;
    if (want > avail) want = avail;
    if (want <= 0) return 0;

    /* 双写布局：从 wr-want 开始往后 want 字节一定连续 */
    start = g_wr - want;
    if (start < 0) start += TALK_RING;

    *p = (const unsigned char *)&g_ring[start];
    return want;
}

int talk_play(const unsigned char *pcm, int len)
{
    if (!pcm || len <= 0) return -1;
    if (!talk_active()) return -3;
    return audio_capture_play_raw((uint8_t *)pcm, len);
}
