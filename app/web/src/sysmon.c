/*
 * sysmon.c - CPU 占用 / 开机时长 / 版本
 *
 * ── 2026-10-30 重写：原来的 tick 采样法系统性低估，能压到 0~2% ──
 *
 * 旧做法（已废弃）：在空闲钩子里数 tick —— 钩子每被调一次，发现 tick 变了
 * 就 +1，再看窗口内 idle_tick / 总 tick。
 *
 * 为什么错：RT-Thread 最常见的工作模式是
 *      tick 中断 → 唤醒线程 → 线程跑几毫秒 → 回到 idle → 空闲钩子被调用
 * 钩子被调用时 tick 早已变化，于是立刻记一次 idle，
 * **这一整格 10ms 全被算成空闲**，包括刚才跑的那几毫秒。
 * 只要负载是脉冲式的（NN 检测 5~15fps、网页请求、视频/NPU 中断），
 * 每一格都长这样，统计就一路向 0 收敛 —— 显示 2%，实际可能是 20%。
 * 而且 tick=100Hz、窗口 500ms 时最小台阶就是 2%，读数只能是 0/2/4。
 *
 * 新做法：在**调度切换**处结算 —— 谁运行过，就把那段时间记给谁。
 *   时钟源用 read_pts()（platform/common/timer.c，PAE_PTS_CLK = 1MHz，
 *   64 位，1 count = 1us），分辨率微秒级，没有量化台阶。
 *
 *   空闲时间 = idle 线程被结算到的总时长
 *              − 发生在 idle 上下文里的中断时间
 *   之所以要减：中断打断 idle 时，这段时间既在 idle 的结算里，
 *   又被中断钩子单独记了一次。不扣就会把网卡/视频/NPU 中断的开销
 *   全当成空闲吃掉 —— 而这恰恰是这类板子最主要的 CPU 开销。
 *
 * 三个钩子（本工程原本都没人用）：
 *   rt_scheduler_sethook       切换时把 (now - 上次切换) 结算给 from
 *   rt_interrupt_enter_sethook 进中断记时刻，并判断是否打断了 idle
 *   rt_interrupt_leave_sethook 出中断把这段累加到"idle 里的中断"
 *
 * ⚠ 调度钩子全系统只有一个。utils/profiling 的 prof_init() 也要占它，
 *   敲过 `prof_view()` 之后我们的钩子会被顶掉，CPU 数字就冻在最后一个值。
 *   敲 `cpu res` 可以重新装回来，不用重启。
 */

#include <rtthread.h>
#include <rthw.h>
#include <stdio.h>
#include <stdlib.h>

#include "sysmon.h"

/* platform/common/timer.c: uint64_t read_pts(void)，1us 一跳。
 * 不 include platform/include/timer.h 是为了少一条编译期路径依赖。 */
extern unsigned long long read_pts(void);

#define PTS_PER_MS   1000ULL

#define SENTINEL_FW_VER  "SENTINEL 1.0 (" __DATE__ ")"

/* 采样窗口下限。网页 2 秒轮询一次，正常窗口就是 2 秒；
 * 这个下限只防"连续快速调用"时拿 1~2ms 的样本算出 0% / 100%。 */
#define CPU_WIN_MIN_TICK   (RT_TICK_PER_SECOND / 5)

static char g_osver[24];

/* 全部是 pts 单位，同量纲相减，算比例不需要知道频率 */
static volatile unsigned long long g_idle_pts;      /* idle 线程被结算到的总时长（含其中中断）*/
static volatile unsigned long long g_irq_idle_pts;  /* 发生在 idle 上下文里的中断总时长 */
static unsigned long long g_sw_pts;                 /* 上一次调度切换的时刻 */
static unsigned long long g_irq_t0;                 /* 本次中断进入的时刻 */
static rt_uint8_t         g_irq_idle;               /* 本次中断是否打断了 idle */
static rt_uint8_t         g_irq_nest;               /* 中断嵌套深度 */

static rt_thread_t g_idle_tid;
static int         g_inited;

/* 上一窗口的旁证数据，只给 msh 的 cpu 命令看，不参与计算 */
static int         g_last_irq_x10 = 0;   /* 窗口内"打断 idle 的中断"占比 */
static int         g_last_win_ms  = 0;   /* 上一次统计窗口的实际长度 */

/* ------------------------------------------------------------------ */
/* 线程级账本（cpu top 用）                                            */
/*                                                                     */
/* 为什么需要它：idle 90% 只能说明"CPU 没事干"，回答不了"那 10% 花在   */
/* 谁身上、剩下的 90% 是在等谁"。把每一段运行时间记到具体线程上，再看  */
/* 每秒被切换次数，就能区分两种情况：                                  */
/*   CPU 高 + 切换少  → 真的在算，性能到顶                             */
/*   CPU 低 + 切换少  → 阻塞在硬件/信号量上，加主频也没用              */
/* ------------------------------------------------------------------ */

#define CPU_TOP_MAX   20            /* 最多记 20 个线程（本工程约 20 个）*/
#define CPU_TOP_ROWS  12            /* 一次打印几行 */

typedef struct
{
    rt_thread_t        tid;
    char               name[RT_NAME_MAX + 1];
    unsigned long long pts;         /* 累计运行时长（pts 单位 = us）*/
    unsigned long long sw;          /* 累计被切换次数 */
    unsigned long long last_pts;    /* 上次打印时的快照 */
    unsigned long long last_sw;
} cpu_top_t;

static cpu_top_t           g_top[CPU_TOP_MAX];
static int                 g_top_n;
static unsigned long long  g_top_other;      /* 表满时兜底，别丢时间 */
static unsigned long long  g_top_win_pts;    /* 上次打印的时刻 */
static unsigned long long  g_top_idle_last;  /* idle 累计快照 */
static unsigned long long  g_top_irq_last;   /* idle 内中断累计快照 */

static void cpu_top_account(rt_thread_t t, unsigned long long d)
{
    int i;
    int k;

    for (i = 0; i < g_top_n; i++)
    {
        if (g_top[i].tid == t)
        {
            g_top[i].pts += d;
            g_top[i].sw++;
            return;
        }
    }

    if (g_top_n >= CPU_TOP_MAX)
    {
        g_top_other += d;
        return;
    }

    /* 首次见到这个线程：登记。名字在切换时刻是有效的（from 还没被删）。 */
    g_top[g_top_n].tid      = t;
    g_top[g_top_n].pts      = d;
    g_top[g_top_n].sw       = 1;
    g_top[g_top_n].last_pts = 0;
    g_top[g_top_n].last_sw  = 0;

    for (k = 0; k < RT_NAME_MAX && t->name[k]; k++)
    {
        g_top[g_top_n].name[k] = t->name[k];
    }
    g_top[g_top_n].name[k] = '\0';

    g_top_n++;
}

/* ------------------------------------------------------------------ */

static void sysmon_sched_hook(struct rt_thread *from, struct rt_thread *to)
{
    unsigned long long now = read_pts();
    unsigned long long d   = now - g_sw_pts;

    /* from 就是"刚才在跑的那个线程"。整段都在它身上，无需切分。 */
    if (from != RT_NULL)
    {
        if (from == g_idle_tid)
        {
            g_idle_pts += d;
        }
        cpu_top_account(from, d);
    }

    g_sw_pts = now;
    (void)to;
}

/* 钩子在 rt_hw_interrupt_disable() 之后被调用，g_irq_nest 的更新天然原子 */
static void sysmon_irq_enter(void)
{
    if (g_irq_nest == 0)
    {
        g_irq_t0   = read_pts();
        g_irq_idle = (rt_thread_self() == g_idle_tid) ? 1 : 0;
    }
    if (g_irq_nest < 255) g_irq_nest++;
}

static void sysmon_irq_leave(void)
{
    if (g_irq_nest > 0) g_irq_nest--;

    if (g_irq_nest == 0 && g_irq_idle)
    {
        g_irq_idle_pts += read_pts() - g_irq_t0;
        g_irq_idle = 0;
    }
}

static void sysmon_attach(void)
{
    g_idle_tid = rt_thread_idle_gethandler();

    g_idle_pts     = 0;
    g_irq_idle_pts = 0;
    g_irq_nest     = 0;
    g_irq_idle     = 0;
    g_sw_pts       = read_pts();

    /* 线程账本一起清掉：基线时刻变了，旧累计值的分母不再成立 */
    g_top_n         = 0;
    g_top_other     = 0;
    g_top_win_pts   = 0;
    g_top_idle_last = 0;
    g_top_irq_last  = 0;

    rt_scheduler_sethook(sysmon_sched_hook);
    rt_interrupt_enter_sethook(sysmon_irq_enter);
    rt_interrupt_leave_sethook(sysmon_irq_leave);
}

void sysmon_init(void)
{
    if (g_inited) return;

    snprintf(g_osver, sizeof(g_osver), "RT-Thread %d.%d.%d",
             (int)RT_VERSION, (int)RT_SUBVERSION, (int)RT_REVISION);

    sysmon_attach();
    g_inited = 1;
}

/* ------------------------------------------------------------------ */

int sysmon_cpu_x10(void)
{
    static rt_tick_t          win_tick;
    static unsigned long long win_pts;
    static unsigned long long win_idle;
    static unsigned long long win_irq;
    static int                x10 = -1;

    rt_base_t          level;
    unsigned long long now;
    unsigned long long idle;
    unsigned long long irq;
    unsigned long long tot;
    unsigned long long didle;
    unsigned long long dirq;
    rt_tick_t          nowt;

    if (!g_inited) return -1;

    nowt = rt_tick_get();
    if (win_tick != 0 && (rt_tick_t)(nowt - win_tick) < CPU_WIN_MIN_TICK)
    {
        return x10;                 /* 样本太短，给上次的值，别抖 */
    }

    level = rt_hw_interrupt_disable();

    now  = read_pts();
    idle = g_idle_pts;
    irq  = g_irq_idle_pts;

    /* 当前正躺在 idle 里（没人占 CPU）：最后一次切换之后到现在这段
     * 还没被结算，得补上，否则长时间空闲会被算成"忙"。 */
    if (rt_thread_self() == g_idle_tid && now > g_sw_pts)
    {
        idle += now - g_sw_pts;
    }

    rt_hw_interrupt_enable(level);

    if (win_tick == 0)
    {
        win_tick = nowt;
        win_pts  = now;
        win_idle = idle;
        win_irq  = irq;
        return -1;                  /* 第一个窗口只建基线 */
    }

    tot = now - win_pts;
    if (tot == 0) return x10;

    /* 窗口内 idle 段总时长，扣掉其中的中断才是真空闲 */
    didle = (idle > win_idle) ? (idle - win_idle) : 0;
    dirq  = (irq  > win_irq)  ? (irq  - win_irq)  : 0;

    if (didle > dirq) didle -= dirq;
    else              didle  = 0;

    if (didle > tot) didle = tot;

    x10 = 1000 - (int)((didle * 1000ULL) / tot);
    if (x10 < 0)    x10 = 0;
    if (x10 > 1000) x10 = 1000;

    g_last_irq_x10 = (int)((dirq * 1000ULL) / tot);
    g_last_win_ms  = (int)(((rt_tick_t)(nowt - win_tick) * 1000) / RT_TICK_PER_SECOND);

    win_tick = nowt;
    win_pts  = now;
    win_idle = idle;
    win_irq  = irq;

    return x10;
}

int sysmon_cpu(void)
{
    int x = sysmon_cpu_x10();

    if (x < 0) return -1;
    return (x + 5) / 10;            /* 四舍五入，别把 2.9% 抹成 2% */
}

unsigned sysmon_up(void)
{
    return (unsigned)(rt_tick_get() / RT_TICK_PER_SECOND);
}

const char *sysmon_os_ver(void)
{
    return g_osver[0] ? g_osver : "RT-Thread";
}

const char *sysmon_fw_ver(void)
{
    return SENTINEL_FW_VER;
}

/* ------------------------------------------------------------------ */
/* msh: cpu [res|burn N|top [N]]                                      */
/* ------------------------------------------------------------------ */

static const char *stat_txt(int s)
{
    switch (s)
    {
    case RT_THREAD_INIT:    return "INIT";
    case RT_THREAD_READY:   return "READY";
    case RT_THREAD_SUSPEND: return "BLOCK";
    case RT_THREAD_RUNNING: return "RUN";
    case RT_THREAD_CLOSE:   return "CLOSE";
    default:                return "-";
    }
}

/* 线程可能已经退出（cf_face / nna_ivs 都是 pthread，能起来也能退）。
 * 直接解引用旧指针会读到已经还给堆的栈内存，所以打印前先在对象链表里
 * 确认它还活着；找不到就只显示名字和 CPU，不碰它的字段。 */
static int cpu_top_stat(rt_thread_t t)
{
    struct rt_object_information *info;
    rt_list_t *n;

    info = rt_object_get_information(RT_Object_Class_Thread);
    if (info == RT_NULL) return -1;

    for (n = info->object_list.next; n != &info->object_list; n = n->next)
    {
        if (rt_list_entry(n, struct rt_thread, list) == t)
        {
            return (int)(t->stat & RT_THREAD_STAT_MASK);
        }
    }

    return -1;
}

static void sysmon_top_dump(int rows)
{
    static cpu_top_t snap[CPU_TOP_MAX];
    static int       pct[CPU_TOP_MAX];
    static int       rate[CPU_TOP_MAX];
    static int       order[CPU_TOP_MAX];

    unsigned long long now;
    unsigned long long win;
    unsigned long long didle;
    unsigned long long dirq;
    unsigned long long dother;
    unsigned long long d;
    rt_base_t level;
    int n;
    int i;
    int j;
    int t;

    if (!g_inited)
    {
        rt_kprintf("[cpu] not ready\n");
        return;
    }
    if (rows <= 0 || rows > CPU_TOP_MAX) rows = CPU_TOP_ROWS;

    level = rt_hw_interrupt_disable();
    now   = read_pts();

    if (g_top_win_pts == 0)
    {
        /* 第一次：只建基线，否则分母是"从开机到现在"，看不出瞬时占用 */
        g_top_win_pts   = now;
        g_top_idle_last = g_idle_pts;
        g_top_irq_last  = g_irq_idle_pts;
        g_top_other     = 0;
        for (i = 0; i < g_top_n; i++)
        {
            g_top[i].last_pts = g_top[i].pts;
            g_top[i].last_sw  = g_top[i].sw;
        }
        rt_hw_interrupt_enable(level);
        rt_kprintf("[cpu] baseline set, run 'cpu top' again in 1s\n");
        return;
    }

    win = now - g_top_win_pts;
    if (win < 200000ULL)                /* 200ms */
    {
        rt_hw_interrupt_enable(level);
        rt_kprintf("[cpu] window %dms too short, try again\n", (int)(win / 1000));
        return;
    }

    n = g_top_n;
    for (i = 0; i < n; i++)
    {
        snap[i] = g_top[i];             /* 锁内整块拷，避免 64 位值被切半 */
        d       = g_top[i].pts - g_top[i].last_pts;

        pct[i]  = (int)((d * 1000ULL) / win);
        rate[i] = (int)(((g_top[i].sw - g_top[i].last_sw) * 1000000ULL) / win);

        g_top[i].last_pts = g_top[i].pts;
        g_top[i].last_sw  = g_top[i].sw;
    }

    didle  = g_idle_pts - g_top_idle_last;
    dirq   = g_irq_idle_pts - g_top_irq_last;
    dother = g_top_other;

    g_top_idle_last = g_idle_pts;
    g_top_irq_last  = g_irq_idle_pts;
    g_top_other     = 0;
    g_top_win_pts   = now;
    rt_hw_interrupt_enable(level);

    if (didle > dirq) didle -= dirq;
    else              didle  = 0;
    if (didle > win)  didle  = win;

    for (i = 0; i < n; i++) order[i] = i;
    for (i = 0; i < n; i++)
    {
        for (j = i + 1; j < n; j++)
        {
            if (pct[order[j]] > pct[order[i]])
            {
                t        = order[i];
                order[i] = order[j];
                order[j] = t;
            }
        }
    }

    rt_kprintf("[cpu] win=%dms idle=%d.%d%% other=%d.%d%%\n",
               (int)(win / 1000),
               (int)(didle * 1000ULL / win) / 10,
               (int)(didle * 1000ULL / win) % 10,
               (int)(dother * 1000ULL / win) / 10,
               (int)(dother * 1000ULL / win) % 10);
    rt_kprintf("  %-14s %7s %7s  %s\n", "THREAD", "CPU", "ctx/s", "STAT");

    for (i = 0; i < n && i < rows; i++)
    {
        int k = order[i];

        rt_kprintf("  %-14s %3d.%d%% %7d  %s\n",
                   snap[k].name,
                   pct[k] / 10,
                   pct[k] % 10,
                   rate[k],
                   stat_txt(cpu_top_stat(snap[k].tid)));
    }

    rt_kprintf("[cpu] idle high + ctx/s low = blocked on hw, not CPU-bound\n");
}

static void sysmon_burn(int ms)
{
    rt_tick_t t0;
    rt_tick_t dur;
    volatile unsigned long long acc = 0;

    if (ms <= 0) ms = 1000;

    dur = (rt_tick_t)(((long)ms * RT_TICK_PER_SECOND) / 1000);
    if (dur == 0) dur = 1;

    t0 = rt_tick_get();
    while ((rt_tick_t)(rt_tick_get() - t0) < dur)
    {
        acc += read_pts();
    }
    (void)acc;
}

static int sysmon_cmd(int argc, char **argv)
{
    const char *sub = (argc >= 2) ? argv[1] : RT_NULL;

    if (sub && !rt_strcmp(sub, "res"))
    {
        sysmon_attach();
        rt_kprintf("[cpu] hook re-attached, baseline reset\n");
        return 0;
    }

    if (sub && !rt_strcmp(sub, "burn"))
    {
        int ms = (argc >= 3) ? atoi(argv[2]) : 2000;

        rt_kprintf("[cpu] burning %d ms, watch the web page now\n", ms);
        sysmon_burn(ms);
        {
            int x = sysmon_cpu_x10();

            if (x < 0) x = 0;
            rt_kprintf("[cpu] burn done, now %d.%d%%\n", x / 10, x % 10);
        }
        return 0;
    }

    if (sub && !rt_strcmp(sub, "top"))
    {
        int rows = (argc >= 3) ? atoi(argv[2]) : 0;

        sysmon_top_dump(rows);
        return 0;
    }

    {
        int x = sysmon_cpu_x10();

        if (x < 0)
        {
            rt_kprintf("[cpu] warming up (need 200ms+ sample)\n");
        }
        else
        {
            /* 两行，避开 rt_kprintf 128 字节的静默截断。
             * 百分比用整数十分位手拼：rt_kprintf 走 rt_vsnprintf，
             * switch 里没有 'f'，写 %.1f 会**什么都不打印**。 */
            rt_kprintf("[cpu] %d.%d%% busy | %d.%d%% idle\n",
                       x / 10, x % 10, (1000 - x) / 10, (1000 - x) % 10);
            rt_kprintf("[cpu] win=%dms irq-in-idle=%d.%d%% nest=%d\n",
                       g_last_win_ms,
                       g_last_irq_x10 / 10, g_last_irq_x10 % 10,
                       (int)g_irq_nest);
        }
        rt_kprintf("[cpu] top [N] -> per-thread breakdown\n");
        rt_kprintf("[cpu] burn N -> selftest | res -> re-hook\n");
    }

    return 0;
}

MSH_CMD_EXPORT_ALIAS(sysmon_cmd, cpu, "cpu [top [N]|burn N|res] : CPU usage");
