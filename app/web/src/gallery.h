/*
 * gallery.h - 证据相册 + 事件日志（给网页的 /api/gallery、/api/evi 用）
 *
 * 为什么放 RAM 不放 flash：
 *   板上没有可写文件系统（也不用为了几张图去挂 littlefs 再擦 flash），
 *   而 .bss 是从 DDR 里切的，50 槽 x 96KB = 4.8MB 只是把堆起点往后挪，
 *   **不占一点 flash**。掉电即失，这在"证据留痕"场景下是可接受的：
 *   真正的存证靠网页上点一下下载到 PC。
 *
 * 为什么每槽定长 96KB：
 *   可变长记录在环形缓冲里会产生碎片，回收时要搬数据（memmove 几十 KB），
 *   搬的时候抓拍线程和网页请求都会被卡住。定长最坏浪费 50KB/槽，
 *   换来的是"抓一帧 = 一次 memcpy"，没有别的开销。
 *   800x480 qp50 实测 30~60KB，复杂场景会更大，所以上限跟
 *   cloud_face 的 CF_JPEG_MAX 一起定在 96KB（两者必须一致，理由见 EVI_JPEG_MAX）。
 *
 * 抓帧复用 cloud_face 的通道（默认 G1CH1 / 1112 那一路），
 * 不另开 vicap，否则和识别线程抢同一个通道会互相丢帧。
 * 2026-10-29 起进一步改成"直接读 cloud_face 已缓存的最新帧"，
 * 完全不碰 vicap、不等帧 —— 见 gallery.c 的 evi_grab_frame()。
 */
#ifndef __GALLERY_H__
#define __GALLERY_H__

/*
 * 同时保留几张。
 *   6 → 16 → 50（2026-10-28）：6 张在连续报警场景下十几秒就被环形覆盖，
 *   16 张也只够几分钟。50 张 x 96KB ≈ 4.8MB（见下面的内存账），
 *   换来的是"最近半小时的记录都在"。
 *
 *   ★ 内存账（这是能不能上 50 的唯一约束，改之前先看这里）
 *     .bss 全部落在 OS 内存区：FH_RTT_OS_MEM_SIZE = 0x01800000 = 24MB，
 *     从 0x40000000 到 0x41800000。堆是 [&__bss_end, 0x41800000]。
 *     50 槽 x 96KB ≈ 4.8MB，.bss 合计约 5.1MB，bss_end ≈ 0x4071C000，
 *     **堆还剩 14.8MB**，够（媒体 + 网络 + 语音一起也就几个 MB）。
 *     再往上加（比如 100 张 = 9.6MB）就危险了：堆掉到 10MB 以下，
 *     MJPEG/RTSP 的缓冲会开始申请失败 —— 50 就是上限。
 *
 *   想调整就改这一个数，代价是每 +1 张 +96KB .bss。
 *   ⚠ 上限还受 /api/gallery 的分页约束：每张元数据约 57 字节，
 *     单次最多回 lim=20 张（1140B，g_body 是 1800B）。50 张靠分页返回，
 *     一次全塞进 JSON 会超 buffer，被截断成非法格式，页面显示"空"。
 */
#define EVI_SLOTS     50           /* 同时保留几张 */
/*
 * 单张上限。必须 ≥ cloud_face 的 CF_JPEG_MAX，否则那边能给的帧
 * 这边会以 -4 拒掉 —— 现象和"抓不到图"一模一样，但根因在尺寸上。
 * 2026-10-29：跟 CF_JPEG_MAX 一起 64KB → 96KB。
 */
#define EVI_JPEG_MAX  (96 * 1024)

/* 证据类型：网页上的筛选页签按这个分 */
#define EVI_T_MANUAL  0            /* 手动抓拍 */
#define EVI_T_AUTO    1            /* 自动/连续抓拍 */
#define EVI_T_FACE    2            /* 识别命中（有人名） */
#define EVI_T_ALERT   3            /* 陌生人 / 异常 */

typedef struct
{
    int  id;          /* 1..N 递增，删除后不复用；0 = 空槽 */
    int  type;        /* EVI_T_* */
    int  size;        /* JPEG 字节数 */
    int  w, h;        /* 从 SOF 段里读出来的分辨率，读不到为 0 */
    unsigned up;      /* 抓拍时刻的开机秒数 —— 板上没有 RTC，这是唯一可信时间 */
    int  valid;
} evi_item_t;

/* 清空所有槽并挂上事件日志。幂等；webd start 时调一次。 */
void evi_init(void);

/* 抓一帧存进相册。type = EVI_T_*。
 * 返回 >=0：槽位号；-2 人脸线程正忙（此时抓帧会抢通道）；
 * -3 抓帧失败；-4 单帧超过 EVI_JPEG_MAX；-5 未初始化。
 * ⚠ 会阻塞（等 cf_busy + 等帧，最长约 1 秒），**不要**从实时线程里调。
 *   越线自动留证请用下面的 evi_post()。 */
int  evi_capture(int type);

/* 异步抓拍：入队立刻返回（几微秒），由 evi 工作线程去抓。
 * 返回 0 已受理，-5 未初始化（webd 还没 start）。
 *
 * 为什么要异步：trip_report 跑在 IVS/NN 线程里，一次同步抓帧能把
 * 整个检测循环卡住一两秒，期间的帧全丢 —— 表现就是"越线只报第一次"。 */
int  evi_post(int type);

/* 抓拍统计：成功张数 / 失败次数 / 队列里还压着几个。全部可传 NULL。 */
void evi_stats(int *ok, int *fail, int *pending);

/* 有效张数（= 已填槽数） */
int  evi_count(void);

/* 按"新→旧"的显示顺序取第 idx 张（0 = 最新）。返回 0 成功，<0 越界。 */
int  evi_item_at(int idx, evi_item_t *out);

/* 按 id 取 JPEG 数据指针（指向静态槽，不要再拷一份）。返回 0 成功。 */
int  evi_data(int id, const unsigned char **p, int *len);

/* 按 id 删除。返回 0 成功，<0 没这张。 */
int  evi_delete(int id);

/* 一键清空：所有槽作废，id 水位不回退（见 gallery.c 的说明）。
 * 返回被清掉的张数。 */
int  evi_clear(void);

/* ---------------- 事件日志 ---------------- */
/* 只记"发生了什么"，不带图。相册管图、日志管事，两者独立：
 * 识别失败也可能值得留痕，但那一刻没有可存的图。
 * 12 → 24：这个列表是纯文本（每条约 42 字节），加一倍只多 500 字节，
 * 却能让"图片来源"和"事件"在时间上对得起来。 */
#define EVT_MAX 24

void evt_add(const char *kind, const char *text);
int  evt_count(void);
/* idx: 0 = 最新 */
int  evt_at(int idx, unsigned *up, char *kind, int kcap, char *text, int tcap);

#endif /* __GALLERY_H__ */
