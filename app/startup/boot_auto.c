/*
 * boot_auto.c —— 开机自启：把手敲的那几条命令固化成代码
 *
 *   vlcview -p 1246              媒体管线 / MJPEG / DMC / 语音线程全在这里
 *   ifconfig e0 192.168.137.100  静态 IP（掩码与网关由 cmd_ifconfig 按同一规则推）
 *   dns 192.168.137.1
 *   date <可靠时间>              见下面"时间"一节
 *   webd start                   网页控制台
 *
 * 为什么不直接把命令字符串丢给 msh_exec：
 *   1) msh 要等 finsh 起来，时序不可控；
 *   2) 命令的失败只打印、没有返回值，自启流程没法据此重试。
 * 所以这里调的是命令背后的同一批 C 函数（cmd_ifconfig / set_dns），
 * 行为与手敲完全一致，但能拿到返回值、能重试、能打自己的日志。
 *
 * 唯一的例外是 `dhcpc stop e0`：它可选（失败也不影响固定 IP 生效），
 * 直接调 cmd_dhcpc() 会把 lwIP 的 DHCP 编译开关绑死在链接期，
 * 用 msh_exec 代劳，命令不存在时只是打印一句找不到，不会把固件编挂。
 */

#include <rtthread.h>
#include <time.h>
#include <string.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "vlcview/vlcview.h"    /* _vlcview() */
#include "voice_asr/voice_cmd.h" /* voice_cmd_time_bootstrap() */
#include "web/src/dog.h"        /* dog_start()：双层看门狗 */

/* config.h 是 .config 生成的那份宏。这里只为看 FH_APP_OPEN_IVS：
 * 关掉 IVS 时 media.mk 根本不编 sample_nna_ivs.c，绊线那两个符号不存在，
 * 不判断就会在链接期报 undefined reference。 */
#include "config.h"

/* ================== 可调参数 ================== */

#define BOOT_NETIF        "e0"
#define BOOT_IP           "192.168.137.100"
#define BOOT_DNS          "192.168.137.1"
#define BOOT_VLCVIEW_PORT 1246

/* 绊线/周界检测跑在 G1（NN 通道 G1CH2 是 512x288 RGB888）。
 * 只有 .config 里 FH_APP_OPEN_IVS=y 时这套代码才会被编进来；
 * 想彻底关掉就把 IVS 关掉，这里会自动跟着失效，不会链接报错。 */
#ifdef FH_APP_OPEN_IVS
#define BOOT_TRIP_ENABLE  1
#else
#define BOOT_TRIP_ENABLE  0
#endif
#define BOOT_TRIP_GRP     1

/* 网卡不一定在第 0 毫秒就 netif_add 完（tcpip 线程里做的），
 * 起不来就等：5 × 400ms = 2s，够用了。 */
#define BOOT_NET_RETRY    5
#define BOOT_NET_GAP_MS   400

/* ================== 外部依赖 ================== */

/* net/lwip-2.1.2/src/apps/utils/net_tools.c —— 就是 msh 的 ifconfig / dns
 * 命令背后的实现。app 层拿不到 lwip 头（SYS_INCDIR 里没有），
 * 所以按 voice_cmd.c 的老办法 extern，不引网络栈头路径。 */
extern int  cmd_ifconfig(int argc, char **argv);
extern void set_dns(int index, char *dns_server);

/* web/src/webd.c 里 webd_spawn() 的对外包装，等价于手敲 webd start */
extern int  webd_start_auto(void);

#if BOOT_TRIP_ENABLE
/* sample_demo/nna_app/src/sample_nna_ivs.c —— 人形检测 + 绊线判定。
 * 模型 persondet.nbg 不在板子上时线程会自己退出，不影响别的功能。 */
extern int  sample_fh_nna_ivs_start(int grp_id);
extern int  sample_fh_nna_ivs_stop(int grp_id);
#endif

/* shell/ 的 finsh 解释器，webd.c 的 /api/cmd 也在用 */
extern int  msh_exec(char *cmd, rt_size_t length);

/* ================== 网络 ================== */

/*
 * 探一下"现在有没有可用路由"。
 *
 * 用 sendto 而不是 connect：lwIP 的 udp_connect() 根本不查路由表
 * （只记 remote_ip 就返回 OK），拿它探路永远"成功"，等于没验。
 * sendto 走 ip4_route，netif 还是 0.0.0.0 时找不到出口，直接返回 -1。
 * 发一个字节到网关的 9 号端口(discard)，回执丢弃，无副作用。
 */
static int net_has_route(void)
{
    struct sockaddr_in dst;
    char poke = 0;
    int fd;
    int rc;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return 0;

    rt_memset(&dst, 0, sizeof(dst));
    dst.sin_family      = AF_INET;
    dst.sin_port        = htons(9);
    dst.sin_addr.s_addr = inet_addr(BOOT_DNS);

    rc = sendto(fd, &poke, 1, 0, (struct sockaddr *)&dst, sizeof(dst));
    close(fd);

    return (rc > 0) ? 1 : 0;
}

/*
 * 写死静态 IP。走 cmd_ifconfig(argc=3) 而不是自己拼 set_if()：
 * 掩码和网关是它按 IP 段推的（192.168.137.100 → /24、网关 .1），
 * 自己拼容易和 `ifconfig e0 <ip>` 的手敲结果不一致 —— 那是最难查的一类 bug。
 */
static int net_apply(void)
{
    /* 注意：cmd_ifconfig 的参数是 char*，不能传字符串字面量 */
    static char ifname[] = BOOT_NETIF;
    static char ipstr[]  = BOOT_IP;
    static char *argv[3] = { "ifconfig", ifname, ipstr };
    int i;

    /* netif_add 时会自动 dhcp_start()（net/lwip-2.1.2/src/arch/sys_arch.c）。
     * 不关掉的话，PC 侧（ICS）一旦发租约，下面写死的地址就会被顶掉，
     * 表现为"开机 20 秒内好好的，之后就 ping 不通了"。 */
    {
        char cmd[] = "dhcpc stop e0";
        msh_exec(cmd, sizeof(cmd) - 1);
    }

    for (i = 0; i < BOOT_NET_RETRY; i++)
    {
        cmd_ifconfig(3, argv);
        if (net_has_route())
        {
            rt_kprintf("[boot] net %s = %s ok\n", BOOT_NETIF, BOOT_IP);
            return 0;
        }
        rt_thread_mdelay(BOOT_NET_GAP_MS);
    }

    rt_kprintf("[boot] net setup FAILED (no route via %s)\n", BOOT_NETIF);
    return -1;
}

/* ================== 时间 ================== */

/*
 * 时间不在这里重新算一遍：voice_cmd.c 的 rtc_init_buildtime() 已经把
 * "RTC 有效就保留、无效才用编译时刻"这套判断踩完了（无条件写编译时刻
 * 会让复位后的板子倒退到烧录那天的同一秒，识别签名必然 401）。
 * 这里只是让它早点发生 —— vlcview 内部的 start_demo 也会调，但那条路
 * 要等媒体管线起来，太晚。
 */
static void time_bootstrap(void)
{
    time_t now;
    struct tm *tm;

    voice_cmd_time_bootstrap();

    now = time(RT_NULL);
    tm  = localtime(&now);
    if (tm)
    {
        rt_kprintf("[boot] time %04d-%02d-%02d %02d:%02d:%02d\n",
                   tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                   tm->tm_hour, tm->tm_min, tm->tm_sec);
    }
}

/* ================== 自启线程 ================== */

/* 这条线程里跑的东西都不重：cmd_ifconfig / set_dns / localtime / webd_spawn。
 * 唯一吃栈的是 webd_spawn 里的 img_tune_init()（ISP 配置结构体，几百字节）。
 * 给 6KB：堆有 20MB，不值得为省 2KB 冒爆栈的风险。 */
#define BOOT_STACK_SIZE  6144
#define BOOT_PRIO        15

static void boot_net_entry(void *arg)
{
    (void)arg;

    /* 让 ISP / DMC 那几个线程先把相位跑起来，再去动网络 */
    rt_thread_mdelay(200);

    net_apply();
    set_dns(0, (char *)BOOT_DNS);
    time_bootstrap();

    if (webd_start_auto() == 0)
    {
        rt_kprintf("[boot] webd on 8080\n");
        rt_kprintf("[boot] done -> http://%s:8080/\n", BOOT_IP);
    }
    else
    {
        rt_kprintf("[boot] webd start failed (msh: boot web)\n");
    }

    /*
     * 绊线放最后。
     * 它是唯一一个会长期吃掉 NN 算力和一条 VPU 通道的东西，
     * 万一把 MJPEG 挤掉帧，用户至少还能打开网页看画面 ——
     * 所以宁可它晚 100ms 起来，也不能排在 webd 前面。
     * 模型缺失时 nna_ivs 线程会打一行 model init fail 然后自己退出，
     * 这里不做重试，也不报错往上抛。
     */
#if BOOT_TRIP_ENABLE
    if (sample_fh_nna_ivs_start(BOOT_TRIP_GRP) == 0)
    {
        rt_kprintf("[boot] trip guard on G%d (model: ./models/persondet.nbg)\n",
                   BOOT_TRIP_GRP);
    }
    else
    {
        rt_kprintf("[boot] trip guard failed (msh: boot trip)\n");
    }
#endif
}

/*
 * 开机自启总入口，由 startup/demo_entry_rtt.c 的 user_main() 调用。
 *
 * ★ vlcview 必须在这个函数里同步跑完，不能扔进 boot_net_entry：
 *   它的调用链很深（dsp/vpu/isp/vicap 全在里面），而 init_drv 线程的栈
 *   是 SDK 按这个用途配的（原始代码里就有一行注释掉的 _vlcview(NULL,1240)）。
 *   自己另开线程容易爆栈，现象是随机跑飞、日志断在半句。
 *   另外这一步跑完，MJPEG/RTSP 的监听套接字已经 bind 好了，
 *   后面再配 IP 立刻就能连上。
 */
void boot_auto_launch(void)
{
    rt_thread_t tid;
    int rc;

    rt_kprintf("[boot] vlcview -p %d ...\n", BOOT_VLCVIEW_PORT);
    rc = _vlcview(RT_NULL, BOOT_VLCVIEW_PORT);
    if (rc != 0)
    {
        rt_kprintf("[boot] vlcview failed rc=%d (continue)\n", rc);
    }

    tid = rt_thread_create("bootnet", boot_net_entry, RT_NULL,
                           BOOT_STACK_SIZE, BOOT_PRIO, 10);
    if (tid == RT_NULL)
    {
        rt_kprintf("[boot] create bootnet failed, run inline\n");
        boot_net_entry(RT_NULL);   /* 起不了线程也别什么都不做 */

        /* 看门狗最后起（幂等）。武装前会等 15s，给媒体管线和人工敲命令留窗口；
         * 只监督"已上报过心跳"的线程，所以哪个线程没起来都不会误判。 */
        dog_start();
        return;
    }
    rt_thread_startup(tid);

    dog_start();
}

/* ================== 诊断命令 ================== */

/*
 * msh: boot              看当前固化的参数与时间
 *      boot net          只重做网络（换线/换 IP 段后不用重启）
 *      boot web          只拉起 webd（webd stop 之后补回来）
 *      boot trip [on|off] 绊线检测开关
 */
static int boot_cmd(int argc, char **argv)
{
    time_t now;
    struct tm *tm;

    if (argc >= 2)
    {
        if (strcmp(argv[1], "net") == 0)
        {
            net_apply();
            set_dns(0, (char *)BOOT_DNS);
            return 0;
        }
        if (strcmp(argv[1], "web") == 0)
        {
            return (webd_start_auto() == 0) ? 0 : -1;
        }
        if (strcmp(argv[1], "trip") == 0)
        {
#if BOOT_TRIP_ENABLE
            if (argc >= 3 && strcmp(argv[2], "off") == 0)
            {
                sample_fh_nna_ivs_stop(BOOT_TRIP_GRP);
                rt_kprintf("[boot] trip guard off\n");
                return 0;
            }
            if (sample_fh_nna_ivs_start(BOOT_TRIP_GRP) == 0)
            {
                rt_kprintf("[boot] trip guard on G%d\n", BOOT_TRIP_GRP);
                return 0;
            }
            rt_kprintf("[boot] trip guard failed\n");
            return -1;
#else
            rt_kprintf("[boot] 固件未启用 IVS（.config: FH_APP_OPEN_IVS）\n");
            return -1;
#endif
        }
        if (strcmp(argv[1], "time") == 0)
        {
            time_bootstrap();
            return 0;
        }
        rt_kprintf("usage: boot [net|web|trip|time]\n");
        return 0;
    }

    rt_kprintf("[boot] vlcview -p %d  (already running)\n", BOOT_VLCVIEW_PORT);
    rt_kprintf("[boot] ifconfig %s %s\n", BOOT_NETIF, BOOT_IP);
    rt_kprintf("[boot] dns %s\n", BOOT_DNS);
    rt_kprintf("[boot] webd start (port 8080)\n");
    rt_kprintf("[boot] trip guard G%d (%s)\n", BOOT_TRIP_GRP,
               BOOT_TRIP_ENABLE ? "auto" : "off");

    now = time(RT_NULL);
    tm  = localtime(&now);
    if (tm)
    {
        rt_kprintf("[boot] now %04d-%02d-%02d %02d:%02d:%02d\n",
                   tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                   tm->tm_hour, tm->tm_min, tm->tm_sec);
    }
    return 0;
}
/*
 * ★ 必须用 ALIAS：MSH_CMD_EXPORT(x, desc) 展开成 FINSH_FUNCTION_EXPORT_CMD(x, __cmd_x, desc)，
 *   里面会取 &x 的函数地址 —— 它要求"被注册的名字本身就是一个函数"。
 *   这里函数叫 boot_cmd，直接写 MSH_CMD_EXPORT(boot, ...) 会去取 &boot，
 *   编译器报 "'boot' undeclared here (not in a function)"，
 *   同时 boot_cmd 变成 unused-function，-Werror 下两处一起挂。
 *   ALIAS 的语义才是"函数 boot_cmd，命令名 boot"。
 */
MSH_CMD_EXPORT_ALIAS(boot_cmd, boot, boot [net|web|trip|time] : show/redo auto-boot steps);
