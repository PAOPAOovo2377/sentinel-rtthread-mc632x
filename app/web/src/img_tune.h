/*
 * img_tune.h - ISP 画面参数在线调节（给 webd 的 /api/img 用）
 *
 * 刻意**不 include 任何 ISP 头文件**：本头会被 webd.c 包含，而 webd.c 只
 * 关心"几个整数"，不该把 isp_api.h / isp_common.h 那一大坨拖进来。
 * 真正的 ISP 调用全部封在 img_tune.c 里。
 *
 * 参数键与取值范围（都是驱动原始值，换算交给网页）：
 *   brt  亮度       -100 ~ 100      （驱动 S8，范围 -128~127）
 *   crt  对比度      32 ~ 128       （驱动 U2.6，64 = 1.0 倍）
 *   sat  饱和度       0 ~ 255       （驱动 U3.5，32 = 1.0 倍）
 *   mir  水平镜像     0 / 1
 *   flp  垂直翻转     0 / 1
 *   rgn  红增益偏移  -50 ~ 50 (%)   （相对该路 AWB 基线的 R 补偿，±50%）
 *   bgn  蓝增益偏移  -50 ~ 50 (%)
 *   ggn  绿增益偏移  -50 ~ 50 (%)   ← 偏品红/偏绿主要调这个
 *
 * ★ 为什么一定要有 ggn（2026-10-28 现场）
 *   截图里 AI 主路整片发紫，实测 G1 全图 R/G=1.239、B/G=1.141，而高光区
 *   R/G=1.013、B/G=1.010 是准的 —— 典型的**缺绿**，不是偏红。
 *   只有 R/B 两个旋钮时，减 R 和 B 等于同时压暗画面去凑比值；有了
 *   u08GEnhance 就能直接抬绿，亮度不掉、底色回中性。
 *   （stAwbCompCfg 里本来就有 GEnhance，之前漏用了。）
 *
 * ★ 两路独立（2026-10-28）
 *   G0（实拍）与 G1（AI 主路）是两颗 sensor、两套 ISP，AWB 收敛点本来就
 *   不一样（实测 G1 的 R/G=1.220，G0 只有 1.025 —— G1 明显偏暖）。
 *   旧实现把白平衡基线存成**全局一对**，给 G1 写的是 G0 的绝对值，
 *   等于把实拍那路的色偏套到 AI 主路上，越调越歪。
 *   现在 brt/crt/sat/mir/flp/rgn/bgn 全部按 dev 分开存、分开写。
 *
 * 返回值统一：0 成功；<0 失败（-1 取原值失败 / -2 写回失败 / -3 参数越界 /
 * -4 不支持该键）。
 */
#ifndef __IMG_TUNE_H__
#define __IMG_TUNE_H__

#define IMG_DEV_BOTH  (-1)   /* 作用路：两路一起改 */
#define IMG_DEV_MAX   2      /* G0 / G1 */

typedef struct
{
    int ok;    /* 至少有一路 ISP 读到了参数 */
    int brt;
    int crt;
    int sat;
    int mir;
    int flp;
    int mode;  /* 当前图像模式 0..4，见 IMG_MODE_*，-1 = 自定义 */
    int wb;    /* 当前白平衡 0..2，见 IMG_WB_*，-1 = 自定义 */
    int rgn;   /* 红增益偏移 %，-50~50 */
    int bgn;   /* 蓝增益偏移 %，-50~50 */
    int ggn;   /* 绿增益偏移 %，-50~50（偏品红就往上抬） */
    int mwb;   /* 1 = 手动锁定白平衡（增益直接写硬件寄存器），0 = AWB 自动 */
} img_state_t;

/* 图像模式：本质是三组 brt/crt/sat 预设。
 * 板上没有"场景模式"这种硬件开关，做成一键组合比让用户自己拧三个滑块实用。 */
#define IMG_MODE_STD   0    /* 标准 */
#define IMG_MODE_BRIGHT 1   /* 明亮 */
#define IMG_MODE_SOFT  2    /* 柔和 */
#define IMG_MODE_VIVID 3    /* 鲜艳 */
#define IMG_MODE_NIGHT 4    /* 夜视（提亮 + 降饱和压噪点） */
#define IMG_MODE_NUM   5

/* 白平衡：走 AWB 的 R/B 补偿增益（U.6，64 = 1.0），不是硬切 AWB 环路 */
#define IMG_WB_AUTO    0
#define IMG_WB_WARM    1
#define IMG_WB_COOL    2
#define IMG_WB_NUM     3

/* 抓一份"出厂值"当恢复基线。webd start 时调一次；
 * 不调也能用（首次 set 前会懒初始化），但那时抓到的就是"调之前"的值。 */
void img_tune_init(void);

/* 设置单个参数，作用范围由 img_tune_sel() 决定（默认两路）。
 * 返回值见文件头。 */
int img_tune_set(const char *key, int val);

/* 恢复**当前作用路**到 img_tune_init() 抓到的那组值 */
int img_tune_reset(void);

/* 套用图像模式（会同时改 brt/crt/sat，滑块位置随之更新） */
int img_tune_preset(int mode);

/* 白平衡：IMG_WB_AUTO/WARM/COOL，本质是 rgn/bgn 的两个预设 */
int img_tune_wb(int mode);

/* 选择作用路：IMG_DEV_BOTH / 0 / 1。
 * 只影响之后的 set/reset，不影响已经写进驱动的值。 */
int img_tune_sel(int dev);
int img_tune_sel_get(void);

/* 读缓存（不碰驱动）。set/init 之后自动刷新，/api/state 每 2 秒轮询走这里。
 * 返回的是**当前作用路**的参数：作用路为两路时取第一路可读的（通常 G0）。 */
void img_tune_get(img_state_t *st);

/*
 * 一键中性化：把当前作用路的白平衡**锁到中性**。
 *
 * 做法不是"调补偿"——是读 AWB 统计出来的 R/G、B/G，再按 4096/比值 直接改
 * 硬件增益（API_ISP_SetAwbGain），同时把 bMwbEn 置 1 让 AWB 停止覆写。
 * 为什么必须这么干：
 *   stAwbCompCfg 那套补偿是"叠在 AWB 输出上的第二层"，它生效与否取决于
 *   bCompEn，而且最终还是被 AWB 环路牵着走。现场反馈"调了半天跟没调一样"
 *   就是这条路被驱动忽略了。直接改增益这条路是驱动明确支持的，改完
 *   AWB 也拉不回去，画面一定动。
 *
 * 返回值：0 成功；-1 读 AWB 状态失败（该路 ISP 没起来）；
 *         -2 读硬件增益失败；-3/-4 写配置/写增益失败（ISP 拒绝）。
 * 作用路由 img_tune_sel() 决定，和别的键一致。
 */
int img_tune_neutral(void);

/* 退回 AWB 自动（bMwbEn=0），补偿偏移同时清零。
 * 换环境（室内→室外）后如果手动锁定那组值不合适，用这个放开。 */
int img_tune_wb_auto(void);

/* 当前是否处于手动锁定。作用路为两路时返回第一路的。 */
int img_tune_manual_get(void);

/* 把两路的实测参数 + AWB 收敛状态打到串口。
 * 现场判"哪一路偏色、偏多少"只看这一行就够了：
 *   [img] dev0 brt=0 crt=84 sat=80 | Rgain=.. Ggain=.. Bgain=.. R/G=1.02 B/G=0.98
 * webd 的 mem 命令里会调它，也可以在 MSH 里 `img_dump`。 */
void img_tune_dump(void);

#endif /* __IMG_TUNE_H__ */
