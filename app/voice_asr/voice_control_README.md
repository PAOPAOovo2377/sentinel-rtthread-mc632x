# MC632X 语音控制（以太网 + 讯飞 HTTP 一句话识别）· 集成与编译

> 方案：以太网接入，用 SDK 自带的 **libcurl + mbedtls** 调讯飞语音听写 WebAPI（iat），
> 非流式（一句话识别）。不依赖任何 WebSocket 库。
> 阶段0 目标：**先用预录 PCM 文件打通"联网 + HMAC-SHA256 签名 + 识别"链路**，再接麦克风硬件。

---

## 一、文件清单（本目录 `voice_asr/`）

| 文件 | 作用 |
|---|---|
| `xf_http_asr.h/.c` | 讯飞 iat WebAPI 客户端（libcurl POST + mbedtls 签名），与硬件无关 |
| `audio_capture.h/.c` | 音频采集抽象层；默认"文件回退"（读 `/pcm_test.raw`），留 AUDIO_HW_ENABLED 接口 |
| `voice_cmd.h/.c`   | RT-Thread 语音控制线程 + `voice_test` msh 命令 + 命令词匹配桩 |
| `lv_conf.h`        | LVGL v8 配置（32 位 ARGB8888 + 透明，配合已有 `lv_port_disp.c`） |

> 已有 `lv_port_disp.c`（ARGB8888→ARGB1555 透明叠加）**无需改动**，本 `lv_conf.h` 与其对齐。

---

## 二、部署位置

把整个 `voice_asr/` 目录拷到 SDK 工作树的 `app/media_demo/voice_asr/`
（与 `lvgl_port/`、`sample_demo/` 并列）。

---

## 三、menuconfig 需开启的项（`make appconfig`）

当前 `media_demo` 默认可能未开网络加密库，需确认以下使能：

```
RT-Thread Components  →  Network  →
    Socket abstraction layer (SAL)           [RT_USING_SAL]      ★
    Network interface device (netdev)        [RT_USING_NETDEV]   ★ 以太网需要
    light-weight TCP/IP stack  →  (lwip 2.1.2，通常已开)

RT-Thread Online Packages  → 
    misc packages  →  mbedtls                       [PKG_USING_MBEDTLS]  ★ TLS/HMAC
    IoT  →  libcurl                             [PKG_USING_LIBCURL]  ★ HTTP 客户端
            (libcurl 的 TLS backend 选 mbedtls)

RT-Thread Components  →  Device virtual file system (DFS)  [RT_USING_DFS]  （文件回退测试才需要）
    →  elmfat (FAT) 文件系统

APP config  →  Open Audio [FH_APP_OPEN_AUDIO]   （后续接麦克风硬件时开）
```

> 带 ★ 为阶段0 必开。开启后 `make`（或 `scons`）会拉包并自动链接 `-lcurl -lmbedtls`。

---

## 四、`media.mk` 改法

编辑 `app/media_demo/script/rtt/chip/MC632X/media.mk`，在 SRC 段追加：

```makefile
# ---- LVGL 屏上 UI ----
LVGL_PORT = $(LOCAL_DIR)/lvgl_port
SRC += $(LVGL_PORT)/lv_port_disp.c
SRC += $(LVGL_PORT)/ui_display.c

# ---- 语音控制（以太网 + 讯飞 HTTP） ----
VOICE_DIR = $(LOCAL_DIR)/voice_asr
SRC += $(VOICE_DIR)/xf_http_asr.c
SRC += $(VOICE_DIR)/audio_capture.c
SRC += $(VOICE_DIR)/voice_cmd.c

# ---- LVGL 源码（v8，全部 src 子目录） ----
SRC += $(shell find $(LVGL_PORT)/lvgl/src -name '*.c')

# ---- 头文件路径 ----
CFLAGS += -I$(LVGL_PORT)
CFLAGS += -I$(LVGL_PORT)/lvgl
CFLAGS += -I$(VOICE_DIR)
```

> 若你的构建系统不支持 `$(shell find ...)`，改为逐目录列出 LVGL 的 `src/lv_core`、`src/lv_draw`、
> `src/lv_font`、`src/lv_gpu`、`src/lv_hal`、`src/lv_misc`、`src/lv_objx`、`src/lv_themes`、`src/lv_widgets` 等。

---

## 五、填入讯飞凭证

二选一：

**方式 A（宏，简单）**：编辑 `xf_http_asr.h` 顶部
```c
#define XF_APPID      "你的AppId"
#define XF_API_KEY    "你的APIKey"
#define XF_API_SECRET "你的APISecret"
```

**方式 B（运行时注入，推荐，避免硬编码）**：在 `demo_entry` 初始化时调用
```c
xf_asr_set_credential("你的AppId", "你的APIKey", "你的APISecret");
```

> 凭证在讯飞开放平台控制台 → 创建"语音听写"应用后获取。

---

## 六、主流程接入（两行）

在 `sample_common_demo.c` 视频/检测初始化之后：

```c
lv_init();
lv_port_disp_init();   /* 已有 lv_port_disp.c */
ui_display_start();    /* 起 LVGL 主循环线程 */
voice_cmd_start();     /* 起语音控制线程 + 注册 voice_test 命令 */
```

---

## 七、编译 & 烧录

```bash
cd <SDK工作树>
make appconfig        # 确认上面菜单项已开
make -j$(nproc)
cp app/media_demo/out/bin/media_demo.bin  D:\share\    # 拷出烧录
```

---

## 八、阶段0 上板验证（文件回退，不依赖麦克风）

1. **准备测试音频**（在 PC 上）：
   ```bash
   ffmpeg -i 任意.wav -ar 16000 -ac 1 -f s16le pcm_test.raw
   ```
   把 `pcm_test.raw` 放到板子文件系统根目录（`/pcm_test.raw`）。

2. **校时（必须）**：讯飞签名用到 RFC1123 时间，系统时间需准确。
   ```bash
   ntp_sync                      # 或开启 NTP 自动同步包
   date                          # 确认时间接近真实时间
   ```

3. **确认以太网已 up**（RTSP 能推说明已通）：
   ```bash
   ifconfig                      # 看到 eth0 有 IP
   ```

4. **触发一次识别**：
   ```bash
   voice_test
   ```
   串口应看到：
   ```
   [voice] captured 160000 bytes pcm, recognizing...
   [xf] partial: 打开
   [xf] FINAL: 打开摄像头
   [voice] cmd: 打开摄像头
   [voice] -> RELAY ON
   ```

> 走通即证明"以太网 + libcurl + mbedtls 签名 + 讯飞识别"全链路 OK。

---

## 九、接麦克风硬件（阶段1）

把 `audio_capture.c` 里 `AUDIO_HW_ENABLED` 分支的 `open/read/close` 换成
`app/audio_demo/sample_audio` 中对 **FH_AC_CODEC_INTERNAL** 的 AI（音频输入）调用：

- 初始化内部 codec：16000 / 单声道 / 16bit；
- `audio_capture_read()` 从 AI 通道循环取 PCM 填缓冲；
- 编译时定义 `AUDIO_HW_ENABLED`（在 media.mk 的 CFLAGS 加 `-DAUDIO_HW_ENABLED`），
  删掉 `#define PCM_TEST_PATH` 回退。

然后 `voice_test` 就会录真实语音上传识别。也可把触发从 msh 命令改成
"NPU 检测到人形 → 自动录音识别"或按键事件（释放 `g_trig` 信号量即可）。

---

## 十、与屏上 UI 整合

`voice_cmd.c` 维护了 `g_voice_text`（最近一次识别文本）。在 `ui_display.c` 的
刷新逻辑里加一个 label，读取 `voice_cmd_last_text()` 显示最后一条指令即可：

```c
/* ui_display_refresh() 内 */
lv_label_set_text(ui_voice_label, voice_cmd_last_text());
```

---

## 十一、后续阶段

- **阶段1**：命令词映射接到真实 GPIO（报警/继电器）、VOU 切画面。
- **阶段2（可选）**：若需要"边说边出字"，再自研 wss 客户端接 iat 流式（工作量大，当前非流式已满足控制场景）。
- **阶段3**：扬声器播"识别中/已执行"提示音（内部 ACODEC 放音，已确认支持）。

---

## 十二、关键约束提醒

- **内存**：mbedtls(~几十KB) + libcurl(~几十KB) + 160KB 音频缓冲 + 现有双摄/NPU/LVGL，
  64MB DDR2 紧张但够；识别期间可降 NPU 帧率或只开一路 RTSP。
- **音频格式**：必须是 16k/16bit/单声道 PCM，否则讯飞返回格式错误。
- **证书**：阶段0 `CURLOPT_SSL_VERIFYPEER=0`（不校验）；量产应开启证书校验。
- **时间**：上电必须 NTP 校时，否则签名 date 偏差过大 → 鉴权失败。
