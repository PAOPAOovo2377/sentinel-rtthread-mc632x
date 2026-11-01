#ifndef VOICE_CMD_H
#define VOICE_CMD_H

#include <rtthread.h>

/*
 * 语音控制线程
 *  - 触发方式：msh 命令 voice_test 释放信号量（阶段0 手动触发）
 *  - 后续可改为：NPU 检测到人形后自动触发，或按键/网络事件触发
 *  - 流程：取 PCM → xf_asr_recognize_once → 结果回调里做关键词匹配 → 执行命令
 */

/* 创建并启动语音控制任务，返回 0 成功 */
int voice_cmd_start(void);

/*
 * 只用编译时刻给 RTC 兜底一次，不建线程、不碰音频。
 *
 * 开机自启流程（startup/boot_auto.c）在跑 vlcview 之前就要有个像样的时钟，
 * 否则 webd 的相册时间戳、事件时间线全是 1970。voice_cmd_start() 内部本来
 * 也会调它（幂等），但那条路要等媒体管线起来才走到，太晚。
 *
 * 语义与 voice_cmd_start 里那次完全一样：RTC 已有可信时间就保留不动，
 * 只有明显无效（< 2025-01-01）才写编译时刻。
 */
int voice_cmd_time_bootstrap(void);

/* 最近一次识别文本（供 UI 在屏上显示最后一条指令） */
const char *voice_cmd_last_text(void);

/* 最近一次识别的失败原因，成功时为空串。
 * 网页 /api/state 的 asr_err 字段就读这个——没有它，识别失败在网页上
 * 与"根本没跑"无法区分，用户只能看到状态从"识别中"退回"待机"。 */
const char *voice_cmd_last_err(void);

/* 语音链路进度：0=空闲 1=等麦收敛 2=录音窗口已开 3=识别中
 * （2 里还分"等你开口"和"正在录"，用 voice_cmd_phase 区分） */
int voice_cmd_state(void);

/* 更细的阶段码，专供网页显示。与 voice_cmd_state 的区别：
 * state 是给"粗粒度状态机"用的（2 一档涵盖等待+录音），
 * phase 才是用户真正需要看到的四段：
 *   0 空闲 / 1 准备中（等麦收敛 3.5~4.5s）/ 2 请说话 / 3 正在录 / 4 识别中
 *                                                           5 上一轮失败
 *
 * ★ 为什么要单独一个函数而不是直接扩 state：
 *   "请说话"和"正在录"必须分开显示。以前合成一档，网页在录音已经跑了
 *   1.5 秒之后才显示"请说话"，用户一开口窗口只剩一半——这就是现场
 *   "显示时间和实际输入对不上"的直接原因。 */
#define VOICE_PH_IDLE    0
#define VOICE_PH_SETTLE  1
#define VOICE_PH_ARMED   2   /* 窗口已开，正在等你开口，此时说话一定录得全 */
#define VOICE_PH_REC     3   /* 已经检测到人声，正在计时 */
#define VOICE_PH_ASR     4   /* 录音结束，上传识别中 */
#define VOICE_PH_FAIL    5   /* 上一轮失败，原因见 voice_cmd_last_err() */

/*
 * 取当前阶段与录音进度。
 *   elapsed_ms —— 已录语音毫秒（仅 VOICE_PH_REC 有效，其余为 0）
 *   total_ms   —— 本轮目标的语音毫秒数（用来画进度条）
 * 返回上面的 VOICE_PH_*。
 *
 * elapsed_ms 由板端采集循环给出，不是网页自己估算的——
 * 前端一旦用自己的 setTimeout 倒计时，和采集时钟必然越走越偏。
 */
int voice_cmd_phase(int *elapsed_ms, int *total_ms);

#endif /* VOICE_CMD_H */
