#ifndef AUDIO_CAPTURE_H
#define AUDIO_CAPTURE_H

#include <rtthread.h>
#include "dsp/fh_audio_mpi.h"   /* FH_UINT32 / FH_AC_AI_* —— 必须是 SDK 的类型 */

/*
 * 音频采集抽象层
 * 目标格式：16k / 16bit / 单声道 PCM（与讯飞 iat 一致）
 *
 * 两种模式（编译期宏选择，缺省即麦克风）：
 *   1) 定义 AUDIO_SRC_MIC    —— 板载麦克风实时采集（MC632X 内部 ACODEC）
 *   2) 定义 PCM_USE_EMBEDDED —— 用 pcm_test_data.c 里的预录音频（离线调试）
 *
 * 麦克风实现要点（参照 app/audio_demo/sample_audio）：
 *   FH_AC_Init → FH_AC_Set_Config(16k/16bit/LPCM/mic0) → FH_AC_AI_Enable
 *   → 循环 FH_AC_AI_GetFrame() 取 period_size*2 字节 → FH_AC_AI_Disable
 */

#ifndef AUDIO_SRC_MIC
#ifndef PCM_USE_EMBEDDED
#define AUDIO_SRC_MIC 1
#endif
#endif

#define AUDIO_CAP_RATE   16000
#define AUDIO_CAP_CHN    1
#define AUDIO_CAP_BITS   16
/* ★ 2026-10-27：5 秒 -> 7 秒。
 * 原因不是"想录更长"，而是录音窗口的语义变了（见 AUDIO_LEAD_MAX_MS）：
 * dur_sec 现在表示**从检测到你开口算起的语音时长**，缓冲区必须同时装下
 * "300ms 前导 + dur_sec 秒语音"。5 秒缓冲区放不下 5 秒语音 + 前导，
 * 会静默截掉尾巴。7 秒 = 5 秒语音 + 300ms 前导 + 余量，实测够用。 */
#define AUDIO_CAP_SEC    7
#define AUDIO_CAP_BUFMAX (AUDIO_CAP_RATE * AUDIO_CAP_CHN * (AUDIO_CAP_BITS / 8) * AUDIO_CAP_SEC) /* 224KB */

/* 单次AI 读取粒度（一帧采样点数）。SDK demo 用 1024，即 2048 字节 */
#define AUDIO_PERIOD_SAMPLES 1024

/* 一帧的时长：1024 点 / 16000Hz = 64ms。采样循环的节奏基准。 */
#define AUDIO_FRAME_MS  ((AUDIO_PERIOD_SAMPLES * 1000) / AUDIO_CAP_RATE)

/* 等待"新一帧"的上限与轮询间隔。
 *
 * ★ 勘误（2026-10-17）：曾判定"GetFrame 不阻塞、112 帧只花 722ms"，
 *   那是把 rt_tick_get() 当成了毫秒。本工程 rtconfig.h 里
 *   RT_TICK_PER_SECOND = 100，**1 tick = 10ms**，722 tick 实际是 7220ms，
 *   与标称 7168ms 吻合到 0.7% 以内；pts 也是每帧稳定 +64ms。
 *   即 GetFrame 确实是按 64ms/帧 阻塞的，采集节奏本来就对。
 *   任何用 rt_tick_get() 当毫秒用的地方都必须走 AUDIO_TICK2MS()。
 *
 * pts 门控予以保留：它不改变正常路径的节奏，只在出现历史积压时把
 * 旧帧挡掉，属于廉价的保险。
 * 上限取 4 个 period（256ms 量级），超过就认为链路真的断了。 */
#define AUDIO_FRAME_WAIT_MS  260
#define AUDIO_FRAME_POLL_MS  5

/* tick -> 毫秒。RT_TICK_PER_SECOND 可能是 100/200/1000，绝不能假设 1000。 */
#define AUDIO_TICK2MS(t)  ((uint32_t)(((uint32_t)(t) * 1000u) / RT_TICK_PER_SECOND))
/* 毫秒 -> tick，用于"跑满 N 毫秒"这类循环条件 */
#define AUDIO_MS2TICK(ms) ((rt_tick_t)(((uint32_t)(ms) * RT_TICK_PER_SECOND) / 1000u))

/* AGC 收敛期要丢弃的帧数（约 768ms）。
 * mic_probe 实测 ch_data[0] 的 RMS 从 642 单调衰减到 85 才是稳定电平，
 * 那是 AGC 在收敛而不是环境音。audio_capture_read() 在这段时间返回 0
 * （"这一帧不要"，不是错误），上层必须 continue 而不是 break。 */
#define AUDIO_AGC_SETTLE_FRAMES 12

/* ---- ACODEC 直流伺服收敛判据（2026-10-18 实测修正）----
 *
 * ★ 血的教训：上面"丢 12 帧(768ms)"远远不够。
 *
 * cmd_mic_dump 12 实测（两组增益都测了，曲线形状完全一致）：
 *   t= 850ms  dc=-1962
 *   t=1550ms  dc=+4032   <- 峰值，ADC 几乎打满
 *   t=3000ms  dc= +400
 *   t=4500ms  dc=  ~30
 *   t>5000ms  dc=    0    <- 到这儿才算干净
 * 即：开机后 ACODEC 的直流伺服要 **4.5 秒**才回零。在这期间
 *   - 有用信号被 ±4000 LSB 的直流台阶淹掉；
 *   - 3A 的 AGC 把这个台阶当成巨响，把增益压到最低（释放是秒级）。
 * 结果就是"人声在 0~4 秒里说 = 全被吃掉"。这是 voice_test 之前
 * 一直识别失败的直接原因之一，与麦克风好不好无关。
 *
 * 不能简单把帧数改成 70 —— 那样每次识别都白等 4.5 秒。改为按实测
 * 直流分量自适应：|dc| 连续 HOLD 帧低于阈值才放行，超时兜底。
 * 注意：3A 全关（hpf_flag=0）时 dc 同样会回零，所以这个判据在
 * "开 3A / 关 3A"两种模式下都成立（实测已验证）。
 *
 * 阈值 400：稳态噪声峰值只有 ±250（50dB 增益下），顶点 4032，
 * 400 落在两者之间，既能挡住台阶又不会被噪声抖动误触。 */
#define AUDIO_DC_SETTLE_THRESHOLD  400
#define AUDIO_DC_SETTLE_HOLD       3
#define AUDIO_DC_SETTLE_MAX_FRAMES 120   /* 120 × 64ms ≈ 7.7s，超时就强行放行 */

/* ============ 采集拥有权（2026-10-18 新增，与手册核对后修正）============
 *
 * 《音视频函数开发参考手册 V2.0》13.13.5 原文：
 *   "此函数必须在 FH_AC_AI_Enable 之前调用。如果已经调用 FH_AC_AI_Enable
 *    使能 AI，则必须先调用 FH_AC_AI_Disable，然后调用 FH_AC_Set_Config
 *    配置 AI。"
 *
 * 本板音频由 ARC 侧 make_arcfirmware/.../fbv_audio.c 的 audio_init() 在开机
 * 时完成并 Enable：MIC_IN / 16k / 16bit / LPCM / period 1024 / channel_mask=1
 * / SetAnologVol(7, 2, 28) / mix_enable=1。
 *
 * 我们原来的 open() 在 AI 已 Enable 的状态下调 Set_Config，违反上面的时序：
 *   - 第一次 open：Set_Config 被静默忽略（返回值仍是 0，极具迷惑性）
 *   - close() 里 AI_Disable，把 ARC 的使能关掉
 *   - **第二次 open 起**：Set_Config 真的生效，用 support_mask（可能=7）
 *     覆盖掉 ARC 的 channel_mask=1，此后一直是 3 通道
 * 板子只有 1 路模拟 MIC，多开的 2 路恒为 0（实测 ch1/ch2 一直是 0），
 * 且实时通道被重新分配，表现为"配置看着都对、数据却不是那个数据"。
 *
 * 1 = 被动模式（推荐）：不碰 Set_Config / MIX_Set_Config / Enable / Disable，
 *     只 GetFrame 读数据；SetAnologVol 与 3A 参数照旧下发（手册说明这两个
 *     都是"即时生效、AI 使能后也可动态配置"，不需要 Disable）。
 * 0 = 旧行为（自己配置并开关 AI，与 ARC 抢所有权）。
 */
/* ★ 2026-10-20 改回 0（主动模式）。
 *
 * 被动模式的前提是"ARC 开机时已经 Enable 了 AI，MCU 直接读就行"，
 * 但实测该前提不成立：
 *     [audio] PASSIVE: use ARC stream, QueryBufSize ret=0x0 size=0
 * 缓冲区 0 字节 → ARC 根本没往里写 → GetFrame 返回 0x8013000f
 * （手册 p.326：0x8013000f = 参数错误，实际语义是"没有帧可取"）。
 *
 * 反证：2026-10-18 在**主动模式**下 `mic_dump 12` 拿到 176 帧 / 12050 ms，
 * 说明 MCU 自己 Set_Config + AI_Enable 这条路是通的。
 * channel_mask 已被固定为 1，之前"第二次 open 被 support_mask=7 覆盖"的坑不再存在。 */
#ifndef AUDIO_PASSIVE_OWNER
#define AUDIO_PASSIVE_OWNER 0
#endif

/* ★★ 2026-10-20 新增：开 AI 的同时把 AO 也打开（只喂静音）。
 *
 * 依据是实测对比，不是推测：
 *   app/audio_demo/src/audio_api_demo.c 用的是与本工程**完全相同**的
 *   MIC_IN / 16k / 16bit / LPCM / period1024 / channel_mask=1 /
 *   SetAnologVol(0x0f, 2, 28)，唯一多出来的动作就是再配一次 AO
 *   （io_type=FH_AC_LINE_OUT + FH_AC_AO_SetAnologVol）并调用
 *   FH_AC_AI_AO_SYNC_Enable() 而不是 FH_AC_AI_Enable()。
 *   用同一根线、同一个耳机麦克风插 LINE IN：
 *       audio_demo  → 能从 LINE OUT 听到自己的声音（较小声，但确实是语音）
 *       本工程      → mic_dump 的 rms 恒为 118，lo/hi 只有 ±1000 零星尖峰
 *
 * 118 这个值不是"声音小"，而是一个**静止的噪声底**（36 帧全部落在 101~139，
 * 标准差个位数，人声包络不可能这么平）。把总模拟增益拉到 40dB
 * （mic_vol=2 即 20dB + ain_vol=28 约 20dB）仍然不变，说明信号在进 ADC
 * 之前就不存在。
 *
 * 机理：这类片内 ACODEC 的 MICBIAS（驻极体偏置）取自耳机驱动器的电荷泵，
 * **AO 不使能时偏置不上电**，驻极体麦克风不工作，ADC 输入悬空，采到的就是
 * 这个噪声底。audio_demo 因为同时开了 AO，偏置有电，所以能录到。
 *
 * 所以这里补上 AO，只为把模拟前端点亮；数据侧每取一帧 AI 就补一帧静音，
 * 保证 DAC 不欠载（不把麦克风回灌到耳机，避免啸叫）。
 *
 * 想退回"只开 AI"的旧行为：改成 0。 */
#ifndef AUDIO_ENABLE_AO_BIAS
#define AUDIO_ENABLE_AO_BIAS 1
#endif

/* AO 音量。**必须与 audio_demo 一致用 3，不要用 0**：
 * 0 很可能被当成"静音"而把耳机驱动器一起关掉，那样电荷泵还是停的，
 * MICBIAS 依然没有电压，等于白开。我们送的是全 0 数据，音量为 3 也只是
 * 把静音放大，听感上仍是静音。 */
#define AUDIO_AO_VOL 3

/* FH_AC_AI_CH_SetAnologVol 的通道掩码。
 *
 * ★ 必须用 0x0f（与 audio_demo 完全一致），不要用 7。
 * 掩码的含义是"给哪几路写模拟增益"，**没写到的通道保持上电默认值**，
 * 而手册写得很清楚：ain_vol 的 0 档就是**静音**。
 * voice_asr 原来写 7（只覆盖 ch0~2），audio_demo 写 0x0f（覆盖 4 路），
 * 而本板的信号恰恰是从 LINE IN 那一路进来的。这一处与 AO 使能是本工程
 * 与 audio_demo 之间仅剩的两个差异。 */
#define AUDIO_VOL_CH_MASK 0x0f

/* 模拟增益合法范围（手册 13.13.10 原文）：
 *   mic_vol : [0~3]    0=0dB(Default)  1=10dB  2=20dB  3=26.4dB
 *   ain_vol : [0~31]   0=静音  11=0dB  31=24dB  (每档 1.2dB)
 * 超出范围写入的行为未定义。我们原来跑过 `mic_cfg 7 28 0`，mic_vol=7
 * 越界 4 档，那一轮测到的"幅度翻倍"不能当成有效证据。 */
#define AUDIO_MIC_VOL_MAX  3
#define AUDIO_AIN_VOL_MAX  31

/* ============ 采集源：固定单源，不再逐帧混切（2026-10-21 修正）============
 *
 * 上一版是"每帧比较 ch_data[0] 与 mix 的 RMS，取强者"。实测证明这个做法
 * 会把一段语音切碎成两种完全不同的信号：
 *
 *   voice_test 实测：`src stats: ch_data[0]=16 frames, mix=82 frames`
 *   —— 5 秒里有 82 帧（约 5.2 秒）选中的是 mix，只有 16 帧（约 1 秒）是 ch0。
 *
 * 原因是两者的行为正好互补：
 *   · 说话时：NR 把 mix 里的人声当噪声压掉 → ch0 更大（历史实测 mix=77 / ch0=895）
 *   · 不说话时：AGC 把底噪抬起来 → mix 更大（本次实测 mix≈432 / ch0≈556 起步）
 * 于是"张嘴用 ch0、闭嘴用 mix"，拼出来的波形是**大部分是放大的底噪、
 * 零星夹几段真人声**，且帧间电平跳变几十 dB。讯飞对这种输入只会回
 * `code=0 / status=2 / words=0`（空结果），表现就是"跑通了但一个字都没有"。
 *
 * 正确做法：**整段录音只用一个源**。运行时用 `mic_src 0|1` 切换。
 *
 * ★ 2026-10-21 现场对比（同一句话，各录 5 秒，指标为逐帧 RMS）：
 *
 *    源           p10(底噪)  max(人声)   peak 样点   norm 后
 *    ch_data[0]    138~158    203~311      1618     x8 -> 12944
 *    mix             1~4      414~788      2828     x7 -> 19796
 *
 *   ch0 的底噪(≈150) 已经超过 AUDIO_RMS_THRESHOLD(100)，导致 heard 恒为 1、
 *   静音累计永远起不来 —— 每轮都录满 5 秒纯噪音送去识别，而人声峰值只有
 *   底噪的 2 倍，讯飞只会回一个空词 `"w":""`。
 *   mix 走的是 3A（HPF 砍掉 ~300 LSB 直流 + NR 降噪 + AGC 抬高电平），
 *   底噪低两个数量级、人声峰值高 2.5 倍，VAD 也能正常工作。
 *
 *   ⇒ 默认改成 mix。回放听着"更吵"是因为后面还做了峰值归一化(norm x7~8)，
 *     把 AGC 已经抬高的底噪一起推到接近满量程；ASR 只看信噪比和电平，
 *     不看回放听感，所以以这里的数字为准。
 *   若现场发现 mix 的人声被 NR 削掉（表现为 max 比 ch0 还小），
 *   直接 `mic_src 0` 切回原始通道即可，不需要重烧。
 */
#define AUDIO_SRC_CH0  0     /* ch_data[0]，原始 MIC0（底噪大，作对照） */
#define AUDIO_SRC_MIX  1     /* 3A 后的虚拟混音（HPF+NR+AGC，当前默认） */

#ifndef AUDIO_SRC_FORCE
#define AUDIO_SRC_FORCE AUDIO_SRC_MIX
#endif

/* 上传前的电平归一化。
 *
 * ★ 2026-10-21-s：从"峰值归一化"改成"语音段 RMS 归一化"。
 *
 * 同一句"打开灯光"连测 7 轮，原始数据（peak=整段最大样点，norm=当时的增益）：
 *
 *   轮  peak样点  norm 放大后  实测帧rms(p25/med/max)  结果
 *   1     955     x8    7640      2 /  21 /  293      失败（太轻）
 *   2    2302     x8   18416      4 /  59 / 1280      失败
 *   3    1776     x8   14208     30 /  94 /  658      成功
 *   4    4519     x4   18076     20 /  84 /  776      失败
 *   5     672     x8    5376      5 /  24 /  127      失败（8 倍封顶仍太轻）
 *   6    9345     x2   18690     35 / 103 /  849      失败（peak/rms=11，峰值是咔哒声）
 *   7    4248     x4   16992     25 /  61 /  436      失败
 *
 * 两个致命缺陷：
 *   ① 增益由"单个最响样点"决定。话筒线碰一下、AGC 跳一次，就把整段增益
 *      压到 x2，语音只剩 -26dBFS —— 讯飞直接判"无有效语音"（resp#1 就 status=2）。
 *   ② 上限 8 倍太死。语音本来只有 -30dBFS 时（peak 672），x8 也只有 -16dBFS。
 *
 * 改成：按 20ms 块算能量，只取"语音块"（能量 ≥ 最响块的 1/4）求平均块 RMS，
 * 再把它拉到 TARGET，最后用峰值上限兜底防削顶。
 * 这样不论用户大声小声、AGC 抬了多少，送上去的电平都一样 ——
 * 正好补上 AGC 没兜住的那 ±12dB 波动，这是"时好时坏"的直接原因。
 */
#define AUDIO_NORM_RMS_TARGET  3200    /* 语音段目标 RMS ≈ -20dBFS */
#define AUDIO_NORM_MAX_GAIN    32      /* +30dB；原来 8 倍在弱信号时不够 */
#define AUDIO_NORM_PEAK_LIMIT  30000   /* 放大后的峰值上限，防削顶 */
#define AUDIO_NORM_MIN_PEAK    200     /* 原始峰值低于此 = 纯底噪，不放大 */

/* 削顶预算：允许被削掉的样点比例，单位 ppm（百万分之一）。
 *
 * ★ 2026-10-27 现场（voice_test 6 那次）暴露的致命缺陷：
 *      [audio] norm off (speech rms 768 ok, peak 16409)
 *   语音段 RMS 只有 768（-32.6dBFS），本该 x4 拉到 3072 才够电平，
 *   但代码用 `lim = 30000 / peak = 1` 把增益直接砍成 1 —— 因为整段里
 *   有一个 16409 的孤立尖峰（值 50% 满量程，而语音只有 2.5%）。
 *   结果：整段以 -32dBFS 送进讯飞，只有最响的那一小段被认出来，
 *   表现就是"说了几秒，只识别出前面几个字"。
 *
 *   为了保住 1~2 个样点的尖峰，牺牲了整段 12dB 的电平 —— 完全划不来。
 *   正确做法是**削顶预算**：按比例允许削掉极少数样点，其余照常放大。
 *   5000ppm = 0.5%，对 6 秒音频约 480 个样点，不足以让 ASR 明显退化。
 */
#define AUDIO_NORM_CLIP_PPM    5000

/* 20ms 块的"典型峰值因子"。块 RMS 乘它就是该块的稳健峰值估计，
 * 用它替代"整段最大样点"作为限幅参考，避免单个尖峰绑架增益。
 * 12dB ≈ 4 倍，是语音在 20ms 窗内的常见波峰因数。 */
#define AUDIO_NORM_CREST       4

/* 上传前掐掉首尾多余静音（2026-10-21-s）。
 * 讯飞是**按实时速率**灌音频的（125 帧 × 40ms = 5 秒），前面 2 秒都是
 * 静音就白等 2 秒；而且服务端前端点(VAD)在长静音后更容易判"无有效语音"。
 * 掐完只留 300ms 前导 / 200ms 尾巴，既省时间又让语音落在音频块靠前的位置。 */
#define AUDIO_TRIM_LEAD_MS   300
#define AUDIO_TRIM_TAIL_MS   200
#define AUDIO_TRIM_THR       25

/* ============ "等开口"与录音窗口的起点（2026-10-27 修"时间对不上"）============
 *
 * 现场现象：网页显示"请说话"，实际说话只有少部分进到识别里。
 *
 * 根因不是麦克风，也不是讯飞，是**录音窗口的起点错了**：
 *   voice_test 一被触发就把 g_voice_state 置 2，而 record() 从那一刻开始
 *   计 dur_sec 秒；可是网页靠 /api/state 轮询，用户看到"请说话"时，
 *   窗口已经跑掉 0.6~2 秒（2 秒基础轮询 + 网络 + 浏览器节流）。
 *   用户再愣 1 秒才开口，4 秒窗口只剩 1~2 秒真正录到语音 —— 正好是
 *   "只有少部分进去"。
 *
 * 正确做法：**窗口从检测到人声才起算**。
 *   阶段 A：读帧但不计入窗口，最多等 AUDIO_LEAD_MAX_MS（3 秒）；
 *          这段时间最后 AUDIO_PRE_MAX 字节保存在滚动前导缓冲里，
 *          这样第一个字不会被切掉。
 *   阶段 B：从人声起点开始录满 dur_sec 秒。
 * 于是"用户什么时候看到提示"不再影响录音内容，哪怕晚 2 秒开口也不丢字。
 *
 * 代价：最坏情况总时长 = 3 秒等待 + dur_sec 秒录音。可接受。
 * AUDIO_LEAD_MAX_MS 取 3000：覆盖 2 秒轮询延迟 + 人的反应时间，
 * 再长就变成"干等"了；超时仍没声音则直接返回 -2（没听到声音）。 */
#define AUDIO_LEAD_MAX_MS   3000

/* 语音起点前保留的前导静音字节数（=AUDIO_TRIM_LEAD_MS）。
 * 与上传前的掐静音用同一个量：先留着，末尾 trim 再统一裁到 300ms。 */
#define AUDIO_PRE_MAX  ((AUDIO_CAP_RATE * AUDIO_CAP_CHN * (AUDIO_CAP_BITS / 8) * AUDIO_TRIM_LEAD_MS) / 1000)

/* 选择采集源（下一次 audio_capture_read 生效，可随时改） */
int audio_capture_set_src(int which);

/* 固定录音时长（2026-10-21-s）。
 *   sec > 0 -> 录满 sec 秒，**完全关闭 VAD 提前收尾**（默认 6 秒）；
 *   sec = 0 -> 恢复 VAD 自动收尾。
 * 现场诉求是"识别的时间不好判断"：VAD 什么时候判你说完，取决于阈值和
 * 环境噪声，每轮都不一样。固定时长把这件事变成确定的——
 * 敲完命令看提示说话，说完就等着，N 秒后必然开始上传。
 * 因为上传前会先掐掉首尾静音，多录的静音不会传上去，定长几乎不额外费时。
 * 上限受 AUDIO_CAP_BUFMAX 限制（7 秒；取 7 是为了给 6 秒语音留足前导位）。 */
int audio_capture_set_fixed_sec(int sec);
int audio_capture_get_fixed_sec(void);

/* 把一段 PCM 通过 LINE_OUT 播出来（16k/16bit/mono）。
 * 用途：录音后立刻回放，人耳一听就知道采到的是不是真人声 ——
 * 这是区分"音频内容不对"与"上传/协议不对"最快的手段。
 * 内部会 open/close 一次音频（需要 AO 已随 AI 一起使能）。
 * 入参用非 const：SDK 的 FH_AC_AO_SendFrame 收 FH_UINT8*，
 * 传 const 指针要强制去除限定符，-Werror 下容易踩坑。 */
int audio_capture_play(uint8_t *pcm, int len);

/* 同 audio_capture_play，但**不**自己 open/close —— 调用方必须已经把音频
 * 打开（AO 随 AI 同步使能）。给语音对讲（web/src/talk.c）用：
 * 每段都重开一次会重新等 4.5 秒直流收敛，对讲就没法用了。 */
int audio_capture_play_raw(uint8_t *pcm, int len);

/* 静音检测阈值（RMS）。低于此值认为没人在说话，直接放弃本次识别。
 *
 * ★ 比较的是 audio_rms() 的输出，而该函数**已经按帧减去均值**，
 *   所以这里量的是纯交流能量，不受 ~300 LSB 直流偏置影响。
 *
 * ★ 取值依据（mic_test 实测 3 轮数据）：
 *     底噪（安静时）      rms = 23 ~ 25
 *     正常说话（30cm）    rms = 57 ~ 155   <- 实际的"人声"区间
 *     大声/靠近          rms = 311 ~ 653
 *   原值 300 正好卡在"正常说话"之上，导致用户正常音量说话时
 *   `rms >= AUDIO_RMS_THRESHOLD` 永远为假 —— 表现为
 *   "一直说话却只有零星几帧是 VOICE"，且 audio_capture_record()
 *   会判定"全程静音"直接返回 -2（没人说话），识别根本进不去。
 *
 *   现取 100：底噪 25 的 4 倍，落在人声区间的下沿，留出足够余量。
 *   若实际环境更吵（底噪 > 60），把这里上调到 底噪*3~4 即可。
 *
 * ★ 2026-10-21 现场：用户回放听到"较大噪音"，且每次录音都录满整段、
 *   从不提前收尾 —— 这两件事指向同一个结论：**底噪的交流 RMS 本身 > 100**，
 *   于是 heard 恒为 1、静音累计永远起不来。此时调阈值没用，要降噪：
 *   优先 `mic_src 1` 切到 3A 处理后的 mix（HPF+NR+AGC），
 *   3A 是否就绪看 `[audio] algo probe ret=0`。
 *
 * ★ 2026-10-21-s 重新定标：100 对 mix 也偏高，会造成"说到一半被腰斩"。
 *   7 轮实测 mix 的逐帧 RMS 分布（同一句话）：
 *       p10 = 1~5      ← 纯静音，NR 直接把输出压到数字 0
 *       p25 = 2~35
 *       med = 21~103   ← 正常说话有大半帧落在 20~100 之间
 *       max = 127~1280
 *   阈值取 100 时，med=21 / 61 / 94 的那几轮里"大部分人声帧都被当成静音"，
 *   silence_run 一路累加，录到 3.2~3.9 秒就判"说完了"——
 *   而用户其实还在说，句子被从中间切断，识别必然失败。
 *   取 30 后：静音（1~5）仍判静音，人声（20~100）算说话，两边都干净。
 *   若环境更吵（底噪 > 15），上调到 底噪*3 即可。 */
#define AUDIO_RMS_THRESHOLD  30

/* 说完的判定阈值：连续静音达到该时长就认为用户说完了，立刻停止录制。
 * 换算成字节时必须 (rate*bps*ms)/1000，先乘后除——写成 (ms/1000) 会被
 * 整数除法截断成 0，导致一遇到静音帧就 break。
 *
 * ★ 实测教训：原值 300ms 太短。用户说一句话中间常有几十分之一秒的换气/
 * 词间停顿，300ms 一到就判定"说完了"，实测只录到 748ms 就返回
 * （"[audio] captured 23936 bytes (748 ms)"），送去识别的音频被腰斩。
 * 改成 1000ms：既能容忍正常语流中的停顿，又能真正截掉句尾的大段静音。 */
#define AUDIO_MIN_SPEECH_MS  1000

/* 一句话的最短录音时长（毫秒）。除了"连续静音够久"，还必须录满这么久，
 * 否则一句话刚起头（heard=1 但才 200ms）就会被尾随静音立刻截断。
 * 讯飞一句话识别需要 1~2 秒有效音频才准。 */
#define AUDIO_MIN_TOTAL_MS   1500

/* 打开音频输入，返回 0 成功 */
int audio_capture_open(void);

/* 读最多 len 字节 PCM 到 buf，返回实际读取字节数（<=0 表示结束/错误） */
int audio_capture_read(uint8_t *buf, int len);

/* 等采集真正可用（直流伺服收敛完成）之后再返回。
 *
 * 为什么需要它：每次 open 之后 ACODEC 的直流伺服要 3.5~4.5 秒才回零
 * （见下面 AUDIO_DC_SETTLE_* 的实测曲线）。这段时间里 audio_capture_read()
 * 一律返回 0，数据被有意丢弃。若上层在 open 之后立刻提示"请说话"，
 * 用户开口的头几秒全落在丢弃区里，录到的只剩静音 → 识别返回空结果。
 * 把"等收敛"单独抽出来，就能做到"能录了再提示"。
 *
 * 实现上会读掉收敛期的帧，并消耗掉第一帧有效数据（64ms）。
 * 返回：0 就绪，-1 打不开或取帧失败，-2 超时。 */
int audio_capture_wait_ready(int timeout_ms);

/* 关闭音频输入 */
void audio_capture_close(void);

/* 计算一段 PCM 的 RMS（已去直流偏置）。用于判断麦克风是否采到声音、
 * 以及音量是否偏小（对外部可见，mic_test 命令使用）。 */
uint32_t audio_rms(const uint8_t *buf, int len);

/* 最近一帧的 pts（毫秒）与字节长度，仅用于诊断打印。
 * pts 是 SDK 给的微秒时间戳（与视频同一时钟基准），是判断"读到的是
 * 实时数据还是缓冲里的旧数据"最可靠的依据 —— 比 memcmp 比对帧内容
 * 可靠得多，因为相邻两帧哪怕内容 99% 相同，LSB 的噪声也会让 memcmp
 * 判为"不同"，于是"重复帧"统计永远是 0，毫无意义。 */
uint32_t audio_capture_last_pts_ms(void);
int      audio_capture_last_len(void);
/* 最近一帧自报的采样率（frame.sample_rate），用来验证"配的 16k 与实际出帧
 * 的 16k 是不是同一个 16k"。恒为 0 说明驱动没填这个字段，不能当证据。 */
uint32_t audio_capture_last_rate(void);

/* ---- 诊断用可调项：用来区分"前端没信号"和"信号被算法/增益吃掉了" ----
 *
 * audio_capture_set_3a(0) 关掉 HPF/NR/AGC，读到的就是最原始的通路；
 * audio_capture_set_analog_vol() 直接下发 ACODEC 的模拟增益。
 * 两者都必须在下一次 audio_capture_open() 之前或当次采集期间调用才生效
 * （3A 参数在 open 里下发，改了要重新 open）。 */
int audio_capture_set_3a(int enable);
int audio_capture_set_analog_vol(uint32_t mic_vol, uint32_t ain_vol);

/* 切换模拟输入口，下一次 open 生效。
 *   0 = FH_AC_MIC_IN   麦克风输入（codec 提供偏置，接驻极体/MEMS 咪头）
 *   1 = FH_AC_LINE_IN  线路输入（无偏置，接手机/电脑耳机口这类线路电平源）
 * 板子 J20 跳线把 AC_MICIN 脚接到 MIC 还是 LINE_IN，软件这边必须一致。 */
int audio_capture_set_io_type(int type);

/* 逐通道探测量：开一次音频、取 frames 帧，对 mix 与每个 ch_data[] 分别
 * 统计 RMS 并打印。用于回答"信号到底在哪个通道 / 增益够不够"这个
 * 单看 mix 无法回答的问题。
 *   mask   —— 通道掩码（FH_AC_AI_Get_Feature_ChannelMask 的返回值）
 *   frames —— 采样帧数
 * 返回 0 成功，-1 失败。
 * mask 用 FH_UINT32 而非 uint32_t：本平台 uint32_t 是 long unsigned int，
 * 而 SDK 的 FH_UINT32 是 unsigned int，同为 32 位但类型不同，
 * 传指针给 SDK 函数会触发 -Werror=incompatible-pointer-types。 */
int audio_probe_channels(FH_UINT32 mask, int frames);

/*
 * 录制并返回有效语音。
 * buf 需 >= AUDIO_CAP_BUFMAX。
 *
 * ★ 2026-10-27 语义变更：dur_sec 是**从检测到人声起算**的语音秒数，
 *   不再是"从进函数起算"的墙钟秒数。进函数后先最多等 AUDIO_LEAD_MAX_MS
 *   让人开口，检测到声音才开始计 dur_sec。
 *   g_fixed_sec > 0（voice_test N）时 dur_sec 强制取 g_fixed_sec，且不提前收尾。
 *   返回：>0 字节数；-1 打不开/取帧失败；-2 等待期内一声人声都没听到。
 */
int audio_capture_record(uint8_t *out_buf, int buf_size, int dur_sec);

/*
 * 录音进度（供网页显示"已录 x/y 秒"）。
 *   speech_ms —— 已录语音毫秒（阶段 A 等开口时为 0）
 *   lead_ms   —— 等开口已花的毫秒
 * 返回 0 正常。-1 表示当前没在录（未开始或已结束）。
 *
 * 为什么要有这个：网页原来用 JS 自己的 setTimeout 估算倒计时，那个时钟和
 * 板子的采集时钟毫无关系，一旦轮询延迟就必然对不上。让板子报真实进度，
 * 显示误差最多一个轮询周期，而且是"滞后"不是"超前"——不会误导用户提前收声。
 */
int audio_capture_progress(int *speech_ms, int *lead_ms);

#endif /* AUDIO_CAPTURE_H */

