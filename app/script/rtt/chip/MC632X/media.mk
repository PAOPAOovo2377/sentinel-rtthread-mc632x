#lib
MEDIA_LIBS = -ldci_rtt -lveu -lvou -llcm_module -lfyfb
MEDIA_LIBS += -lsensor_rtt -lmipi_rtt -lrtt_isp -lisp_rtt -lispcore_rtt -lmedia_process_rtt -ldsp_rtt

# DEMO
# AF
ifeq ($(FH_APP_OPEN_AF), y)
SRCS += $(wildcard $(SAMPLE_DEMO_DIR)/af/src/sample_common_af_demo.c)
MEDIA_LIBS += -laf
CFLAGS += -DFH_AF_ENABLE
endif
# ISP Strategy
ifeq ($(FH_APP_OPEN_ISP_STRATEGY_DEMO), y)
SRCS += $(wildcard $(SAMPLE_DEMO_DIR)/isp_strategy/src/sample_common_isp_demo.c)
CFLAGS += -DFH_ISP_DEMO_ENABLE
endif
# MD and CD
ifneq (, $(findstring y, $(FH_APP_OPEN_MOTION_DETECT) $(FH_APP_OPEN_RECT_MOTION_DETECT) $(FH_APP_OPEN_COVER_DETECT)))
SRCS += $(wildcard $(SAMPLE_DEMO_DIR)/motion_and_cover_detect/src/sample_common_md_cd.c)
CFLAGS += -DFH_MD_CD_ENABLE
endif
# NNA_APP
MEDIA_LIBS += -lnnprimary_det_post -lnna_rtt
NN_ENABLE = n
ifeq ($(NN_ENABLE_G0), y)
NN_ENABLE = y
else ifeq ($(NN_ENABLE_G1), y)
NN_ENABLE = y
endif
ifeq ($(NN_ENABLE), y)
CFLAGS += -DFH_NN_ENABLE
ifneq (, $(findstring y, $(FH_NN_PERSON_DETECT_ENABLE) $(FH_NN_C2D_ENABLE) $(FH_NN_FACE_DET_ENABLE) $(FH_APP_OPEN_PVF_DETECT)))
# NNA IVS
CFLAGS += -DFH_NN_DETECT_ENABLE
ifeq ($(FH_APP_OPEN_IVS), y)
SRCS += $(wildcard $(SAMPLE_DEMO_DIR)/nna_app/src/sample_nna_ivs.c)
MEDIA_LIBS += -ltrip_area_arm_rtt -ltrack_arm_rtt -lm
CFLAGS += -DFH_NN_IVS_ENABLE
else
SRCS += $(wildcard $(SAMPLE_DEMO_DIR)/nna_app/src/sample_nna_obj_detect.c)
endif
endif
ifeq ($(FH_NN_FACE_SNAP_ENABLE), y)
SRCS += $(wildcard $(SAMPLE_DEMO_DIR)/nna_app/src/sample_nna_face_snap.c)
MEDIA_LIBS += -lnnfacedet_full_post -lfhalg_face_quality
CFLAGS += -DFH_FACE_SNAP_ENABLE
endif
ifeq ($(FH_NN_FACE_REC_ENABLE), y)
SRCS += $(wildcard $(SAMPLE_DEMO_DIR)/nna_app/src/sample_nna_face_rec.c)
MEDIA_LIBS += -lnnfacedet_full_post -lfhalg_face_quality -lnnfacerec_post -lfhalg_face_align -lfhalg_face_rec
CFLAGS += -DFH_FACE_REC_ENABLE
endif
ifeq ($(FH_NN_GESTURE_REC_ENABLE), y)
SRCS += $(wildcard $(SAMPLE_DEMO_DIR)/nna_app/src/sample_nna_gesture_rec.c)
MEDIA_LIBS += -lnngesturedet_post -lnngesturerec_post -lfhalg_imgcrop
CFLAGS += -DFH_GESTURE_REC_ENABLE
endif
ifeq ($(FH_NN_LP_REC_ENABLE), y)
SRCS += $(wildcard $(SAMPLE_DEMO_DIR)/nna_app/src/sample_nna_lpr.c)
MEDIA_LIBS += -lnnlpdet_full_post -lnnlprec_post -lfhalg_plate_rectify
CFLAGS += -DFH_LP_REC_ENABLE
endif
endif
# OSD
ifeq ($(FH_APP_OPEN_OVERLAY), y)
SRCS += $(wildcard $(SAMPLE_DEMO_DIR)/overlay/src/sample_common_overlay_v2.c)
SRCS += $(wildcard $(SAMPLE_DEMO_DIR)/overlay/src/overlay_gbox.c)
CFLAGS += -DFH_OVERLAY_ENABLE

ifeq ($(FH_APP_OVERLAY_LOGO), y)
SRCS += $(wildcard $(SAMPLE_DEMO_DIR)/overlay/src/overlay_logo.c)
endif

ifeq ($(FH_APP_OVERLAY_TOSD), y)
SRCS += $(wildcard $(SAMPLE_DEMO_DIR)/overlay/src/overlay_tosd.c)

ifeq ($(FH_APP_OVERLAY_TOSD_MENU), y)
SRCS += $(wildcard $(SAMPLE_DEMO_DIR)/overlay/osd_menu/mod/*.c)
SRCS += $(wildcard $(SAMPLE_DEMO_DIR)/overlay/osd_menu/top/*.c)
SRCS += $(wildcard $(SAMPLE_DEMO_DIR)/overlay/osd_menu/mod/menu/*.c)
SRCS += $(wildcard $(SAMPLE_DEMO_DIR)/overlay/osd_menu/mod/callback/*.c)
SRCS += $(wildcard $(SAMPLE_DEMO_DIR)/overlay/osd_menu/component/*.c)
SRCS += $(wildcard $(SAMPLE_DEMO_DIR)/overlay/osd_menu/core/*.c)
SRCS += $(wildcard $(SAMPLE_DEMO_DIR)/overlay/osd_menu/*.c)
endif
endif

ifeq ($(FH_APP_OVERLAY_MASK), y)
SRCS += $(wildcard $(SAMPLE_DEMO_DIR)/overlay/src/overlay_mask.c)
endif

endif
# SmartIR
FH_APP_USING_IRCUT = n
ifeq ($(FH_APP_USING_IRCUT_G0), y)
FH_APP_USING_IRCUT = y
else ifeq ($(FH_APP_USING_IRCUT_G1), y)
FH_APP_USING_IRCUT = y
else ifeq ($(FH_APP_USING_IRCUT_G2), y)
FH_APP_USING_IRCUT = y
else ifeq ($(FH_APP_USING_IRCUT_G3), y)
FH_APP_USING_IRCUT = y
endif
ifeq ($(FH_APP_USING_IRCUT), y)
SRCS += $(wildcard $(SAMPLE_DEMO_DIR)/smartIR/src/sample_common_smartIR.c)
CFLAGS += -DFH_SMART_IR_ENABLE
endif
# VENC
ifeq ($(FH_APP_OPEN_VENC), y)
SRCS += $(wildcard $(SAMPLE_DEMO_DIR)/venc/src/sample_common_venc.c)
CFLAGS += -DFH_VENC_DEMO_ENABLE
endif
# Vise
MEDIA_LIBS += -lvise_rtt
ifeq ($(FH_APP_OPEN_VISE), y)
SRCS += $(wildcard $(SAMPLE_DEMO_DIR)/vise_demo/src/sample_common_vise_demo.c)
CFLAGS += -DFH_VISE_DEMO_ENABLE
endif

#sensor
OVOS02K_MIPI_ENABLE = 0
ifeq ($(FH_USING_OVOS02K_MIPI_G0),y)
OVOS02K_MIPI_ENABLE = 1
else ifeq ($(FH_USING_OVOS02K_MIPI_G1),y)
OVOS02K_MIPI_ENABLE = 1
else ifeq ($(FH_USING_OVOS02K_MIPI_G2),y)
OVOS02K_MIPI_ENABLE = 1
endif
ifeq ($(OVOS02K_MIPI_ENABLE),1)
CFLAGS += -DFH_USING_OVOS02K_MIPI
MEDIA_LIBS += -lovos02k_mipi_rtt
endif

OVOS02D_MIPI_ENABLE = 0
ifeq ($(FH_USING_OVOS02D_MIPI_G0),y)
OVOS02D_MIPI_ENABLE = 1
else ifeq ($(FH_USING_OVOS02D_MIPI_G1),y)
OVOS02D_MIPI_ENABLE = 1
else ifeq ($(FH_USING_OVOS02D_MIPI_G2),y)
OVOS02D_MIPI_ENABLE = 1
endif
ifeq ($(OVOS02D_MIPI_ENABLE),1)
CFLAGS += -DFH_USING_OVOS02D_MIPI
MEDIA_LIBS += -lovos02d_mipi_rtt
endif

OVOS04C10_MIPI_ENABLE = 0
ifeq ($(FH_USING_OVOS04C10_MIPI_G0),y)
OVOS04C10_MIPI_ENABLE = 1
else ifeq ($(FH_USING_OVOS04C10_MIPI_G1),y)
OVOS04C10_MIPI_ENABLE = 1
else ifeq ($(FH_USING_OVOS04C10_MIPI_G2),y)
OVOS04C10_MIPI_ENABLE = 1
endif
ifeq ($(OVOS04C10_MIPI_ENABLE),1)
CFLAGS += -DFH_USING_OVOS04C10_MIPI
MEDIA_LIBS += -lovos04c10_mipi_rtt
endif

OVOS05_MIPI_ENABLE = 0
ifeq ($(FH_USING_OVOS05_MIPI_G0),y)
OVOS05_MIPI_ENABLE = 1
else ifeq ($(FH_USING_OVOS05_MIPI_G1),y)
OVOS05_MIPI_ENABLE = 1
else ifeq ($(FH_USING_OVOS05_MIPI_G2),y)
OVOS05_MIPI_ENABLE = 1
endif
ifeq ($(OVOS05_MIPI_ENABLE),1)
CFLAGS += -DFH_USING_OVOS05_MIPI
MEDIA_LIBS += -lovos05_mipi_rtt
endif

OVOS08_MIPI_ENABLE = 0
ifeq ($(FH_USING_OVOS08_MIPI_G0),y)
OVOS08_MIPI_ENABLE = 1
else ifeq ($(FH_USING_OVOS08_MIPI_G1),y)
OVOS08_MIPI_ENABLE = 1
endif
ifeq ($(OVOS08_MIPI_ENABLE),1)
CFLAGS += -DFH_USING_OVOS08_MIPI
MEDIA_LIBS += -lovos08_mipi_rtt
endif

IMX415_MIPI_ENABLE = 0
ifeq ($(FH_USING_IMX415_MIPI_G0),y)
IMX415_MIPI_ENABLE = 1
else ifeq ($(FH_USING_IMX415_MIPI_G1),y)
IMX415_MIPI_ENABLE = 1
endif
ifeq ($(IMX415_MIPI_ENABLE),1)
CFLAGS += -DFH_USING_IMX415_MIPI
MEDIA_LIBS += -limx415_mipi_rtt
endif

SC3336_MIPI_ENABLE = 0
ifeq ($(FH_USING_SC3336_MIPI_G0),y)
SC3336_MIPI_ENABLE = 1
else ifeq ($(FH_USING_SC3336_MIPI_G1),y)
SC3336_MIPI_ENABLE = 1
endif
ifeq ($(SC3336_MIPI_ENABLE),1)
CFLAGS += -DFH_USING_SC3336_MIPI
MEDIA_LIBS += -lsc3336_mipi_rtt
endif

SC5336_MIPI_ENABLE = 0
ifeq ($(FH_USING_SC5336_MIPI_G0),y)
SC5336_MIPI_ENABLE = 1
else ifeq ($(FH_USING_SC5336_MIPI_G1),y)
SC5336_MIPI_ENABLE = 1
endif
ifeq ($(SC5336_MIPI_ENABLE),1)
CFLAGS += -DFH_USING_SC5336_MIPI
MEDIA_LIBS += -lsc5336_mipi_rtt
endif

SC4336_MIPI_ENABLE = 0
ifeq ($(FH_USING_SC4336_MIPI_G0),y)
SC4336_MIPI_ENABLE = 1
else ifeq ($(FH_USING_SC4336_MIPI_G1),y)
SC4336_MIPI_ENABLE = 1
endif
ifeq ($(SC4336_MIPI_ENABLE),1)
CFLAGS += -DFH_USING_SC4336_MIPI
MEDIA_LIBS += -lsc4336_mipi_rtt
endif

SC530AI_MIPI_ENABLE = 0
ifeq ($(FH_USING_SC530AI_MIPI_G0),y)
SC530AI_MIPI_ENABLE = 1
else ifeq ($(FH_USING_SC530AI_MIPI_G1),y)
SC530AI_MIPI_ENABLE = 1
endif
ifeq ($(SC530AI_MIPI_ENABLE),1)
CFLAGS += -DFH_USING_SC530AI_MIPI
MEDIA_LIBS += -lsc530ai_mipi_rtt
endif

OV2735_MIPI_ENABLE = 0
ifeq ($(FH_USING_OV2735_MIPI_G0),y)
OV2735_MIPI_ENABLE = 1
else ifeq ($(FH_USING_OV2735_MIPI_G1),y)
OV2735_MIPI_ENABLE = 1
endif
ifeq ($(OV2735_MIPI_ENABLE),1)
CFLAGS += -DFH_USING_OV2735_MIPI
MEDIA_LIBS += -lov2735_mipi_rtt
endif

SC3335_MIPI_ENABLE = 0
ifeq ($(FH_USING_SC3335_MIPI_G0),y)
SC3335_MIPI_ENABLE = 1
else ifeq ($(FH_USING_SC3335_MIPI_G1),y)
SC3335_MIPI_ENABLE = 1
endif
ifeq ($(SC3335_MIPI_ENABLE),1)
CFLAGS += -DFH_USING_SC3335_MIPI
MEDIA_LIBS += -lsc3335_mipi_rtt
endif

CV2005_MIPI_ENABLE = 0
ifeq ($(FH_USING_CV2005_MIPI_G0),y)
CV2005_MIPI_ENABLE = 1
else ifeq ($(FH_USING_CV2005_MIPI_G1),y)
CV2005_MIPI_ENABLE = 1
else ifeq ($(FH_USING_CV2005_MIPI_G2),y)
CV2005_MIPI_ENABLE = 1
endif
ifeq ($(CV2005_MIPI_ENABLE),1)
CFLAGS += -DFH_USING_CV2005_MIPI
MEDIA_LIBS += -lcv2005_mipi_rtt
endif

DUMMY_SENSOR_ENABLE = 0
ifeq ($(FH_USING_DUMMY_SENSOR_G0),y)
DUMMY_SENSOR_ENABLE = 1
else ifeq ($(FH_USING_DUMMY_SENSOR_G1),y)
DUMMY_SENSOR_ENABLE = 1
else ifeq ($(FH_USING_DUMMY_SENSOR_G2),y)
DUMMY_SENSOR_ENABLE = 1
endif
ifeq ($(DUMMY_SENSOR_ENABLE),1)
CFLAGS += -DFH_USING_DUMMY_SENSOR
MEDIA_LIBS += -ldummy_sensor_rtt
endif

# ---- 音频输入源选择（只改这两行）----
#   内嵌音频（默认，已实测识别成功）：启用 -DPCM_USE_EMBEDDED
#   板载麦克风：启用 -DAUDIO_SRC_MIC、注释掉 -DPCM_USE_EMBEDDED
#
# 麦克风模式额外要求：ARC 第二核固件必须开启音频，即
# make_arcfirmware/app/arc_aov_demo/config.h 里 #define ENABLE_AUDIO，
# 并重新编译 ARC 固件（rtthread_arc_aov.bin.img）。
# ARC 固件是**打包进 media_demo.img 的 seg1**一起烧的，不需要单独烧录：
#   build/tools/gengz.sh 生成的 media_demo.img 含两段（16 字节段头 + 载荷）
#     seg0 = MCU 固件(raw)，seg1 = ARC 固件(lzop)，段间按 4KB 对齐填 0xFF。
#   app/media_demo/local_post_build.sh 会在 make 时自动把自编译的 ARC 固件
#   注入 seg1，所以只要先编 ARC 再 make MCU，就得到带音频的完整镜像。
# 音频库 -lacw_mpi_arm_rtt / -laudio_codec_arm_rtt 已由 build/rtt_config.mk 自动链接
# （受 CONFIG_FH_USING_ACW 门控，该项在 drivers/Kconfig 里叫 "audio"，默认已开），
# 此处无需重复添加。

CFLAGS += -DAUDIO_SRC_MIC
# CFLAGS += -DPCM_USE_EMBEDDED          # 想切回内嵌音频时取消注释

LIBS += $(MEDIA_LIBS)
# ★ 用 SDKROOT 拼路径，不要写死 /home/fullhan/... 绝对路径 ——
#   否则整份 SDK 换目录/换机器就编不过（找不到头文件）。
#   SDKROOT 由 build/apps.mk 定义，指向 rt-thread 根目录。
VOICE_DIR = $(SDKROOT)/app/media_demo/voice_asr
SRCS += $(VOICE_DIR)/xf_http_asr.c
SRCS += $(VOICE_DIR)/audio_capture.c
SRCS += $(VOICE_DIR)/voice_cmd.c
SRCS += $(VOICE_DIR)/ws_client.c
# pcm_test_data.c 已删除（2026-10-26）：那是 51.5KB rodata 的离线调试预录音频，
# 只在 -DPCM_USE_EMBEDDED 时被 audio_capture.c 引用。当前固定走 -DAUDIO_SRC_MIC
# （真实麦克风），该分支是死代码，去掉省 51.5KB flash。
# 需要恢复离线调试时，从 _deleted_code_20261026/pcm_test_data.c 拷回并放开下面这行。
# SRCS += $(VOICE_DIR)/pcm_test_data.c
EXT = $(SDKROOT)/external
CFLAGS += -I$(VOICE_DIR)
CFLAGS += -I$(EXT)/mbedtls/include
CFLAGS += -I$(EXT)/libcurl
# 音频 API 头 <dsp/fh_audio_mpi.h> 的搜索路径已由 SDK 自带：
#   script/rtt/Rtt.mk:42  INCS += -I$(SDKROOT)/lib/$(CHIP_ID_LOWER)/inc/dsp
#   script/rtt/Rtt.mk:38  INCS += -I$(SDKROOT)/lib/inc
#   script/rtt/Rtt.mk:41  INCS += -I$(SDKROOT)/lib/inc/types
# 因此此处无需重复添加 -I。
# Rtt.mk:36 的 INCS = 覆盖了 kernel/include，这里补回（voice_asr 头文件 #include <rtthread.h> 需要）
CFLAGS += -I$(SDKROOT)/kernel/include
# rtthread.h 内部 #include <rtconfig.h>，该文件由 appconfig 生成在 out/rtconfig/
CFLAGS += -I$(SDKROOT)/out/rtconfig
# rtthread.h:634 在 RT_USING_FINSH 时 #include <finsh_api.h>（MSH_CMD_EXPORT 也需要），该头在 shell/
CFLAGS += -I$(SDKROOT)/shell

# ---------------------------------------------------------------------------
# LVGL 8.3 UI（app/media_demo/ui）
#
# 目录结构：
#   ui/ui_main.c        界面本体（状态栏/摄像头卡片/语音面板/系统面板）
#   ui/lv_port_disp.c   LVGL -> VO Graphic Layer 的移植层
#   ui/lv_conf.h        配置（LV_COLOR_DEPTH=32 对齐 PIXEL_FORMAT_RGB_8888）
#   ui/font/*.c         lv_font_conv 生成的中文子集字体
#   ui/lvgl/            LVGL 8.3.11 源码（已裁掉 demos/examples/docs/tests）
#
# 用 find 而不是逐个 wildcard：LVGL 的 src 有 20+ 层子目录，
# 少写一层就是一个"undefined reference lv_xxx"的坑。
# ---------------------------------------------------------------------------
# ------------------------- LVGL 总开关（2026-10-25 封存）-------------------------
#   UI_ENABLE = y  → 编 LVGL + ui/*.c，`ui` 命令可用
#   UI_ENABLE = n  → 一个 .c 都不编，`ui` 命令消失
#
# 封存理由：
#   1) flash —— img 分区 3MB(0x120000~0x420000) 只剩约 60KB，LVGL 内核 + 控件
#      + 三套子集字体编译后要占 400KB 上下，是当前最大的一块可回收空间；
#   2) RAM —— lv_conf.h 里 LV_MEM_CUSTOM=0 ⇒ LV_MEM_SIZE(128KB) 是**静态数组**，
#      写在 .bss，不管有没有敲过 `ui` 命令都一直占着；敲 `ui` 之后还会再从 SDK
#      池里拿 1.5MB 帧缓冲 + 576KB YUV，这两个只在运行时分配；
#   3) 用途 —— 屏目前不出图（硬件待定责），界面改用网页控制台(webd + MJPEG)顶替。
#
# 源码、字体、lv_conf.h 全部原样保留，**不做任何删除**，恢复只需把下面这行改成 y：
#   UI_ENABLE = y
# 也可以在命令行临时覆盖（命令行变量优先于本文件的赋值）：
#   make UI_ENABLE=y
# ------------------------------------------------------------------------------
UI_ENABLE = n

ifeq ($(UI_ENABLE), y)

UI_DIR   = $(SDKROOT)/app/media_demo/ui
LVGL_DIR = $(UI_DIR)/lvgl

# 只编 LVGL 的内核/控件/软件渲染，下面这些整段丢掉：
#   draw/sdl|nxp|renesas|arm2d|stm32_dma2d|swm341_dma2d
#       —— 别的芯片和 PC 仿真器的 GPU 后端，lv_conf.h 里 LV_USE_GPU_* 全是 0
#   extra/libs
#       —— png/bmp/gif/sjpg/qrcode/freetype/rlottie/ffmpeg/tiny_ttf 这些第三方库，
#          文件开头**没有** LV_USE_xxx 保护（比如 lodepng.c 是原样搬进来的），
#          不排掉就会真的编进镜像 —— 单 lodepng 就 ~100KB，
#          而 img 分区 0x120000~0x420000 只有 3MB。
# 头文件都还在原处，lvgl.h 里的 #if LV_USE_PNG 之类照常能 include，只是不产 .o。
LVGL_SKIP_DIRS = \
    $(LVGL_DIR)/src/draw/sdl \
    $(LVGL_DIR)/src/draw/nxp \
    $(LVGL_DIR)/src/draw/renesas \
    $(LVGL_DIR)/src/draw/arm2d \
    $(LVGL_DIR)/src/draw/stm32_dma2d \
    $(LVGL_DIR)/src/draw/swm341_dma2d \
    $(LVGL_DIR)/src/extra/libs

LVGL_SRCS := $(shell find $(LVGL_DIR)/src -name '*.c')
LVGL_SRCS := $(filter-out $(addsuffix /%.c,$(LVGL_SKIP_DIRS)),$(LVGL_SRCS))

SRCS += $(wildcard $(UI_DIR)/*.c)
SRCS += $(wildcard $(UI_DIR)/font/*.c)
SRCS += $(LVGL_SRCS)

# LVGL 上游是按 -Wall -Wextra 自测的，但这里的工具链版本更老，
# 偶尔会在 -O2 下报 -Wmaybe-uninitialized 这类误报。UI 自己的代码仍然严格
# （-Werror 照旧），只对 LVGL 的 .o 放开 -Werror，免得一个上游误报就卡住整个
# 编译。target-specific 变量在 apps.mk 的规则里同样生效。
#   注：APPOUT 在 build/apps.mk 里才定义（本文件先于它被 include），
#   所以这里用同样的表达式自行展开一次，保证 .o 路径字符串完全一致。
UI_APPOUT   := $(shell pwd)/out
LVGL_OBJS   := $(patsubst %.c,$(UI_APPOUT)/%.o,$(LVGL_SRCS))
$(LVGL_OBJS): APP_CFLAGS += -Wno-error

# -I$(UI_DIR)      ：font/*.c 里 #include "lvgl/lvgl.h"
# -I$(LVGL_DIR)    ：ui_main.c / lv_port_disp.c 里 #include "lvgl.h"
# LV_CONF_INCLUDE_SIMPLE 让 lv_conf_internal.h 直接 #include "lv_conf.h"，
# 而不是走默认的相对路径 ../../lv_conf.h（虽然也能对，但显式更稳）。
# LV_KCONFIG_IGNORE：跳过 lv_conf_kconfig.h。那个文件里有一段
#   #ifdef __RTTHREAD__  →  #include <lv_rt_thread_conf.h>
# 本工程没定义 __RTTHREAD__（用的是 __RTTHREAD_OS__），所以现在不会踩到；
# 但只要哪天有人加了 -D__RTTHREAD__，就会因为找不到 lv_rt_thread_conf.h 直接编不过。
CFLAGS += -I$(UI_DIR) -I$(LVGL_DIR) -DLV_CONF_INCLUDE_SIMPLE -DLV_KCONFIG_IGNORE
CFLAGS += -I$(UI_DIR)/font

else
# UI_ENABLE != y：LVGL 与 ui/*.c 整体不参与编译。
# 把开关状态打到构建日志里，避免"改了 y 却没生效"这种低级误判。
$(info [media.mk] UI_ENABLE=$(UI_ENABLE) -> LVGL/UI 源码已封存，未参与编译)

endif # UI_ENABLE

# ---------------------------------------------------------------------------
# 云端人脸：板端抓帧 + PC/讯飞 双后端（app/media_demo/cloud_face）
#
# 背景：SDK 没有交付本地人脸识别所需的 5 个闭源库与 8.1MB 三合一模型，
#       本地跑不了。改成"板上只采集、云端做比对"，用现有物料即可落地。
#
# 内存账（全部静态 .bss，无堆分配，可在 map 里核对）：
#     g_jpeg   64KB   单帧 JPEG（PC 模式即请求体，零拷贝）
#     g_resp    1KB   HTTP 响应
#     g_b64     2KB   流式 base64 输出块（仅讯飞模式）
#     = 67KB + 16KB 线程栈
#   底图 base64 在 .rodata（flash），不占 RAM。
#   若按常规写法"先拷再转 base64"，这里要 64+88=152KB，现在只花 64KB。
#
# 依赖：
#   libdmc   已在 Rtt.mk:59 无条件编译，这里直接用它的 dmc_subscribe
#   mbedtls  CFLAGS 里的 -I$(EXT)/mbedtls/include 由 voice_asr 段提供
#   jpeg_config.h 的 -I 由 Rtt.mk:175 的 -I$(CHIP_DIR) 覆盖
#   时间偏移逻辑复用 voice_asr（本 SDK 系统时间写不进去，讯飞签名要求 <300s 偏差）
#
# 不想用就把下面 4 行注释掉，不会影响其它模块。
# ---------------------------------------------------------------------------
CLOUD_FACE_DIR = $(SDKROOT)/app/media_demo/cloud_face
SRCS += $(CLOUD_FACE_DIR)/src/cf_http.c
SRCS += $(CLOUD_FACE_DIR)/src/cloud_face.c
SRCS += $(CLOUD_FACE_DIR)/src/face_ref_jpg.c
CFLAGS += -I$(CLOUD_FACE_DIR)/include

# ---------------------------------------------------------------------------
# 屏诊断模块 vo_test —— 已删除（2026-10-26）
#
# 该模块只做 ShowChn + SendFrame，是排查 LCD 白屏时的一次性判据。
# 白屏已定性为硬件问题（见 LCD白屏判定报告.md），屏幕方向整体搁置，
# 这个诊断模块随之移除，省一份 .text/.rodata。
# 源码目录 app/media_demo/vo_test 已一并删除，需要时从 git/备份恢复。
# ---------------------------------------------------------------------------
