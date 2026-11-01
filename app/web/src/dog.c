/*
 * dog.c —— 双层看门狗实现
 *
 * 依赖：MC632X 的 WDT 由 mol_wdt 驱动封装成 RT-Thread 设备 "fh_wdt0"
 *       （platform/mc632x/board.c: plat_wdt -> fh_wdt_probe -> fh_wdt0），
 *       命令字在 lib/inc/bsp/wdt.h。
 *
 * 硬件行为：WDOG_RST_EN=1 / WDOG_IRQ_EN=0（drivers/wdt/mol_wdt.c:34-35），
 *           即超时**直接复位**，不产生中断。
 */
#include <rtthread.h>       /* 本 SDK 没有 rtdevice.h：rt_device_* 都在 rtthread.h 里 */
#include "bsp/wdt.h"
#include "dog.h"

#define DOG_DEV_NAME        "fh_wdt0"
#define DOG_HW_TIMEOUT_SEC  20          /* 硬件狗超时：留足一轮 SD 写/握手 */
#define DOG_PRIO            20          /* 低于全部业务线程，与 tshell 同档 */
#define DOG_STACK           2048
#define DOG_POLL_MS         1000
#define DOG_BOOT_DELAY_MS   15000       /* 开机 window：等媒体管线跑完 */

/*
 * 各线程的容忍间隔（毫秒）。定值依据是它们自己的节拍，不是拍脑袋：
 *   talk  20ms 一拍 -> 4s 等于 200 拍没动
 *   evi   500ms sem 超时 + 抓帧 400ms -> 8s 富余
 *   webd  accept 设了 1s 超时（webd.c:1864）-> 15s 富余
 *   face  HTTPS 上行大图可能几十秒 -> 60s
 *   nn    15fps，10s 等于 150 帧没出结果；给到 10s 是因为它在极端负载下
 *         可能被网络线程压住几秒，阈值太紧会把"慢"误判成"死"
 */
static const rt_uint32_t g_limit_ms[DOG_NUM] =
{
    4000, 8000, 15000, 60000, 10000
};

/*
 * 线程名必须与 rt_thread_create / prctl(PR_SET_NAME) 里的实际名字一致：
 *   前四个是 rt_thread_create 的第 1 个参数；
 *   NN 走 pthread，创建后又被 prctl 改成 "nna_ivs_0"（compat/linux/prctl.c:22
 *   直接写 thread->name），所以这里用改名后的。
 * 类型是 char* 而非 const char*：rt_thread_find() 的形参就是 char*。
 */
static char *g_name[DOG_NUM] =
{
    "talk", "evi", "webd", "cf_face", "nna_ivs_0"
};

static volatile rt_tick_t g_beat[DOG_NUM];
static volatile rt_uint8_t g_seen[DOG_NUM];   /* 该线程是否已上报过心跳 */
static rt_thread_t  g_tid;
static rt_device_t  g_wdt;
static volatile int g_run;                    /* 1 = 正在喂狗 */
static volatile int g_trip;                   /* 1 = 已判定异常，故意不喂 */
static volatile int g_reported;               /* 卡死只报一次，避免刷屏 */

static int hw_start(void)
{
    int sec = DOG_HW_TIMEOUT_SEC;

    g_wdt = rt_device_find(DOG_DEV_NAME);
    if (g_wdt == RT_NULL)
    {
        rt_kprintf("[dog] no %s, sw monitor only\n", DOG_DEV_NAME);
        return -1;
    }
    if (rt_device_open(g_wdt, RT_DEVICE_OFLAG_RDWR) != RT_EOK)
    {
        rt_kprintf("[dog] open %s failed\n", DOG_DEV_NAME);
        g_wdt = RT_NULL;
        return -2;
    }
    /* 注意：不要调 rt_device_close() —— 框架层 rt_watchdog_close 无条件打印
     *      "This watchdog can not be stoped" 并返回 -RT_ERROR，关不掉还刷日志。 */
    rt_device_control(g_wdt, RT_DEVICE_CTRL_WDT_SET_TIMEOUT, &sec);
    if (rt_device_control(g_wdt, RT_DEVICE_CTRL_WDT_START, RT_NULL) != RT_EOK)
    {
        rt_kprintf("[dog] %s start failed\n", DOG_DEV_NAME);
        g_wdt = RT_NULL;
        return -3;
    }
    return 0;
}

void dog_beat(dog_id_t id)
{
    if ((rt_uint32_t)id >= (rt_uint32_t)DOG_NUM)
    {
        return;
    }
    g_beat[id] = rt_tick_get();
    g_seen[id] = 1;
}

static void dog_entry(void *arg)
{
    int i, bad;
    rt_tick_t now, age, limit;

    (void)arg;

    rt_thread_mdelay(DOG_BOOT_DELAY_MS);

    if (hw_start() == 0)
    {
        rt_kprintf("[dog] hw wdt armed %ds, poll %dms\n",
                   DOG_HW_TIMEOUT_SEC, DOG_POLL_MS);
    }

    while (1)
    {
        if (!g_run)
        {
            rt_thread_mdelay(DOG_POLL_MS);
            continue;
        }

        now = rt_tick_get();
        bad = -1;

        for (i = 0; i < DOG_NUM; i++)
        {
            if (!g_seen[i])
            {
                continue;               /* 线程还没起来，不纳入监督 */
            }
            limit = rt_tick_from_millisecond((rt_int32_t)g_limit_ms[i]);
            age   = now - g_beat[i];
            if (age > limit)
            {
                /*
                 * ★ 关键：线程可能是被"正常关掉"的（talk_stop / webd stop /
                 *   boot trip off / cf_stop），心跳停了是应该的，不能当卡死。
                 *   所以只在疑似超时时才确认一次线程是否还存在——正常路径
                 *   完全不调 rt_thread_find（它会进临界区、遍历对象链表）。
                 *   线程退出时 rt_thread_exit 会把对象从链表摘掉，find 返回 NULL。
                 */
                if (rt_thread_find(g_name[i]) == RT_NULL)
                {
                    continue;
                }
                bad = i;
                break;
            }
        }

        if (bad >= 0)
        {
            if (!g_reported)
            {
                rt_kprintf("[dog] %s stuck %ds (>%ds), halt feeding\n",
                           g_name[bad],
                           (int)((now - g_beat[bad]) / RT_TICK_PER_SECOND),
                           (int)(g_limit_ms[bad] / 1000));
                g_reported = 1;
            }
            g_trip = 1;                 /* 不喂 => 硬件狗 20s 后复位 */
        }
        else
        {
            g_trip = 0;
            if (g_wdt != RT_NULL)
            {
                rt_device_control(g_wdt, RT_DEVICE_CTRL_WDT_KEEPALIVE, RT_NULL);
            }
        }

        rt_thread_mdelay(DOG_POLL_MS);
    }
}

int dog_start(void)
{
    int i;

    if (g_tid != RT_NULL)
    {
        return 0;
    }
    for (i = 0; i < DOG_NUM; i++)
    {
        g_beat[i] = 0;
        g_seen[i] = 0;
    }
    g_trip = 0;
    g_reported = 0;
    g_run = 1;

    g_tid = rt_thread_create("dog", dog_entry, RT_NULL,
                             DOG_STACK, DOG_PRIO, 10);
    if (g_tid == RT_NULL)
    {
        rt_kprintf("[dog] thread create failed\n");
        return -1;
    }
    rt_thread_startup(g_tid);
    return 0;
}

void dog_stop(void)
{
    g_run = 0;
    g_trip = 0;
    if (g_wdt != RT_NULL)
    {
        rt_device_control(g_wdt, RT_DEVICE_CTRL_WDT_STOP, RT_NULL);
        rt_kprintf("[dog] hw wdt stopped (debug)\n");
    }
}

int dog_resume(void)
{
    int sec = DOG_HW_TIMEOUT_SEC;

    g_trip = 0;
    g_reported = 0;
    g_run = 1;

    if (g_wdt == RT_NULL)
    {
        return hw_start();
    }
    rt_device_control(g_wdt, RT_DEVICE_CTRL_WDT_SET_TIMEOUT, &sec);
    rt_device_control(g_wdt, RT_DEVICE_CTRL_WDT_START, RT_NULL);
    return 0;
}

void dog_dump(void)
{
    int i;
    rt_tick_t now, age, limit;

    now = rt_tick_get();
    rt_kprintf("dog: hw=%s run=%d trip=%d\n",
               (g_wdt != RT_NULL) ? "on" : "off", (int)g_run, (int)g_trip);

    for (i = 0; i < DOG_NUM; i++)
    {
        if (!g_seen[i])
        {
            rt_kprintf("  %-7s not started\n", g_name[i]);
            continue;
        }
        limit = rt_tick_from_millisecond((rt_int32_t)g_limit_ms[i]);
        age   = now - g_beat[i];
        rt_kprintf("  %-7s age=%ds limit=%ds %s\n",
                   g_name[i],
                   (int)(age / RT_TICK_PER_SECOND),
                   (int)(g_limit_ms[i] / 1000),
                   (age > limit) ? "STUCK" : "ok");
    }
}

static int dog_cmd(int argc, char **argv)
{
    const char *a;

    a = (argc >= 2) ? argv[1] : RT_NULL;

    if (a == RT_NULL || rt_strcmp(a, "dump") == 0)
    {
        dog_dump();
        return 0;
    }
    if (rt_strcmp(a, "on") == 0)
    {
        rt_kprintf("[dog] resume rc=%d\n", dog_resume());
        return 0;
    }
    if (rt_strcmp(a, "off") == 0)
    {
        dog_stop();
        return 0;
    }
    rt_kprintf("usage: dog [on|off|dump]\n");
    return 0;
}
/*
 * ★ 必须用 ALIAS，不能用 MSH_CMD_EXPORT(dog, ...)：
 *   MSH_CMD_EXPORT(x, desc) 展开成 FINSH_FUNCTION_EXPORT_CMD(x, __cmd_x, desc)，
 *   里面取的是 &x 的函数地址 —— 它要求"被注册的字面名本身就是一个函数"。
 *   这里的函数叫 dog_cmd，直接写 MSH_CMD_EXPORT(dog, ...) 会去取 &dog，
 *   编译器报 "'dog' undeclared here (not in a function)"，
 *   同时 dog_cmd 变成未使用函数，-Werror 下两处一起挂。
 */
MSH_CMD_EXPORT_ALIAS(dog_cmd, dog, dog [on|off|dump] : hardware+thread watchdog);
