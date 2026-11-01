#include "sample_common_demo.h"

/* ===== 语音识别（以太网 + 讯飞 HTTP，阶段0 暂不含 LVGL） ===== */
#include "voice_cmd.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <unistd.h>      /* RT-Thread SAL 套接字用 dfs 的 close() 关闭，非 closesocket */

static char *model_name = MODEL_TAG_DEMO;

/* ---- 最小 SNTP：直连阿里 NTP(203.107.6.1)，经 rtc 驱动写回系统时间 ---- */
/* SDK 自带的 rt_rtc_ntp_sync_init 是空壳（RTC_SYNC_USING_NTP 未定义且无实现），故自包含实现 */
extern rt_err_t set_date(rt_uint32_t year, rt_uint32_t month, rt_uint32_t day);
extern rt_err_t set_time(rt_uint32_t hour, rt_uint32_t minute, rt_uint32_t second);

/* 非 static：voice_cmd.c 在识别前会调用本函数做 NTP 补偿重试 */
int board_ntp_sync(void)
{
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return -1;

    int ntp_to_ms = 3000;   /* lwIP 2.1.2 的 SO_RCVTIMEO 期望毫秒(u32)，非 struct timeval */
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const void *)&ntp_to_ms, sizeof(ntp_to_ms));

    struct sockaddr_in srv;
    rt_memset(&srv, 0, sizeof(srv));
    srv.sin_family      = AF_INET;
    srv.sin_port        = htons(123);
    srv.sin_addr.s_addr = inet_addr("203.107.6.1");

    unsigned char pkt[48] = {0};
    pkt[0] = 0x1B;   /* LI=0, VN=3, Mode=3(client) */

    for (int i = 0; i < 5; i++)
    {
        if (sendto(sock, pkt, 48, 0, (struct sockaddr *)&srv, sizeof(srv)) < 0)
            { rt_thread_mdelay(1000); continue; }

        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        if (recvfrom(sock, pkt, 48, 0, (struct sockaddr *)&from, &flen) >= 48)
        {
            unsigned long s1900 = ((unsigned long)pkt[40] << 24) |
                                  ((unsigned long)pkt[41] << 16) |
                                  ((unsigned long)pkt[42] << 8)  |
                                   (unsigned long)pkt[43];
            time_t now = (time_t)(s1900 - 2208988800UL);   /* 转 Unix 纪元 */
            if (now > 1000000000)   /* 合理性检查（>2001年） */
            {
                struct tm *tmv = gmtime(&now);
                set_date(tmv->tm_year + 1900, tmv->tm_mon + 1, tmv->tm_mday);
                set_time(tmv->tm_hour, tmv->tm_min, tmv->tm_sec);
                close(sock);
                rt_kprintf("[ntp] synced\n");
                return 0;
            }
        }
        rt_thread_mdelay(1000);
    }
    close(sock);
    rt_kprintf("[ntp] sync FAILED\n");
    return -1;
}

/* NTP 后台线程：失败/超时都不影响启动主流程 */
static void ntp_bg_task(void *arg)
{
    board_ntp_sync();
}

FH_SINT32 sample_common_start_demo(FH_VOID)
{
    FH_SINT32 ret = FH_SDK_SUCCESS;

    SDK_PRT(model_name, "Start Demo!\n");

    /* ===== 语音识别 + NTP 校时（阶段0，暂不含 LVGL）=====
     * 语音线程先起（RTC 由 rtc_init_buildtime 用编译时刻兜底，第一秒就有时间）；
     * NTP 放后台线程，失败也不阻塞启动。以太网在板级初始化时已 up。 */
    voice_cmd_start();
    {
        rt_thread_t ntp_t = rt_thread_create("ntpbg", ntp_bg_task, RT_NULL,
                                             4096, 20, 10);
        if (ntp_t) rt_thread_startup(ntp_t);
    }

#if defined(DUAL_CAMERA_ENABLE)
    ret = sample_dual_camera_start();
    SDK_FUNC_ERROR(model_name, ret);
#endif

#if defined(FH_VISE_DEMO_ENABLE)
    ret = sample_fh_vise_demo_start(FH_APP_GRP_ID);
    SDK_FUNC_ERROR(model_name, ret);
#endif

#if defined(FH_OVERLAY_ENABLE)
    ret = sample_fh_overlay_start(FH_APP_GRP_ID);
    SDK_FUNC_ERROR(model_name, ret);
#endif

#if defined(UVC_ENABLE)
    ret = sample_common_uvc_start();
    SDK_FUNC_ERROR(model_name, ret);
#endif

#if defined(FH_VOU_ENABLE)
    ret = sample_fh_vou_start();
    SDK_FUNC_ERROR(model_name, ret);
#endif

#if defined(FH_NN_DETECT_ENABLE)
#if defined(FH_NN_IVS_ENABLE)
    ret = sample_fh_nna_ivs_start(FH_APP_GRP_ID);
    SDK_FUNC_ERROR(model_name, ret);
#else
    ret = sample_fh_nna_obj_detect_start(FH_APP_GRP_ID);
    SDK_FUNC_ERROR(model_name, ret);

#endif
#endif

#if defined(FH_NN_AINR_ENABLE)
#if defined(FH_APP_USING_AINR_G0)
    ret = sample_fh_ainr_start(0);
    SDK_FUNC_ERROR(model_name, ret);
#endif

#if defined(FH_APP_USING_AINR_G1)
    ret = sample_fh_ainr_start(1);
    SDK_FUNC_ERROR(model_name, ret);
#endif
#endif

#if defined(FH_WAVE_HAND_ENABLE)
    ret = sample_fh_nna_whd_start();
    SDK_FUNC_ERROR(model_name, ret);
#endif

#if defined(FH_GESTURE_REC_ENABLE)
    ret = sample_fh_nna_gesture_rec_start(FH_APP_GRP_ID);
    SDK_FUNC_ERROR(model_name, ret);
#endif

#if defined(FH_FACE_SNAP_ENABLE)
    ret = sample_fh_nna_face_snap_start(FH_APP_GRP_ID);
    SDK_FUNC_ERROR(model_name, ret);
#endif

#if defined(FH_FACE_REC_ENABLE)
    ret = sample_fh_nna_face_rec_start(FH_APP_GRP_ID);
    SDK_FUNC_ERROR(model_name, ret);
#endif

#if defined(FH_LP_REC_ENABLE)
    ret = sample_fh_nna_lprec_start(FH_APP_GRP_ID);
    SDK_FUNC_ERROR(model_name, ret);
#endif

#if defined(FH_IVS_ENABLE)
    ret = sample_fh_ivs_start(FH_APP_GRP_ID);
    SDK_FUNC_ERROR(model_name, ret);
#endif

#if defined(FH_MD_CD_ENABLE)
    ret = sample_fh_md_cd_start(FH_APP_GRP_ID);
    SDK_FUNC_ERROR(model_name, ret);
#endif

#if defined(FH_ISP_DEMO_ENABLE)
    ret = sample_fh_isp_demo_start(FH_APP_GRP_ID);
    SDK_FUNC_ERROR(model_name, ret);
#endif

#if defined(FH_VENC_DEMO_ENABLE)
    ret = sample_fh_venc_start(FH_APP_GRP_ID);
    SDK_FUNC_ERROR(model_name, ret);
#endif

#if defined(FH_AF_ENABLE)
    ret = sample_fh_af_start(FH_APP_GRP_ID);
    SDK_FUNC_ERROR(model_name, ret);
#endif

    /* ===== 语音识别 + NTP 校时已移到函数最前，确保任何情况下都执行 ===== */

    return ret;
}

FH_SINT32 sample_common_stop_demo(FH_VOID)
{
    FH_SINT32 ret = FH_SDK_SUCCESS;

    SDK_PRT(model_name, "Demo exit!\n");
#if defined(FH_AF_ENABLE)
    SDK_PRT(model_name, "sample_fh_af_stop!\n");
    ret = sample_fh_af_stop(FH_APP_GRP_ID);
    SDK_FUNC_ERROR(model_name, ret);
#endif

#if defined(FH_VENC_DEMO_ENABLE)
    SDK_PRT(model_name, "sample_fh_venc_stop!\n");
    ret = sample_fh_venc_stop(FH_APP_GRP_ID);
    SDK_FUNC_ERROR(model_name, ret);
#endif

#if defined(FH_ISP_DEMO_ENABLE)
    SDK_PRT(model_name, "sample_fh_isp_demo_stop!\n");
    ret = sample_fh_isp_demo_stop(FH_APP_GRP_ID);
    SDK_FUNC_ERROR(model_name, ret);
#endif

#if defined(FH_MD_CD_ENABLE)
    SDK_PRT(model_name, "sample_fh_md_cd_stop!\n");
    ret = sample_fh_md_cd_stop(FH_APP_GRP_ID);
    SDK_FUNC_ERROR(model_name, ret);
#endif

#if defined(FH_IVS_ENABLE)
    SDK_PRT(model_name, "sample_fh_ivs_stop!\n");
    ret = sample_fh_ivs_stop(FH_APP_GRP_ID);
    SDK_FUNC_ERROR(model_name, ret);
#endif

#if defined(FH_FACE_REC_ENABLE)
    SDK_PRT(model_name, "sample_fh_nna_face_rec_stop!\n");
    ret = sample_fh_nna_face_rec_stop(FH_APP_GRP_ID);
    SDK_FUNC_ERROR(model_name, ret);
#endif

#if defined(FH_FACE_SNAP_ENABLE)
    SDK_PRT(model_name, "sample_fh_nna_face_snap_stop!\n");
    ret = sample_fh_nna_face_snap_stop(FH_APP_GRP_ID);
    SDK_FUNC_ERROR(model_name, ret);
#endif

#if defined(FH_LP_REC_ENABLE)
    SDK_PRT(model_name, "sample_fh_nna_lprec_stop!\n");
    ret = sample_fh_nna_lprec_stop(FH_APP_GRP_ID);
    SDK_FUNC_ERROR(model_name, ret);
#endif

#if defined(FH_GESTURE_REC_ENABLE)
    SDK_PRT(model_name, "sample_fh_nna_gesture_rec_stop!\n");
    ret = sample_fh_nna_gesture_rec_stop(FH_APP_GRP_ID);
    SDK_FUNC_ERROR(model_name, ret);
#endif

#if defined(FH_APP_OPEN_WAVE_HAND_DETECT)
    SDK_PRT(model_name, "sample_fh_nna_whd_stop!\n");
    ret = sample_fh_nna_whd_stop();
    SDK_FUNC_ERROR(model_name, ret);
#endif

#if defined(FH_NN_AINR_ENABLE)
#if defined(FH_APP_USING_AINR_G0)
    ret = sample_fh_ainr_stop(0);
    SDK_FUNC_ERROR(model_name, ret);
#endif

#if defined(FH_APP_USING_AINR_G1)
    ret = sample_fh_ainr_stop(1);
    SDK_FUNC_ERROR(model_name, ret);
#endif
#endif

#if defined(FH_NN_DETECT_ENABLE)
#if defined(FH_NN_IVS_ENABLE)
    SDK_PRT(model_name, "sample_fh_nna_ivs_stop!\n");
    ret = sample_fh_nna_ivs_stop(FH_APP_GRP_ID);
    SDK_FUNC_ERROR(model_name, ret);
#else
    SDK_PRT(model_name, "sample_fh_nna_obj_detect_stop!\n");
    ret = sample_fh_nna_obj_detect_stop(FH_APP_GRP_ID);
    SDK_FUNC_ERROR(model_name, ret);
#endif
#endif

#if defined(FHB_VOU_ENABLE)
    SDK_PRT(model_name, "sample_fh_vou_stop!\n");
    ret = sample_fh_vou_stop();
    SDK_FUNC_ERROR(model_name, ret);
#endif

#if defined(UVC_ENABLE)
    SDK_PRT(model_name, "sample_common_uvc_stop!\n");
    ret = sample_common_uvc_stop();
    SDK_FUNC_ERROR(model_name, ret);
#endif

#if defined(FH_OVERLAY_ENABLE)
    SDK_PRT(model_name, "sample_fh_overlay_stop!\n");
    ret = sample_fh_overlay_stop(FH_APP_GRP_ID);
    SDK_FUNC_ERROR(model_name, ret);
#endif

#if defined(FH_VISE_DEMO_ENABLE)
    SDK_PRT(model_name, "sample_fh_vise_demo_stop!\n");
    ret = sample_fh_vise_demo_stop(FH_APP_GRP_ID);
    SDK_FUNC_ERROR(model_name, ret);
#endif

#if defined(DUAL_CAMERA_ENABLE)
    SDK_PRT(model_name, "sample_dual_camera_stop!\n");
    ret = sample_dual_camera_stop();
    SDK_FUNC_ERROR(model_name, ret);
#endif

    return ret;
}