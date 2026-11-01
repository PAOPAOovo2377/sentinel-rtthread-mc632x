/*
 * sysmon.h - 系统占用采样（CPU / 开机时长 / 版本串）
 *
 * CPU 占用没有现成接口可用：本 SDK 没开 RT_USING_CPU_USAGE，
 * 也没有 components/drivers/misc/cpu_usage.c。
 *
 * 采样原理（2026-10-30 重写，详见 sysmon.c 顶部注释）：
 *   不用空闲钩子数 tick —— 那种算法会把"tick 中断唤醒线程、线程干完活回 idle"
 *   的整格时间都记成空闲，脉冲式负载下会一路跌到 0~2%。
 *   改成在调度切换处按 read_pts()（1us 一跳）结算，微秒级分辨率、无量化台阶，
 *   并把"打断 idle 的中断时间"从空闲里扣掉，避免吃掉网卡/视频中断的开销。
 *
 * 注意：调度钩子全系统唯一，utils/profiling 的 `prof_view()` 会抢占它，
 *       之后 CPU 读数会冻结。msh 敲 `cpu res` 可重装，不必重启。
 *
 * msh 命令：
 *   cpu            总占用 + 窗口 + idle 里的中断占比
 *   cpu top [N]    每个线程占多少、每秒被切换几次、当时是不是 BLOCK
 *   cpu burn N     死循环 N 毫秒自检（验证统计是否跟得上）
 *   cpu res        重装钩子（被 prof_view 抢占后用）
 *
 * 为什么要有 cpu top：idle 90% 只能说"CPU 闲"，说不出"那 10% 在谁身上、
 * 其余时间在等谁"。有了每线程的 CPU% 和 ctx/s，就能一眼分出
 * 「真在算」（CPU 高、切换少）和「卡在等硬件」（CPU 低、切换少）。
 */
#ifndef __SYSMON_H__
#define __SYSMON_H__

/* 装钩子并起算。幂等；webd start 时调一次。 */
void sysmon_init(void);

/* 返回 0~100 的整数占用率（四舍五入）；样本不足一个窗口时返回 -1（网页显示 "—"）。 */
int sysmon_cpu(void);

/* 返回 0~1000 的占用率（0.1% 分辨率），-1 同 sysmon_cpu。
 * 网页只显示整数，这个给 msh 的 `cpu` 命令和排障用。 */
int sysmon_cpu_x10(void);

/* 开机秒数 */
unsigned sysmon_up(void);

/* RT-Thread 版本串，如 "3.1.3" */
const char *sysmon_os_ver(void);

/* 本固件版本串 */
const char *sysmon_fw_ver(void);

#endif /* __SYSMON_H__ */
