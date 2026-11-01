# 云端人脸识别（cloud_face）

## 为什么是这个方案

SDK 没有交付本地人脸识别所需的东西，硬缺：

| 类型 | 缺什么 |
|---|---|
| 静态库 | `libnnfacedet_full_post.a` `libnnfacerec_post.a` `libfhalg_face_quality.a` `libfhalg_face_align.a` `libfhalg_face_rec.a` |
| 头文件 | `fhalg_face_rec_*` 全部 |
| 模型 | `facedet_full.nbg` `facerec.nbg`（官方是三合一 8.1MB 包） |

而自己训练也走不通：`trans_ckpt_to_nnip` 只吃 ZTV2 的检测网络，facerec 是 112×112
embedding 网络，不在支持范围；比对函数 `fhalg_face_rec_probe` 还是闭源的。

**所以换个分工**：板上只采集，云端做比对。板子本来就在跑人形检测（5fps），
抓到人形后把一帧 JPEG 发出去，云端返回"是谁"。这条路不需要任何新物料。

## 两种后端

| 模式 | 目标 | 登记动作实际发生在 | 1:N 由谁做 |
|---|---|---|---|
| `pc` | 局域网 PC（`tools/face_server.py`） | PC 硬盘 `known/名字.jpg` | PC 端 face_recognition |
| `xf` | 讯飞人脸比对 WebAPI | 板端内存槽（`g_ref_pool`） | 板端逐人发请求取最高分 |

两种模式共用同一块 JPEG 缓冲、同一套结果处理、同一套网页接口，切模式只改 URL/签名/解析。

> 讯飞只提供 1:1 比对接口，没有免费人脸库。所以 `xf` 模式的"多人"是板端循环实现的：
> 2 个人 = 每次识别 2 个 HTTPS 请求（约 2 秒）。`CF_REF_SLOTS` 默认 2，就是这个原因。
> `pc` 模式没有这个限制，PC 端做的是真正的 1:N。

## 实时登记（不需要重新编译固件）

这是本模块的关键能力：**要认的人随时加，不用改代码、不用重烧固件**。

三种方式，任选：

| 方式 | 操作 | 适用 |
|---|---|---|
| 网页点一下 | 人站在摄像头 1 前 → 网页「人脸识别」卡片 → 填姓名 → **登记** | 现场最快，答辩推荐 |
| 串口命令 | `face enroll 张三` | 调试时顺手 |
| 上传照片 | 网页选一张照片 + 填姓名 → **上传并登记** | 人不在现场时 |

网页上传那条会自动在浏览器里把照片压到 640px / ≤64KB（板端 `CF_REF_MAX` 是 64KB），
所以手机拍的 3MB 原图也能直接选。压完用 `canvas.toBlob` 重编码，不依赖任何后端能力。

两种模式的登记语义不同，要知道：

- **pc 模式**：板子把这一帧 POST 到 PC 的 `/api/enroll?name=张三`，PC 存成
  `known/张三.jpg` 并立刻加入索引。**掉电不丢**，同名再登记就是覆盖（想换张更好的照片就再来一次）。
- **xf 模式**：这一帧被存进板端的内存槽，**掉电会丢**（没有可写文件系统，
  不敢往跑代码的 SPI NOR 上写）。重启后重新登记一次即可，几秒钟的事。

## 内存账（这是本模块的设计核心）

| 缓冲 | 大小 | 说明 |
|---|---|---|
| `g_jpeg` | 64 KB | 单帧 JPEG 工作缓冲。PC 模式下它就是请求体本身（零拷贝） |
| `g_ref_pool` | 128 KB | 运行时登记的参考图，2 槽 × 64KB，存**二进制 JPEG** |
| `g_resp` | 1 KB | HTTP 响应（两种后端都 < 300 字节） |
| 线程栈 | 16 KB | mbedtls 握手要 6~10KB |
| 编译期底图 | 0（在 flash） | `.rodata`，不占 RAM |
| 发送块、请求头 | 3.5 KB | 在 `cf_http.c` 内部，复用 |

合计 **193 KB 静态 + 16 KB 栈**，无堆分配。
（本板 RT-Thread 堆有 21MB，`.bss` 结束地址 0x403684c1 → 0x41800000，这点占比不到 1%。）

四个省内存的关键设计：

1. **不拷贝就 base64**。常规写法是「memcpy 到 A → base64 到 B」，两张图要 64+88=152KB。
   这里 dmc 回调只 memcpy 一次到 `g_jpeg`，base64 交给 HTTP 的 body 回调
   **边读边编码**（1536 字节进、2048 字符出），全程只占一个 2KB 输出块。
2. **参考图存二进制**。base64 存法每人要 88KB，二进制只要 64KB，省 25%。
   发送时临时编码，不落地。
3. **上传暂存区复用参考图池**。浏览器发来的 64KB 照片直接流式写进
   将要转正的那个槽位（`cf_enroll_begin/feed/commit`），
   webd 侧不需要再开一块 64KB 接收缓冲。
4. **编译期底图放 .rodata**。默认是空的；只有"开机就要有某个人"才用 `make_ref.py` 生成，
   而且现在存二进制，同样一张图比 base64 版省 1/3 的 flash。

> ⚠ 已知约束：`rt_kprintf` 内部缓冲只有 128 字节，超长会**静默丢弃**（连 `\n` 一起）。
> 本模块所有日志都控制在 100 字节内，不要往 `rt_kprintf` 里塞 JSON。

## 快速开始：PC 模式（建议先走这条）

### 1. 电脑上起服务

```bash
cd cloud_face/tools
python face_server.py
```

零依赖（纯标准库）。看到 `监听 : http://0.0.0.0:5000` 即可。
浏览器打开 `http://localhost:5000` 能看到状态页。

可选增强（不做也能用）：
```bash
pip install opencv-python     # 加人脸检测，返回 face=0/1
pip install face_recognition  # 加 1:N 识别（**这条才是真的能认人**）
```

> 装 `face_recognition` 时 dlib 可能要编译，Windows 上建议用预编译 wheel：
> `pip install dlib-bin` 再 `pip install face_recognition`。

### 2. 板子上配置

```
ifconfig e0 192.168.137.100          # 板子 IP（按你实际网段）
dns 192.168.137.1
webd start                           # 网页控制台 :8080
face url 192.168.137.1 5000 /api/face
face mode pc
```

### 3. 打开网页，登记 + 识别

浏览器访问 `http://192.168.137.100:8080/`，右侧「人脸识别」卡片：

1. 人站在**摄像头 1** 前（默认抓 G1 CH1，就是 1112 那一路）
2. 填姓名 → 点**登记** → 等 1~2 秒，`已登记` 会变成 `1 人`
3. 点**识别一次** → 结果显示 `张三  0.93`（绿）或 `未匹配`（黄）
4. 想看连续效果就点**连续识别**（每 2.2 秒一次，识别中自动跳过，不会堆积）

串口同步会打（拆两行是因为 `rt_kprintf` 只有 128 字节缓冲）：

```
[face] pc: ok=1 match=1 score=0.931 img=42133 ms=412 ref=1
       msg=- name=张三
```

PC 端 `received/` 会存下每次识别的原始帧，**打开看一眼**——这是确认
"抓的是哪一路、画面正常"最快的方式。

### 4. 确认抓的是哪一路

```
face src 0 0     # 改成抓 G0 CH0（1080p，1111 那一路）
face src 1 1     # 改回来
```

> 1080p 那帧可能超过 64KB，超了会丢弃并打印带实际大小的告警
> （`frame 7xxxx > CF_JPEG_MAX 65536`），按提示调 `CF_JPEG_MAX` 或换回 `1 1`。

## 讯飞模式

### 1. 控制台开通

登录 https://console.xfyun.cn/ → 你的应用 → 添加能力 → 搜「人脸比对」。
凭证**和应用绑定，和语音听写共用同一组** `APIKey / APISecret`，
`cloud_face.c` 默认直接用 `xf_http_asr.h` 里的宏，不用重新填。

### 2. 切换 + 登记

```
face mode xf
face xfip 114.80.x.x        # 可选：本工程网络 DNS 不可靠，能拿到 IP 就填上跳过 DNS
                            # 填 IP 只改「连到哪」，SNI 和 Host 头仍是 api.xf-yun.com
                            # （讯飞 LB 靠 SNI 选 vhost，SNI 对不上会直接 RST）
face th 670                 # 可选：相似度阈值，默认 0.670（官方建议）
face enroll 张三             # ← 就在这里登记，不用重编译
```

或者直接在网页上点**登记**（网页和串口调的是同一个入口）。

### 3. 测试

```
face run
```

预期：

```
[face] xf: ok=1 match=1 score=0.8xx img=42xxx ms=1200 ref=1
       msg=- name=张三
```

判定逻辑：`score >= 0.67` 算同一人；多个人时取最高分，且
分数 ≥ 0.90 就提前结束（没必要再问下一个人）。

### 开机的默认底图（可选，一般不需要）

只有"上电就必须认得某个人、不接受手工登记"才需要：

```bash
pip install Pillow
python tools/make_ref.py --in zhangsan.jpg --max 480
```

会重写 `src/face_ref_jpg.c`，重新编译即可。注意这是占 **flash** 的
（.rodata），而 flash 只剩几十 KB，别放太大的图 —— 脚本会拦 64KB 以上。

## msh 命令一览

```
face run [timeout_ms]     抓一帧并识别（默认抓帧超时 3000ms）
face enroll <name>        抓当前画面登记为 <name>   ← 不用重编译
face refs                 列出已登记的人（含编译期底图）
face del all              清除运行时登记的人
face grab                 只抓帧不上传，自检用
face mode [pc|xf]         查看/切换后端
face url <host> [port] [path]
face xfip [ip]            讯飞预置 IP，传空串清除
face th <千分数>          讯飞阈值，如 face th 670
face src <grp> <chn>      抓帧源，默认 1 1
face stat                 当前配置 + 堆用量 + 上次结果
```

## 网页接口

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/api/state` | 状态 JSON，含 `f_ok / f_mt / f_sc / f_nm / f_ref / f_busy` |
| GET | `/api/face?m=run` | 触发一次识别，**立即返回** `OK:started` / `BUSY` |
| GET | `/api/face?m=enroll&n=张三` | 抓当前画面登记，立即返回 `OK:enrolling` |
| GET | `/api/face?m=clear` | 清空运行时登记 |
| POST | `/api/enroll?n=张三` | body = 一整张 JPEG，登记这张照片 |
| GET | `/api/cmd?c=<命令>` | 执行任意 msh 命令 |

识别/登记都设计成**异步**：webd 是单线程，同步等 1~15 秒会把整个控制台堵住，
网页看起来像卡死。任务由 `cf_face` 线程跑，网页轮询 `/api/state` 取结果。

`f_sc` 是**千分数整数**（931 = 0.931），板端不用 `%f`：
newlib-nano 的浮点格式化不稳，而且 float 截断会把 0.996 打成 0.995。

## 故障排查

| 现象 | 原因 / 处理 |
|---|---|
| `VPU[1] CHN[1] MJPEG not enabled` (grab code=-2) | MJPEG 通道没起来。先确认 `vlcview -p 1246` 跑过、1112 端口能看 |
| `grab frame failed, code=-3` | `dmc_subscribe` 失败，订阅表（8 个槽）被别的模块占满 |
| `grab frame failed, code=-4` | 3 秒内没等到帧。同上；确认抓的通道真的有 MJPEG 在出流 |
| `grab frame failed, code=-6` | 帧太大超过 `CF_JPEG_MAX`，日志里会给出实际大小 |
| `grab frame failed, code=-5` | 空帧（`frame_len<=0`），一般是编码通道刚启动或已停 |
| `http -3` | TCP 连不上。先 `ping` 一下 PC，检查 PC 防火墙是否放行 5000 |
| `http 401/403` | 讯飞鉴权失败。检查 APIKey/APISecret；再查系统时间（偏差要 < 300 秒，`date` 看一下） |
| `face xfip` 之后 TLS 直接断、无 alert | 预置 IP 只该换「连到哪」，SNI/Host 仍必须是 `api.xf-yun.com`。本模块已分开处理，若自己改 `cf_http_post` 别把两者合成一个参数 |
| `no ref face` | xf 模式下一个人都没登记。`face enroll 张三` 或网页点登记 |
| `ERR:no-slot,clear-first` | 上传照片时两个槽都被人占了。`face del all` 后再传，或调大 `CF_REF_SLOTS` |
| `ERR:too-big` | 照片超过 64KB。网页会自动压，串口直传时自己先缩到 640px |
| `ERR:not-jpeg` | 选错文件了（板端查 `FFD8` 头） |
| `BUSY` | 上一次任务还没跑完，等它结束（抓帧 3s + 上传 15s×人数）。此时拒绝改参考图，避免读到半张 |
| PC 端收到是黑图 | 抓错通道了，`face src` 换一路试 |
| 网页点登记没反应 | 看串口有没有 `[face] enrolling`。没有就是 webd 没起来或 `/api/face` 404 |
| 名字显示成乱码 | 名字上限 31 字节（UTF-8），约 10 个汉字；超了会按字符边界截断，不会出半个字 |

## 文件清单

```
cloud_face/
├── include/
│   ├── cloud_face.h      对外接口 + 内存说明 + 登记 API
│   └── cf_http.h         极简 HTTP 客户端接口
├── src/
│   ├── cloud_face.c      抓帧 / 双后端 / 参考图池 / 登记 / 结果解析 / msh 命令
│   ├── cf_http.c         TCP + mTLS + 流式 POST
│   └── face_ref_jpg.c    编译期底图（默认空，由 make_ref.py 生成）
├── tools/
│   ├── face_server.py    PC 端服务：/api/face 识别 + /api/enroll 登记
│   └── make_ref.py       生成编译期底图源文件
└── README.md
```

网页侧配套（`app/media_demo/web/`）：

```
web/
├── webui/index.html      网页源码（含人脸面板）
├── tools/pack_web.py     把 html 压成 gzip 的 C 数组
└── src/
    ├── webd.c            HTTP 服务 + /api/face + POST /api/enroll
    └── web_index_gz.c    由 pack_web.py 生成，勿手改
```

编译接入已写进 `script/rtt/chip/MC632X/media.mk` 末尾，不想用就注释掉那 4 行。

**改了网页必须重跑** `python web/tools/pack_web.py`，否则固件里还是旧页面。

## 关于内存的一个提醒

`cloud_face` 的静态缓冲是 193KB，编译后 `.bss` 会多这么多。
如果之后发现堆不够用（`face stat` 里看 `heap peak`），按这个顺序砍：

1. `CF_REF_SLOTS` 2 → 1：省 64KB，代价是讯飞模式只能登记 1 个人
2. `CF_JPEG_MAX` 64KB → 48KB：省 16KB（800×480 正常在 30~50KB）
3. `CF_REF_MAX` 64KB → 48KB：省 2×16KB，代价是只能传更小的登记照
