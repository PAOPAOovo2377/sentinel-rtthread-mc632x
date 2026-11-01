/*
 * cloud_face.h - 云端人脸识别（板端采集 + 云端比对 + 运行时登记）
 *
 * 背景：本 SDK 没有交付本地人脸识别所需的 5 个闭源库与 8.1MB 三合一模型
 *       （facedet_full / facerec / face_base），本地跑不了。但"知道是谁"这件事
 *       不必在板上做：板上本来就在跑人形检测，抓到人形后把一帧 JPEG 送出去，
 *       由云端返回相似度即可。整个链路用现有物料就能跑通。
 *
 * 两种后端，同一套抓帧与结果处理：
 *   CF_MODE_PC : POST 原始 JPEG 到局域网 PC（tools/face_server.py）。
 *                登记 = 把这一帧发给 PC，PC 落盘成 known/<name>.jpg，
 *                之后由 PC 端的 face_recognition 做真正的 1:N。
 *   CF_MODE_XF : POST 讯飞"人脸比对" WebAPI（api.xf-yun.com/v1/private/s67c9c78c）。
 *                讯飞只做 1:1 比对、没有免费人脸库，所以"登记"在板端做：
 *                把登记时的 JPEG 存进 CF_REF_SLOTS 个内存槽，识别时逐张比对、
 *                取最高分。2 个人 = 每次 2 个请求。
 *
 * 登记（注册）不需要重新编译固件：
 *   face enroll <name>        抓当前画面登记
 *   网页 POST /api/enroll     上传一张照片登记（浏览器侧先用 canvas 压到 640px）
 *
 * 内存占用（全部静态，无堆分配，可在 map 文件里核对）：
 *   g_jpeg      96KB   单帧 JPEG 工作缓冲（两模式共用，PC 模式直接就是请求体）
 *   g_ref_pool  128KB  运行时登记的参考图（CF_REF_SLOTS x CF_REF_MAX，二进制 JPEG）
 *   g_resp      1KB    HTTP 响应
 *   合计约 225KB。参考图存二进制而不是 base64，省掉 1/3 的膨胀
 *   （base64 存法要 88KB/人）。
 *   编译期默认底图放 .rodata（flash，不占 RAM），默认是空的，见 tools/make_ref.py。
 */
#ifndef CLOUD_FACE_H
#define CLOUD_FACE_H

#ifdef __cplusplus
extern "C" {
#endif

#define CF_MODE_PC   0
#define CF_MODE_XF   1

/* 单帧 JPEG 上限。800x480 @ qp50 实测约 30~60KB；
 * 超过此值会丢弃该帧并打印告警，不会截断（截断的 JPEG 云端解析会失败）。
 *
 * ★ 2026-10-29：64KB → 96KB。
 *   64KB 对 800x480 的复杂场景（逆光、大片纹理、人贴着镜头）是不够的，
 *   一旦超限这一帧就被丢掉 —— 绊线那边等来的就是"没有新帧"，
 *   表现成"明明越线了但没抓到图"。多留 32KB 静态 RAM 把这根刺拔掉，
 *   比在现场猜"是不是模型/判定出问题"省事得多。 */
#define CF_JPEG_MAX  (96 * 1024)

/* 单个参考图上限，以及可在线登记的人数。
 * 每加 1 个 slot 多 64KB .bss；讯飞模式识别耗时也随人数线性增长，
 * 因为讯飞没有人脸库能力，只能一个人发一次请求。 */
#define CF_REF_MAX    (64 * 1024)
#define CF_REF_SLOTS  2

typedef struct {
    int   ok;           /* 1 = 本次请求整体成功 */
    int   matched;      /* 1 = 判定为同一人 */
    float score;        /* 相似度 0~1；PC 模式由服务端回传，讯飞模式为比对得分 */
    char  name[32];     /* 身份标签（PC 服务端返回；讯飞模式取命中的那个登记名） */
    int   ms;           /* 端到端耗时（毫秒） */
    int   img_len;      /* 本次上传的 JPEG 字节数 */
    int   http_code;    /* HTTP 状态码，或负的传输层错误码 */
    int   known;        /* 当前已登记人数（PC 服务端返回 / 板端参考图数） */
    char  msg[48];      /* 错误摘要 / 服务端 message */
} cf_result_t;

typedef void (*cf_result_cb_t)(const cf_result_t *r);

/* 初始化：创建信号量、拉起工作线程。幂等。返回 0 成功。 */
int  cf_init(void);
void cf_deinit(void);

/* 后端选择与参数 */
int  cf_set_mode(int mode);
int  cf_get_mode(void);
const char *cf_mode_name(void);

/* PC 模式目标，默认 192.168.137.1:5000 /api/face，登记走 /api/enroll */
void cf_set_pc_target(const char *host, int port, const char *path);
void cf_set_pc_enroll_path(const char *path);

/* 讯飞模式凭证（不调用则用 xf_http_asr.h 里的默认宏） */
void cf_set_xf_credential(const char *appid, const char *api_key, const char *api_secret);
/* 讯飞域名预置 IP：本工程网络 DNS 不可靠，填了就直接连，跳过 DNS */
void cf_set_xf_server_ip(const char *ip);
/* 讯飞同一人判定阈值，默认 0.67（官方建议值） */
void cf_set_xf_threshold(float th);

/* 抓帧源：默认 G1 CH1（800x480，就是网页 1112 那一路，带人形框） */
void cf_set_capture_channel(int grp_id, int chn_id);

/* 结果回调（可在里面刷 OSD / 推网页），传 NULL 取消 */
void cf_set_result_cb(cf_result_cb_t cb);

/* 详细日志开关 */
void cf_set_verbose(int on);

/* ---------- 识别 ---------- */

/* 同步：抓一帧并上传，阻塞到出结果或超时。
 * 返回 0 = 请求成功（不代表比对成功，比对结果看 out->matched） */
int  cf_run(int frame_timeout_ms, cf_result_t *out);

/* 异步：立刻返回，结果由 cf_last_result() 轮询。
 * 网页按钮走这条路 —— webd 是单线程，同步等 1~15 秒会把整个控制台堵住。
 * 返回 0 = 已受理，-6 = 上一个任务还没跑完。 */
int  cf_run_async(void);

/* 1 = 有任务在跑。网页据此显示"识别中…"并避免重复触发。 */
int  cf_busy(void);

/* 强制清掉 busy / 未完成的上传（返回之前是否 busy）。
 *
 * 为什么需要：cf_face 线程正常会在任务结束时置 g_busy=0，但只要它卡在
 * 某次 TLS 握手/读响应里（换网络、DNS 半通、对端不回包都可能），g_busy
 * 就一直为 1，此后 run 和 enroll **全部**返回 BUSY，插电重启才能恢复。
 * 现场表现就是"人脸登记请求失败"，而且怎么点都一样。
 *   · 自动：cf_submit 里有 45 秒看门狗，超时会自己清掉并打串口；
 *   · 手动：网页 /api/face?m=reset 或 msh 的 face 命令，立刻清。
 * 清掉之后正在跑的线程若还活着，它结束时再置 0 也无害。 */
int  cf_busy_reset(void);

/* 只抓帧不发送，用于自检：返回字节数，<0 失败。 */
int  cf_grab_only(int frame_timeout_ms);
const unsigned char *cf_last_jpeg(int *len);

/* 最近一帧的年龄（ms），<0 表示当前没有可用帧（没抓过 / 正在写 / 上次失败）。
 *
 * 用途：绕过 cf_busy 的等待，直接从 g_jpeg 里取已经编好的那一帧。
 * 绊线抓拍走这条路 —— 识别线程忙不忙都不影响留证，也不会去抢 vicap 通道。
 * 调用方自己判断新鲜度（建议 ≤300ms，5fps 流里就是两帧以内）。 */
int  cf_last_jpeg_age_ms(void);

/* 抓帧源预检（不碰帧、不订阅、不阻塞）。
 *
 * 为什么需要它：真正的订阅/抓帧都在 cf_face 线程里做，一旦通道没开，
 * cf_run_async() 仍然返回 0（任务已受理），失败原因只会写进结果结构体，
 * 网页要等下一轮轮询才看得到；更糟的是如果用户根本没启动 vlcview，
 * get_mjpeg_config() 里通道号还是 0，界面上表现为"点了没反应"。
 * 网页在 m=run 之前先调这个，通道没开就当场把原因回给浏览器。
 *
 * 返回 0 = 通道就绪；-2 = MJPEG 没使能（多半是没跑 vlcview）；
 *      -1 = 参数/内部状态异常。 */
int  cf_check_source(void);

/* 当前抓帧源（供网页显示"GPx CHy -> VENC n"） */
void cf_get_capture_channel(int *grp_id, int *chn_id);

/* 最近一次结果（供网页 /api/state 查询） */
const cf_result_t *cf_last_result(void);

/* 距最近一次作业结束过了多少毫秒。-1 = 还没跑过。
 * 网页用它判断结果是否已过期：超过 15s 就不再当成"当前画面"来显示。 */
int  cf_result_age_ms(void);

/* ---------- 登记 ---------- */

/* 同步：抓当前画面登记为 name。PC 模式转发给 PC，讯飞模式存进内存槽。
 * 返回 0 成功。 */
int  cf_enroll(const char *name, int frame_timeout_ms);

/* 异步：立刻返回，用 cf_last_result() 看结果（网页登记按钮走这条） */
int  cf_enroll_async(const char *name);

/* 同步：用一段现成的 JPEG 登记（网页上传照片用）。 */
int  cf_enroll_mem(const char *name, const unsigned char *jpg, int len);

/* 流式登记（给 webd 用，避免为了收 64KB 再开一块缓冲）：
 *   begin  选一个空闲槽当暂存区，返回 0 成功
 *   feed   追加数据，长度超 CF_REF_MAX 返回 -1
 *   commit 讯飞模式=落库并返回 0（同步，微秒级）
 *          PC   模式=投递给后台线程，返回 **1**（异步！结果看 cf_last_result()）
 *   abort  放弃（未 commit 前池槽内容无效，不会影响已登记的人）
 *
 * ⚠ commit 返回 1 时**不能**立刻认为登记完成：PC 模式的 HTTP 往返在
 *   人脸线程里跑。webd 收到 1 就回 OK:uploading，网页靠 /api/state
 *   的 f_msg 取最终结果。这样 webd 不会被一个跨网请求堵住。 */
int  cf_enroll_begin(const char *name);
int  cf_enroll_feed(const unsigned char *p, int n);
int  cf_enroll_commit(void);
void cf_enroll_abort(void);

/* 已登记人数 / 取第 idx 个的名字 */
int  cf_ref_count(void);
const char *cf_ref_name(int idx);

/* 清空**运行时**登记的参考图（编译期底图不受影响）。返回清掉的个数。 */
int  cf_ref_clear(void);

/* ---------------- 编译期默认底图（可选） ----------------
 * 由 tools/make_ref.py 生成 face_ref_jpg.c，内容是 .rodata 里的二进制 JPEG。
 * 默认是空的（长度 0），需要用"开机就有一个人"的场景才生成。
 * 注意 .rodata 占的是 flash，而 flash 只剩几十 KB，别放太大的图。 */
extern const unsigned char cf_ref_jpg[];
extern const int  cf_ref_jpg_len;

#ifdef __cplusplus
}
#endif

#endif /* CLOUD_FACE_H */
