#include "nna_app/include/sample_common_nna.h"
#include "fh_trip_area.h"

/*
 * config.h 必须显式引进来（2026-10-28）。
 * 这个文件的 include 链（sample_common_nna.h → vpu_config.h → ...）里
 * 一个都没带 config.h，于是 FH_APP_TRIP_NUM / FH_APP_PERI_NUM 在预处理
 * 阶段被当成 0，trip_setting()/area_setting() 整段被 #if 掉 ——
 * 结果就是"库链上了、示例编过了、规则线一条都没设，永远不触发"。
 * -I./ 就是 app/media_demo，所以直接写 "config.h"。
 */
#include "config.h"

/*
 * 越线留证。
 * 绊线库只管算事件，抓图存证是我们自己的事：命中就调证据相册那个
 * evi_capture()，网页上的「证据相册」立刻多一张，事件时间线多一条。
 */
#include <rtthread.h>
#include <string.h>
#include <stdlib.h>
#include "web/src/gallery.h"
#include "web/src/dog.h"

static char *model_name = MODEL_TAG_DEMO_NNA_IVS;

/*
 * ★ 2026-10-28：0 → 1（"人物识别红框没了"的根因）
 *
 *   这个宏同时决定三件事：人形框画在哪条 VPU 通道、规则线画在哪、
 *   NN 的归一化坐标乘哪一路的宽高。
 *   G1 的通道分配是：
 *     CH0 1920x1088 H.264  → 绑屏(VO)
 *     CH1  800x480  MJPEG  → 网页第二路 :1112 / RTSP :1260，也是人脸抓帧源
 *     CH2  512x288  RGB888 → NN
 *   原来写 0，框和规则线全画在 CH0 上 —— 那一路只送到屏，
 *   网页和 RTSP 一根线都看不到，现象就是"红框凭空消失"。
 *   旧 demo 里写的是 draw_box(grp_id, 1, ...)，正好是 CH1，两版一对比就清楚了。
 *
 *   坐标没问题：NN 输出是 0~1 归一化的，乘哪一路的宽高就贴在那一路上，
 *   所以这里改 1 不需要动任何换算代码。
 */
#define IVS_CHN 1

static FH_UINT32 g_nna_grpid[MAX_GRP_NUM] = {0};
static FH_SINT32 g_nna_running[MAX_GRP_NUM] = {0};
static FH_SINT32 g_nna_stop[MAX_GRP_NUM] = {0};

static FH_TASK_RULE_t trip_task_cfg[2];
static FH_TASK_RULE_t peri_task_cfg[2];

static FH_UINT32 current_max_gbox_id[MAX_GRP_NUM] = {0};
static FH_UINT32 g_box_id_start = 4; // 0-3为规则线

/*
 * 规则线的横坐标（像素，G1 的 800 宽坐标系）。
 *
 * ★ 2026-10-28 深夜：默认值从 chn_w/25 退回 chn_w/3。
 *
 *   上一版为了满足"挪到最左边"，默认取 1/25（800 宽下 32px）。
 *   结果现场反馈"人走过去还是不报" —— 这不是判定逻辑坏了，是**几何上不可能**：
 *   人站在画面里时，人形框中心点的 x 正常在 100~700 之间。
 *   线放在 x=32，只有整个人贴到画面最左边缘、中心点也压进 32px 以内才跨得过去。
 *   走直线穿过画面的人，x 从 700 变到 200，全程都在线的同一侧（右侧），
 *   判定条件 `s0 != s1` 永远不成立 —— 再准的算法也不可能报。
 *
 *   默认值取 chn_w/12（800 宽下约 66px，画面最左边 8%）：视觉上就贴在
 *   左边缘，用户要的"边边一点"就是这个位置。
 *   想调就 msh 敲 `trip x N`，改完立刻生效。
 *   `trip` 不带参数会打印当前线位和"左右两侧各有多宽"，一眼就能看出
 *   这条线到底是不是在人走的那条路上。
 *
 * ★ 线贴边以后，"跨越"这条判据就不可靠了（2026-10-28）
 *   相机近大远小，人走到画面左边缘时，人形框中心点常常还停在 100 上下，
 *   进不了 66px 以内 ⇒ `s0 != s1` 永远不成立 ⇒ 人贴着边走也不报。
 *   所以另加一条"区域进入"判据（见 simple_trip 里的 g_zone_latch）：
 *   只要中心点落进线左侧就报一次，退回到线右侧 TRIP_ZONE_HYST 以外才复位。
 *   这样线可以安稳地贴边，判定也不再依赖"必须跨过去"。
 *
 *   下限仍然是 2 而不是 0：线贴到 x=0 时左侧区域宽度为 0，
 *   库那边永远凑不满 preminHits 帧。
 */
static FH_SINT32 g_trip_x = -1;      /* -1 = 用默认值 chn_w/12 */

/* 调试开关：打开后每帧打印检出框的中心点，现场看"人在哪、线在哪"。 */
static FH_SINT32 g_trip_dbg = 0;

/*
 * 本地兜底越线判定（不依赖绊线库的状态机）。
 *
 * ★ 为什么要有这一层
 *   libtrip_area 的判定是"同一 objId 先在线的这一侧连续 preminHits 帧，
 *   再在另一侧连续 curminHits 帧"。它靠跟踪器的 id 贯穿整次穿越；
 *   一旦检测框中间丢几帧、id 变号，状态就清零，人走过去了也不报。
 *   现场最受不了的就是"明明走过去了却什么都没发生"。
 *   这里的判据简单粗暴：相邻两帧里同一个框的中心点跨到了线另一侧就报。
 *   两套并行跑，共用 trip_report() 的静默期（2 秒，失败时 600ms），不会重复留证。
 */
#define TRIP_PREV_MAX 16
static FH_SINT32 g_prev_cx[TRIP_PREV_MAX];
static FH_SINT32 g_prev_cy[TRIP_PREV_MAX];
static FH_SINT32 g_prev_n = 0;

/*
 * 连续多少帧"一个人都没检出"。超过 TRIP_PREV_HOLD 帧才清历史。
 *
 * ★ 为什么不能一帧清零（2026-10-28 深夜，这是"越线不报"的第二个真因）
 *   simple_trip() 的判据是"本帧的中心点 vs 上一帧的中心点，跨没跨线"。
 *   上一版在 `nn_result_num == 0` 时直接 `g_prev_n = 0`。而这个 NN
 *   跑在 512×288 上，行人检测偶尔漏一帧是常态（人侧身、弯腰、被遮挡、
 *   运动模糊都会漏）。一旦中途漏一帧，历史被抹掉，下一帧重新开始记，
 *   人正好在这次空洞里跨过了线 —— 这次穿越就**永远不会被记到**。
 *   一帧漏检的概率不低，走十次能漏掉好几次，用户感受就是"时灵时不灵"。
 *
 *   现在允许中间空 2 帧（≈130ms@15fps）还保留历史。真实的人走过画面时
 *   不可能 2 帧全无，而"人真的离开画面"超过 2 帧后历史才清，
 *   不会造成"左出右进配成一对"的误报（那个场景中间一定隔着很多帧）。
 */
#define TRIP_PREV_HOLD 2
static FH_SINT32 g_prev_miss = 0;

/*
 * 区域进入判定的锁存（线贴左边以后的主力判据）。
 *   1 = 已经有人的中心点进过线左侧、这次"进入"已经报过，站在里面不再重复报；
 *   等到所有人退回 lx + TRIP_ZONE_HYST 右侧，才清 0 允许下一次进入上报。
 *   HYST 给 40px：人站在线附近时中心点会来回抖几像素，
 *   没有这段回差就会反复"进入/退出"，串口和相册被刷屏。
 */
#define TRIP_ZONE_HYST 40
static FH_SINT32 g_zone_latch = 0;

/*
 * 锁存的最长存活时间。超过就强制复位，允许下一次上报。
 *
 * 为什么必须有：判据放宽到"框左沿过线"之后，只要画面左侧始终存在一个框
 * （人站着不动、或模型把背景物体误判成人），"所有人退回右侧"就永远不成立，
 * 锁存卡死 ⇒ 之后所有越线事件都被吞掉。表现和"灵敏度不够"一模一样，
 * 但根因是状态机没出口。5 秒是权衡：足够覆盖一个人慢慢走过（2~3 秒），
 * 又不会让"画面里有个假框"这件事把功能废掉太久。
 */
#define TRIP_LATCH_MAX_MS 5000
static rt_tick_t g_zone_tick  = 0;

/*
 * 给网页看的两路运行态（/api/state 的 ivs 字段）。
 * 之前只有串口心跳能看到 det 人数，用户不接串口就完全不知道
 * "是不是根本没认出人"。这两个值由 nna_result_to_tad 每帧更新，
 * webd 只读，不加锁（32 位对齐的 int 读写本身是原子的）。
 */
static volatile FH_SINT32 g_last_det = 0;   /* 本帧检出人数 */
static volatile FH_SINT32 g_last_cx  = -1;  /* 最大目标的中心点 x，-1 表示无 */

static FH_SINT32 clear_screen(FH_SINT32 grp_id, FH_SINT32 chn_id, FH_SINT32 reserve_num)
{
    FH_SINT32 ret = FH_SDK_SUCCESS;
    OSD_GBOX gbox;
    /*
     * ★ 2026-10-28：原来这两行是共用 `#if (TRIP>0)||(PERI>0)` 的，
     *   而 chn_w 其实只被 `#if (TRIP>1)` 和 `#if (PERI>0)` 这两段用到。
     *   本工程是 TRIP_NUM=1 / PERI_NUM=0 —— 于是 chn_w 被赋值却没有任何读者，
     *   gcc 报 `-Wunused-but-set-variable`，-Werror 直接中断编译。
     *   这不是"逻辑错误"，是条件编译的作用域比变量用途宽了一圈。
     *   现在按**实际使用范围**拆成两个块：
     *     chn_h ← TRIP>0（第 1 条线的 gbox.h）或 PERI>0
     *     chn_w ← TRIP>1（第 2 条横线）或 PERI>0
     *   任意宏组合都不会再出现"赋值没人用"，也不用 (void) 硬消警报。
     */
#if (FH_APP_TRIP_NUM > 0) || (FH_APP_PERI_NUM > 0)
    FH_UINT32 chn_h = 0;

    chn_h = get_vpu_chn_h(grp_id, chn_id);
#endif
#if (FH_APP_TRIP_NUM > 1) || (FH_APP_PERI_NUM > 0)
    FH_UINT32 chn_w = 0;

    chn_w = get_vpu_chn_w(grp_id, chn_id);
#endif

    gbox.enable = 0;
    for (FH_SINT32 i = g_box_id_start; i < current_max_gbox_id[grp_id]; i++)
    {
        gbox.gbox_id = i;
        ret = sample_set_gbox(grp_id, chn_id, &gbox);
        SDK_FUNC_ERROR(model_name, ret);
    }

    gbox.color.fAlpha = 255;
    gbox.color.fRed = 255;
    gbox.color.fGreen = 0;
    gbox.color.fBlue = 0;

#if (FH_APP_TRIP_NUM > 0)
    gbox.enable = 1;
    gbox.gbox_id = 0;
    gbox.x = trip_task_cfg[0].trip_cfg.endPoint.x;
    gbox.y = trip_task_cfg[0].trip_cfg.endPoint.y;
    gbox.w = 4;
    gbox.h = chn_h;
    ret = sample_set_gbox(grp_id, chn_id, &gbox);
    SDK_FUNC_ERROR(model_name, ret);
#endif

#if (FH_APP_TRIP_NUM > 1)
    gbox.enable = 1;
    gbox.gbox_id = 1;
    gbox.x = trip_task_cfg[1].trip_cfg.endPoint.x;
    gbox.y = trip_task_cfg[1].trip_cfg.endPoint.y;
    gbox.w = chn_w;
    gbox.h = 4;
    ret = sample_set_gbox(grp_id, chn_id, &gbox);
    SDK_FUNC_ERROR(model_name, ret);
#endif

#if (FH_APP_PERI_NUM > 0)
    gbox.enable = 1;
    gbox.gbox_id = 2;
    gbox.x = peri_task_cfg[0].area_cfg.rectPoint[0].x;
    gbox.y = peri_task_cfg[0].area_cfg.rectPoint[0].y;
    gbox.w = chn_w / 6;
    gbox.h = chn_h * 4 / 6;
    ret = sample_set_gbox(grp_id, chn_id, &gbox);
    SDK_FUNC_ERROR(model_name, ret);
#endif

#if (FH_APP_PERI_NUM > 1)
    gbox.enable = 1;
    gbox.gbox_id = 3;
    gbox.x = peri_task_cfg[1].area_cfg.rectPoint[0].x;
    gbox.y = peri_task_cfg[1].area_cfg.rectPoint[0].y;
    gbox.w = chn_w / 6;
    gbox.h = chn_h * 4 / 6;
    ret = sample_set_gbox(grp_id, chn_id, &gbox);
    SDK_FUNC_ERROR(model_name, ret);
#endif

    return ret;
}

static FH_SINT32 draw_box(FH_SINT32 grp_id, FH_SINT32 chn_id, FH_TAD_GROUP_t tad_in)
{
    FH_SINT32 ret = FH_SDK_SUCCESS;
    OSD_GBOX gbox;

    gbox.enable = 1;
    gbox.color.fAlpha = 255;
    gbox.color.fRed = 255;
    gbox.color.fGreen = 0;
    gbox.color.fBlue = 0;

    current_max_gbox_id[grp_id] = g_box_id_start;
    for (FH_SINT32 i = 0; i < tad_in.num; i++)
    {
        gbox.gbox_id = current_max_gbox_id[grp_id]++;
        gbox.x = tad_in.object[i].pos.x;
        gbox.y = tad_in.object[i].pos.y;
        gbox.w = tad_in.object[i].pos.w;
        gbox.h = tad_in.object[i].pos.h;
        ret = sample_set_gbox(grp_id, chn_id, &gbox);
        SDK_FUNC_ERROR(model_name, ret);
    }

    return ret;
}

static TAD_HANDLE tad_init(FH_VOID)
{
    FH_SINT32 ret = FH_SDK_SUCCESS;
    TAD_HANDLE tadHandle = NULL;
    FH_TAD_HANDLE_CFG_t tad_cfg;

    /*
     * 这四个数取手册 3.2.2 给的"建议默认配置"，别自己发挥。
     *
     * 原来写的是 iouThresh=10 / minHits=3 / maxAge=20：
     *   minHits=3  新目标要连续 3 次匹配才建轨迹，行人检测偶尔丢一帧就重来，
     *              轨迹建不起来 ⇒ 越线状态机永远是空的；
     *   iouThresh=10 重叠只要 10% 就算同一个目标，人走动时容易和旁边的框串号；
     *   maxAge=20  轨迹消失后还能挂 20 帧，新目标会被分配给旧 id，编号乱换。
     * 这三条叠在一起，现象正是"人走过去了，一次都不报"。
     */
    tad_cfg.maxTrackNum = 20;
    tad_cfg.iouThresh = 20;
    tad_cfg.minHits = 1;
    tad_cfg.maxAge = 2;

    ret = FH_TAD_CreatHandle(&tadHandle, &tad_cfg);
    SDK_FUNC_ERROR_GOTO(model_name, ret);

    return tadHandle;

Exit:
    return NULL;
}

static FH_VOID tad_deinit(TAD_HANDLE tadHandle)
{
    if (tadHandle)
    {
        FH_TAD_HandleRelease(tadHandle);
    }
}

static FH_SINT32 trip_setting(FH_SINT32 grp_id, FH_SINT32 chn_id)
{
    FH_SINT32 ret = FH_SDK_SUCCESS;

#if FH_APP_TRIP_NUM
    /*
     * 1 帧就够。
     *
     * 手册里 curminHits / preminHits 是"越线前后各要连续命中几帧"，
     * 用来躲开目标骑在线上时的抖动。取 2 时，只要跟踪器中途丢一帧
     * （这种板子上很常见：NV12 小尺寸输入 + 行人检测），计数就清零，
     * 人整个走过去也凑不满 —— 现场表现就是"跨线不报警"。
     * 取 1 的代价是线上抖动时可能多报一次，但 trip_report 有 2 秒静默期，
     * 现场宁可多报也不能漏报。
     */
    FH_SINT32 trip_curminHits = 1;
    FH_SINT32 trip_preminHits = 1;
    OSD_GBOX gbox;
    FH_UINT32 chn_w = 0;
    FH_UINT32 chn_h = 0;
    FH_UINT32 lx;

    gbox.color.fAlpha = 255;
    gbox.color.fRed = 255;
    gbox.color.fGreen = 0;
    gbox.color.fBlue = 0;

    chn_w = get_vpu_chn_w(grp_id, chn_id);
    chn_h = get_vpu_chn_h(grp_id, chn_id);

    /* 画面最左 8%（见 g_trip_x 的注释：贴边 + 区域判据兜底） */
    lx = (g_trip_x >= 0) ? (FH_UINT32)g_trip_x : (chn_w / 12);
    if (lx < 2) lx = 2;
    if (lx + 6 > chn_w) lx = chn_w - 6;

#if (FH_APP_TRIP_NUM > 0)
    trip_task_cfg[0].ruleid = 0;
    trip_task_cfg[0].enable = 1;
    trip_task_cfg[0].curminHits = trip_curminHits;
    trip_task_cfg[0].preminHits = trip_preminHits;
    trip_task_cfg[0].mode = TAD_TRIP_ALL;
    trip_task_cfg[0].trip_cfg.strategy = CENTRAL_POINT_STRATEGY;
    trip_task_cfg[0].trip_cfg.startPoint.x = lx;
    trip_task_cfg[0].trip_cfg.startPoint.y = chn_h;
    trip_task_cfg[0].trip_cfg.endPoint.x = lx;
    trip_task_cfg[0].trip_cfg.endPoint.y = 0;

    gbox.enable = 1;
    gbox.gbox_id = 0;
    gbox.x = lx;
    gbox.y = 0;
    gbox.w = 4;
    gbox.h = chn_h;
    ret = sample_set_gbox(grp_id, chn_id, &gbox);
    SDK_FUNC_ERROR(model_name, ret);

    SDK_PRT(model_name, "[trip] line x=%d (chn %dx%d) hits=%d/%d\n",
            (int)lx, (int)chn_w, (int)chn_h,
            (int)trip_preminHits, (int)trip_curminHits);
#endif

#if (FH_APP_TRIP_NUM > 1)
    trip_task_cfg[1].ruleid = 1;
    trip_task_cfg[1].enable = 1;
    trip_task_cfg[1].curminHits = trip_curminHits;
    trip_task_cfg[1].preminHits = trip_preminHits;
    trip_task_cfg[1].mode = TAD_TRIP_ALL;
    trip_task_cfg[1].trip_cfg.startPoint.x = chn_w; // 上出下进
    trip_task_cfg[1].trip_cfg.startPoint.y = chn_h / 2;
    trip_task_cfg[1].trip_cfg.endPoint.x = 0;
    trip_task_cfg[1].trip_cfg.endPoint.y = chn_h / 2;

    gbox.enable = 1;
    gbox.gbox_id = 1;
    gbox.x = trip_task_cfg[1].trip_cfg.endPoint.x;
    gbox.y = trip_task_cfg[1].trip_cfg.endPoint.y;
    gbox.w = chn_w;
    gbox.h = 4;
    ret = sample_set_gbox(grp_id, chn_id, &gbox);
    SDK_FUNC_ERROR(model_name, ret);
#endif
#endif

    return ret;
}

static FH_SINT32 area_setting(FH_SINT32 grp_id, FH_SINT32 chn_id)
{
    FH_SINT32 ret = FH_SDK_SUCCESS;

#if FH_APP_PERI_NUM
    FH_SINT32 peri_curminHits = 2;
    FH_SINT32 peri_preminHits = 2;
    OSD_GBOX gbox;
    FH_UINT32 chn_w = 0;
    FH_UINT32 chn_h = 0;

    gbox.color.fAlpha = 255;
    gbox.color.fRed = 255;
    gbox.color.fGreen = 0;
    gbox.color.fBlue = 0;

    chn_w = get_vpu_chn_w(grp_id, chn_id);
    chn_h = get_vpu_chn_h(grp_id, chn_id);

#if (FH_APP_PERI_NUM > 0)
    peri_task_cfg[0].ruleid = 2;
    peri_task_cfg[0].enable = 1;
    peri_task_cfg[0].curminHits = peri_curminHits;
    peri_task_cfg[0].preminHits = peri_preminHits;
    peri_task_cfg[0].mode = TAD_AREA_ALL;
    peri_task_cfg[0].area_cfg.iTimeMin = 10;
    peri_task_cfg[0].area_cfg.iPointNum = 4;
    peri_task_cfg[0].area_cfg.rectPoint[0].x = chn_w / 6;
    peri_task_cfg[0].area_cfg.rectPoint[0].y = chn_h / 6;
    peri_task_cfg[0].area_cfg.rectPoint[1].x = chn_w * 2 / 6;
    peri_task_cfg[0].area_cfg.rectPoint[1].y = chn_h / 6;
    peri_task_cfg[0].area_cfg.rectPoint[2].x = chn_w * 2 / 6;
    peri_task_cfg[0].area_cfg.rectPoint[2].y = chn_h * 5 / 6;
    peri_task_cfg[0].area_cfg.rectPoint[3].x = chn_w / 6;
    peri_task_cfg[0].area_cfg.rectPoint[3].y = chn_h * 5 / 6;

    gbox.enable = 1;
    gbox.gbox_id = 2;
    gbox.x = peri_task_cfg[0].area_cfg.rectPoint[0].x;
    gbox.y = peri_task_cfg[0].area_cfg.rectPoint[0].y;
    gbox.w = chn_w / 6;
    gbox.h = chn_h * 4 / 6;
    ret = sample_set_gbox(grp_id, chn_id, &gbox);
    SDK_FUNC_ERROR(model_name, ret);
#endif

#if (FH_APP_PERI_NUM > 1)
    peri_task_cfg[1].ruleid = 3;
    peri_task_cfg[1].enable = 1;
    peri_task_cfg[1].curminHits = peri_curminHits;
    peri_task_cfg[1].preminHits = peri_preminHits;
    peri_task_cfg[1].mode = TAD_AREA_ALL;
    peri_task_cfg[1].area_cfg.iTimeMin = 10;
    peri_task_cfg[1].area_cfg.iPointNum = 4;
    peri_task_cfg[1].area_cfg.rectPoint[0].x = chn_w * 4 / 6;
    peri_task_cfg[1].area_cfg.rectPoint[0].y = chn_h / 6;
    peri_task_cfg[1].area_cfg.rectPoint[1].x = chn_w * 5 / 6;
    peri_task_cfg[1].area_cfg.rectPoint[1].y = chn_h / 6;
    peri_task_cfg[1].area_cfg.rectPoint[2].x = chn_w * 5 / 6;
    peri_task_cfg[1].area_cfg.rectPoint[2].y = chn_h * 5 / 6;
    peri_task_cfg[1].area_cfg.rectPoint[3].x = chn_w * 4 / 6;
    peri_task_cfg[1].area_cfg.rectPoint[3].y = chn_h * 5 / 6;

    gbox.enable = 1;
    gbox.gbox_id = 3;
    gbox.x = peri_task_cfg[1].area_cfg.rectPoint[0].x;
    gbox.y = peri_task_cfg[1].area_cfg.rectPoint[0].y;
    gbox.w = chn_w / 6;
    gbox.h = chn_h * 4 / 6;
    ret = sample_set_gbox(grp_id, chn_id, &gbox);
    SDK_FUNC_ERROR(model_name, ret);
#endif
#endif

    return ret;
}

/*
 * 命中就留证：往相册队列丢一个请求 + 记一条事件。
 *
 * ★ 2026-10-29 重写（"越线了却没抓到图"的根因）
 *   原来这里是同步 evi_capture()：
 *     cf_busy 轮询最长 1.2s + 等帧 0.8s + 补救再 0.8s ≈ 最坏 3 秒，
 *     而这段代码跑在 IVS/NN 线程里。一次触发就把检测循环冻住三秒，
 *     人在这三秒里走完整条越线路径，帧全丢；更糟的是抓帧成功后
 *     还要空等 3 秒静默期 —— 所以现场感受就是"第一次有反应，之后完全没反应"。
 *
 *   现在三步改掉：
 *     1) 只调 evi_post()（入队，几微秒），抓帧交给 gallery 的独立线程；
 *        帧直接取 cloud_face 缓冲区里刚编好的那张，不等 cf_busy，不抢 vicap。
 *     2) 静默期改"可变"：上一次留证成功 → 2 秒（同一次穿越/同一人来回蹭线
 *        不会刷屏）；上一次受理失败 → 600ms 后就能再试，不再一失败就哑三秒。
 *     3) 事件先记，图后到：判定成立本身就比图重要，队列满/线程没起来
 *        也必须留痕，网页上能看到"何时越的线"，只是那张图缺了。
 */
#define TRIP_GAP_MS       2000   /* 留证成功后的静默期 */
#define TRIP_RETRY_MS      600   /* 失败了多久允许再试 */

static FH_VOID trip_report(FH_TAD_MODE_e mode)
{
    static rt_tick_t last = 0;
    static FH_SINT32 fail = 0;

    rt_tick_t now = rt_tick_get();
    rt_tick_t gap = rt_tick_from_millisecond(fail ? TRIP_RETRY_MS : TRIP_GAP_MS);
    const char *what;

    if (last != 0 && (rt_tick_t)(now - last) < gap)
    {
        return;
    }
    last = now;

    switch (mode)
    {
    case TAD_TRIP_LTOR:      what = "cross L->R"; break;
    case TAD_TRIP_RTOL:      what = "cross R->L"; break;
    case TAD_TRIP_ALL:       what = "cross line"; break;
    case TAD_AREA_ENTER:     what = "enter area"; break;
    case TAD_AREA_EXIT:      what = "leave area"; break;
    case TAD_AREA_INTRUSION:
    case TAD_AREA_ALL:       what = "intrusion";  break;
    default:                 what = "alert";      break;
    }

    /* 先记事件，再投递抓拍 —— 顺序不能反：
     * 万一 evi_post 那边队列满了要挤掉一条，事件列表里至少还有这次的时间点。 */
    evt_add("trip", what);

    if (evi_post(EVI_T_ALERT) == 0)
    {
        fail = 0;
        SDK_PRT(model_name, "[trip] %s -> queued\n", what);
    }
    else
    {
        /* 只有 webd 没启动时才会走到这里（相册没初始化）。
         * 缩短静默期，等它起来之后能立刻开始留图。 */
        fail = 1;
        SDK_PRT(model_name, "[trip] %s (capture skipped: gallery not ready)\n", what);
    }
}

/*
 * 本地兜底越线判定。
 *
 * 判据：相邻两帧里"同一个目标"（按中心点最近邻匹配）的中心点跨到了线另一侧。
 * 只用中心点，和库里 CENTRAL_POINT_STRATEGY 的口径一致，
 * 这样两套判定的边界条件是一样的，不会一个报一个不报。
 *
 * 匹配半径取 chn_w/2（800 宽下 400px）：人走一步的位移远小于这个数，
 * 留这么大是为了容忍低帧率 —— NN 被别的线程挤到 3~5fps 时，
 * 帧间位移可能上百像素，半径给窄了就会"匹配不上 ⇒ 不报"。
 * 宁可偶尔把两个人配错（顶多多报一次，还有 2 秒静默期兜着），
 * 也不能让人走过去了却什么都没发生。
 */
#if (FH_APP_TRIP_NUM > 0)
static FH_VOID simple_trip(const FH_TAD_GROUP_t *in, FH_SINT32 lx, FH_SINT32 chn_w)
{
    FH_SINT32 cx[TRIP_PREV_MAX];
    FH_SINT32 cy[TRIP_PREV_MAX];
    FH_SINT32 n = in->num;
    FH_SINT32 i;
    FH_SINT32 j;

    if (n > TRIP_PREV_MAX) n = TRIP_PREV_MAX;

    for (i = 0; i < n; i++)
    {
        cx[i] = in->object[i].pos.x + in->object[i].pos.w / 2;
        cy[i] = in->object[i].pos.y + in->object[i].pos.h / 2;
    }

    /*
     * 记下"最靠右的那个框"的中心点，给网页显示用。
     * 现场排查时最有用的一个数：线在 x=266，人却一直待在 x=680，
     * 那一眼就知道不是判定坏了，是人根本没走到线那里。
     */
    {
        FH_SINT32 mx = -1;

        for (i = 0; i < n; i++)
        {
            if (cx[i] > mx) mx = cx[i];
        }
        g_last_cx = mx;
    }

    /* 逐帧调试输出（`trip dbg on` 打开）。只在打开时打，正常不刷串口。 */
    if (g_trip_dbg)
    {
        for (i = 0; i < n; i++)
        {
            SDK_PRT(model_name, "[trip] dbg cx=%d cy=%d line=%d side=%s\n",
                    (int)cx[i], (int)cy[i], (int)lx,
                    (cx[i] < lx) ? "LEFT" : "RIGHT");
        }
    }

    for (i = 0; i < n; i++)
    {
        FH_SINT32 best = -1;
        FH_SINT32 bestd = 0x7fffffff;

        for (j = 0; j < g_prev_n; j++)
        {
            FH_SINT32 dx = cx[i] - g_prev_cx[j];
            FH_SINT32 dy = cy[i] - g_prev_cy[j];
            FH_SINT32 d;

            if (dx < 0) dx = -dx;
            if (dy < 0) dy = -dy;
            d = dx + dy;

            if (d < bestd)
            {
                bestd = d;
                best = j;
            }
        }

        if (best >= 0 && bestd < chn_w / 2)
        {
            FH_SINT32 s0 = (g_prev_cx[best] < lx) ? 0 : 1;
            FH_SINT32 s1 = (cx[i] < lx) ? 0 : 1;

            if (s0 != s1)
            {
                SDK_PRT(model_name, "[trip] local cross x=%d->%d line=%d\n",
                        (int)g_prev_cx[best], (int)cx[i], (int)lx);
                trip_report(TAD_TRIP_ALL);
            }
        }
    }

    /*
     * ★ 区域进入判定（线贴边以后的主力判据，2026-10-28）
     *   线放到 chn_w/12 之后，靠"跨过去"报不出来：人站在画面左边缘时，
     *   人形框中心点还会停在 100 上下，永远压不进 66px 以内。
     *   这里不再要求"先右后左"，只要中心点落进线左侧就报一次；
     *   所有人退回 lx + TRIP_ZONE_HYST 右边才复位锁存，人站在里面不会连报。
     *
     *   多人时要按"有没有任何一个人在左侧"来判，不能拿单人做 else-if ——
     *   否则第一个人锁上、第二个人站在右边就立刻把锁存清掉，等于没锁。
     */
    {
        FH_SINT32 in_left  = 0;
        FH_SINT32 all_right = 1;
        FH_SINT32 left_cx  = 0;

        for (i = 0; i < n; i++)
        {
            /*
             * ★ 2026-10-29：判据从"中心点过线"放宽到"中心点或框左沿过线"。
             *
             *   线在 chn_w/12（800 宽下 66px）时，人走到画面最左边，
             *   人形框中心点通常还停在 90~130 —— 身体的一半已经压在线左边了，
             *   中心点却永远进不了 66px。这就是"人明明走过去了，就是不报"。
             *   改成看框左沿（pos.x）：人一贴近左边缘，框左沿必然先越过线，
             *   这才是"人越线"在视觉上的真实含义。
             *
             *   代价是框宽的人站得稍远也会触发一次 —— 对安防场景来说，
             *   "宁多报不缺报"是正确的取舍（后面还有 2 秒静默期兜底）。
             */
            FH_SINT32 bx = in->object[i].pos.x;

            if (cx[i] < lx || bx < lx)
            {
                in_left = 1;
                left_cx = cx[i];
            }
            if (cx[i] <= lx + TRIP_ZONE_HYST)
            {
                all_right = 0;
            }
        }

        if (!g_zone_latch && in_left)
        {
            g_zone_latch = 1;
            g_zone_tick  = rt_tick_get();
            SDK_PRT(model_name, "[trip] enter zone cx=%d line=%d\n",
                    (int)left_cx, (int)lx);
            trip_report(TAD_AREA_ENTER);
        }
        else if (g_zone_latch)
        {
            /*
             * 复位条件有两条：
             *   a) 所有人都退回线右侧；
             *   b) 超时强制复位（TRIP_LATCH_MAX_MS = 5 秒）。
             *
             * (b) 是必须的：改用框左沿判定后，"center 还在右边、只有框沿过线"
             * 这种情况会让 (a) 永远不成立——只要画面左边始终有个框
             * （人站着不动、或者模型把某个物体当人），锁存就卡死，
             * 之后所有越线都不再上报。超时把死锁变成"最多漏 5 秒"。
             */
            if (all_right ||
                (rt_tick_t)(rt_tick_get() - g_zone_tick) >
                    rt_tick_from_millisecond(TRIP_LATCH_MAX_MS))
            {
                g_zone_latch = 0;
            }
        }
    }

    g_prev_n = n;
    for (i = 0; i < n; i++)
    {
        g_prev_cx[i] = cx[i];
        g_prev_cy[i] = cy[i];
    }
}
#endif /* FH_APP_TRIP_NUM > 0 */

static FH_VOID res_deal(FH_TAD_GROUP_t *tad_data)
{
    FH_SINT32 i = 0;
    for (i = 0; i < tad_data->num; i++)
    {
        if (tad_data->object[i].objId != -1)
        {
            switch (tad_data->object[i].mode)
            {
            case TAD_NO:
                /* 每帧都会走到这里，不打日志，否则串口被刷得看不见别的 */
                break;
            case TAD_TRIP_LTOR:
            case TAD_TRIP_RTOL:
            case TAD_TRIP_ALL:
            case TAD_AREA_ENTER:
            case TAD_AREA_EXIT:
            case TAD_AREA_INTRUSION:
            case TAD_AREA_ALL:
                trip_report(tad_data->object[i].mode);
                break;
            }
        }
    }
}

static FH_SINT32 nna_result_to_tad(FH_SINT32 grp_id, TAD_HANDLE tadHandle, NN_RESULT nn_result)
{
    FH_TAD_GROUP_t tad_in;
    FH_TAD_GROUP_t tad_out;
    FH_UINT32 chn_w = 0;
    FH_UINT32 chn_h = 0;
    FH_SINT32 i = 0;

    /*
     * ★ tad_out 清零（防御性）。
     *   库里的 FH_TAD_Process 会把 mode 逐个写一遍，但只写它处理过的那些
     *   目标框；一旦某帧的框数和上一帧不同，多出来的槽位就保留着原来的值。
     *   res_deal() 是按 num 遍历的，正常够不到这些残留；不过这里只花 480 字节
     *   就把"读到一个没写过的 mode"这条路径彻底堵死，值这个价。
     */
    memset(&tad_out, 0, sizeof(tad_out));
    memset(&tad_in,  0, sizeof(tad_in));

    /*
     * 5 秒一次的心跳：帧率 + 本帧检出人数。
     *
     * "跨线不报警"要先分清到底是哪一环坏了：
     *   这里 det 一直是 0  ⇒ 模型没跑/认不出人，跟绊线无关；
     *   det 有数但没人形框 ⇒ OSD 画在别的通道；
     *   有框但没 [trip] 日志 ⇒ 判定参数问题（本地兜底也该报，那就更可疑）。
     * 没有这行日志，现场只能靠猜。
     */
    {
        static rt_tick_t hb_last = 0;
        static int       hb_frame = 0;
        rt_tick_t hb_now = rt_tick_get();

        hb_frame++;
        if (hb_last == 0)
        {
            hb_last = hb_now;
        }
        else if ((rt_tick_t)(hb_now - hb_last) >= (rt_tick_t)(5 * RT_TICK_PER_SECOND))
        {
            /*
             * 这一行是现场判断"到底哪一环坏了"的唯一依据，所以四个数一次给全：
             *   fps   太低（<5）说明 NN 被别的线程饿着，判定自然跟不上；
             *   det   一直是 0 ⇒ 模型没跑或认不出人，跟绊线无关；
             *   cx    目标中心点（-1 = 上一帧没人）。和 line 一比就知道
             *         人是"还没走到线"还是"已经走过去了但没报"。
             */
            SDK_PRT(model_name, "[ivs] %d fps, det=%d, cx=%d, line=%d\n",
                    hb_frame / 5, (int)nn_result.result_det.nn_result_num,
                    (int)g_last_cx,
                    (int)trip_task_cfg[0].trip_cfg.endPoint.x);
            hb_frame = 0;
            hb_last  = hb_now;
        }
    }


    /*
     * ★ 没人时也必须擦框（2026-10-28）
     *   原来这里第一句就是 `if (num == 0) return;`，于是最后一帧的红框
     *   会永久冻在画面上 —— 人走了框还在，比不画更容易让人误判。
     *   clear_screen() 会把上一帧占过的 gbox_id 全部 disable，
     *   顺手把规则线重画一遍，所以这里直接调它，再把 id 水位落回去。
     */
    if (nn_result.result_det.nn_result_num == 0)
    {
        clear_screen(grp_id, IVS_CHN, FH_APP_TRIP_NUM + FH_APP_PERI_NUM);
        current_max_gbox_id[grp_id] = g_box_id_start;

        /*
         * ★ 不要立刻清历史（详见 TRIP_PREV_HOLD 的注释）。
         *   漏检一帧就把"上一帧人在哪"抹掉，正好抵消掉"人在这一帧跨线"的判定。
         *   这里给 2 帧宽限，超过才认为人真的离开画面。
         */
        g_last_det = 0;
        if (g_prev_n > 0)
        {
            if (++g_prev_miss > TRIP_PREV_HOLD)
            {
                g_prev_n    = 0;
                g_prev_miss = 0;
                /* 人真的离开画面了，区域锁存一起放掉，否则下次进入永远不报 */
                g_zone_latch = 0;
            }
        }
        return FH_SDK_SUCCESS;
    }

    /* 本帧有人：宽限计数清零 */
    g_prev_miss = 0;
    g_last_det  = nn_result.result_det.nn_result_num;

    chn_w = get_vpu_chn_w(grp_id, IVS_CHN);
    chn_h = get_vpu_chn_h(grp_id, IVS_CHN);

    tad_in.num = nn_result.result_det.nn_result_num;

    /* 硬上限：object[] 只有 FH_TAD_MAX_RECT_NUM(30) 个槽。
     * 模型一帧最多也就几个人，但这里不能赌 —— 越界写会直接砸坏栈上
     * 的 tad_out / 返回地址，现象是"随机死机"，极难定位。 */
    if (tad_in.num > FH_TAD_MAX_RECT_NUM) tad_in.num = FH_TAD_MAX_RECT_NUM;

    for (i = 0; i < tad_in.num; i++)
    {
        tad_in.object[i].pos.x = nn_result.result_det.nn_result_info[i].box.x * chn_w;
        tad_in.object[i].pos.y = nn_result.result_det.nn_result_info[i].box.y * chn_h;
        tad_in.object[i].pos.w = nn_result.result_det.nn_result_info[i].box.w * chn_w;
        tad_in.object[i].pos.h = nn_result.result_det.nn_result_info[i].box.h * chn_h;
    }

    clear_screen(grp_id, IVS_CHN, FH_APP_TRIP_NUM + FH_APP_PERI_NUM);

    /*
     * ★ 人形框只画一次（2026-10-28）
     *   原来 draw_box() 放在规则循环里，有几条规则就把同一批框画几遍，
     *   而且每遍开头还把 current_max_gbox_id 重置回 4 —— 纯属浪费 OSD 带宽，
     *   多规则时还会让 clear_screen 的擦除范围算错。挪到循环外面。
     */
    draw_box(grp_id, IVS_CHN, tad_in);

    /*
     * 兜底判定先跑：它只看"这一帧和上一帧的中心点"，
     * 不依赖库里的跟踪状态，哪怕库那边一次都不报，这里也一定能报。
     */
#if (FH_APP_TRIP_NUM > 0)
    {
        FH_SINT32 lx = trip_task_cfg[0].trip_cfg.endPoint.x;

        simple_trip(&tad_in, lx, (FH_SINT32)chn_w);
    }
#endif

    for (i = 0; i < FH_APP_TRIP_NUM; i++)
    {
        FH_TAD_Process(tadHandle, &trip_task_cfg[i], &tad_in, &tad_out);
        res_deal(&tad_out);
    }

    for (i = 0; i < FH_APP_PERI_NUM; i++)
    {
        FH_TAD_Process(tadHandle, &peri_task_cfg[i], &tad_in, &tad_out);
        res_deal(&tad_out);
    }

    return FH_SDK_SUCCESS;
}

FH_VOID *nna_ivs(FH_VOID *args)
{
    FH_SINT32 ret = FH_SDK_SUCCESS;

    TAD_HANDLE tadHandle = NULL;
    NN_HANDLE nn_handle = NULL;
    NN_TYPE nn_type;
    FH_CHAR model_path[256];
    VPU_INFO *vpu_info;
    VPU_CHN_INFO *vpu_chn_info;
    NN_INPUT_DATA nn_input_data;
    NN_RESULT nn_result;
    FH_SINT32 grp_id = *((FH_SINT32 *)args);
    FH_CHAR thread_name[20];
    FH_VIDEO_FRAME pstVpuframeinfo;
    FH_UINT32 handle_lock;

    vpu_info = get_vpu_config(grp_id);

    if (vpu_info->enable)
    {
        vpu_chn_info = get_vpu_chn_config(grp_id, NN_CHN);
        if (!vpu_chn_info->enable)
        {
            SDK_ERR_PRT(model_name, "VPU CHN[%d] not enable !\n", NN_CHN);
            goto Exit;
        }
    }
    else
    {
        SDK_ERR_PRT(model_name, "VPU GRP[%d] not enable !\n", grp_id);
        goto Exit;
    }

#ifdef FH_NN_PERSON_DETECT_ENABLE
    nn_type = PERSON_DET;
    strcpy(model_path, "./models/persondet.nbg");
#elif defined(FH_NN_C2D_ENABLE)
    nn_type = PERSON_VEHICLE_DET;
    strcpy(model_path, "./models/c2det.nbg");
#elif defined(FH_NN_FACE_DET_ENABLE)
    nn_type = FACE_DET;
    strcpy(model_path, "./models/facedet.nbg");
#else
    SDK_ERR_PRT(model_name, "nn_type NULL !\n");
    goto Exit;
#endif

    tadHandle = tad_init(); // tad 初始化
    SDK_CHECK_NULL_PTR(model_name, tadHandle);

    ret = trip_setting(grp_id, IVS_CHN);
    SDK_FUNC_ERROR_GOTO(model_name, ret);

    ret = area_setting(grp_id, IVS_CHN);
    SDK_FUNC_ERROR_GOTO(model_name, ret);

    ret = nna_init(grp_id, &nn_handle, nn_type, model_path, vpu_chn_info->width, vpu_chn_info->height);
    SDK_FUNC_ERROR_GOTO(model_name, ret);

    nn_input_data.width = vpu_chn_info->width;
    nn_input_data.height = vpu_chn_info->height;
    nn_input_data.stride = vpu_chn_info->width;

    SDK_PRT(model_name, "nna_ivs[%d] Start!\n", grp_id);

    g_nna_running[grp_id] = 1;

    memset(thread_name, 0, sizeof(thread_name));
    sprintf(thread_name, "nna_ivs_%d", grp_id);
    prctl(PR_SET_NAME, thread_name);
    sleep(1);
    while (!g_nna_stop[grp_id])
    {
        /* 每处理一帧报一次活（G1CH2 约 15fps）。lock_vpu_stream 等不到新帧时
         * 会一直阻塞，这里就停跳 —— 10s 约等于 150 帧没出结果，判为异常。 */
        dog_beat(DOG_NN);

        ret = lock_vpu_stream(grp_id, NN_CHN, &pstVpuframeinfo, 0, &handle_lock, 0);
        SDK_FUNC_ERROR_CONTINUE(model_name, ret);

        ret = getVPUStreamToNN(pstVpuframeinfo, &nn_input_data);
        if (!ret)
        {
            ret = nna_get_obj_det_result(nn_handle, nn_input_data, &nn_result);
            SDK_FUNC_ERROR_CONTINUE(model_name, ret);

            ret = unlock_vpu_stream(grp_id, NN_CHN, handle_lock, &pstVpuframeinfo);
            SDK_FUNC_ERROR_CONTINUE(model_name, ret);

            ret = nna_result_to_tad(grp_id, tadHandle, nn_result);
            SDK_FUNC_ERROR_CONTINUE(model_name, ret);
        }
        else
        {
            ret = unlock_vpu_stream(grp_id, NN_CHN, handle_lock, &pstVpuframeinfo);
            SDK_FUNC_ERROR_CONTINUE(model_name, ret);
            usleep(10 * 1000);
        }
    }

    /* 退出循环时补一次心跳：nna_release/tad_deinit 走硬件释放，若耗时超过
     * 6s 的判定阈值，会被 dog 误判成卡死。补这一下把余量留给收尾。 */
    dog_beat(DOG_NN);

    ret = nna_release(grp_id, &nn_handle);
    SDK_FUNC_ERROR_GOTO(model_name, ret);
    SDK_PRT(model_name, "nna_ivs[%d] End!\n", grp_id);
Exit:
    tad_deinit(tadHandle); // tad 反初始化
    g_nna_running[grp_id] = 0;

    return NULL;
}

FH_SINT32 sample_fh_nna_ivs_start(FH_SINT32 grp_id)
{
    FH_SINT32 ret = FH_SDK_SUCCESS;
    pthread_t thread;
    pthread_attr_t attr;

    if (g_nna_running[grp_id])
    {
        SDK_PRT(model_name, "nna_ivs[%d] already running!\n", grp_id);
        return FH_SDK_SUCCESS;
    }

    g_nna_grpid[grp_id] = grp_id;
    g_nna_stop[grp_id] = 0;

    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
#ifdef __RTTHREAD_OS__
    pthread_attr_setstacksize(&attr, 256 * 1024);
    /*
     * ★ 必须显式设优先级（2026-10-31）。
     *
     *   pthread_attr_init() 给的是 PTHREAD_DEFAULT_PRIORITY
     *   = RT_THREAD_PRIORITY_MAX/2 + MAX/4 = 192（compat/posix/pthread_attr.c:20），
     *   而 pthread_create 会把它原样传给 rt_thread_init（pthread.c:318）。
     *
     *   两套语义正好相反：POSIX 里 sched_priority 越大越高，RT-Thread 里越小越高。
     *   于是检测线程实际被放在 192 —— 比 tcpip(100)、网卡收发(126) 都低。
     *   现象：网页一开双路预览，帧全被网络线程抢走，检测变慢、NN 通道积压，
     *   越线那一刻的帧被覆盖，表现为"越线了但没抓到"。
     *
     *   定 14 的依据：
     *     - 必须 < 100，否则又会被协议栈压住（这是本次的核心修复）；
     *     - 低于 cf_face/talk(12)、tkdown(13)：音频丢一块就是爆音，检测晚一帧只是 66ms；
     *     - 高于 tshell 与各类一次性网络任务(20)：命令行卡住不该拖慢检测。
     *   主循环阻塞在 lock_vpu_stream 等帧上，帧没来就让出 CPU，不会饿死低优先级线程。
     */
    {
        struct sched_param sp;
        sp.sched_priority = 14;
        pthread_attr_setschedparam(&attr, &sp);
    }
#endif
    if (pthread_create(&thread, &attr, nna_ivs, &g_nna_grpid[grp_id]))
    {
        SDK_ERR_PRT(model_name, "pthread_create failed\n");
        goto Exit;
    }

    return ret;

Exit:
    return -1;
}

FH_SINT32 sample_fh_nna_ivs_stop(FH_SINT32 grp_id)
{
    FH_SINT32 ret = FH_SDK_SUCCESS;

    if (g_nna_running[grp_id])
    {
        g_nna_stop[grp_id] = 1;

        while (g_nna_running[grp_id])
        {
            usleep(10);
        }
    }
    else
    {
        SDK_PRT(model_name, "nna_ivs[%d] not running!\n", grp_id);
    }

    return ret;
}

/* ================= 给网页读的运行态 =================
 *
 * 之前"绊线不报"排查不下去，最大的障碍是板上状态只能从串口看。
 * 把 det / cx / line 三个数暴露出来以后，网页上就能直接判断：
 *   det=0        → 模型没跑或认不出人（跟绊线无关）
 *   cx 与 line   → 人在线的哪一侧；人一直没跨过 line 就不会报，这是几何不是 bug
 * 全部读全局变量，不碰 NN 线程，代价可以忽略。
 *
 * 返回正在运行的组数（0 = IVS 没起来）。
 */
int ivs_status(int *det, int *cx, int *line, int *run)
{
    int grp;
    int n = 0;

    if (det)  *det  = (int)g_last_det;
    if (cx)   *cx   = (int)g_last_cx;
    if (line) *line = (int)trip_task_cfg[0].trip_cfg.endPoint.x;

    for (grp = 0; grp < MAX_GRP_NUM; grp++)
    {
        if (g_nna_running[grp]) n++;
    }
    if (run) *run = n;

    return n;
}

/* ================= 运行期调规则线 =================
 *
 *   trip              看当前线位 + 左侧/右侧各多宽
 *   trip x 400        把线挪到 x=400（G1 的 800 宽坐标）
 *   trip dbg on|off   逐帧打印检出框中心点（排查"为什么没报"）
 *
 * 为什么留这个命令：规则线位置是最需要现场试的参数 —— 装在哪、人从哪边过，
 * 只有站在现场才知道。改一次烧一次固件太慢，这里改完立刻生效，不用重启。
 * 改完会重新下发 OSD 竖线和规则，上一帧的人形框会闪一下属正常。
 */
static int trip_cmd(int argc, char **argv)
{
    int grp;

    if (argc >= 3 && !strcmp(argv[1], "dbg"))
    {
        g_trip_dbg = (argv[2][0] == '1' || argv[2][0] == 'o' || argv[2][0] == 'y');
        rt_kprintf("[trip] debug %s\n", g_trip_dbg ? "ON (每帧打印中心点)" : "off");
        return 0;
    }

    if (argc >= 3 && !strcmp(argv[1], "x"))
    {
        g_trip_x = atoi(argv[2]);
        if (g_trip_x < 2) g_trip_x = 2;

        for (grp = 0; grp < MAX_GRP_NUM; grp++)
        {
            if (!g_nna_running[grp]) continue;
            trip_setting(grp, IVS_CHN);
            /* 规则线是 OSD 画的，重画一次才看得见新位置 */
            current_max_gbox_id[grp] = g_box_id_start;
        }
        rt_kprintf("[trip] x -> %d\n", (int)g_trip_x);
        return 0;
    }

    rt_kprintf("[trip] x=%s  (default = chn_w/12 ≈ 贴左边缘, use 'trip x N' to move)\n",
               (g_trip_x >= 0) ? "custom" : "auto");
    rt_kprintf("[trip] dbg=%s  det=%d  cx=%d  zone=%s\n",
               g_trip_dbg ? "ON" : "off", (int)g_last_det, (int)g_last_cx,
               g_zone_latch ? "LATCHED" : "clear");

    /*
     * 抓拍统计。现场判断"越线没图"到底卡在哪一段就看这一行：
     *   ok  = 0 且 fail 在涨  → 判定在触发，但抓不到图（看是不是没开 vlcview）
     *   ok 和 fail 都不动     → 判定根本没触发（cx 一直在线右侧，或 det=0）
     *   pending 恒 > 0        → 抓拍线程被卡住（cf_grab 出不来了）
     */
    {
        int ok = 0;
        int fail = 0;
        int pend = 0;

        evi_stats(&ok, &fail, &pend);
        rt_kprintf("[trip] snap ok=%d fail=%d pending=%d\n", ok, fail, pend);
    }
    for (grp = 0; grp < MAX_GRP_NUM; grp++)
    {
        FH_UINT32 w;
        FH_UINT32 lx;

        if (!g_nna_running[grp]) continue;

        w  = get_vpu_chn_w(grp, IVS_CHN);
        lx = trip_task_cfg[0].trip_cfg.endPoint.x;

        rt_kprintf("  grp%d chn%d line x=%d w=%d h=%d | left=%d right=%d\n",
                   grp, IVS_CHN, (int)lx,
                   (int)w, (int)get_vpu_chn_h(grp, IVS_CHN),
                   (int)lx, (int)(w - lx));

        /*
         * 线贴到最左边以后，"跨越"这条判据本来就失效了 —— 能不能报
         * 取决于中心点进不进得了线左侧，所以这里提示的是去看 cx，
         * 而不是像上一版那样劝用户把线往右挪。
         */
        if (lx < w / 10)
        {
            rt_kprintf("  note: 线已贴边，按\"区域进入\"判定（cx<%d 即报）。"
                       "若不报请看上面 cx 是否一直 >%d\n", (int)lx, (int)lx);
        }
    }
    return 0;
}

MSH_CMD_EXPORT_ALIAS(trip_cmd, trip, trip [x N] : show/move trip line on AI channel);
