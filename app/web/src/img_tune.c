/*
 * img_tune.c - ISP 画面参数在线调节
 *
 * 为什么单独一个文件：
 *   webd.c 只该关心"几个整数"，不该被 isp_api.h / isp_common.h 的
 *   一大堆类型定义污染（那几个头里还有和 lwIP、RTT 重名的风险）。
 *   这里按 `#include "isp/isp_common.h"` 的写法引，和 sample_common 一致。
 *
 * 关键做法：**每次都 Get 原值 -> 只改一个字段 -> Set 回去**。
 *   直接 memset 一个结构体再填字段是危险的：ISP_BRIGHTNESS_CFG 里有
 *   s08BrtMap[GAIN_NODES]、ISP_SAT_CFG 里有 u08Gain[32][4] 这些映射表，
 *   归零等于把色彩增强整段关掉，画面会明显发灰 —— 那不是"调亮度"，
 *   是"顺手把别的功能砸了"。现场很难查回到这里。
 *
 * 另一点：bGainMappingEn 必须置 0。它为 1 时驱动用 Map 表而不是手动的
 * s08Brt/u08Crt/u08Sat，滑块就会看起来"完全没反应"。
 *
 * ★ 2026-10-28 两路拆开（现场：实拍正常，AI 主路发红）
 *
 *   实测同一时刻两路 MJPEG 帧的通道均值：
 *       G0 (:1111, 1920x1088)  R/G=1.025  B/G=0.983   ← 中性，正常
 *       G1 (:1112,  800x480)  R/G=1.220  B/G=1.059   ← 明显偏暖/偏红
 *   两颗 sensor、两套独立 ISP + AWB，收敛点不同是**物理事实**，不是 bug。
 *   真正的 bug 是我们把 AWB 补偿基线存成了**全局一对**：
 *     g_wb_r0/g_wb_b0 只从最先读到的那一路取，然后给两路都写同一个绝对值。
 *   于是调一次白平衡，G1 就被塞进 G0 的色偏量，越调越歪。
 *
 *   现在：brt/crt/sat/mir/flp/rgn/ggn/bgn 和 AWB 基线全部按 dev 分开。
 *   另加 rgn/ggn/bgn 三个**连续**偏移（±50%），现场看着画面拉滑块就能把
 *   AI 主路的偏红拧回来，不必依赖"暖/冷"这种 12% 的粗档。
 */

#include <rtthread.h>
#include <string.h>
#include <stdlib.h>   /* atoi：img 命令用，别指望 rtthread.h 一定带进来 */

#include "isp/isp_api.h"
#include "isp/isp_common.h"

#include "img_tune.h"

#define IMG_LOG(fmt, ...)  rt_kprintf("[img] " fmt, ##__VA_ARGS__)

/* 中性默认值：只在 Get 完全失败时兜底显示，不会写进驱动 */
#define IMG_DEF_BRT   0
#define IMG_DEF_CRT   64     /* U2.6：64 = 1.0 倍 */
#define IMG_DEF_SAT   32     /* U3.5：32 = 1.0 倍 */

/* R/B 补偿增益的 1.0 倍值（U.6） */
#define IMG_ENH_UNIT  64

static img_state_t g_cur[IMG_DEV_MAX];
static img_state_t g_base[IMG_DEV_MAX];
static int         g_dev_ok[IMG_DEV_MAX];
static int         g_inited = 0;

/* 图像模式 / 白平衡是**软件状态**，ISP 里读不回来，只能自己记。
 * g_mode = -1 表示用户手动拧过滑块，已经不对应任何预设。 */
static int g_mode = -1;
static int g_wb   = IMG_WB_AUTO;

/* 作用路：-1 = 两路一起改（保持旧行为），0/1 = 只改那一路 */
static int g_sel = IMG_DEV_BOTH;

/* AWB 结构体不小（含色温/场景/手动增益几张表），放静态，
 * 别让它压在 webd 那 8KB 线程栈上。webd 是单线程，不存在重入。 */
static AWB_DEFAULT_CFG g_awb_cfg;
/* "一键校色"的回滚快照：写增益之前把原始配置整份存下来，
 * 一旦复测发现越校越偏，原样写回去。 */
static AWB_DEFAULT_CFG g_awb_snap;
static ISP_AWB_GAIN   g_gain_snap[IMG_DEV_MAX];
static FH_UINT8        g_wb_r0[IMG_DEV_MAX];
static FH_UINT8        g_wb_b0[IMG_DEV_MAX];
static FH_UINT8        g_wb_g0[IMG_DEV_MAX];
static FH_UINT8        g_wb_base_ok[IMG_DEV_MAX];

/*
 * 手动白平衡（2026-10-28 新增）
 *
 * g_manual[dev]=1 时不再走 stAwbCompCfg 补偿表，而是直接把 ISP_AWB_GAIN
 * 写进硬件寄存器 —— 这是驱动文档里明确"设置awb增益至硬件寄存器"的那个口，
 * 写完立刻生效，AWB 环路也不会覆写（bMwbEn 同时被置 1）。
 *
 * 为什么需要这条路：现场调右侧偏色，拖 rgn/ggn/bgn 画面"纹丝不动"。
 * 补偿表要 bCompEn=1 才算，而且乘在 AWB 输出之后，一旦某版驱动忽略这个
 * 开关，滑块就成了摆设 —— 从外部完全看不出来。直接写增益则一定动，
 * 是排查"到底生效没生效"最干净的对照实验。
 */
static FH_UINT8       g_manual[IMG_DEV_MAX];
static ISP_AWB_GAIN   g_mwb_base[IMG_DEV_MAX];   /* 锁定时那组增益，U.9，512 = 1.0 */

#define IMG_GAIN_UNIT  512     /* ISP_AWB_GAIN 是 U.9 */
#define IMG_GAIN_MAX   0x1fff  /* 驱动范围 [0, 0x1fff] */

/* 图像模式预设：brt / crt(U2.6，64=1.0) / sat(U3.5，32=1.0) */
typedef struct
{
    int brt;
    int crt;
    int sat;
} img_preset_t;

static const img_preset_t g_presets[IMG_MODE_NUM] =
{
    {   0, 64, 32 },   /* 标准 */
    {  20, 72, 40 },   /* 明亮 */
    {   6, 52, 24 },   /* 柔和 */
    {   4, 78, 60 },   /* 鲜艳 */
    {  38, 88, 16 },   /* 夜视：提亮 + 压饱和，噪点没那么花 */
};

static int img_clamp(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* ---------------- 读 ---------------- */

/* 返回成功读到的项数（0 表示这个 dev 根本不可读，通常是该路没初始化） */
static int read_one(int dev, img_state_t *st)
{
    ISP_BRIGHTNESS_CFG b;
    ISP_CONTRAST_CFG   c;
    ISP_SAT_CFG        s;
    FH_BOOL mir = 0;
    FH_BOOL flp = 0;
    int got = 0;

    memset(&b, 0, sizeof(b));
    if (API_ISP_GetBrightnessCfg((FH_UINT32)dev, &b) == 0)
    {
        st->brt = (int)b.s08Brt;
        got++;
    }

    memset(&c, 0, sizeof(c));
    if (API_ISP_GetContrastCfg((FH_UINT32)dev, &c) == 0)
    {
        st->crt = (int)c.u08Crt;
        got++;
    }

    memset(&s, 0, sizeof(s));
    if (API_ISP_GetSaturation((FH_UINT32)dev, &s) == 0)
    {
        st->sat = (int)s.u08Sat;
        got++;
    }

    if (API_ISP_GetMirrorAndflip((FH_UINT32)dev, &mir, &flp) == 0)
    {
        st->mir = mir ? 1 : 0;
        st->flp = flp ? 1 : 0;
        got++;
    }

    return got;
}

/* 逐路刷新缓存。软件状态（mode/wb/rgn/bgn）在最后统一补上。 */
static void refresh(void)
{
    int dev;

    for (dev = 0; dev < IMG_DEV_MAX; dev++)
    {
        img_state_t t;

        memset(&t, 0, sizeof(t));
        t.brt = IMG_DEF_BRT;
        t.crt = IMG_DEF_CRT;
        t.sat = IMG_DEF_SAT;

        /* 关键：**不要**用一路的值兜底另一路。
         * 旧代码 read_all() 把所有 dev 读进同一个结构体，G1 的读失败会被
         * G0 的值掩盖，网页上看起来"两路一致"，实际 G1 压根没被读到。 */
        if (read_one(dev, &t) > 0)
        {
            t.ok       = 1;
            t.rgn      = g_cur[dev].rgn;
            t.ggn      = g_cur[dev].ggn;
            t.bgn      = g_cur[dev].bgn;
            t.mwb      = g_manual[dev];
            g_cur[dev] = t;
            g_dev_ok[dev] = 1;
        }
        else
        {
            g_cur[dev].ok = 0;
            g_dev_ok[dev] = 0;
        }
    }

    for (dev = 0; dev < IMG_DEV_MAX; dev++)
    {
        g_cur[dev].mode = g_mode;
        g_cur[dev].wb   = g_wb;
    }
}

/* 显示用的那一路：作用路指定了就给它，否则取第一路可读的（通常 G0 实拍） */
static int view_dev(void)
{
    int dev;

    if (g_sel >= 0 && g_sel < IMG_DEV_MAX) return g_sel;

    for (dev = 0; dev < IMG_DEV_MAX; dev++)
    {
        if (g_dev_ok[dev]) return dev;
    }
    return 0;
}

/* ---------------- 写 ---------------- */

static int set_brt(int dev, int v)
{
    ISP_BRIGHTNESS_CFG c;

    memset(&c, 0, sizeof(c));
    if (API_ISP_GetBrightnessCfg((FH_UINT32)dev, &c) != 0) return -1;

    c.bYcEn          = 1;      /* 打开亮度控制 */
    c.bGainMappingEn = 0;      /* 必须手动，否则 s08Brt 被 Map 表顶掉 */
    c.s08Brt         = (FH_SINT8)v;

    return (API_ISP_SetBrightnessCfg((FH_UINT32)dev, &c) == 0) ? 0 : -2;
}

static int set_crt(int dev, int v)
{
    ISP_CONTRAST_CFG c;

    memset(&c, 0, sizeof(c));
    if (API_ISP_GetContrastCfg((FH_UINT32)dev, &c) != 0) return -1;

    c.bYcEn          = 1;
    c.bGainMappingEn = 0;
    c.u08Crt         = (FH_UINT8)v;

    return (API_ISP_SetContrastCfg((FH_UINT32)dev, &c) == 0) ? 0 : -2;
}

static int set_sat(int dev, int v)
{
    ISP_SAT_CFG s;

    memset(&s, 0, sizeof(s));
    if (API_ISP_GetSaturation((FH_UINT32)dev, &s) != 0) return -1;

    s.bSatEn         = 1;
    s.bGainMappingEn = 0;
    s.u08Sat         = (FH_UINT8)v;

    return (API_ISP_SetSaturation((FH_UINT32)dev, &s) == 0) ? 0 : -2;
}

/* 镜像和翻转是同一个 API，必须"读-改-写"，否则设置其中一个会把另一个清掉 */
static int set_mirflp(int dev, int set_mir, int val)
{
    FH_BOOL mir = 0;
    FH_BOOL flp = 0;

    if (API_ISP_GetMirrorAndflip((FH_UINT32)dev, &mir, &flp) != 0) return -1;

    if (set_mir) mir = val ? 1 : 0;
    else         flp = val ? 1 : 0;

    return (API_ISP_SetMirrorAndflip((FH_UINT32)dev, mir, flp) == 0) ? 0 : -2;
}

/*
 * 单路的白平衡补偿。rp/gp/bp 是**相对该路基线**的百分比（-50~50）。
 *
 * 走 stAwbCompCfg.u08REnhance / u08GEnhance / u08BEnhance：这三个是加在 AWB
 * 输出之后的补偿层，不会被 AWB 环路拉回去（直接改 stAwbManualCfg 的 gain
 * 会被下一次收敛覆盖，滑块拖完两秒画面自己回去，现场会以为"没生效"）。
 *
 * 基线为 0 时用 64（U.6 的 1.0）起跳：0 在这套驱动里表示"该项未启用"，
 * 乘任何百分比还是 0，用户怎么拖都没反应。1.0 倍本身不改变画面。
 */
static int set_wb_gain(int dev, int rp, int gp, int bp)
{
    int base_r;
    int base_g;
    int base_b;

    /*
     * 手动模式：直接改硬件增益。
     *
     * 百分比作用在"锁定那一刻的增益"（g_mwb_base）上，而不是作用在补偿表上。
     * rgn=-20 就是实打实把 R 增益压 20%，一定改变画面，
     * 不会再出现"滑块拖到底、画面一动不动"。
     */
    if (g_manual[dev])
    {
        ISP_AWB_GAIN g = g_mwb_base[dev];

        g.u16Rgain = (FH_UINT16)img_clamp((int)g.u16Rgain * (100 + rp) / 100, 0, IMG_GAIN_MAX);
        g.u16Ggain = (FH_UINT16)img_clamp((int)g.u16Ggain * (100 + gp) / 100, 0, IMG_GAIN_MAX);
        g.u16Bgain = (FH_UINT16)img_clamp((int)g.u16Bgain * (100 + bp) / 100, 0, IMG_GAIN_MAX);

        /*
         * 两处都要写。
         *   SetAwbGain   → 直接落硬件寄存器，立即出图；
         *   SetAwbDefaultCfg.bMwbEn=1 + manual 增益 → 算法每帧重新下发现场用的
         *   就是这份配置，只写寄存器不写它，下一帧就被打回原值 ——
         *   现象正是"画面闪一下就回去了"，比完全没反应更难查。
         */
        if (API_ISP_GetAwbDefaultCfg((FH_UINT32)dev, &g_awb_cfg) == 0)
        {
            g_awb_cfg.stAwbEnableCfg.bMwbEn    = FH_TRUE;
            g_awb_cfg.stAwbManualCfg.stAwbGain = g;
            API_ISP_SetAwbDefaultCfg((FH_UINT32)dev, &g_awb_cfg);
        }

        if (API_ISP_SetAwbGain((FH_UINT32)dev, &g) != 0) return -2;
        return 0;
    }

    if (API_ISP_GetAwbDefaultCfg((FH_UINT32)dev, &g_awb_cfg) != 0) return -1;

    if (!g_wb_base_ok[dev])
    {
        g_wb_r0[dev] = g_awb_cfg.stAwbCompCfg.u08REnhance;
        g_wb_g0[dev] = g_awb_cfg.stAwbCompCfg.u08GEnhance;
        g_wb_b0[dev] = g_awb_cfg.stAwbCompCfg.u08BEnhance;
        g_wb_base_ok[dev] = 1;
        IMG_LOG("dev%d wb baseline r=%d g=%d b=%d\n",
                dev, (int)g_wb_r0[dev], (int)g_wb_g0[dev], (int)g_wb_b0[dev]);
    }

    base_r = g_wb_r0[dev] ? (int)g_wb_r0[dev] : IMG_ENH_UNIT;
    base_g = g_wb_g0[dev] ? (int)g_wb_g0[dev] : IMG_ENH_UNIT;
    base_b = g_wb_b0[dev] ? (int)g_wb_b0[dev] : IMG_ENH_UNIT;

    /*
     * ★ 必须显式打开 bCompEn。
     * 这三张补偿表只有在 bCompEn=1 时才会被算进去；默认值不一定是 1，
     * 一旦是 0，滑块拖到底画面也一动不动 —— 现象就是"参数写了但没反应"，
     * 极难从外部判断。这里无条件打开，代价只是多一层乘法。
     */
    g_awb_cfg.stAwbEnableCfg.bCompEn = FH_TRUE;

    g_awb_cfg.stAwbCompCfg.u08REnhance =
        (FH_UINT8)img_clamp(base_r * (100 + rp) / 100, 0, 255);
    g_awb_cfg.stAwbCompCfg.u08GEnhance =
        (FH_UINT8)img_clamp(base_g * (100 + gp) / 100, 0, 255);
    g_awb_cfg.stAwbCompCfg.u08BEnhance =
        (FH_UINT8)img_clamp(base_b * (100 + bp) / 100, 0, 255);

    return (API_ISP_SetAwbDefaultCfg((FH_UINT32)dev, &g_awb_cfg) == 0) ? 0 : -2;
}

/*
 * 把 AWB 从"手动锁死"里救回来。
 *
 * bMwbEn=1 时 ISP 停止统计、直接用 stAwbManualCfg 里那组固定增益，
 * 画面会一直停在出厂那一刻的色温上 —— 换环境后就是整片偏色，
 * 而且怎么调补偿都只是往上叠，回不到中性。
 * 网页上点白平衡"自动"就会走这里：bAwbEn=1 / bMwbEn=0 / bCompEn=1，
 * 等于让 AWB 重新开始收敛。
 */
static int set_awb_mode(int manual)
{
    int dev;
    int done = 0;

    for (dev = 0; dev < IMG_DEV_MAX; dev++)
    {
        if (g_sel >= 0 && dev != g_sel) continue;
        if (API_ISP_GetAwbDefaultCfg((FH_UINT32)dev, &g_awb_cfg) != 0) continue;

        g_awb_cfg.stAwbEnableCfg.bAwbEn  = FH_TRUE;
        g_awb_cfg.stAwbEnableCfg.bMwbEn  = manual ? FH_TRUE : FH_FALSE;
        g_awb_cfg.stAwbEnableCfg.bCompEn = FH_TRUE;

        if (API_ISP_SetAwbDefaultCfg((FH_UINT32)dev, &g_awb_cfg) == 0) done++;
    }

    if (done) IMG_LOG("awb mode -> %s (%d dev)\n", manual ? "manual" : "auto", done);
    return done ? 0 : -2;
}

/* 作用范围：g_sel>=0 只动那一路，否则两路都动。
 * 返回 done = 成功路数，last = 最后一次错误码。 */
static int apply_scope(const char *key, int val, int *done_out)
{
    int dev;
    int done = 0;
    int last = -1;

    for (dev = 0; dev < IMG_DEV_MAX; dev++)
    {
        int r;

        if (g_sel >= 0 && dev != g_sel) continue;

        if      (!strcmp(key, "brt")) r = set_brt(dev, val);
        else if (!strcmp(key, "crt")) r = set_crt(dev, val);
        else if (!strcmp(key, "sat")) r = set_sat(dev, val);
        else if (!strcmp(key, "mir")) r = set_mirflp(dev, 1, val);
        else if (!strcmp(key, "flp")) r = set_mirflp(dev, 0, val);
        else                          return -4;

        if (r == 0) done++;
        else        last = r;
    }

    if (done_out) *done_out = done;
    if (done > 0) return 0;
    return (last < 0) ? -1 : last;
}

/* 只改偏移值时不去碰 brt/crt/sat，但同样要按作用路写。
 * 三通道一起写：只改其中一个时，另外两个必须带着当前值重发，
 * 否则 SetAwbDefaultCfg 会把没赋值的那两个当成 0（= 关掉补偿）。 */
static int apply_wb_offset(int rp, int gp, int bp)
{
    int dev;
    int done = 0;
    int last = -1;

    for (dev = 0; dev < IMG_DEV_MAX; dev++)
    {
        int r;

        if (g_sel >= 0 && dev != g_sel) continue;

        r = set_wb_gain(dev, rp, gp, bp);
        if (r == 0)
        {
            g_cur[dev].rgn = rp;
            g_cur[dev].bgn = bp;
            g_cur[dev].ggn = gp;
            done++;
        }
        else
        {
            last = r;
        }
    }

    if (done == 0) return (last < 0) ? -1 : last;
    return 0;
}

/* ---------------- 对外接口 ---------------- */

/*
 * 读一路的实测比值（U4.12，4096 = 1.0）。
 * 返回 0 成功；-1 接口失败；-5 数值不可信。
 *
 * ★ 为什么要有 -5 这档
 *   这两个数是**除数**。统计没收敛时驱动可能回 0 或者回一个没缩放的原始值，
 *   一旦当成 4096 的比值去算，乘出来就是几十倍增益 —— 画面直接变成纯紫/纯红，
 *   而且因为写进了手动增益，还会一直停在那儿。宁可不动，也不能拿脏数据算。
 */
static int awb_ratio(int dev, int *rg, int *bg)
{
    ISP_AWB_STATUS st;

    *rg = 0;
    *bg = 0;

    memset(&st, 0, sizeof(st));
    if (API_ISP_GetAwbStatus((FH_UINT32)dev, &st) != 0) return -1;

    *rg = (int)st.u16CurrROverG;
    *bg = (int)st.u16CurrBOverG;

    if (*rg < 1024 || *rg > 16384) return -5;   /* 0.25 ~ 4.0 之外一律不信 */
    if (*bg < 1024 || *bg > 16384) return -5;
    return 0;
}

/* 距中性(4096)的最大偏差，用来判断"这次修正有没有用" */
static int ratio_err(int rg, int bg)
{
    int er = rg - 4096;
    int eb = bg - 4096;

    if (er < 0) er = -er;
    if (eb < 0) eb = -eb;
    return (er > eb) ? er : eb;
}

/*
 * 单路中性化（带闭环校验与自动回滚）。
 *
 * 思路还是"把 R/B 增益乘 4096/比值"，但加了三道闸：
 *   1) 比值必须落在 0.25~4.0；否则视为统计不可用，直接不动。
 *   2) 单次修正限幅 ±30%。宁可让用户点两次，也不能一次把增益推到极限 ——
 *      推到极限后画面变成纯紫，用户只能重烧固件。
 *   3) 写完等 300ms 复测：如果偏差变大了，说明方向反了（不同平台
 *      u16CurrROverG 到底是 R/G 还是 G/R 并不统一），立刻把增益和补偿表
 *      一起退回原样，并报错。**"一键校色"绝不能把画面弄得更糟。**
 *
 * 返回值：0 成功 / 1 本来就是中性 / -4 增益没写进去 / -5 统计不可用 /
 *         -6 越校越偏（已回滚）
 */
static int neutral_one(int dev)
{
    ISP_AWB_GAIN g0;
    ISP_AWB_GAIN g;
    int rg0 = 0;
    int bg0 = 0;
    int rg1 = 0;
    int bg1 = 0;
    int e0;
    int fr;
    int fb;

    if (awb_ratio(dev, &rg0, &bg0) != 0)
    {
        IMG_LOG("dev%d neutral: 统计不可用(R/G=%d B/G=%d)，拒绝动手\n", dev, rg0, bg0);
        return -5;
    }

    e0 = ratio_err(rg0, bg0);

    /*
     * 已经足够中性就别碰。
     * 这就是上一次"点了一键校色反而更紫"的另一半原因：偏差本来只有几个
     * 百分点时，任何一次过冲都会被眼睛放大成明显的偏色。4% 以内按中性处理。
     */
    if (e0 < 4096 * 4 / 100)
    {
        IMG_LOG("dev%d neutral: 已中性 R/G=%d.%02d B/G=%d.%02d，不改\n",
                dev, rg0 / 4096, (rg0 % 4096) * 100 / 4096,
                bg0 / 4096, (bg0 % 4096) * 100 / 4096);
        return 1;
    }

    memset(&g0, 0, sizeof(g0));
    if (API_ISP_GetAwbGain((FH_UINT32)dev, &g0) != 0) return -2;

    /* 增益没读到有效值时按 1.0 起跳，别把它乘成 0 */
    if (g0.u16Rgain < 16) g0.u16Rgain = IMG_GAIN_UNIT;
    if (g0.u16Ggain < 16) g0.u16Ggain = IMG_GAIN_UNIT;
    if (g0.u16Bgain < 16) g0.u16Bgain = IMG_GAIN_UNIT;

    g = g0;

    fr = img_clamp(4096 * 100 / rg0, 70, 140);
    fb = img_clamp(4096 * 100 / bg0, 70, 140);

    g.u16Rgain = (FH_UINT16)img_clamp((int)g0.u16Rgain * fr / 100, 0, IMG_GAIN_MAX);
    g.u16Bgain = (FH_UINT16)img_clamp((int)g0.u16Bgain * fb / 100, 0, IMG_GAIN_MAX);

    /* AWB_DEFAULT_CFG 有几百字节（色温框/白点/权重几张表），
     * 不能压在 webd 那 8KB 栈上 —— 用文件级静态，webd 是单线程不存在重入。 */
    if (API_ISP_GetAwbDefaultCfg((FH_UINT32)dev, &g_awb_snap) != 0) return -3;
    g_awb_cfg = g_awb_snap;
    g_gain_snap[dev] = g0;

    /* 第一次走到这里才记出厂补偿值，下面要用它把补偿表归位 */
    if (!g_wb_base_ok[dev])
    {
        g_wb_r0[dev] = g_awb_cfg.stAwbCompCfg.u08REnhance;
        g_wb_g0[dev] = g_awb_cfg.stAwbCompCfg.u08GEnhance;
        g_wb_b0[dev] = g_awb_cfg.stAwbCompCfg.u08BEnhance;
        g_wb_base_ok[dev] = 1;
    }

    /*
     * 补偿表必须归位。
     * 它是乘在 AWB 输出之后的另一层乘法：如果用户之前拖过 rgn=-30，
     * 0.7 倍还压在那儿，此时按 AWB 统计算出来的"中性增益"再乘上去，
     * 结果仍然是偏的，用户会以为"一键校色没用"。
     */
    g_awb_cfg.stAwbCompCfg.u08REnhance = g_wb_r0[dev];
    g_awb_cfg.stAwbCompCfg.u08GEnhance = g_wb_g0[dev];
    g_awb_cfg.stAwbCompCfg.u08BEnhance = g_wb_b0[dev];

    g_awb_cfg.stAwbEnableCfg.bAwbEn    = FH_TRUE;
    g_awb_cfg.stAwbEnableCfg.bMwbEn    = FH_TRUE;   /* 交给手动，AWB 停止覆写 */
    g_awb_cfg.stAwbManualCfg.stAwbGain = g;

    if (API_ISP_SetAwbDefaultCfg((FH_UINT32)dev, &g_awb_cfg) != 0) return -3;
    if (API_ISP_SetAwbGain((FH_UINT32)dev, &g) != 0) return -4;

    /*
     * 回读校验。这套驱动最坑的一类问题是"接口返回 0、寄存器没变"，
     * 只靠返回值根本判断不出来。
     */
    {
        ISP_AWB_GAIN chk;
        int dr;
        int db;

        memset(&chk, 0, sizeof(chk));
        if (API_ISP_GetAwbGain((FH_UINT32)dev, &chk) == 0)
        {
            dr = (int)chk.u16Rgain - (int)g.u16Rgain;
            db = (int)chk.u16Bgain - (int)g.u16Bgain;
            if (dr < 0) dr = -dr;
            if (db < 0) db = -db;

            IMG_LOG("dev%d neutral %d.%02d/%d.%02d -> gain r=%d g=%d b=%d (back r=%d b=%d)\n",
                    dev,
                    rg0 / 4096, (rg0 % 4096) * 100 / 4096,
                    bg0 / 4096, (bg0 % 4096) * 100 / 4096,
                    (int)g.u16Rgain, (int)g.u16Ggain, (int)g.u16Bgain,
                    (int)chk.u16Rgain, (int)chk.u16Bgain);

            if (dr > 8 || db > 8)
            {
                IMG_LOG("dev%d gain NOT latched (dr=%d db=%d) -> 驱动拒绝了写入\n", dev, dr, db);
                API_ISP_SetAwbDefaultCfg((FH_UINT32)dev, &g_awb_snap);
                API_ISP_SetAwbGain((FH_UINT32)dev, &g0);
                return -4;
            }
        }
    }

    /*
     * 给 AE/AWB 两三帧时间把新增益算进画面，再复测一次。
     *
     * 300ms → 180ms：15fps 下三帧就够，而 webd 是单线程，这个 mdelay 期间
     * 整个控制台都在排队（校色一按，内存/CPU/相册全停住）。能省的要省。
     */
    rt_thread_mdelay(180);

    if (awb_ratio(dev, &rg1, &bg1) == 0)
    {
        int e1 = ratio_err(rg1, bg1);

        IMG_LOG("dev%d neutral result: R/G %d.%02d -> %d.%02d, B/G %d.%02d -> %d.%02d, err %d -> %d\n",
                dev,
                rg0 / 4096, (rg0 % 4096) * 100 / 4096,
                rg1 / 4096, (rg1 % 4096) * 100 / 4096,
                bg0 / 4096, (bg0 % 4096) * 100 / 4096,
                bg1 / 4096, (bg1 % 4096) * 100 / 4096,
                e0, e1);

        if (rg1 == rg0 && bg1 == bg0)
        {
            /*
             * 统计还没刷新（读数与改前一模一样），不能据此判"越校越偏"。
             * 少了这一条，节省的 120ms 会变成误回滚：一次正确的修正被撤掉，
             * 用户看到"画面闪一下就变回原来的紫"。
             */
            IMG_LOG("dev%d neutral: 统计未刷新，跳过回滚判据\n", dev);
        }
        else if (e1 > e0)
        {
            /*
             * 越校越偏 —— 方向判断错了，或者增益根本没进画面。
             * 两种情况下继续留着这组数都是灾难：全部退回原样，把 AWB 也交回去。
             */
            API_ISP_SetAwbDefaultCfg((FH_UINT32)dev, &g_awb_snap);
            API_ISP_SetAwbGain((FH_UINT32)dev, &g_gain_snap[dev]);
            g_manual[dev] = 0;
            IMG_LOG("dev%d neutral: 偏差变大，已回滚到原增益\n", dev);
            return -6;
        }
    }

    g_mwb_base[dev] = g;
    g_manual[dev]   = 1;

    g_cur[dev].rgn = 0;
    g_cur[dev].ggn = 0;
    g_cur[dev].bgn = 0;

    return 0;
}

int img_tune_neutral(void)
{
    int dev;
    int done = 0;
    int already = 0;
    int last = -1;

    if (!g_inited) img_tune_init();

    for (dev = 0; dev < IMG_DEV_MAX; dev++)
    {
        int r;

        if (g_sel >= 0 && dev != g_sel) continue;

        r = neutral_one(dev);

        /* 1 = 本来就中性。它不是失败，网页上要显示成"无需调整"，
         * 不能一看到非 0 就报错 —— 那正是上一版让人误判"一键校色失败"的地方。 */
        if (r == 0)          done++;
        else if (r == 1)     already++;
        else                 last = r;
    }

    /* g_wb 必须在 refresh 之前改：refresh 会把全局软件状态刷进 g_cur，
     * 反过来的话网页要等下一轮轮询才看到"自定义"。 */
    g_wb = -1;   /* 不再对应 自动/暖/冷 */
    refresh();

    if (done > 0)     return 0;
    if (already > 0)  return 1;
    return last;
}

/* 退回 AWB 自动。换环境（室内→室外）后如果锁定值不合适，用这个放开。 */
int img_tune_wb_auto(void)
{
    int dev;
    int done = 0;

    if (!g_inited) img_tune_init();

    for (dev = 0; dev < IMG_DEV_MAX; dev++)
    {
        if (g_sel >= 0 && dev != g_sel) continue;

        memset(&g_awb_cfg, 0, sizeof(g_awb_cfg));
        if (API_ISP_GetAwbDefaultCfg((FH_UINT32)dev, &g_awb_cfg) != 0) continue;

        g_awb_cfg.stAwbEnableCfg.bAwbEn  = FH_TRUE;
        g_awb_cfg.stAwbEnableCfg.bMwbEn  = FH_FALSE;
        g_awb_cfg.stAwbEnableCfg.bCompEn = FH_TRUE;

        if (API_ISP_SetAwbDefaultCfg((FH_UINT32)dev, &g_awb_cfg) != 0) continue;

        g_manual[dev] = 0;
        g_cur[dev].rgn = 0;
        g_cur[dev].ggn = 0;
        g_cur[dev].bgn = 0;
        done++;
    }

    g_wb = IMG_WB_AUTO;
    refresh();

    IMG_LOG("wb -> auto (%d dev)\n", done);
    return done ? 0 : -2;
}

int img_tune_manual_get(void)
{
    int dev = view_dev();

    return g_manual[dev] ? 1 : 0;
}

void img_tune_init(void)
{
    int dev;

    if (g_inited) return;

    memset(g_cur,  0, sizeof(g_cur));
    memset(g_base, 0, sizeof(g_base));

    for (dev = 0; dev < IMG_DEV_MAX; dev++)
    {
        g_cur[dev].brt  = IMG_DEF_BRT;
        g_cur[dev].crt  = IMG_DEF_CRT;
        g_cur[dev].sat  = IMG_DEF_SAT;
        g_cur[dev].mode = -1;
        g_cur[dev].wb   = IMG_WB_AUTO;
    }

    refresh();

    for (dev = 0; dev < IMG_DEV_MAX; dev++)
    {
        g_base[dev] = g_cur[dev];
        if (g_dev_ok[dev])
        {
            IMG_LOG("init: dev%d brt=%d crt=%d sat=%d mir=%d flp=%d\n",
                    dev, g_cur[dev].brt, g_cur[dev].crt, g_cur[dev].sat,
                    g_cur[dev].mir, g_cur[dev].flp);
        }
        else
        {
            /* 典型原因：webd 在 ISP 起来之前就启动了，或者该路没使能。
             * 不报错退出 —— 之后每次 set 都会重新尝试，ISP 起来后自动能用。 */
            IMG_LOG("init: dev%d not readable (will retry on set)\n", dev);
        }
    }

    g_cur[view_dev()].mode = g_mode;
    g_cur[view_dev()].wb   = g_wb;
    g_inited = 1;
}

int img_tune_sel(int dev)
{
    if (dev < IMG_DEV_BOTH || dev >= IMG_DEV_MAX) return -3;

    g_sel = dev;

    /* 切了作用路就把缓存里的软件状态换成那一套，网页滑块立刻跳到该路的值 */
    if (dev >= 0)
    {
        g_cur[dev].mode = g_mode;
        g_cur[dev].wb   = g_wb;
    }
    return 0;
}

int img_tune_sel_get(void)
{
    return g_sel;
}

int img_tune_set(const char *key, int val)
{
    int rc;

    if (!key || !key[0]) return -4;
    if (!g_inited) img_tune_init();

    /* 越界不报错，夹到合法范围：网页滑块的边界值很容易差 1，
     * 直接拒会让用户以为"这一档坏了"。 */
    if      (!strcmp(key, "brt")) val = img_clamp(val, -100, 100);
    else if (!strcmp(key, "crt")) val = img_clamp(val, 0, 255);
    else if (!strcmp(key, "sat")) val = img_clamp(val, 0, 255);
    else if (!strcmp(key, "mir") || !strcmp(key, "flp")) val = val ? 1 : 0;
    else if (!strcmp(key, "rgn") || !strcmp(key, "bgn") ||
             !strcmp(key, "ggn")) val = img_clamp(val, -50, 50);
    else return -4;

    if (!strcmp(key, "rgn") || !strcmp(key, "bgn") || !strcmp(key, "ggn"))
    {
        int vd = (g_sel >= 0) ? g_sel : view_dev();
        int rp = g_cur[vd].rgn;
        int gp = g_cur[vd].ggn;
        int bp = g_cur[vd].bgn;

        if (!strcmp(key, "rgn"))      rp = val;
        else if (!strcmp(key, "ggn")) gp = val;
        else                          bp = val;

        rc = apply_wb_offset(rp, gp, bp);
        if (rc != 0)
        {
            IMG_LOG("%s=%d fail rc=%d\n", key, val, rc);
            return rc;
        }

        /* 手动动了色偏就不再对应 自动/暖/冷 */
        g_wb = -1;
        refresh();
        IMG_LOG("%s=%d ok (rgn=%d ggn=%d bgn=%d)\n", key, val, rp, gp, bp);
        return 0;
    }

    rc = apply_scope(key, val, RT_NULL);
    if (rc != 0)
    {
        IMG_LOG("%s=%d fail rc=%d\n", key, val, rc);
        return rc;
    }

    /* 手动动过 brt/crt/sat，就不再对应任何预设了。
     * 不这么做的话，网页上"图像模式"会一直高亮着"鲜艳"，
     * 实际参数早就被拧成别的了。 */
    if (strcmp(key, "mir") && strcmp(key, "flp")) g_mode = -1;

    refresh();
    IMG_LOG("%s=%d ok (dev=%d)\n", key, val, g_sel);
    return 0;
}

int img_tune_reset(void)
{
    int rc = 0;
    int dev;

    if (!g_inited) img_tune_init();

    /* 逐路恢复到**各自**的基线。
     *
     * 不能复用 apply_scope()：那个函数给作用范围内所有 dev 写同一个值，
     * 两路一起恢复时就会把 G0 的基线写进 G1 —— 正是这次要修的 bug
     * （现场：实拍正常，AI 主路偏红）。 */
    for (dev = 0; dev < IMG_DEV_MAX; dev++)
    {
        if (g_sel >= 0 && dev != g_sel) continue;

        if (!g_dev_ok[dev]) continue;

        if (set_brt(dev, g_base[dev].brt) != 0) rc = -1;
        if (set_crt(dev, g_base[dev].crt) != 0) rc = -1;
        if (set_sat(dev, g_base[dev].sat) != 0) rc = -1;
        if (set_mirflp(dev, 1, g_base[dev].mir) != 0) rc = -1;
        if (set_mirflp(dev, 0, g_base[dev].flp) != 0) rc = -1;
    }

    /*
     * 色偏也回中性，**并且解开手动锁定**。
     * 用户按"恢复默认"期望的是"回到 AWB 自动跟着环境走"，
     * 而不是"锁在我上次算出来的那一组数上"。
     * img_tune_wb_auto() 内部会把 rgn/ggn/bgn 一起清零，所以不用再循环一遍。
     */
    if (img_tune_wb_auto() != 0) rc = -1;

    /* 恢复出厂值之后就不再对应任何预设；网页的模式下拉显示"自定义" */
    g_mode = -1;
    g_wb   = IMG_WB_AUTO;

    refresh();
    IMG_LOG("reset dev=%d -> brt=%d crt=%d sat=%d\n", g_sel,
            g_base[view_dev()].brt, g_base[view_dev()].crt, g_base[view_dev()].sat);
    return rc;
}

/* ---------------- 图像模式 / 白平衡 ---------------- */

/* 暖/冷两档 = R/B 的两个固定偏移。
 * 12% 是现场一眼能看出来、又不至于把白色染成红茶的最小步长；
 * 想要更细就用网页上的红/绿/蓝增益滑块（1% 一档）。
 * G 通道不参与冷暖和，它专门用来压品红/压绿。 */
#define IMG_WB_STEP  12

int img_tune_wb(int mode)
{
    int rc;

    if (mode < 0 || mode >= IMG_WB_NUM) return -3;
    if (!g_inited) img_tune_init();

    if (mode == IMG_WB_AUTO)
    {
        /* "自动"= 清补偿 + 让 AWB 重新收敛。
         * 只清补偿是不够的：如果 bMwbEn 被置成 1，清了补偿画面还是歪的，
         * 用户会以为"自动档坏了"。 */
        rc = apply_wb_offset(0, 0, 0);
        if (rc == 0) rc = set_awb_mode(0);
    }
    else if (mode == IMG_WB_WARM) rc = apply_wb_offset(IMG_WB_STEP, 0, -IMG_WB_STEP);
    else                          rc = apply_wb_offset(-IMG_WB_STEP, 0, IMG_WB_STEP);

    if (rc != 0) return rc;

    g_wb = mode;
    refresh();
    IMG_LOG("wb=%d ok\n", mode);
    return 0;
}

int img_tune_preset(int mode)
{
    int r1;
    int r2;
    int r3;

    if (mode < 0 || mode >= IMG_MODE_NUM) return -3;
    if (!g_inited) img_tune_init();

    r1 = apply_scope("brt", g_presets[mode].brt, RT_NULL);
    r2 = apply_scope("crt", g_presets[mode].crt, RT_NULL);
    r3 = apply_scope("sat", g_presets[mode].sat, RT_NULL);

    /* 三组全失败才算失败：某一路 ISP 只支持部分参数是正常的 */
    if (r1 != 0 && r2 != 0 && r3 != 0) return -1;

    g_mode = mode;
    refresh();
    IMG_LOG("preset=%d brt=%d crt=%d sat=%d\n", mode,
            g_presets[mode].brt, g_presets[mode].crt, g_presets[mode].sat);
    return 0;
}

void img_tune_get(img_state_t *st)
{
    int dev;

    if (!st) return;
    if (!g_inited) img_tune_init();

    dev = view_dev();
    *st = g_cur[dev];

    /* mode/wb/rgn/bgn 是软件状态，refresh 里可能被覆盖成另一路的，
     * 这里统一以全局 + 该路偏移为准 */
    st->mode = g_mode;
    st->wb   = g_wb;
    st->rgn  = g_cur[dev].rgn;
    st->ggn  = g_cur[dev].ggn;
    st->bgn  = g_cur[dev].bgn;
    st->mwb  = g_manual[dev] ? 1 : 0;
}

void img_tune_dump(void)
{
    int dev;

    for (dev = 0; dev < IMG_DEV_MAX; dev++)
    {
        ISP_AWB_STATUS awb;

        memset(&awb, 0, sizeof(awb));
        if (API_ISP_GetAwbStatus((FH_UINT32)dev, &awb) != 0)
        {
            rt_kprintf("[img] dev%d brt=%d crt=%d sat=%d | awb status n/a\n",
                       dev, g_cur[dev].brt, g_cur[dev].crt, g_cur[dev].sat);
            continue;
        }

        /*
         * AWB 的几个使能位是"为什么调不动/为什么偏色"的第一现场：
         *   en=0    AWB 压根没跑，画面用的是固定增益 —— 补偿必须开
         *   mwb=1   手动白平衡，AWB 停止收敛，色温被锁死
         *   comp=0  补偿表不参与运算，红/绿/蓝三个滑块全部失效
         *   fine=1  肤色保护，人像场景会被有意推暖（手/脸偏红就是它）
         * AWB_DEFAULT_CFG 有几百字节，放静态，别压在 webd 的 8KB 栈上。
         */
        {
            static AWB_DEFAULT_CFG acfg;

            memset(&acfg, 0, sizeof(acfg));
            if (API_ISP_GetAwbDefaultCfg((FH_UINT32)dev, &acfg) == 0)
            {
                rt_kprintf("[img] dev%d awb en=%d mwb=%d comp=%d fine=%d alg=%d\n",
                           dev,
                           (int)acfg.stAwbEnableCfg.bAwbEn,
                           (int)acfg.stAwbEnableCfg.bMwbEn,
                           (int)acfg.stAwbEnableCfg.bCompEn,
                           (int)acfg.stAwbEnableCfg.bFineTuneEn,
                           (int)acfg.stAwbEnableCfg.u08AwbAlgType);
            }
        }

        /* R/G、B/G 是 U4.12，4096 = 1.0。这两个数就是"偏不偏色"的量化指标，
         * 中性场景下都该接近 4096。 */
        rt_kprintf("[img] dev%d brt=%d crt=%d sat=%d | Rg=%d Gg=%d Bg=%d\n",
                   dev, g_cur[dev].brt, g_cur[dev].crt, g_cur[dev].sat,
                   (int)awb.u16RGain, (int)awb.u16GGain, (int)awb.u16BGain);

        /*
         * 增益比 vs 统计比 —— 这一行是判断"到底谁在说谎"的关键。
         *
         *   u16RGain 是 U4.6 的**实际生效增益**（64 = 1.0），
         *   u16CurrROverG 是 AWB 统计出来的**画面 R/G**（U4.12，4096 = 1.0）。
         *   两者在 AWB 收敛后应当互为倒数附近；如果增益比是 1.24 而统计比
         *   还是 1.00，就说明统计根本没看到真实画面的偏色（统计点在增益之前，
         *   或者是另一路的统计），此时按统计值去乘增益只会越乘越偏。
         */
        if (awb.u16GGain > 8)
        {
            rt_kprintf("[img] dev%d gain ratio R/G=%d.%02d B/G=%d.%02d (U4.6)\n",
                       dev,
                       (int)((int)awb.u16RGain * 100 / (int)awb.u16GGain / 100),
                       (int)((int)awb.u16RGain * 100 / (int)awb.u16GGain % 100),
                       (int)((int)awb.u16BGain * 100 / (int)awb.u16GGain / 100),
                       (int)((int)awb.u16BGain * 100 / (int)awb.u16GGain % 100));
        }
        rt_kprintf("[img] dev%d R/G=%d.%03d B/G=%d.%03d comp r=%d g=%d b=%d\n",
                   dev,
                   (int)(awb.u16CurrROverG / 4096), (int)(awb.u16CurrROverG % 4096) * 1000 / 4096,
                   (int)(awb.u16CurrBOverG / 4096), (int)(awb.u16CurrBOverG % 4096) * 1000 / 4096,
                   (int)g_wb_r0[dev], (int)g_wb_g0[dev], (int)g_wb_b0[dev]);

        /*
         * 硬件寄存器里的真实增益（U.9，512 = 1.0）。
         * mwb=0 说明 AWB 在自动跑，这几个数是它自己算的；
         * mwb=1 说明已经被我们锁住了，画面偏色就只能是传感器/镜头/滤光片。
         * 这一行是"到底手动生效没生效"的唯一直接证据。
         */
        {
            ISP_AWB_GAIN hw;

            memset(&hw, 0, sizeof(hw));
            if (API_ISP_GetAwbGain((FH_UINT32)dev, &hw) == 0)
            {
                rt_kprintf("[img] dev%d mwb=%d hw R=%d G=%d B=%d (512=1.0)\n",
                           dev, g_manual[dev] ? 1 : 0,
                           (int)hw.u16Rgain, (int)hw.u16Ggain, (int)hw.u16Bgain);
            }
        }

        /*
         * 建议值。目标是让 R/G 和 B/G 同时回到 4096（=1.0）。
         *
         * 三个未知数两个方程，多一个自由度，取"总亮度不变"：
         *   (1+r)(1+g)(1+b) = 1，且 (1+r)= (1+g)/rg'、(1+b)= (1+g)/bg'
         *   ⇒ (1+g)^3 = rg'·bg'，g 就是两路偏差的几何平均。
         * 立方根用不了浮点（软浮点配置下会告警），一阶近似 (dr+db)*5/13
         * 在 ±40% 以内误差不到 1 个百分点，够现场用了。
         *
         * 只压 R/B 不抬 G 也能回中性，但整幅会暗一档，现场会误以为"调坏了"。
         */
        {
            int rg = (int)awb.u16CurrROverG;
            int bg = (int)awb.u16CurrBOverG;
            int dr;
            int db;
            int sr;
            int sb;
            int sg;

            if (rg < 512)  rg = 512;      /* 防止除零/爆表 */
            if (bg < 512)  bg = 512;

            dr = (rg - 4096) * 100 / rg;  /* R 相对 G 超出/不足多少 % */
            db = (bg - 4096) * 100 / bg;

            sg = (dr + db) * 5 / 13;      /* 抬(或降)绿 */
            sr = ((100 + sg) * 4096 / rg) - 100;
            sb = ((100 + sg) * 4096 / bg) - 100;

            rt_kprintf("[img] dev%d suggest rgn=%d ggn=%d bgn=%d  "
                       "(/api/img?dev=%d&k=rgn&v=%d ...)\n",
                       dev, sr, sg, sb, dev, sr);
        }
    }
}

/* ================== MSH 命令 ==================
 *
 * 串口上直接调，不依赖网页（网页卡住时这是唯一能用的路）。
 *   img                 等价于 img dump，看两路实测参数
 *   img neutral [dev]   把该路白平衡锁到中性（dev 省略 = 当前作用路）
 *   img auto [dev]      退回 AWB 自动
 *   img sel 0|1|-1      切作用路
 *
 * 为什么用 ALIAS：MSH_CMD_EXPORT 取的是 &函数名，函数叫 img_cmd 就必须
 * 写成 ALIAS(img_cmd, img, ...)，否则 -Werror 下报 undeclared + unused。
 */
static int img_cmd(int argc, char **argv)
{
    int rc;

    if (argc >= 2 && !strcmp(argv[1], "sel"))
    {
        if (argc < 3) { rt_kprintf("usage: img sel -1|0|1\n"); return 0; }
        rc = img_tune_sel(atoi(argv[2]));
        rt_kprintf("[img] sel=%d rc=%d\n", img_tune_sel_get(), rc);
        return 0;
    }

    if (argc >= 2 && (!strcmp(argv[1], "neutral") || !strcmp(argv[1], "auto")))
    {
        if (argc >= 3) img_tune_sel(atoi(argv[2]));

        if (argv[1][0] == 'n') rc = img_tune_neutral();
        else                   rc = img_tune_wb_auto();

        rt_kprintf("[img] %s dev=%d rc=%d\n", argv[1], img_tune_sel_get(), rc);
        img_tune_dump();
        return 0;
    }

    img_tune_dump();
    return 0;
}

MSH_CMD_EXPORT_ALIAS(img_cmd, img,
                     img [dump|sel N|neutral [N]|auto [N]] : ISP 画面体检/中性化);
