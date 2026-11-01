/*
 * audio_capture.c - 音频采集（板载麦克风 / 内嵌音频）
 *
 * 麦克风路径参照 app/audio_demo/sample_audio 的官方用法：
 *   FH_AC_Init()
 *   FH_AC_AI_Get_Feature_ChannelMask(&mask)
 *   FH_AC_Set_Config(io_type=FH_AC_MIC_IN, sample_rate=16000, bit_width=16,
 *                    enc_type=FH_PT_LPCM, period_size=1024,
 *                    channel_mask=mask, frame_num=0)
 *   FH_AC_AI_Enable()
 *   FH_AC_AI_GetFrame(&frame)  -> 取 frame.mix，退回 frame.ch_data[0]，长度 ch_data_len
 *   FH_AC_AI_Disable()
 */

#include "audio_capture.h"

#include <fcntl.h>
#include <unistd.h>
#include <string.h>

/* =======================================================================
 * 模式一：板载麦克风（默认）
 * ======================================================================= */
#ifdef AUDIO_SRC_MIC

/* SDK 音频 API。路径由 SDK 自带的 Rtt.mk 提供（INCS 里已有
 * -I$(SDKROOT)/lib/$(CHIP_ID_LOWER)/inc/dsp 与 -I$(SDKROOT)/lib/inc），
 * 这里直接 include 即可；若真的找不到，gcc 会明确报
 * "fatal error: dsp/fh_audio_mpi.h: No such file or directory"，一眼可辨。 */
#include <dsp/fh_audio_mpi.h>

static int g_ai_enabled = 0;

#if AUDIO_ENABLE_AO_BIAS
/* AO 与 AI 同步使能后必须持续喂帧，否则 DAC 欠载。
 * 缓冲区放静态区：语音线程栈只有 40KB，且这里必须常驻（SendFrame 期间有效）。 */
static int      g_ao_enabled = 0;
static FH_UINT8 g_ao_silence[AUDIO_PERIOD_SAMPLES * 2];   /* 全 0 = 静音 */

/* 送一帧静音。作用只有一个：让 AO 时钟/电荷泵持续工作，
 * 从而给 ACODEC 的 MICBIAS 供电。**不**把麦克风数据回灌，
 * 否则耳机挂在 LINE OUT 上会啸叫。 */
static void ao_feed_silence(void) {
    FH_AC_AO_FRAME_S f;
    FH_SINT32 r;
    static int warned = 0;

    if (!g_ao_enabled) return;

    f.len  = (FH_UINT32)sizeof(g_ao_silence);
    f.data = g_ao_silence;
    r = FH_AC_AO_SendFrame(&f);
    if (r != 0 && !warned) {
        warned = 1;           /* 只喊一次，避免刷屏拖慢实时线程 */
        rt_kprintf("[audio] WARN: AO_SendFrame ret=0x%x (只报一次)\n", (unsigned)r);
    }
}
#else
#define ao_feed_silence() do { } while (0)
#endif /* AUDIO_ENABLE_AO_BIAS */

/* AGC 收敛期需要丢弃的帧数。
 *
 * mic_probe 实测 ch_data[0] 的 RMS 序列：
 *   frame 0..12 : 642 618 534 417 302 237 229 161 153 87 85 106 152
 *   frame 13    : 895   <- 真实人声
 *   frame 14..19: 234 44 97 97 130 74
 *
 * 开头那段从 642 单调衰减到 85 是 **AGC 正在收敛**（目标电平往下压），
 * 不是环境里的声音。若不丢弃：
 *   - VAD 会把开头误判成"用户一直在说话"（因为 642 > threshold 300），
 *     于是"说完即停"逻辑永远等不到静音；
 *   - 更糟的是"静音累积"从错误的基线开始算，阈值判定完全失准。
 * 丢掉前 12 帧（约 768ms）后拿到的才是稳定电平的真实人声。
 *
 * AUDIO_AGC_SETTLE_FRAMES 定义在 audio_capture.h（mic_test 也要用）。
 *
 * ★ 2026-10-18 修正：不再按固定帧数丢包，改为按实测 |dc| 自适应收敛
 *   （实测直流伺服要 4.5 秒才回零，固定 12 帧只有 768ms）。
 *   下面三个状态量替代原来的 g_settle_left。 */
static int g_settle_done   = 0;   /* 1 = 已收敛，正常出帧 */
static int g_settle_hold   = 0;   /* 连续满足阈值的帧数 */
static int g_settle_frames = 0;   /* 已经等了多少帧（用于超时兜底） */

/* ---------------- 帧新旧判定（pts）----------------
 * FH_AC_AI_Frame_S.pts 是微秒时间戳，与视频同一时钟基准。
 * 用它判断"这次 GetFrame 给的是新采到的音频，还是缓冲里的旧帧"。
 * 见 audio_capture_read() 里的说明。 */
static uint64_t g_last_pts   = 0;   /* 上一帧 pts（微秒） */
static int      g_have_pts   = 0;   /* 是否已经拿到过一帧有效的 pts */
static uint32_t g_last_pts_ms = 0;  /* 最近一帧 pts（毫秒），诊断用 */
static int      g_last_len   = 0;   /* 最近一帧的单通道字节长度，诊断用 */
static uint32_t g_last_rate  = 0;   /* 最近一帧自报的采样率，诊断用 */

/* ---------------- 数据源自适应（2026-10-20-f 新增）----------------
 *
 * 事实 1：audio_demo 用 `ai_frame.mix` 回放，同一根线、同一个耳机麦，
 *         能在 LINE OUT 听到声音 —— 说明 mix 通路上有被放大过的信号。
 * 事实 2：本模块读 `ch_data[0]`，mic_dump 说话与不说话都是 rms≈100。
 * 事实 3：两者 AI 配置逐字相同（MIC_IN / 16k / 16bit / period1024 / ch1 /
 *         mic_vol=2 / ain_vol=28 / HPF+NR+AGC 全开）。
 *         ⇒ 唯一差别就是**取哪一路**。
 *
 * 强弱关系随信号电平反转，所以不能固定选一路：
 *   信号强 -> NR 把它当噪声压掉   （历史实测 mix=77  vs ch0=895）
 *   信号弱 -> AGC 把它抬高 30dB+  （当前耳机麦直连 LINE IN 的情况）
 * 因此每帧比较两者的 RMS，取强者。统计值在 close() 时打印。 */
static int g_src_mix_cnt = 0;   /* 选 mix 的帧数 */
static int g_src_ch0_cnt = 0;   /* 选 ch_data[0] 的帧数 */
static int g_src_logged  = 0;   /* 首帧选择是否已打印（避免刷屏） */

/* 当前使用的采集源。整段录音固定不变，避免"说混用 ch0、不说混用 mix"
 * 造成帧间几十 dB 的电平跳变（详细原因见 audio_capture.h 的说明）。 */
static int g_force_src = AUDIO_SRC_FORCE;

/* 3A 算法服务是否就绪（FH_AC_Get_Algo_Version 返回 0 才算）。
 * mix 就是 3A 的输出，服务没起来时 mix 不可信 —— 见 read() 里的保护分支。 */
static int g_algo_ok = 0;

int audio_capture_set_src(int which) {
    g_force_src = (which == AUDIO_SRC_MIX) ? AUDIO_SRC_MIX : AUDIO_SRC_CH0;
    rt_kprintf("[audio] src -> %s\n", (g_force_src == AUDIO_SRC_MIX) ? "mix" : "ch_data[0]");
    return 0;
}

/* 固定录音时长（2026-10-21-s）。
 * 现场"识别的时间不好判断"的根因就是这个开关：VAD 什么时候判你说完，
 * 取决于阈值、环境噪声和说话节奏，每轮都不同；而 mix 通路上人声帧的
 * RMS 只在 20~100 之间晃，跟阈值 30 挨得很近，边界非常不稳定。
 * 固定时长后，"提示 → 说话 → N 秒后必然开始上传"，全程可预测。
 *
 * ★ 默认 4 秒（不是 0）。因为上传前会先掐掉首尾静音，多录的静音
 *   根本不会传上去，所以定长几乎不增加等待，却彻底消除了"说到一半
 *   被判说完了"这一类失败。voice_test 0 可切回 VAD 模式。
 *
 * ★ 2026-10-27 改成 6 秒。现场 `act` 时间轴显示第二段人声正好从
 *   3.58s 开始，而 4 秒窗口在 4.03s 就截止了 —— 后半句被整段切掉，
 *   所以只识别出前面几个字。5 秒时余量仍只有 1.4 秒，说话稍慢就又切。
 *   6 秒 = 4 秒窗口 + 2 秒余量，仍远小于 AUDIO_CAP_SEC(7)：
 *   6*32000 + 前导 9600 = 201600 字节 ≤ 224000，不会溢出。
 *   多出来的静音在归一化前会被 trim 掉，不会增加讯飞的计费时长。 */
static int g_fixed_sec = 6;

int audio_capture_set_fixed_sec(int sec) {
    if (sec < 0) sec = 0;
    if (sec > AUDIO_CAP_SEC) sec = AUDIO_CAP_SEC;
    g_fixed_sec = sec;
    return g_fixed_sec;
}

int audio_capture_get_fixed_sec(void) {
    return g_fixed_sec;
}

/* ---- 可调项（诊断命令 mic_cfg / mic_io 会改它们）---- */
static int      g_3a_on   = 1;      /* 0 = 关掉 HPF/NR/AGC，读原始通路 */
static uint32_t g_mic_vol = 2;      /* ACODEC 麦克风模拟增益（ARC 侧同值） */
static uint32_t g_ain_vol = 28;     /* ACODEC 输入通道增益（ARC 侧同值） */
/* 模拟输入口选择：0 = FH_AC_MIC_IN（带偏置，接驻极体/MEMS 咪头）
 *                 1 = FH_AC_LINE_IN（线路电平，接手机/电脑耳机口输出）
 * 板子上 J20 跳线决定 AC_MICIN 脚接到 MIC 还是 LINE_IN，
 * 软件这边必须与之一致，否则采到的是另一个脚的信号（或悬空）。 */
static int      g_io_type = 0;

/* 前向声明：定义在下面（3A 参数下发） */
void ac_enable_algorithms_apply(void);
/* audio_rms 定义在文件后部，read() 里的自适应选源要用到 */
uint32_t audio_rms(const uint8_t *buf, int len);

uint32_t audio_capture_last_pts_ms(void) { return g_last_pts_ms; }
int      audio_capture_last_len(void)    { return g_last_len; }
uint32_t audio_capture_last_rate(void)   { return g_last_rate; }

/* 改 3A 开关。已打开时立即重新下发一次，免得必须重新 open。 */
int audio_capture_set_3a(int enable) {
    g_3a_on = enable ? 1 : 0;
    if (g_ai_enabled) {
        ac_enable_algorithms_apply();
    } else {
        rt_kprintf("[audio] 3A will be %s on next open\n", g_3a_on ? "ON" : "OFF");
    }
    return 0;
}

/* 改模拟增益。只能在通道已使能后下发，open 之前调用只记录、open 时生效。
 *
 * ★ 必须钳位：手册 13.13.10 规定 mic_vol ∈ [0,3]、ain_vol ∈ [0,31]，
 *   越界写入的行为未定义。曾用 `mic_cfg 7 28 0` 测过一轮，mic_vol=7
 *   越界 4 档，那次的幅度变化不能作为增益有效的证据。 */
int audio_capture_set_analog_vol(uint32_t mic_vol, uint32_t ain_vol) {
    FH_SINT32 r;
    uint32_t mic_raw = mic_vol, ain_raw = ain_vol;

    if (mic_vol > AUDIO_MIC_VOL_MAX) mic_vol = AUDIO_MIC_VOL_MAX;
    if (ain_vol > AUDIO_AIN_VOL_MAX) ain_vol = AUDIO_AIN_VOL_MAX;

    g_mic_vol = mic_vol;
    g_ain_vol = ain_vol;

    if (mic_raw != mic_vol || ain_raw != ain_vol)
        rt_kprintf("[audio] vol clamped: mic %u->%u (max %d), ain %u->%u (max %d)\n",
                   (unsigned)mic_raw, (unsigned)mic_vol, AUDIO_MIC_VOL_MAX,
                   (unsigned)ain_raw, (unsigned)ain_vol, AUDIO_AIN_VOL_MAX);

    if (!g_ai_enabled) {
        rt_kprintf("[audio] vol mic=%u ain=%u recorded, applies on next open\n",
                   (unsigned)mic_vol, (unsigned)ain_vol);
        return 0;
    }
    r = FH_AC_AI_CH_SetAnologVol(AUDIO_VOL_CH_MASK, mic_vol, ain_vol);
    rt_kprintf("[audio] SetAnologVol(mic=%u ain=%u) ret=0x%x\n",
               (unsigned)mic_vol, (unsigned)ain_vol, (unsigned)r);
    return (r == 0) ? 0 : -1;
}

/* 切换模拟输入口。io_type 只在 FH_AC_Set_Config() 时下发，
 * 所以这里只记录，下一次 open 生效。
 *   type=0 -> FH_AC_MIC_IN   (麦克风输入，codec 会给偏置)
 *   type=1 -> FH_AC_LINE_IN  (线路输入，无偏置，接手机/电脑耳机口)
 * 注意：ARC 侧开机时已按 MIC_IN 配置并使能过 AI，
 * MCU 侧再次 Set_Config 是否真的能改掉硬件配置，需要用 mic_dump 的
 * 幅度变化来验证 —— 不能只看返回值（channel_mask 那次就是返回值 0 但没生效）。 */
int audio_capture_set_io_type(int type) {
    g_io_type = type ? 1 : 0;
    rt_kprintf("[audio] io_type -> %s (%d), applies on next open\n",
               g_io_type ? "LINE_IN" : "MIC_IN", g_io_type);
    return 0;
}

/* ---------------- 音频时钟门控（FH_AC_Init 的前置条件）----------------
 * media_demo 的user_main() 只配置了视频/图像链路的时钟，
 * 音频 ACW(Audio Codec Wrapper) 与 DMIC 的时钟门控默认是关的，
 * 此时调用 FH_AC_Init() 会直接返回错误（本板实测 -2146238451 = 0x8013000D）。
 *
 * 取值来自 platform/mc632x/clock_config.h 的 fh_clk_list_low：
 *   acw_clk_gate  : VEU_SYS_APB_REG_BASE  + 0x14, mask 0x400
 *   dmic_apb_gate : CPU_SYS_AHB_REG_BASE  + 0xDC, mask 0x80000
 * 这两项在时钟表里都只有 .clk_gate、没有 mux/div，属于**纯门控**，直接置位即可。
 * 若将来SDK 换成 fh_clockv2.c，地址需按新的 clock_config.h 核对。 */
#define REG_CPU_SYS_AHB_BASE   0x0C000000UL
#define REG_VEU_SYS_APB_BASE   0x1C000000UL
#define REG_ACW_CLK_GATE_ADDR  (REG_VEU_SYS_APB_BASE + 0x14UL)
#define REG_ACW_CLK_GATE_BIT   0x400UL
#define REG_DMIC_APB_GATE_ADDR (REG_CPU_SYS_AHB_BASE + 0xDCUL)
#define REG_DMIC_APB_GATE_BIT  0x80000UL

static uint32_t ac_reg_read(uint32_t addr) {
    return *(volatile uint32_t *)addr;
}
static void ac_reg_set_bits(uint32_t addr, uint32_t mask) {
    *(volatile uint32_t *)addr |= mask;
}

static void ac_enable_clocks(void) {
    uint32_t a0 = ac_reg_read(REG_ACW_CLK_GATE_ADDR);
    uint32_t d0 = ac_reg_read(REG_DMIC_APB_GATE_ADDR);

    ac_reg_set_bits(REG_ACW_CLK_GATE_ADDR,  REG_ACW_CLK_GATE_BIT);
    ac_reg_set_bits(REG_DMIC_APB_GATE_ADDR, REG_DMIC_APB_GATE_BIT);

    rt_kprintf("[audio] clk acw : %x -> %x (need bit 400)\n",
               a0, ac_reg_read(REG_ACW_CLK_GATE_ADDR));
    rt_kprintf("[audio] clk dmic: %x -> %x (need bit 80000)\n",
               d0, ac_reg_read(REG_DMIC_APB_GATE_ADDR));
}

/* 打开 3A 算法：HPF(去低频隆隆声) + ANC(降噪) + AGC(自动增益)。
 * AGC 是麦克风模式的关键——它把音量归一化，使 audio_rms() 的阈值判定
 * 不再依赖物理音量/跳线。调用序列参照同工程的
 * app/media_demo/components/libusb_rtt/usb_audio/src/usb_audio.c:191
 * （config_AI_alg_parameter(1, 1, 1, 0, 0)）。
 * 失败不致命：只是没有 3A 处理，仍可录音。 */
void ac_enable_algorithms_apply(void) {
    static FH_AC_SesParam alg;      /* 结构体较大，放静态区省线程栈 */
    int on = g_3a_on;

    if (FH_AC_AI_MIX_Get_Algo_Param(&alg, sizeof(alg)) != 0) {
        rt_kprintf("[audio] WARN: get algo param failed, 3A unchanged\n");
        return;
    }
    alg.hpf_flag = on ? 1 : 0;    /* 高通：滤掉低频隆隆声 */
    alg.anc_flag = on ? 1 : 0;    /* 降噪 */
    alg.agc_flag = on ? 1 : 0;    /* 自动增益 */
    alg.doa_flag = 0;             /* 不做波束成形 */
    alg.aec_flag = 0;             /* 无远端参考信号，不开回声消除 */

    if (FH_AC_AI_MIX_Set_Algo_Param(&alg, sizeof(alg)) != 0) {
        rt_kprintf("[audio] WARN: set algo param failed, 3A unchanged\n");
        return;
    }

    /* ★ 回读一次，证明参数**真的落到了算法里**，而不是"调用没报错"。
     * 之前所有结论都建立在"设置成功"的假设上，回读才是证据。 */
    memset(&alg, 0, sizeof(alg));
    if (FH_AC_AI_MIX_Get_Algo_Param(&alg, sizeof(alg)) == 0)
        rt_kprintf("[audio] 3A %s -> readback hpf=%d nr=%d agc=%d doa=%d aec=%d\n",
                   on ? "ON" : "OFF",
                   (int)alg.hpf_flag, (int)alg.anc_flag, (int)alg.agc_flag,
                   (int)alg.doa_flag, (int)alg.aec_flag);
    else
        rt_kprintf("[audio] 3A %s (write ok, readback failed)\n", on ? "ON" : "OFF");
}

int audio_capture_open(void) {
    FH_AC_CONFIG ac_config;
    FH_UINT32 support_mask = 0;
    FH_SINT32 ret;

    if (g_ai_enabled) return 0;

    /* ★ 前置条件：先开音频时钟门控，否则 FH_AC_Init 必失败 */
    ac_enable_clocks();

    /* 探测音频算法服务是否已就绪。SDK demo 是在 Init 之后才查的，
     * 这里提前查，用来区分"服务没起来"与"只是时钟没开"两种情况。 */
    {
        FH_UINT32 av1 = 0, av2 = 0;
        FH_SINT32 r = FH_AC_Get_Algo_Version(&av1, &av2);
        g_algo_ok = (r == 0);
        rt_kprintf("[audio] algo probe ret=%d ver=%x.%x (3A %s)\n",
                   (int)r, (unsigned)av1, (unsigned)av2,
                   g_algo_ok ? "ready" : "NOT ready");
    }

    ret = FH_AC_Init();
    if (ret != 0) {
        /* 不直接失败！ARC 侧 make_arcfirmware/.../fbv_audio.c 的 audio_init()
         * 在开机时已经做过一次 FH_AC_Init()+FH_AC_AI_Enable()（配置与我们一致：
         * MIC_IN/16k/16bit/LPCM/period1024/mix_enable=1），所以这里的 RPC
         * 很可能返回"已初始化"类错误。能否真正取到数据由 GetFrame 决定。 */
        rt_kprintf("[audio] FH_AC_Init ret=0x%x (likely already-inited on ARC), continue\n",
                   (unsigned)ret);
    } else {
        rt_kprintf("[audio] FH_AC_Init ok\n");
    }

    /* ★ 3A 服务是 FH_AC_Init() 之后才起来的，必须在 Init 之后再探一次
     * （2026-10-21 现场实测）。
     *
     * 证据：开机后第一轮 voice_test 是 `algo probe ret=-2146238450 ver=0.0`
     * （3A NOT ready）→ 强制退回 ch_data[0] → 讯飞回空词、识别失败；
     * 紧接着第二次 open 就是 `ret=0 ver=134b1db.2000d00`。
     * 也就是说 **Init 之前的探针结果没有意义**，它只反映"服务还没起"。
     *
     * 这里用"Init 后重探 + 短暂重试"，把"开机第一轮必然失败"这个坑填掉。
     * 最多等 5×100ms，只在服务真没起来时才付这个代价。 */
    if (!g_algo_ok) {
        int i;
        for (i = 0; i < 5; i++) {
            FH_UINT32 bv1 = 0, bv2 = 0;
            FH_SINT32 r2 = FH_AC_Get_Algo_Version(&bv1, &bv2);
            if (r2 == 0) {
                g_algo_ok = 1;
                rt_kprintf("[audio] algo ready after Init (try %d) ver=%x.%x\n",
                           i + 1, (unsigned)bv1, (unsigned)bv2);
                break;
            }
            rt_thread_mdelay(100);
        }
        if (!g_algo_ok)
            rt_kprintf("[audio] algo STILL not ready after Init -> mix unusable\n");
    }

    FH_AC_AI_Get_Feature_ChannelMask(&support_mask);
    if (!support_mask) support_mask = 1;      /* 兜底：mic0 */

    memset(&ac_config, 0, sizeof(ac_config));
#ifndef CONFIG_ARCH_MC632X
    ac_config.codec_sel = FH_AC_CODEC_INTERNAL;   /* 开发板走片内 ACODEC */
#endif
    /* 采样率/位宽直接用 SDK 枚举常量，避免"整数赋给枚举"在不同 GCC 版本下
     * 可能触发的 -Wassign-enum 之类警告（-Werror 下即失败）。 */
    ac_config.io_type       = g_io_type ? FH_AC_LINE_IN : FH_AC_MIC_IN;
    ac_config.sample_rate   = AC_SR_16K;
    ac_config.bit_width     = AC_BW_16;
    ac_config.enc_type      = FH_PT_LPCM;
    ac_config.period_size   = AUDIO_PERIOD_SAMPLES;
    /* ★★ 固定为 1，不要用 support_mask。
     *
     * 手册 13.13.5：channel_mask "表示配置几个音频输入通道，bit0 对应 mic0"，
     * 且 13.13.10 补了一句"对于多数芯片来说，仅支持一个通道，因此
     * channel_mask 等于 1 即可"。
     * 官方 app/audio_demo/src/audio_api_demo.c:228 的写法也是
     *     int channel_mask = 1;   // default enable mic0
     * 只有当 1 不在 support 里时才回退到 support_chmask。
     *
     * 我们原来直接写 channel_mask = support_mask。本板只有 1 路模拟 MIC
     * （J20 跳线选 MIC 或 LINE_IN，二选一），若 support 返回 7 就会开
     * 3 个通道，其中 ch1/ch2 恒为 0（实测正是如此），还让实时通道被重新分配。
     * 旧代码里 close() 调 AI_Disable，使得**第二次 open 起** Set_Config
     * 真正生效，于是这个 7 被真正写进硬件 —— 前几轮测试里"配置看着对、
     * 数据却不是那个数据"就是这么来的。 */
    ac_config.channel_mask  = 1;
    ac_config.frame_num     = 0;

    /* 以下几步 ARC 侧 audio_init() 开机时已经做过一遍，参数与我们完全一致，
     * 重复下发是幂等的；即使返回"已配置/已使能"类错误也继续往下走。
     * 特别注意：**不要**在这里调 FH_AC_DeInit()——那会拆掉 ARC 侧持有的
     * 音频服务，而那个资源并不是我们创建的。真正的成败由 GetFrame 判定。 */
#if AUDIO_PASSIVE_OWNER
    /* ★★ 被动模式：以下三步（Set_Config / MIX_Set_Config / AI_Enable）全部跳过。
     *
     * 手册 13.13.5 原文："此函数必须在 FH_AC_AI_Enable 之前调用。如果已经
     * 调用 FH_AC_AI_Enable 使能 AI，则必须先调用 FH_AC_AI_Disable，然后
     * 调用 FH_AC_Set_Config 配置 AI。"
     *
     * ARC 开机就 Enable 了，所以在这里 Set_Config 要么被静默忽略，要么
     * 必须先把 ARC 的流 Disable 掉才能生效 —— 两种都不是我们想要的。
     * 只读 ARC 的流，架构上最干净，也把"配置到底生效没有"这个变量彻底消掉。 */
    rt_kprintf("[audio] PASSIVE: ARC owns codec, skip Set_Config/MIX/Enable\n");
    rt_kprintf("[audio]   ARC cfg = MIC_IN 16k 16bit LPCM period%d ch_mask=1\n",
               AUDIO_PERIOD_SAMPLES);
    rt_kprintf("[audio]   feature_mask=%x (ignored on purpose)\n", (unsigned)support_mask);
#else
    ret = FH_AC_Set_Config(&ac_config);
    if (ret != 0)
        rt_kprintf("[audio] Set_Config ret=0x%x (may be already-configured)\n", (unsigned)ret);

    /* 打开虚拟 mix 通道——这一步是 frame.mix 非空的前提。
     * 官方 demo 在单通道时可能跳过它，但同工程的 usb_audio.c 无条件调用，
     * 且后续读取 frame.mix 依赖它；这里照usb_audio.c 的做法打开。 */
    {
        FH_AC_MIX_CONFIG mix;
        memset(&mix, 0, sizeof(mix));
        mix.mix_enable        = 1;
        mix.mix_channel_mask  = 1;      /* 与 ac_config.channel_mask 保持一致 */
        mix.reserved          = 0;
        if (FH_AC_AI_MIX_Set_Config(&mix) != 0)
            rt_kprintf("[audio] WARN: MIX_Set_Config fail, will use ch_data[0]\n");
    }
#endif /* AUDIO_PASSIVE_OWNER */

    /* 3A：必须在 AI_Enable 之前配置 */
    ac_enable_algorithms_apply();

    /* 麦克风模拟增益。参数与 app/audio_demo/src/audio_api_demo.c:306 完全一致：
     *   FH_AC_AI_CH_SetAnologVol(0x0f, 2, 28)
     * ★ 掩码是 0x0f 不是原来的 7 —— 掩码决定"给哪几路写增益"，
     *   没写到的通道保持默认值，而 ain_vol 的 0 档就是静音。
     * ★ 返回值必须打印：这是"模拟增益到底下没下发成功"的唯一直接证据。 */
    {
        FH_SINT32 vr = FH_AC_AI_CH_SetAnologVol(AUDIO_VOL_CH_MASK, g_mic_vol, g_ain_vol);
        rt_kprintf("[audio] SetAnologVol(mask=0x%x mic=%u ain=%u) ret=0x%x\n",
                   (unsigned)AUDIO_VOL_CH_MASK,
                   (unsigned)g_mic_vol, (unsigned)g_ain_vol, (unsigned)vr);
    }

#if AUDIO_PASSIVE_OWNER
    /* 被动模式：ARC 已经 Enable 过，这里不再重复 Enable。
     * 重复 Enable 拿到的 0x8013000e 究竟是"已使能"还是"失败"从来没定论，
     * 而被动模式下根本不需要它 —— 直接读 GetFrame 即可。 */
    {
        FH_UINT32 bsz = 0;
        FH_SINT32 qr = FH_AC_AI_QueryBufSize(&bsz);
        rt_kprintf("[audio] PASSIVE: use ARC stream, QueryBufSize ret=0x%x size=%u\n",
                   (unsigned)qr, (unsigned)bsz);
    }
#else
#if AUDIO_ENABLE_AO_BIAS
    /* ★★ 先配 AO，再一次性同步使能 AI+AO —— 与 audio_demo 的顺序完全一致。
     *
     * io_type 在 Set_Config 里是"配哪一侧"的选择器：传 MIC_IN 配 AI、
     * 传 LINE_OUT 配 AO，两者互不覆盖。证据：audio_demo 最后一次
     * Set_Config 传的就是 FH_AC_LINE_OUT，而它的 AI 侧照样录到了声音。
     *
     * 为什么要开 AO：片内 ACODEC 的 MICBIAS（驻极体偏置）取自耳机驱动器
     * 的电荷泵，AO 不使能时偏置不上电 → 麦克风不工作 → ADC 采到的是
     * 悬空输入的噪声底（实测 rms 恒 118、lo/hi 只有 ±1000 零星尖峰）。
     * audio_demo 因为同时开了 AO，同一根线、同一个耳机麦克风就能录到。 */
    {
        FH_AC_CONFIG ao_config;
        FH_SINT32 ar;

        memset(&ao_config, 0, sizeof(ao_config));
        ao_config.io_type      = FH_AC_LINE_OUT;
        ao_config.sample_rate  = AC_SR_16K;
        ao_config.bit_width    = AC_BW_16;
        ao_config.enc_type     = FH_PT_LPCM;
        ao_config.period_size  = AUDIO_PERIOD_SAMPLES;
        ao_config.channel_mask = 1;          /* AO 只支持单声道 */
        ao_config.frame_num    = 0;

        ar = FH_AC_Set_Config(&ao_config);
        if (ar != 0)
            rt_kprintf("[audio] AO Set_Config ret=0x%x (continue)\n", (unsigned)ar);

        ar = FH_AC_AO_SetAnologVol(AUDIO_AO_VOL);
        if (ar != 0)
            rt_kprintf("[audio] AO SetAnologVol ret=0x%x (continue)\n", (unsigned)ar);
    }

    ret = FH_AC_AI_AO_SYNC_Enable();
    if (ret == 0) {
        g_ao_enabled = 1;
        rt_kprintf("[audio] AI+AO sync enabled (AO vol=%d, 只喂静音 -> 给 MICBIAS 供电)\n",
                   AUDIO_AO_VOL);
    } else {
        rt_kprintf("[audio] AI_AO_SYNC_Enable ret=0x%x, fallback: AI only\n", (unsigned)ret);
        ret = FH_AC_AI_Enable();
    }
#else
    ret = FH_AC_AI_Enable();
#endif /* AUDIO_ENABLE_AO_BIAS */

    if (ret != 0) {
        rt_kprintf("[audio] AI_Enable ret=0x%x\n", (unsigned)ret);
        /* 0x8013000e 到底是"已使能"还是"使能失败"，SDK 没有文档说明。
         * 用 QueryBufSize 独立验证：只有真正 Enable 成功才拿得到缓冲大小。 */
        FH_UINT32 bsz = 0;
        FH_SINT32 qr = FH_AC_AI_QueryBufSize(&bsz);
        rt_kprintf("[audio]   QueryBufSize ret=0x%x size=%u  <- 0/有值=已使能, 报错=未使能\n",
                   (unsigned)qr, (unsigned)bsz);
        if (qr != 0) {
            rt_kprintf("[audio]   ClearBuf ret=0x%x\n",
                       (unsigned)FH_AC_AI_ClearBuf());
        }
    }
#endif /* AUDIO_PASSIVE_OWNER */

    /* 每次 open 都复位收敛计数：不能只依赖 close() 复位 —— 一旦上游
     * 中途 return（比如 pts 门控超时）没走到 close，计数会残留，
     * 下一轮就会少丢帧，把 AGC 收敛段的假信号当成语音。 */
    g_settle_done   = 0;
    g_settle_hold   = 0;
    g_settle_frames = 0;
    g_src_mix_cnt   = 0;
    g_src_ch0_cnt   = 0;
    g_src_logged    = 0;

    g_ai_enabled = 1;   /* 放行：让 audio_capture_read 去验证到底能不能取到帧 */

    /* ★★ 关键：清空启用前积压在 ARC 环形缓冲里的旧帧。
     *
     * `ac_config.frame_num = 0` 在 SDK 里的含义是"使用最大缓冲"
     * （见 app/audio_demo/src/audio_api_demo.c:291 的原注释
     *  "default 0, mean use max buffer"），而 ARC 侧 audio_init() 在**开机**
     * 时就已经 Enable 了 AI，从此一直往里写。我们通常在开机几秒到几十秒后
     * 才来读，缓冲里攒的是一大段"过去的录音"。
     *
     * 实测证据：mic_test 112 帧（标称 7168ms）只花了 722ms 就跑完 ——
     * 说明 GetFrame 一直在把积压帧快速吐出来，读到的根本不是当前时刻的声音。
     * 这解释了"全程说话"和"全程不说话"两轮结果完全一样。
     *
     * QueryBufSize 的返回值直接告诉我们积压了多少字节，
     * size / (ch_data_len) 就是积压帧数，可据此估算延迟了多少秒。
     * 必须先 ClearBuf 再采集，否则录到的第一段永远是"开机以来的历史音频"。 */
    {
        FH_UINT32 bsz = 0;
        FH_SINT32 qr = FH_AC_AI_QueryBufSize(&bsz);
        FH_SINT32 cr = FH_AC_AI_ClearBuf();
        rt_kprintf("[audio] flush stale: QueryBufSize ret=0x%x size=%u, ClearBuf ret=0x%x\n",
                   (unsigned)qr, (unsigned)bsz, (unsigned)cr);
    }
    /* 冲刷之后 pts 基准要重新建立，否则第一帧会被判成"旧帧"而空等 */
    g_have_pts = 0;
    g_last_pts = 0;

    /* 只用 %d/%x 这类基础转换符：rt_kprintf 是精简实现，不支持 %08x 的
     * 宽度与补零标志，用了会打印出多余字符。 */
    rt_kprintf("[audio] mic ready: %dHz/%dbit/mono, mask=%x, period=%d, io=%s\n",
               AUDIO_CAP_RATE, AUDIO_CAP_BITS, ac_config.channel_mask,
               AUDIO_PERIOD_SAMPLES, ac_config.io_type == FH_AC_LINE_IN ? "LINE_IN" : "MIC_IN");
    rt_kprintf("[audio] source=auto(ch0|mix 取强), "
               "等待直流伺服收敛 (|dc|<%d 连续 %d 帧, 上限 %d ms)\n",
               AUDIO_DC_SETTLE_THRESHOLD, AUDIO_DC_SETTLE_HOLD,
               AUDIO_DC_SETTLE_MAX_FRAMES * AUDIO_FRAME_MS);
    return 0;
}

/* 等待采集真正可用（直流伺服收敛完成）。
 *
 * 背景：每次 open 之后 ACODEC 的直流伺服要 3.5~4.5 秒才回零
 * （2026-10-20 实测 55 帧 = 3520ms）。这期间 audio_capture_read() 一律返回 0，
 * 数据被有意丢弃。上层若在 open 之后立刻提示"请说话"，用户开口那几秒
 * 全落在这个丢弃区里，录到的只剩静音，讯飞回一个空的 status=2
 * —— 现象就是"连上了 101，然后一句输出都没有"。
 *
 * 这里把"等收敛"从录音窗口里拿出来单独做一次：一直读到第一帧有效数据
 * （顺带消耗 64ms），然后才让上层提示用户开口。
 * 返回：0 就绪，-1 打不开/取帧失败，-2 超时。
 */
int audio_capture_wait_ready(int timeout_ms) {
    static uint8_t sink[AUDIO_PERIOD_SAMPLES * 2];
    rt_tick_t end;

    if (audio_capture_open() != 0) return -1;
    if (timeout_ms <= 0) timeout_ms = 10000;
    end = rt_tick_get() + AUDIO_MS2TICK((uint32_t)timeout_ms);

    for (;;) {
        int r = audio_capture_read(sink, sizeof(sink));
        if (r > 0) return 0;      /* 收敛完成，已有有效帧 */
        if (r < 0) return -1;     /* 真错误：一帧都取不到 */
        if (rt_tick_get() > end) return -2;
    }
}

int audio_capture_read(uint8_t *buf, int len) {
    FH_AC_AI_Frame_S frame;
    FH_SINT32 ret;
    char *src;
    int n;
    int waited = 0;

    if (!g_ai_enabled) return -1;
    if (buf == NULL || len <= 0) return -1;

    /* ★★ 取一帧数据，并用 pts 做"只前进"门控。
     *
     * ★ 勘误：曾把"112 帧只花 722ms"当成 GetFrame 非阻塞的证据 —— 错的。
     *   本工程 RT_TICK_PER_SECOND=100，rt_tick_get() 的单位是 10ms，
     *   722 tick = 7220ms ≈ 标称 7168ms，说明 GetFrame 确实按 64ms/帧阻塞，
     *   采集节奏一直是正确的。此前据此推断的"读到历史积压"不成立
     *   （QueryBufSize 也一直是 size=0，双重佐证无积压）。
     *
     * pts 门控保留：正常路径下每帧 pts 都 +64000us，条件恒真、零开销；
     * 一旦出现真正的积压或重复帧，它能把旧帧挡在门外，是廉价保险。
     * 判据用 frame.pts（微秒），不用 memcmp —— 相邻帧哪怕同源，LSB 噪声
     * 也会让 memcmp 判为"不同"，统计永远是 0，毫无意义。 */
    src = RT_NULL;
    for (;;) {
        memset(&frame, 0, sizeof(frame));
        ret = FH_AC_AI_GetFrame(&frame);

        /* AI 出一帧、AO 补一帧静音，严格 1:1。
         * 只在 GetFrame 成功时喂，取帧失败的那条路径不喂 ——
         * 那里是 10ms 一次的轮询，1:1 会导致 AO 缓冲被灌爆。 */
        if (ret == 0) ao_feed_silence();

        if (ret == 0 && frame.ch_data_len > 0) {
            /* ★ 2026-10-21：**固定单源**，不再逐帧比强弱。
             *
             * 旧逻辑是每帧比较 ch_data[0] 与 mix 的 RMS 取强者。实测结果是
             * `src stats: ch_data[0]=16 frames, mix=82 frames` —— 5 秒里
             * 82 帧选了 mix、只有 16 帧选了 ch0。
             *
             * 因为两者的强弱关系随"说没说话"反转：
             *   说话时 NR 把 mix 里的人声压掉   -> ch0 更大（实测 895 vs 77）
             *   不说话的 AGC 把底噪抬起来       -> mix 更大（本次 432 vs 556 起步）
             * 于是拼出来的是"放大后的底噪为主 + 零星真人声"，帧间电平跳变几十 dB，
             * 讯飞只会回 code=0/status=2/words=0 的空结果。
             *
             * 现在整段录音只用一个源，源在 g_force_src 里固定，可用 mic_src 切换。 */
            char *p = RT_NULL;
            /* ★ 3A 没就绪时不能信 mix（2026-10-21）。
             * 默认源改成 mix 之后，这里多了一道保护：mix 是 3A 算法的输出，
             * 现场日志里出现过 `algo probe ret=-2146238450 ver=0.0`（服务没起来），
             * 那种情况下 frame.mix 指针非空、但内容是陈旧/全零的，
             * 送上去只会得到空结果，而日志上一点异常都看不出来。
             * 算法没就绪就退回 ch_data[0]，只提示一次。 */
            if (g_force_src == AUDIO_SRC_MIX && !g_algo_ok) {
                static int warned = 0;
                if (!warned) {
                    warned = 1;
                    rt_kprintf("[audio] WARN: 3A not ready -> force ch_data[0] (mix unreliable)\n");
                }
                p = frame.ch_data[0];
                if (p == RT_NULL) p = (char *)frame.mix;
            } else if (g_force_src == AUDIO_SRC_MIX) {
                p = (char *)frame.mix;
                if (p == RT_NULL) p = frame.ch_data[0];
            } else {
                p = frame.ch_data[0];
                if (p == RT_NULL) p = (char *)frame.mix;
            }

            if (p == (char *)frame.mix)      g_src_mix_cnt++;
            else if (p != RT_NULL)           g_src_ch0_cnt++;

            if (!g_src_logged && p != RT_NULL) {
                /* 只在首帧算一次，避免每帧两次 audio_rms 的开销 */
                uint32_t r_ch = frame.ch_data[0]
                              ? audio_rms((const uint8_t *)frame.ch_data[0], (int)frame.ch_data_len) : 0;
                uint32_t r_mx = frame.mix
                              ? audio_rms((const uint8_t *)frame.mix, (int)frame.ch_data_len) : 0;
                g_src_logged = 1;
                rt_kprintf("[audio] src fixed=%s (ch0 rms=%u, mix rms=%u) 单源不混切\n",
                           (p == (char *)frame.mix) ? "mix" : "ch_data[0]",
                           (unsigned)r_ch, (unsigned)r_mx);
            }

            if (p != RT_NULL &&
                (frame.pts == 0 || !g_have_pts || frame.pts > g_last_pts)) {
                src           = p;
                g_last_pts    = frame.pts;
                g_have_pts    = (frame.pts != 0);
                g_last_pts_ms = (uint32_t)(frame.pts / 1000);
                g_last_len    = (int)frame.ch_data_len;
                g_last_rate   = (uint32_t)frame.sample_rate;
                break;
            }
        }

        if (waited >= AUDIO_FRAME_WAIT_MS) {
            /* 只用 %d/%x 这类基础转换符：rt_kprintf 是精简实现，
             * %p 与宽度/对齐标志未必支持，-Werror 下直接编译失败。 */
            rt_kprintf("[audio] no new frame in %d ms: ret=0x%x len=%d pts=%u\n",
                       AUDIO_FRAME_WAIT_MS, (unsigned)ret,
                       (int)frame.ch_data_len,
                       (unsigned)(uint32_t)(frame.pts / 1000));
            return -1;
        }
        rt_thread_mdelay(AUDIO_FRAME_POLL_MS);
        waited += AUDIO_FRAME_POLL_MS;
    }

    /* ★ 数据源选择：读 ch_data[0]，**不要**用 mix。
     *
     * mic_probe 实测（20 帧，稳态峰值）：
     *     mix           77
     *     ch_data[0]   895     <- 真实人声在这里
     *     ch_data[1]     0
     *     ch_data[2]     0
     * 逐帧对比更明显：frame 13 时 mix=77 而 ch0=895，差 11.6 倍。
     *
     * 原因：mix 是 3A(HPF+NR+AGC) 处理后的虚拟混音通道，本板上
     * **NR(降噪) 把人声当噪声滤掉了**，幅度被压到 1/10 以下。
     * 直接用 mix 会导致 AUDIO_RMS_THRESHOLD=300 永远判不过，
     * 识别流程误判"没人说话"。
     *
     * ch_data[0] 是 MIC0 的原始数据，未经 3A 破坏，幅度充足。
     * 注意 ch_data_len 是**单通道**字节长度（2048 = 1024 采样 × 2B），
     * 与 ch_data[i] 一一对应，可直接 memcpy。
     * （src 已在上面取帧的循环里选定，这里只做兜底检查。） */
    if (src == RT_NULL || frame.ch_data_len <= 0) {
        rt_kprintf("[audio] frame ok but no data (mix=%x ch0=%x len=%d)\n",
                   (unsigned)(rt_uint32_t)frame.mix,
                   (unsigned)(rt_uint32_t)frame.ch_data[0],
                   (int)frame.ch_data_len);
        return -1;
    }

    n = (int)frame.ch_data_len;
    if (n > len) n = len;
    if (n <= 0) return -1;

    /* ★ 直流伺服收敛判定（2026-10-18 实测后重写）。
     * 只需看这一帧的均值 |dc|：开机台阶是 ±4000 LSB 量级，稳态噪声
     * 只有 ±250，两者差一个数量级，判据非常干净。
     * 未收敛时返回 0（"这一帧不要"），上层必须 continue 不能 break。 */
    if (!g_settle_done) {
        const int16_t *s = (const int16_t *)src;
        int32_t sum = 0;
        int32_t dc;
        int cnt = n / 2;
        int i;

        if (cnt > 0) {
            for (i = 0; i < cnt; i++) sum += s[i];
            dc = sum / cnt;
        } else {
            dc = 0;
        }
        if (dc < 0) dc = -dc;

        g_settle_frames++;
        if (dc < AUDIO_DC_SETTLE_THRESHOLD) {
            g_settle_hold++;
        } else {
            g_settle_hold = 0;
        }

        if (g_settle_hold >= AUDIO_DC_SETTLE_HOLD) {
            g_settle_done = 1;
            rt_kprintf("[audio] DC settled: %d frames (%d ms), |dc|=%d\n",
                       g_settle_frames, g_settle_frames * AUDIO_FRAME_MS, (int)dc);
        } else if (g_settle_frames >= AUDIO_DC_SETTLE_MAX_FRAMES) {
            g_settle_done = 1;
            rt_kprintf("[audio] WARN: DC not settled after %d ms, |dc|=%d - force start\n",
                       g_settle_frames * AUDIO_FRAME_MS, (int)dc);
        } else {
            return 0;
        }
    }

    memcpy(buf, src, (size_t)n);
    return n;
}

void audio_capture_close(void) {
    if (!g_ai_enabled) return;
    /* 只 Disable 采集，**绝不调 FH_AC_DeInit()**：
     * 音频模块的生命周期归 ARC 侧 audio_init() 所有，我们只是 RPC 使用方。
     * 若在这里 DeInit，会把 ARC 持有的音频服务拆掉，导致第二次 vt 彻底失败。
     * 保持模块常驻还能让后续识别更快。 */
#if AUDIO_PASSIVE_OWNER
    /* ★ 被动模式下连 Disable 也不能调。
     * 一旦 Disable，下一次 open 里的 Set_Config 就会"真正生效"
     * （手册 13.13.5：必须先 Disable 才能重新配置），于是我们写的
     * channel_mask 会覆盖掉 ARC 的 channel_mask=1。
     * 我们只是这个流的读者，不关闭别人打开的资源。 */
#else
#if AUDIO_ENABLE_AO_BIAS
    /* AI/AO 是同步使能的，关的时候也成对关，避免 DAC 留着空转。 */
    if (g_ao_enabled) {
        FH_AC_AO_Disable();
        g_ao_enabled = 0;
    }
#endif
    FH_AC_AI_Disable();
#endif
    if (g_src_mix_cnt || g_src_ch0_cnt)
        rt_kprintf("[audio] src stats: ch_data[0]=%d frames, mix=%d frames\n",
                   g_src_ch0_cnt, g_src_mix_cnt);
    g_ai_enabled = 0;
    /* 每次重新打开都要重新等 AGC 收敛 —— 上一次的收敛状态不再有效 */
    g_settle_done   = 0;
    g_settle_hold   = 0;
    g_settle_frames = 0;
    g_src_mix_cnt   = 0;
    g_src_ch0_cnt   = 0;
    g_src_logged    = 0;
    /* pts 基准也必须复位：下次 open() 会 ClearBuf 冲刷，时间戳不连续 */
    g_have_pts = 0;
    g_last_pts = 0;
}

/* ---- 回放一段 PCM 到 LINE_OUT ----------------------------------------
 *
 * 目的：录音之后立刻让用户**用自己的耳朵**判断采到的到底是不是人声。
 * 这一步能把"音频内容不对"和"上传/协议不对"彻底分开：
 *   · 回放清楚可懂  -> 音频没问题，去查上传格式/服务端解析
 *   · 回放是噪声/闷响 -> 采集通路问题，再改识别代码也是白费
 *
 * 实现：复用 AI 的 open/close（AO 是随 AI 同步使能的），录音已经结束、
 * AI 缓冲不会被读取，让它自己转一会儿没有影响，播完立刻 close。
 * 节奏按 32 字节/毫秒（16kHz × 2 字节）推，喂快了 DAC 会欠载。 */
/* 与上面的 audio_capture_play 同一条推送路径，但**不** open/close。
 *
 * 给语音对讲（web/src/talk.c）用：对讲时音频流是常开的，如果每播一段都
 * open 一次，先不说 open 里的 FH_AC_Init/AI_Enable 会被 ARC 侧拒掉，
 * 仅"重新进入直流收敛"这一项就要 4.5 秒，用户按一下要等五秒才出声。
 *
 * 调用前提：外面已经 audio_capture_open() 过，且 AO 已随 AI 同步使能。
 * 返回：0 成功；-1 参数错或没开 AO；-2 AO 未使能。 */
int audio_capture_play_raw(uint8_t *pcm, int len) {
#if AUDIO_ENABLE_AO_BIAS
    FH_AC_AO_FRAME_S f;
    int off = 0;

    if (!pcm || len <= 0) return -1;
    if (!g_ao_enabled) return -2;

    while (off < len) {
        int n = len - off;
        if (n > AUDIO_PERIOD_SAMPLES * 2) n = AUDIO_PERIOD_SAMPLES * 2;

        f.len  = (FH_UINT32)n;
        f.data = (FH_UINT8 *)(pcm + off);
        FH_AC_AO_SendFrame(&f);

        off += n;
        rt_thread_mdelay(n / (AUDIO_CAP_RATE * 2 / 1000));   /* 2048B -> 64ms */
    }
    return 0;
#else
    (void)pcm;
    (void)len;
    return -1;
#endif
}

int audio_capture_play(uint8_t *pcm, int len) {
#if AUDIO_ENABLE_AO_BIAS
    int rc;

    if (!pcm || len <= 0) return -1;

    if (audio_capture_open() != 0) {
        rt_kprintf("[audio] play: open fail\n");
        return -1;
    }
    if (!g_ao_enabled) {
        rt_kprintf("[audio] play: AO 未使能（AUDIO_ENABLE_AO_BIAS=0 或 SYNC_Enable 失败）\n");
        audio_capture_close();
        return -2;
    }

    rt_kprintf("[audio] play back %d bytes (%d ms) -> LINE_OUT\n",
               len, (int)((long)len * 1000 / (AUDIO_CAP_RATE * 2)));

    rc = audio_capture_play_raw(pcm, len);

    audio_capture_close();
    rt_kprintf("[audio] play done\n");
    return rc;
#else
    (void)pcm;
    (void)len;
    rt_kprintf("[audio] play: 编译时关闭了 AO，无法回放\n");
    return -1;
#endif
}


/* =======================================================================
 * 模式二：内嵌 PCM（-DPCM_USE_EMBEDDED，离线调试用）
 * ======================================================================= */
#else

extern const unsigned int pcm_test_data_len;
extern const unsigned char pcm_test_data[];

static const unsigned char *g_pos  = RT_NULL;
static int                  g_left = 0;

int audio_capture_open(void) {
    g_pos  = pcm_test_data;
    g_left = (int)pcm_test_data_len;
    rt_kprintf("[audio] embedded pcm, %d bytes\n", g_left);
    return 0;
}

int audio_capture_read(uint8_t *buf, int len) {
    if (g_left <= 0) return 0;
    if (len > g_left) len = g_left;
    memcpy(buf, g_pos, (size_t)len);
    g_pos  += len;
    g_left -= len;
    return len;
}

void audio_capture_close(void) {
    g_pos  = RT_NULL;
    g_left = 0;
}

/* 离线 PCM 模式没有收敛期，开完即可用。
 * 必须提供这个桩：wait_ready 声明在公共头文件里，voice_cmd 无条件调用，
 * 否则 PCM_USE_EMBEDDED 构建会缺符号链接失败。 */
int audio_capture_wait_ready(int timeout_ms) {
    (void)timeout_ms;
    return audio_capture_open() == 0 ? 0 : -1;
}

/* 离线 PCM 模式没有 pts / 真实帧长，返回 0 让诊断打印不报错 */
uint32_t audio_capture_last_pts_ms(void) { return 0; }
int      audio_capture_last_len(void)    { return 0; }

/* 离线模式没有真实硬件：源选择与回放都是空实现。
 * 必须提供符号，否则 voice_cmd 无条件调用会链接失败。 */
int audio_capture_set_src(int which) { (void)which; return 0; }
int audio_capture_play(uint8_t *pcm, int len) { (void)pcm; (void)len; return -1; }

#endif /* AUDIO_SRC_MIC */

/* ============================ 通用：录制 ============================== */

/* 整数平方根（牛顿/逐位法），避免为一次开方引入 libm 依赖 */
static uint32_t isqrt64(uint64_t v) {
    uint64_t rem = v;
    uint64_t root = 0;
    uint64_t bit  = 1ULL << 62;

    while (bit > rem) bit >>= 2;
    while (bit) {
        if (rem >= root + bit) {
            rem -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
        bit >>= 2;
    }
    return (uint32_t)root;
}

/* 计算一段 PCM 的 RMS（先去掉直流偏置，否则静音也算很大）
 * 两个必须注意的点：
 *  1) 累加器要 64 位——单样本 v*v 最大约 1.07e9，1024 个样本就超过 uint32 上限；
 *  2) 均方值必须开平方才是 RMS，否则阈值含义会差一个平方关系。 */
uint32_t audio_rms(const uint8_t *buf, int len) {
    int n = len / 2;
    const int16_t *s = (const int16_t *)buf;
    int64_t sum = 0;
    uint64_t acc = 0;

    if (n <= 0) return 0;
    for (int i = 0; i < n; i++) sum += s[i];
    int32_t dc = (int32_t)(sum / n);

    for (int i = 0; i < n; i++) {
        int32_t v = (int32_t)s[i] - dc;
        acc += (uint64_t)(uint32_t)(v * v);
    }
    return isqrt64(acc / (uint64_t)n);   /* sqrt(均方) 才是 RMS */
}

/* =======================================================================
 * 逐通道探测：mix 与每个 ch_data[] 分别测 RMS
 *
 * 为什么要这个函数：
 *   mask=7 说明硬件开了 3 个通道（FH_AI_CHANNEL_NUM=3），但 audio_capture_read()
 *   只取 frame.mix ?: ch_data[0]。若麦克风实际接在 ch1/ch2 上，mix 里就只剩
 *   极小的底噪，看起来"通了"其实没采到人声。单看 mix 无法区分"增益不够"和
 *   "读错通道"，必须逐通道看。
 *
 * ★ 为什么同时打印"稳态峰值"和"全程峰值"：
 *   AI 通道刚打开的前两帧有启动冲击，RMS 可达数百甚至上千。若只用全程峰值，
 *   一个 400+ 的冲击会让结论完全失真（曾据此误判"信号够大"）。
 *   稳态峰值 = 排除前 2 帧后的最大值，才代表人声真正能达到的幅度。
 *
 * 判读表：
 *   ch_data[x] 稳态峰值明显高于其它  -> 人声在那个通道，改 audio_capture_read 读它
 *   所有通道稳态都是十几            -> 增益太低（mic_vol=2），调大 FH_AC_AI_CH_SetAnologVol
 *   mix 稳态远高于各 ch_data 稳态    -> 3A(AGC) 在 mix 上做了归一化，可直接用 mix
 *   只有全程峰值高、稳态低          -> 采到的只是启动冲击，仍需调增益
 * ======================================================================= */
int audio_probe_channels(FH_UINT32 mask, int frames) {
    FH_AC_AI_Frame_S frame;
    FH_SINT32 ret;
    uint32_t chmax[FH_AI_CHANNEL_NUM];      /* 全程峰值 */
    uint32_t chsteady[FH_AI_CHANNEL_NUM];   /* 排除前 2 帧后的峰值 */
    uint32_t mixmax = 0, mixsteady = 0;
    int i, ch;
    uint32_t lastlen = 0;
    uint64_t prev_pts = 0;                  /* 帧新旧判据，见下面的循环 */
    int      have_pts = 0;

    if (frames <= 0) frames = 10;
    if (frames < 3) frames = 3;             /* 至少要留出 1 帧稳态 */
    for (ch = 0; ch < FH_AI_CHANNEL_NUM; ch++) { chmax[ch] = 0; chsteady[ch] = 0; }

    if (audio_capture_open() != 0) {
        rt_kprintf("[probe] open failed\n");
        return -1;
    }
    rt_kprintf("[probe] mask=%x, %d frames, SPEAK NOW:\n", mask, frames);
    rt_kprintf("[probe] frame | mix | ch0 | ch1 | ch2   (RMS)\n");

    for (i = 0; i < frames; i++) {
        int waited = 0;
        uint32_t r_mix = 0, r_ch[FH_AI_CHANNEL_NUM];
        for (ch = 0; ch < FH_AI_CHANNEL_NUM; ch++) r_ch[ch] = 0;

        /* 同 audio_capture_read()：用 pts 判定新旧，旧帧就让出 CPU 等。
         * 不这样的话 probe 抽干的也只是缓冲里的历史音频，
         * 逐帧 RMS 与"用户说没说话"完全无关，测了也白测。 */
        for (;;) {
            memset(&frame, 0, sizeof(frame));
            ret = FH_AC_AI_GetFrame(&frame);
            if (ret == 0 && frame.pts == 0) break;              /* 无 pts：退回老行为 */
            if (ret == 0 && (!have_pts || frame.pts > prev_pts)) break;  /* 新帧 */
            if (waited >= AUDIO_FRAME_WAIT_MS) {
                rt_kprintf("[probe] no new frame at %d ret=0x%x pts=%u len=%d\n",
                           i, (unsigned)ret,
                           (unsigned)(uint32_t)(frame.pts / 1000),
                           (int)frame.ch_data_len);
                audio_capture_close();
                return -1;
            }
            rt_thread_mdelay(AUDIO_FRAME_POLL_MS);
            waited += AUDIO_FRAME_POLL_MS;
        }
        prev_pts = frame.pts;
        have_pts = (frame.pts != 0);

        lastlen = (uint32_t)frame.ch_data_len;
        if (frame.mix && frame.ch_data_len > 0) {
            r_mix = audio_rms((const uint8_t *)frame.mix, (int)frame.ch_data_len);
            if (r_mix > mixmax) mixmax = r_mix;
            if (i >= 2 && r_mix > mixsteady) mixsteady = r_mix;
        }
        for (ch = 0; ch < FH_AI_CHANNEL_NUM; ch++) {
            if (frame.ch_data[ch] && frame.ch_data_len > 0) {
                r_ch[ch] = audio_rms((const uint8_t *)frame.ch_data[ch],
                                     (int)frame.ch_data_len);
                if (r_ch[ch] > chmax[ch]) chmax[ch] = r_ch[ch];
                if (i >= 2 && r_ch[ch] > chsteady[ch]) chsteady[ch] = r_ch[ch];
            }
        }
        rt_kprintf("[probe] frame %d | mix=%u ch0=%u ch1=%u ch2=%u %s\n",
                   i, r_mix, r_ch[0], r_ch[1], r_ch[2],
                   (i < 2) ? "<- startup transient" : "");
        /* 不再 mdelay：会让采样只覆盖约 39% 的时间，看不到连续的人声 */
    }
    audio_capture_close();

    rt_kprintf("[probe] ---- summary over %d frames (ch_data_len=%u per ch) ----\n",
               frames, lastlen);
    rt_kprintf("[probe] source        peak  steady\n");
    rt_kprintf("[probe] mix           %u  %u\n", mixmax, mixsteady);
    for (ch = 0; ch < FH_AI_CHANNEL_NUM; ch++) {
        rt_kprintf("[probe] ch_data[%d]     %u  %u %s\n", ch,
                   chmax[ch], chsteady[ch],
                   (mask & (1u << ch)) ? "(in mask)" : "(not in mask)");
    }

    /* 结论只用稳态峰值下判断 —— 全程峰值会被启动瞬态污染 */
    {
        int best = -1;
        for (ch = 0; ch < FH_AI_CHANNEL_NUM; ch++)
            if (best < 0 || chsteady[ch] > chsteady[best]) best = ch;
        rt_kprintf("[probe] strongest steady channel = ch_data[%d] rms=%u ; "
                   "threshold=%d\n", best, best < 0 ? 0 : chsteady[best],
                   AUDIO_RMS_THRESHOLD);
        if (mixsteady > (best < 0 ? 0u : chsteady[best]))
            rt_kprintf("[probe] -> mix 稳态强于所有单通道，3A 归一化有效，继续用 mix\n");
        else if (best >= 0 && chsteady[best] > AUDIO_RMS_THRESHOLD)
            rt_kprintf("[probe] -> 稳态峰值超过 threshold，识别可用；"
                       "最强通道是 ch_data[%d]\n", best);
        else
            rt_kprintf("[probe] -> 所有通道稳态都低于 threshold=%d —— "
                       "若全程峰值很高那只是启动冲击。\n"
                       "[probe]    需调大 FH_AC_AI_CH_SetAnologVol 的 mic_vol（当前 2）\n",
                       AUDIO_RMS_THRESHOLD);
    }
    return 0;
}

/* ---------------- 直流阻断（一阶高通） ----------------
 *
 * 实测本通路 ch_data[0] 带 ~300 LSB 的直流偏置（日志 `DC settled ... |dc|=311`），
 * 且它并不恒定 —— 直流伺服在录音窗口里持续微调，这段"缓慢漂移"就是回放时
 * 听到的低频嗡声/隆隆声的来源之一。
 *
 * 作用范围要说清楚，别指望它解决别的问题：
 *   · **对静音检测没有影响**。audio_rms() 内部已经按帧减去均值再算均方
 *     （见该函数注释），DC 早被扣掉，所以 `rr >= AUDIO_RMS_THRESHOLD`
 *     反映的是真正的交流能量。曾误以为"DC 让 heard 恒为真"，那是错的。
 *   · **对上传的 PCM 有影响**。原始样本带着这 300 LSB 偏置，归一化时
 *     会把它一起放大 8 倍，白白吃掉一大截动态范围（且放大后的偏置在
 *     DAC 上是直流，听不出来但确实占了编码位）。所以顺序必须是
 *     "先砍 DC 再归一化"，两者不可交换。
 *
 * y[n] = x[n] - x[n-1] + R*y[n-1]，R=0.995 -> fc ≈ (1-R)*fs/(2π) ≈ 13 Hz @16k。
 * 只砍 13Hz 以下，人声基频（85~255Hz）不受影响。 */
static void audio_dc_block(int16_t *s, int ns) {
    const int32_t R = 32604;      /* 0.995 * 32768 */
    int32_t px = 0, py = 0;
    int i;

    for (i = 0; i < ns; i++) {
        int32_t x = (int32_t)s[i];
        int32_t y = x - px + ((R * py) >> 15);
        px = x;
        if (y > 32767)  y = 32767;
        if (y < -32768) y = -32768;
        py = y;
        s[i] = (int16_t)y;
    }
}

static int32_t audio_mean(const int16_t *s, int ns) {
    int32_t acc = 0;
    int i;

    if (ns <= 0) return 0;
    for (i = 0; i < ns; i++) acc += s[i];
    return acc / ns;
}

/* ---- 20ms 块级分析（2026-10-21-s）------------------------------------
 * 上传前要做的两件事都得按"块"算，不能按整段算：
 *   ① 掐首尾静音：整段最大/最小样点看不出"语音从第几秒开始"；
 *   ② 归一化：增益必须由**语音段**电平决定，不能被整段静音稀释，
 *      更不能被单个喀哒声绑架 —— 那正是上一版峰值归一化失败的原因。
 */
#define AUDIO_BLK_MS    20
#define AUDIO_BLK_SAMP  (AUDIO_CAP_RATE * AUDIO_BLK_MS / 1000)   /* 320 样点 */
#define AUDIO_BLK_MAX   512                                      /* 5s/20ms = 250 */

static int audio_blk_rms(const int16_t *s) {
    return (int)audio_rms((const uint8_t *)s, AUDIO_BLK_SAMP * 2);
}

/* 掐掉首尾静音，返回新样点数；不需要动时原样返回 ns。 */
static int audio_trim_silence(int16_t *s, int ns, int thr, int lead_ms, int tail_ms) {
    int nb, i, first = -1, last = -1, start, end, spms;

    if (ns < AUDIO_BLK_SAMP * 4) return ns;      /* 太短，别动 */
    nb = ns / AUDIO_BLK_SAMP;
    for (i = 0; i < nb; i++) {
        if (audio_blk_rms(s + i * AUDIO_BLK_SAMP) >= thr) {
            if (first < 0) first = i;
            last = i;
        }
    }
    if (first < 0) return ns;                    /* 全静音：交给上层判"没人说话" */

    spms  = AUDIO_CAP_RATE / 1000;               /* 每毫秒样点数 = 16 */
    start = (first * AUDIO_BLK_MS - lead_ms) * spms;
    end   = ((last + 1) * AUDIO_BLK_MS + tail_ms) * spms;
    if (start < 0) start = 0;
    if (end > ns) end = ns;
    if (end - start < AUDIO_CAP_RATE / 2) return ns;   /* 剪完不足 0.5s，保守放弃 */
    if (start > 0) memmove(s, s + start, (size_t)(end - start) * sizeof(int16_t));
    return end - start;
}

/* 语音段平均块 RMS。
 * 只统计"能量 ≥ 最响块 1/4（-12dB）"的块，把静音和远处的低声咕哝排除掉。
 * 用"块 RMS 的平均"而非整段真 RMS：绝对值略偏高，但**一致性极好** ——
 * 这里要的正是"每轮都落到同一个目标电平"，不是物理精确。
 *
 * out_max 回传最响那一块的 RMS（可选）。归一化要拿它当"稳健峰值"参考：
 * 块级峰值不会被单个样点尖峰绑架，比整段最大样点可靠得多。 */
static int audio_rms_speech(const int16_t *s, int ns, int *out_max) {
    static int env[AUDIO_BLK_MAX];
    int nb, i, mx = 0, thr;
    int32_t acc = 0;
    int cnt = 0;

    if (out_max) *out_max = 0;
    if (ns < AUDIO_BLK_SAMP) return 0;
    nb = ns / AUDIO_BLK_SAMP;
    if (nb > AUDIO_BLK_MAX) nb = AUDIO_BLK_MAX;
    for (i = 0; i < nb; i++) {
        env[i] = audio_blk_rms(s + i * AUDIO_BLK_SAMP);
        if (env[i] > mx) mx = env[i];
    }
    if (out_max) *out_max = mx;
    if (mx <= 0) return 0;
    thr = mx / 4;
    if (thr < AUDIO_TRIM_THR) thr = AUDIO_TRIM_THR;
    for (i = 0; i < nb; i++) {
        if (env[i] >= thr) { acc += env[i]; cnt++; }
    }
    return (cnt > 0) ? (int)(acc / cnt) : 0;
}

/* 统计按 gain 放大后会被削顶的样点数（削顶预算用）。
 * 一趟线性扫描，6 秒 96k 样点约 1~2ms，归一化每次最多跑 4 趟，可忽略。 */
static int audio_count_clip(const int16_t *s, int ns, int gain) {
    int i, c = 0;

    for (i = 0; i < ns; i++) {
        int32_t v = (int32_t)s[i] * gain;
        if (v > 32767 || v < -32768) c++;
    }
    return c;
}

/* 录音进度（2026-10-27）。
 * 网页要靠它显示"已录 x/y 秒"，而且必须用**板子的时钟**——
 * 前端自己的 setTimeout 与采集时钟无关，轮询一延迟就对不上，
 * 那正是"显示时间和实际输入不符"的来源之一。 */
static volatile int g_prog_ms   = 0;   /* 已录语音毫秒（阶段 A 为 0） */
static volatile int g_prog_lead = 0;   /* 等开口已花毫秒 */
static volatile int g_prog_live = 0;   /* 1 = 正在录 */

int audio_capture_progress(int *speech_ms, int *lead_ms)
{
    if (speech_ms) *speech_ms = g_prog_ms;
    if (lead_ms)   *lead_ms   = g_prog_lead;
    return g_prog_live ? 0 : -1;
}

/*
 * 录制。麦克风模式采用"说完即停"策略：
 * 持续录音，一旦检测到连续 AUDIO_MIN_SPEECH_MS 的静音就认为用户说完了，立刻返回。
 * 这样既不截断句子，也不会把开头/结尾的大段静音送去识别（明显影响识别率）。
 */
int audio_capture_record(uint8_t *out_buf, int buf_size, int dur_sec) {
    int need;
    int got = 0;              /* 阶段 B 已写进 out_buf 的语音字节 */
    int total = 0;            /* out_buf 实际有效字节 = 前导 + 语音 */
    int lead_ms = 0;          /* 阶段 A 等开口已花毫秒 */
    int pre_len = 0;          /* 滚动前导缓冲里当前字节数 */
    int silence_run = 0;      /* 连续静音字节数 */
    int max_silence;          /* 允许的最大连续静音字节数 */
    int min_total;            /* 最短录音字节数，防止刚起头就被截断 */
    int heard = 0;            /* 是否检测到过人声 */
    int end_reason = 0;       /* 0=录满上限 1=VAD 连续静音收尾 2=读失败 */
    int peak_rms = 0;         /* 整段录音的峰值 RMS，诊断空结果用 */
    int first_rms = -1;       /* 首帧 RMS（旧指标，保留仅供对比） */
    /* ★ 逐帧 RMS 采样（2026-10-21）。原来直接拿"首帧 RMS"当底噪，两个方向都不准：
     *   · 用 mix 时，首帧是 AGC 还没起增益的绝对静音 → rms=1，
     *     算出来 `SNR x621/x788`，纯属假象；
     *   · 用 ch0 时，首帧常撞上 codec 上电的直流大跳变 → rms 能到 19000，
     *     反而比人声大几十倍。
     * 改成把每帧 RMS 存下来，事后取分位数：p10≈底噪、med≈中位水平、max≈人声。
     * 5 秒 / period 1024 @16k ≈ 78 帧，256 的槽位绰绰有余。 */
    static int fr[256];
    int nf = 0;
    /* 阶段 A 的滚动前导缓冲（只保留最近 AUDIO_PRE_MAX 字节）+ 单帧临时缓冲 */
    static uint8_t pre[AUDIO_PRE_MAX];
    static uint8_t tmp[AUDIO_PERIOD_SAMPLES * 2];

    if (audio_capture_open() != 0) return -1;

    /* 固定时长模式优先（voice_test 4 之类）。0 表示按 dur_sec + VAD。 */
    if (g_fixed_sec > 0) dur_sec = g_fixed_sec;
    if (dur_sec <= 0) dur_sec = 4;
    /* 目标语音字节数。必须给滚动前导留出 AUDIO_PRE_MAX 的位置，
     * 否则"前导 + 语音"会超过 buf_size，尾巴被静默截掉——
     * 那就又变成"只有一部分被识别"了。 */
    need = AUDIO_CAP_RATE * AUDIO_CAP_CHN * (AUDIO_CAP_BITS / 8) * dur_sec;
    if (buf_size > AUDIO_PRE_MAX && need > buf_size - AUDIO_PRE_MAX)
        need = buf_size - AUDIO_PRE_MAX;
    if (need <= 0) { audio_capture_close(); return -1; }

    /* 说完后保留 AUDIO_MIN_SPEECH_MS 的静音尾巴，避免切掉最后一个字。
     * 注意括号位置：必须先乘后除，写成 (AUDIO_MIN_SPEECH_MS / 1000) 会被整数
     * 除法截断成 0，导致 max_silence==0，一遇到静音帧就 break（且 got 被撑大
     * 指向未初始化数据），表现为只录到一帧就没声音了。 */
    max_silence = (AUDIO_CAP_RATE * (AUDIO_CAP_BITS / 8) * AUDIO_MIN_SPEECH_MS) / 1000;
    if (max_silence < AUDIO_CAP_RATE * (AUDIO_CAP_BITS / 8) / 10)   /* 至少 100ms */
        max_silence = AUDIO_CAP_RATE * (AUDIO_CAP_BITS / 8) / 10;

    /* 最短录音：不能超过目标总长，否则失去"说完即停"的意义 */
    min_total = (AUDIO_CAP_RATE * (AUDIO_CAP_BITS / 8) * AUDIO_MIN_TOTAL_MS) / 1000;
    if (min_total > need) min_total = need;
    if (min_total < max_silence) min_total = max_silence;   /* 别短于一个静音尾巴 */

    /* ================= 阶段 A：等开口（不计入 dur_sec）=================
     * 网页靠轮询显示"请说话"，用户真正看到时窗口已经跑掉 0.6~2 秒
     * （2 秒基础轮询 + 浏览器后台节流）。这里把"等"和"录"彻底分开：
     * 读到第一帧人声之前不消耗 dur_sec，只把最近 AUDIO_PRE_MAX 字节
     * 滚动保存下来当前导，第一个字不会被切掉。
     * 这样"什么时候看到提示"不再影响录到多少内容。 */
    g_prog_ms   = 0;
    g_prog_lead = 0;
    g_prog_live = 1;

    while (!heard) {
        int r = audio_capture_read(tmp, sizeof(tmp));

        if (r == 0) continue;                  /* 收敛期丢帧，不是错误 */
        if (r < 0) { end_reason = 2; break; }

        /* 滚动前导：超了丢最老的字节，只留最近 AUDIO_PRE_MAX */
        if (r >= AUDIO_PRE_MAX) {
            memcpy(pre, tmp + r - AUDIO_PRE_MAX, (size_t)AUDIO_PRE_MAX);
            pre_len = AUDIO_PRE_MAX;
        } else {
            if (pre_len + r > AUDIO_PRE_MAX) {
                int drop = pre_len + r - AUDIO_PRE_MAX;
                memmove(pre, pre + drop, (size_t)(pre_len - drop));
                pre_len -= drop;
            }
            memcpy(pre + pre_len, tmp, (size_t)r);
            pre_len += r;
        }

        if (audio_rms(tmp, r) >= AUDIO_RMS_THRESHOLD) { heard = 1; break; }

        lead_ms += AUDIO_FRAME_MS;
        g_prog_lead = lead_ms;
        if (lead_ms >= AUDIO_LEAD_MAX_MS) {
            g_prog_live = 0;
            audio_capture_close();
            rt_kprintf("[audio] no speech in %d ms (lead-in timeout)\n",
                       AUDIO_LEAD_MAX_MS);
            return -2;                         /* 语义与"全程静音"一致 */
        }
    }

    if (!heard) {                              /* 阶段 A 读失败，一帧都没拿到 */
        g_prog_live = 0;
        audio_capture_close();
        rt_kprintf("[audio] capture error during lead-in\n");
        return -1;
    }

    /* 前导先落到输出缓冲，语音部分从 total 处继续写 */
    memcpy(out_buf, pre, (size_t)pre_len);
    total = pre_len;
    rt_kprintf("[audio] speech onset after %d ms, pre-roll %d ms, target %d ms\n",
               lead_ms, (int)((long)pre_len * 1000 / (AUDIO_CAP_RATE * 2)),
               dur_sec * 1000);

    /* ================= 阶段 B：从人声起点录满 dur_sec ================= */
    while (got < need) {
        int room = buf_size - total;
        int r;

        if (room <= 0) break;
        r = audio_capture_read(out_buf + total, room);
        /* r == 0 不是错误，而是"AGC 收敛期，这一帧被有意丢弃"。
         * 必须 continue 而不是 break —— 否则刚开始录音就会退出，一帧不录。
         * r < 0 才是真错误。 */
        if (r == 0) continue;
        if (r < 0) { end_reason = 2; break; }

        /* 记录本段峰值 RMS：识别返回空结果时，这一项能直接区分
         * "送进去的本来就是静音" 与 "有人声但服务端没识别出来"。
         * 只多一次比较，代价可以忽略。 */
        uint32_t rr = audio_rms(out_buf + total, r);
        if ((int)rr > peak_rms) peak_rms = (int)rr;
        if (first_rms < 0) first_rms = (int)rr;
        if (nf < (int)(sizeof(fr) / sizeof(fr[0]))) fr[nf++] = (int)rr;

        if (rr >= AUDIO_RMS_THRESHOLD) {
            silence_run = 0;
        } else {
            silence_run += r;
        }
        total += r;
        got   += r;                           /* 先无条件计入，再判断是否说完了 */
        g_prog_ms = (int)((long)got * 1000 / (AUDIO_CAP_RATE * 2));

        /* 只有"录够最短时长"之后才允许因静音而结束。
         * 否则一句话刚起头（heard=1 但才 200ms）就会被尾随静音立刻截断，
         * 实测曾只录到 748ms 就返回，音频被腰斩送去识别。
         * g_fixed_sec > 0 时完全不走这条路：用户要的是确定的时长。 */
        if (g_fixed_sec <= 0 && silence_run >= max_silence && got >= min_total) {
            /* 说完了：掐掉超出 max_silence 的那段静音尾巴。
             * 注意 total 要跟着收，否则前导之后的长度和 got 对不上。 */
            got -= (silence_run - max_silence);
            if (got < 0) got = 0;
            total = pre_len + got;
            g_prog_ms = (int)((long)got * 1000 / (AUDIO_CAP_RATE * 2));
            end_reason = 1;
            break;
        }
    }

    audio_capture_close();
    g_prog_live = 0;

    if (got <= 0) return -1;

    /* 下游（掐静音 / 归一化 / 打印 / 返回长度）一律按"总有效字节"处理，
     * 这样新加的前导不需要改后面任何一行。 */
    got = total;

    /* ★ 把"这一轮为什么结束"打出来（2026-10-21）。
     * 现场诉求是"识别的时间不好判断"——录音窗口到底是几秒、有没有提前收尾，
     * 光看 `captured 160000 bytes` 只能反推，还得自己算。现在直接说清楚：
     *   fixed dur -> 用了固定时长模式（voice_test N），到点必停，最可预测
     *   VAD 静音  -> 说完 N 秒后自动收尾（阈值现为 30，见 audio_capture.h）
     *   hit cap   -> 一直没判出"说完"，录满上限
     *   read err  -> 取帧中断，本轮音频可能被截断 */
    rt_kprintf("[audio] record end: %s, %d ms, %d frames (thr=%d)\n",
               (end_reason == 1) ? "VAD silence" :
               ((end_reason == 2) ? "read error" :
                ((g_fixed_sec > 0) ? "fixed dur" : "hit cap")),
               (int)((long)got * 1000 / (AUDIO_CAP_RATE * 2)), nf,
               AUDIO_RMS_THRESHOLD);

    /* ★ 语音活动时间轴（2026-10-21-s）。
     * "识别的时间不好判断"的根子在于：用户完全不知道录音窗口里
     * 哪几段被判定成人声。这里一帧一个字符，一眼就能对上——
     *   '#' 明显人声(frame rms >= 阈值)  '+' 弱人声(>= 阈值/3)  '-' 静音
     * 正常结果应当是 `--++++####++++--` 这种一小段连续字符；
     * 若全程 '-'，说明录音窗口里麦克风根本没听到你说话（提示与开口错位）；
     * 若全程都是 '+'（没有 '#'），说明电平太低，VAD 永远等不到"说完"。
     * 每帧 64ms（period=1024 @16k）。分两段打：rt_kprintf 只有 128B 缓冲。 */
    {
        char act[100];
        int i, n = nf;

        if (n > (int)sizeof(act) - 1) n = (int)sizeof(act) - 1;
        for (i = 0; i < n; i++) {
            int e = fr[i];
            act[i] = (e >= AUDIO_RMS_THRESHOLD) ? '#'
                   : ((e >= AUDIO_RMS_THRESHOLD / 3) ? '+' : '-');
        }
        act[n] = '\0';
        if (n > 45) {
            char tail[60];
            memcpy(tail, act + 45, (size_t)(n - 45));
            tail[n - 45] = '\0';
            act[45] = '\0';
            rt_kprintf("[audio] act  0s: %s\n", act);
            rt_kprintf("[audio] act  3s: %s   (#人声 +弱 -静音)\n", tail);
        } else {
            rt_kprintf("[audio] act: %s   (#人声 +弱 -静音)\n", act);
        }
    }

    /* ★ 直流阻断（2026-10-21）。顺序不能反：必须先砍 DC 再归一化，
     * 否则那 ~300 LSB 的偏置会被一起放大 8 倍，吃掉一大截动态范围。
     * 注意它**不影响**静音检测 —— audio_rms() 本来就按帧减均值。 */
    {
        int ns = got / 2;
        int16_t *s = (int16_t *)out_buf;
        int32_t dc0 = audio_mean(s, ns);
        audio_dc_block(s, ns);
        rt_kprintf("[audio] dc block: mean %d -> %d\n",
                   (int)dc0, (int)audio_mean(s, ns));
    }

    /* ★ 掐掉首尾静音（2026-10-21-s）。必须放在归一化**之前**：
     * 否则整段静音会把"语音段 RMS"稀释，增益算偏小。 */
    {
        int ns = got / 2;
        int16_t *s = (int16_t *)out_buf;
        int ns2 = audio_trim_silence(s, ns, AUDIO_TRIM_THR,
                                     AUDIO_TRIM_LEAD_MS, AUDIO_TRIM_TAIL_MS);
        if (ns2 > 0 && ns2 < ns) {
            rt_kprintf("[audio] trim %d ms -> %d ms (少传 %d ms)\n",
                       (int)((long)ns * 1000 / AUDIO_CAP_RATE),
                       (int)((long)ns2 * 1000 / AUDIO_CAP_RATE),
                       (int)((long)(ns - ns2) * 1000 / AUDIO_CAP_RATE));
            got = ns2 * 2;
        }
    }

    /* ★ 语音段 RMS 归一化（2026-10-21-s，替换峰值归一化；2026-10-27 修限幅）。
     *
     * 增益由**语音段块 RMS**决定，这一步是对的；错的是后面的限幅方式。
     * 原来写 `lim = 30000/peak; if (gain > lim) gain = lim;`，
     * peak 取的是整段最大样点，一个孤立尖峰就能把 lim 压到 1，
     * 于是 gain 变成 1、整段不放大 —— 现场日志正是
     *     norm off (speech rms 768 ok, peak 16409)
     * 语音只剩 -32dBFS，讯飞只认得出最响的那一小段。
     *
     * 现在改成"稳健峰值 + 削顶预算"：
     *   ① 稳健峰值 = 最响的 20ms 块 RMS × AUDIO_NORM_CREST，
     *      块级统计不会被单点尖峰绑架；
     *   ② 若仍超出预算（AUDIO_NORM_CLIP_PPM），按 3/4 逐步回退增益，
     *      而不是一步砍到 1。
     * 代价是允许削掉 ≤0.5% 的样点 —— 对 ASR 完全无害，换来 12dB 电平。 */
    {
        int ns = got / 2;
        int16_t *s = (int16_t *)out_buf;
        int peak = 0;
        int maxblk = 0;
        int srms = audio_rms_speech(s, ns, &maxblk);
        int i;

        for (i = 0; i < ns; i++) {
            int v = s[i];
            if (v < 0) v = -v;
            if (v > peak) peak = v;
        }

        if (peak > AUDIO_NORM_MIN_PEAK && srms > 10) {
            int gain = AUDIO_NORM_RMS_TARGET / srms;
            int rpeak = maxblk * AUDIO_NORM_CREST;   /* 稳健峰值估计 */
            int clip = 0;
            int budget;

            if (gain > AUDIO_NORM_MAX_GAIN) gain = AUDIO_NORM_MAX_GAIN;
            if (rpeak < 1) rpeak = 1;
            if (gain > AUDIO_NORM_PEAK_LIMIT / rpeak)
                gain = AUDIO_NORM_PEAK_LIMIT / rpeak;
            if (gain < 1) gain = 1;

            budget = (ns * AUDIO_NORM_CLIP_PPM) / 1000000;
            /* 逐步回退，最多 5 轮，够从 32 降到 1 还有余 */
            for (i = 0; i < 5 && gain > 1; i++) {
                clip = audio_count_clip(s, ns, gain);
                if (clip <= budget) break;
                gain = gain * 3 / 4;
            }
            if (gain < 1) gain = 1;
            if (gain == 1) clip = 0;

            if (gain > 1) {
                for (i = 0; i < ns; i++) {
                    int32_t v = (int32_t)s[i] * gain;
                    if (v > 32767)  v = 32767;
                    if (v < -32768) v = -32768;
                    s[i] = (int16_t)v;
                }
                rt_kprintf("[audio] norm x%d (rms %d->%d peak %d blk %d clip %d/%d)\n",
                           gain, srms, srms * gain, peak, maxblk, clip, ns);
            } else {
                rt_kprintf("[audio] norm off (rms %d >= target, peak %d blk %d)\n",
                           srms, peak, maxblk);
            }
        } else {
            rt_kprintf("[audio] norm skip (peak %d, speech rms %d)\n", peak, srms);
        }
    }

#ifdef AUDIO_SRC_MIC
    if (!heard) {
        rt_kprintf("[audio] no speech detected, please speak\n");
        return -2;                           /* 全程静音：上层提示"没人说话" */
    }
#endif
    /* 只用 %d：RT-Thread 的 rt_kprintf 不支持浮点(%.2f)，GCC 的格式检查会在
     * -Werror 下直接报错，并把后续所有头文件的类型解析带崩，产生上千行噪声。
     * 这行放在 #ifdef 外面：peak_rms 在两种模式下都会统计，
     * 若只在 MIC 分支里读，PCM_USE_EMBEDDED 构建会报 "set but not used"。 */
    rt_kprintf("[audio] captured %d bytes (%d ms), peak rms=%d, first frame rms=%d\n",
               got, (int)((long)got * 1000 / (AUDIO_CAP_RATE * 2)),
               peak_rms, first_rms);
    {
        /* 排序后取分位数：p10 ≈ 底噪，med ≈ 中位电平，max = 人声峰值。
         * 判读（两路要横向比，不要只看绝对值）：
         *   max/p10 < 3   -> 底噪压住人声，识别基本无望，先降噪
         *   max/p10 3~10  -> 能识别但脆弱
         *   max/p10 > 10  -> 正常
         * 注意：peak 归一化(norm xN)在**这之后**才做，这里报的都是原始电平。 */
        int p10 = 0, p25 = 0, med = 0;
        if (nf > 0) {
            int i, j;
            for (i = 1; i < nf; i++) {
                int v = fr[i];
                for (j = i - 1; j >= 0 && fr[j] > v; j--) fr[j + 1] = fr[j];
                fr[j + 1] = v;
            }
            p10 = fr[nf / 10];
            p25 = fr[nf / 4];
            med = fr[nf / 2];
        }
        rt_kprintf("[audio] frame rms: p10=%d p25=%d med=%d max=%d n=%d\n",
                   p10, p25, med, peak_rms, nf);
        rt_kprintf("[audio] SNR(peak/p10) x%d\n",
                   (p10 > 0) ? (peak_rms / p10) : 0);
    }
    return got;
}
