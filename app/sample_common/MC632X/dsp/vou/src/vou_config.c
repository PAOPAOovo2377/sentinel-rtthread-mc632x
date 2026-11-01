#include "vou_config.h"

static char *model_name = MODEL_TAG_VOU_CONFIG;

static DHD_INFO g_dhd_dev_infos[MAX_DHD_DEV_NUM] = {
    {
        .enable = 0,
#if defined DEVICE_DHD0
        .enable = 1,
        .VoDev = 0,
        .enIntfSync = DHD0_INTF_SYNC,
        .enIntfType = DHD0_INTF,
        .u32BgColor = DHD0_BG_COLOR,
#endif
    },
};

static VIDEO_LAYER_INFO g_video_layer_infos[MAX_VIDEO_LAYER_NUM] = {
    {
        .enable = 0,
#if defined DHD0_VIDEO_LAYER0
        .enable = 1,
        .VoDev = 0,
        .VoLayer = 0,
        .enPixFormat = DHD0_VIDEO_LAYER0_INPUT,
        .stDispRect.u32X = DHD0_VIDEO_LAYER0_START_X,
        .stDispRect.u32Y = DHD0_VIDEO_LAYER0_START_Y,
        .stDispRect.u32Width = DHD0_VIDEO_LAYER0_WIDTH,
        .stDispRect.u32Height = DHD0_VIDEO_LAYER0_HEIGHT,
        .u32BufLen = DHD0_VIDEO_LAYER0_BUFFER_NUM,
#endif
    },
};

static VIDEO_LAYER_CHN_INFO g_video_layer_chn_infos[MAX_VIDEO_LAYER_CHN_NUM] = {
    {
        .enable = 0,
#if defined DHD0_VIDEO_LAYER0_CHN0
        .enable = 1,
        .VoLayer = 0,
        .VoChn = 0,
        .stDispRect.u32X = DHD0_VIDEO_LAYER0_CHN0_X,
        .stDispRect.u32Y = DHD0_VIDEO_LAYER0_CHN0_Y,
        .stDispRect.u32Width = DHD0_VIDEO_LAYER0_CHN0_WIDTH,
        .stDispRect.u32Height = DHD0_VIDEO_LAYER0_CHN0_HEIGHT,
#endif
    },
};

static OSD_LAYER_INFO g_osd_layer_infos[MAX_OSD_LAYER_NUM] = {
    {
        .enable = 0,
#if defined DHD0_VIDEO_LAYER0_CHN0 && defined DEVICE_DHD0_OSD_LAYER0
        .enable = 1,
        .VoDev = 0,
        .GraphicLayer = 0,
        .stPosOsd.u32X = DEVICE_DHD0_OSD0_START_X,
        .stPosOsd.u32Y = DEVICE_DHD0_OSD0_START_Y,
        .stSizeOsd.u32Width = DEVICE_DHD0_OSD0_WIDTH,
        .stSizeOsd.u32Height = DEVICE_DHD0_OSD0_HEIGHT,
        .stAlphaOsd.bAlphaEnable = 1,
        .stAlphaOsd.u8GlobalAlpha = DEVICE_DHD0_OSD0_ALPHA,
        .osd_file = DEVICE_DHD0_OSD0_FILE,
        .enPixelFormat = OSD_FILE_FORMAT,
#endif
    },
};

DHD_INFO *get_dhd_config(FH_SINT32 dhd_id)
{
    return &g_dhd_dev_infos[dhd_id];
}

VIDEO_LAYER_INFO *get_video_layer_config(FH_SINT32 layer_id)
{
    return &g_video_layer_infos[layer_id];
}

VIDEO_LAYER_CHN_INFO *get_video_layer_chn_config(FH_SINT32 chn_id)
{
    return &g_video_layer_chn_infos[chn_id];
}

OSD_LAYER_INFO *get_osd_layer_config(FH_SINT32 layer_id)
{
    return &g_osd_layer_infos[layer_id];
}

static FH_SINT32 get_whf(VO_INTF_SYNC_E enIntfSync, FH_UINT32 *pu32W, FH_UINT32 *pu32H, FH_UINT32 *pu32Frm)
{
    switch (enIntfSync)
    {
    case VO_OUTPUT_960H_NTSC:
        *pu32W = 480;
        *pu32H = 960;
        *pu32Frm = 60;
        break;
    case VO_OUTPUT_PAL:
        *pu32W = 720;
        *pu32H = 576;
        *pu32Frm = 25;
        break;
    case VO_OUTPUT_NTSC:
        *pu32W = 720;
        *pu32H = 480;
        *pu32Frm = 30;
        break;
    case VO_OUTPUT_576P50:
        *pu32W = 720;
        *pu32H = 576;
        *pu32Frm = 50;
        break;
    case VO_OUTPUT_480P60:
        *pu32W = 720;
        *pu32H = 480;
        *pu32Frm = 60;
        break;
    case VO_OUTPUT_800x480_60:
        *pu32W = 800;
        *pu32H = 480;
        *pu32Frm = 60;
        break;
    case VO_OUTPUT_800x600_60:
        *pu32W = 800;
        *pu32H = 600;
        *pu32Frm = 60;
        break;
    case VO_OUTPUT_800x1280_60:
        *pu32W = 800;
        *pu32H = 1280;
        *pu32Frm = 60;
        break;
    case VO_OUTPUT_720P50:
        *pu32W = 1280;
        *pu32H = 720;
        *pu32Frm = 50;
        break;
    case VO_OUTPUT_720P60:
        *pu32W = 1280;
        *pu32H = 720;
        *pu32Frm = 60;
        break;
    case VO_OUTPUT_1080I50:
        *pu32W = 1920;
        *pu32H = 1080;
        *pu32Frm = 25;
        break;
    case VO_OUTPUT_1080I60:
        *pu32W = 1920;
        *pu32H = 1080;
        *pu32Frm = 30;
        break;
    case VO_OUTPUT_1080P24:
        *pu32W = 1920;
        *pu32H = 1080;
        *pu32Frm = 24;
        break;
    case VO_OUTPUT_1080P25:
        *pu32W = 1920;
        *pu32H = 1080;
        *pu32Frm = 25;
        break;
    case VO_OUTPUT_1080P30:
        *pu32W = 1920;
        *pu32H = 1080;
        *pu32Frm = 30;
        break;
    case VO_OUTPUT_1080P50:
        *pu32W = 1920;
        *pu32H = 1080;
        *pu32Frm = 50;
        break;
    case VO_OUTPUT_1080P60:
        *pu32W = 1920;
        *pu32H = 1080;
        *pu32Frm = 60;
        break;
    case VO_OUTPUT_1024x600_60:
        *pu32W = 1024;
        *pu32H = 600;
        *pu32Frm = 60;
        break;
    case VO_OUTPUT_1024x768_60:
        *pu32W = 1024;
        *pu32H = 768;
        *pu32Frm = 60;
        break;
    case VO_OUTPUT_1280x1024_60:
        *pu32W = 1280;
        *pu32H = 1024;
        *pu32Frm = 60;
        break;
    case VO_OUTPUT_1366x768_60:
        *pu32W = 1366;
        *pu32H = 768;
        *pu32Frm = 60;
        break;
    case VO_OUTPUT_1440x900_60:
        *pu32W = 1440;
        *pu32H = 900;
        *pu32Frm = 60;
        break;
    case VO_OUTPUT_1280x800_60:
        *pu32W = 1280;
        *pu32H = 800;
        *pu32Frm = 60;
        break;
    case VO_OUTPUT_1600x1200_60:
        *pu32W = 1600;
        *pu32H = 1200;
        *pu32Frm = 60;
        break;
    case VO_OUTPUT_1680x1050_60:
        *pu32W = 1680;
        *pu32H = 1050;
        *pu32Frm = 60;
        break;
    case VO_OUTPUT_1920x1200_60:
        *pu32W = 1920;
        *pu32H = 1200;
        *pu32Frm = 60;
        break;
    case VO_OUTPUT_2560x1440_30:
        *pu32W = 2560;
        *pu32H = 1440;
        *pu32Frm = 30;
        break;
    case VO_OUTPUT_2560x1440_60:
        *pu32W = 2560;
        *pu32H = 1440;
        *pu32Frm = 60;
        break;
    case VO_OUTPUT_3840x2160_30:
        *pu32W = 3840;
        *pu32H = 2160;
        *pu32Frm = 30;
        break;
    case VO_OUTPUT_3840x2160_60:
        *pu32W = 3840;
        *pu32H = 2160;
        *pu32Frm = 60;
        break;
    case VO_OUTPUT_640x480_60:
        *pu32W = 640;
        *pu32H = 480;
        *pu32Frm = 60;
        break;
    case VO_OUTPUT_USER:
        *pu32W = 720;
        *pu32H = 576;
        *pu32Frm = 25;
        break;
    default:
        SDK_ERR_PRT(model_name, "vo enIntfSync not support!\n");
        return FH_SDK_FAILED;
    }
    return FH_SDK_SUCCESS;
}

FH_SINT32 dhd_device_init(FH_SINT32 dhd_id)
{
    FH_SINT32 ret = FH_SDK_SUCCESS;
    VO_PUB_ATTR_S stVoPubAttr;
    VO_DEV VoDev;

    VoDev = g_dhd_dev_infos[dhd_id].VoDev;
    stVoPubAttr.enIntfSync = g_dhd_dev_infos[dhd_id].enIntfSync; // 符合外接屏幕规格
    stVoPubAttr.enIntfType = g_dhd_dev_infos[dhd_id].enIntfType;
    stVoPubAttr.u32BgColor = g_dhd_dev_infos[dhd_id].u32BgColor;

    ret = FH_MPI_VO_SetPubAttr(VoDev, &stVoPubAttr);
    SDK_FUNC_ERROR(model_name, ret);

    ret = FH_MPI_VO_Enable(VoDev);
    SDK_FUNC_ERROR(model_name, ret);

    return ret;
}

FH_SINT32 vou_video_layer_init(FH_SINT32 layer_id)
{
    FH_SINT32 ret = FH_SDK_SUCCESS;
    VO_VIDEO_LAYER_ATTR_S stLayerAttr;
    VO_LAYER VoLayer;
    VO_DEV VoDev;
    VO_INTF_SYNC_E enIntfSync;

    VoLayer = g_video_layer_infos[layer_id].VoLayer;
    memset(&(stLayerAttr), 0, sizeof(VO_VIDEO_LAYER_ATTR_S));

    VoDev = g_video_layer_infos[layer_id].VoDev;
    enIntfSync = g_dhd_dev_infos[VoDev].enIntfSync;

    ret = get_whf(enIntfSync, &stLayerAttr.stImageSize.u32Width, &stLayerAttr.stImageSize.u32Height, &stLayerAttr.u32DispFrmRt); // 帧率根据屏幕的规格
    SDK_FUNC_ERROR(model_name, ret);

    stLayerAttr.enPixFormat = g_video_layer_infos[layer_id].enPixFormat;
    stLayerAttr.stDispRect.u32X = g_video_layer_infos[layer_id].stDispRect.u32X;
    stLayerAttr.stDispRect.u32Y = g_video_layer_infos[layer_id].stDispRect.u32Y;
    // stLayerAttr.stDispRect.u32Width = stLayerAttr.stImageSize.u32Width;
    // stLayerAttr.stDispRect.u32Height = stLayerAttr.stImageSize.u32Height;
    stLayerAttr.stDispRect.u32Width = g_video_layer_infos[layer_id].stDispRect.u32Width;
    stLayerAttr.stDispRect.u32Height = g_video_layer_infos[layer_id].stDispRect.u32Height;
    ret = FH_MPI_VO_SetVideoLayerAttr(VoLayer, &stLayerAttr);
    SDK_FUNC_ERROR(model_name, ret);

    ret = FH_MPI_VO_SetDispBufLen(VoLayer, g_video_layer_infos[layer_id].u32BufLen); // buffer num
    SDK_FUNC_ERROR(model_name, ret);

    ret = FH_MPI_VO_EnableVideoLayer(VoLayer);
    SDK_FUNC_ERROR(model_name, ret);

    /* ★ 关键：视频层使能后必须 Bind 到 VO 设备，否则这个层上的帧永远不会
     * 进入合成器 —— 屏上只显示 VO 设备的背景色（DHD0_BG_COLOR 就是纯白），
     * 而 SendFrame / EnableChn / ShowChn 全都照样返回 0x0，看不出任何异常。
     *
     * 对照 vou_osd_layer_init()：图形层是
     *     EnableGraphicLayer → BindGraphicLayer → Pos → Size → Alpha → Show
     * 而这里原厂只做了 SetVideoLayerAttr → SetDispBufLen → EnableVideoLayer，
     * **漏掉了 BindVideoLayer**（全工程唯一的 Bind 调用是图形层那一处）。
     * 这正是"纯白背景色、有帧但不上屏"的根源。
     *
     * 失败不中断：重复 Bind 时驱动可能返回"已绑定"，此时层本身是可用的。 */
    {
        FH_SINT32 bind_ret = FH_MPI_VO_BindVideoLayer(VoLayer, VoDev);

        if (bind_ret != FH_SDK_SUCCESS)
        {
            SDK_PRT(model_name, "VO[%d] BindVideoLayer(dev %d) failed 0x%x\n",
                    (int)VoLayer, (int)VoDev, (unsigned)bind_ret);
        }
    }

    return ret;
}

FH_SINT32 vou_video_layer_chn_init(FH_SINT32 chn_id)
{
    FH_SINT32 ret = FH_SDK_SUCCESS;
    VO_VIDEO_LAYER_ATTR_S stLayerAttr;
    VO_CHN_ATTR_S stChnAttr;
    VO_LAYER VoLayer;
    VO_CHN VoChn;

    VoLayer = g_video_layer_chn_infos[chn_id].VoLayer;
    VoChn = g_video_layer_chn_infos[chn_id].VoChn;

    ret = FH_MPI_VO_GetVideoLayerAttr(VoLayer, &stLayerAttr);
    SDK_FUNC_ERROR(model_name, ret);

    stChnAttr.u32Priority = 0;
    stChnAttr.bDeflicker = FH_FALSE;
    stChnAttr.stRect.u32X = g_video_layer_chn_infos[chn_id].stDispRect.u32X;
    stChnAttr.stRect.u32Y = g_video_layer_chn_infos[chn_id].stDispRect.u32Y;
    stChnAttr.stRect.u32Width = g_video_layer_chn_infos[chn_id].stDispRect.u32Width;
    stChnAttr.stRect.u32Height = g_video_layer_chn_infos[chn_id].stDispRect.u32Height;

    ret = FH_MPI_VO_SetChnAttr(VoLayer, VoChn, &stChnAttr);
    SDK_FUNC_ERROR(model_name, ret);

    ret = FH_MPI_VO_SetChnFrameRate(VoLayer, VoChn, stLayerAttr.u32DispFrmRt);
    SDK_FUNC_ERROR(model_name, ret);

    ret = FH_MPI_VO_EnableChn(VoLayer, VoChn);
    SDK_FUNC_ERROR(model_name, ret);

    /* EnableChn 只把通道"使能"，通道仍可能停在 hidden 状态、不参与合成，
     * 屏上就只剩 DHD0 背景色（表现为"均匀灰/白，无画面"）。
     * 对照 vou_osd_layer_init()：图形层那边 Enable 之后紧跟着 ShowGraphicLayer，
     * 而视频层这里原厂漏了 ShowChn —— 全工程唯一一处 ShowChn 是我们自己在 ui 里补的。
     * 这里补一刀；失败不中断，因为通道可能本来就处于显示态（重复 Show 是幂等的）。 */
    if (FH_MPI_VO_ShowChn(VoLayer, VoChn) != FH_SDK_SUCCESS)
    {
        SDK_PRT(model_name, "VO[%d] ShowChn failed\n", (int)VoChn);
    }

    return ret;
}

FH_SINT32 vou_osd_layer_init(FH_SINT32 layer_id)
{
    FH_SINT32 ret = FH_SDK_SUCCESS;
    VO_DEV VoDev;
    GRAPHIC_LAYER GraphicLayer;

    VoDev = g_osd_layer_infos[layer_id].VoDev;
    GraphicLayer = g_osd_layer_infos[layer_id].GraphicLayer;

    ret = FH_MPI_VO_EnableGraphicLayer(GraphicLayer);
    SDK_FUNC_ERROR(model_name, ret);

    ret = FH_MPI_VO_BindGraphicLayer(GraphicLayer, VoDev);
    SDK_FUNC_ERROR(model_name, ret);

    ret = FH_MPI_VO_SetGraphicLayerPos(GraphicLayer, &g_osd_layer_infos[layer_id].stPosOsd);
    SDK_FUNC_ERROR(model_name, ret);

    ret = FH_MPI_VO_SetGraphicLayerSize(GraphicLayer, &g_osd_layer_infos[layer_id].stSizeOsd);
    SDK_FUNC_ERROR(model_name, ret);

    ret = FH_MPI_VO_SetGraphicLayerAlpha(GraphicLayer, &g_osd_layer_infos[layer_id].stAlphaOsd);
    SDK_FUNC_ERROR(model_name, ret);

    ret = FH_MPI_VO_ShowGraphicLayer(GraphicLayer);
    SDK_FUNC_ERROR(model_name, ret);

    return ret;
}

FH_SINT32 vou_close_fd(FH_VOID)
{
    FH_SINT32 ret = FH_SDK_SUCCESS;

    ret = FH_MPI_VO_CloseFd();
    SDK_FUNC_ERROR(model_name, ret);

    return ret;
}

FH_SINT32 dhd_device_exit(FH_SINT32 dhd_id)
{
    FH_SINT32 ret = FH_SDK_SUCCESS;
    VO_DEV VoDev;

    VoDev = g_dhd_dev_infos[dhd_id].VoDev;

    ret = FH_MPI_VO_Disable(VoDev);
    SDK_FUNC_ERROR(model_name, ret);

    return ret;
}

FH_SINT32 vou_video_layer_exit(FH_SINT32 layer_id)
{
    FH_SINT32 ret = FH_SDK_SUCCESS;
    VO_LAYER VoLayer;

    VoLayer = g_video_layer_infos[layer_id].VoLayer;

    ret = FH_MPI_VO_DisableVideoLayer(VoLayer);
    SDK_FUNC_ERROR(model_name, ret);

    return ret;
}

FH_SINT32 vou_video_layer_chn_exit(FH_SINT32 chn_id)
{
    FH_SINT32 ret = FH_SDK_SUCCESS;
    VO_LAYER VoLayer;
    VO_CHN VoChn;

    VoLayer = g_video_layer_chn_infos[chn_id].VoLayer;
    VoChn = g_video_layer_chn_infos[chn_id].VoChn;

    ret = FH_MPI_VO_DisableChn(VoLayer, VoChn);
    SDK_FUNC_ERROR(model_name, ret);

    return ret;
}

FH_SINT32 vou_osd_layer_exit(FH_SINT32 layer_id)
{
    FH_SINT32 ret = FH_SDK_SUCCESS;
    GRAPHIC_LAYER GraphicLayer;

    GraphicLayer = g_osd_layer_infos[layer_id].GraphicLayer;

    ret = FH_MPI_VO_HideGraphicLayer(GraphicLayer);
    SDK_FUNC_ERROR(model_name, ret);

    ret = FH_MPI_VO_DisableGraphicLayer(GraphicLayer);
    SDK_FUNC_ERROR(model_name, ret);

    return ret;
}

FH_SINT32 vou_send_stream(FH_VOID)
{
    FH_SINT32 ret = FH_SDK_SUCCESS;
    VO_LAYER VoLayer;
    VO_CHN VoChn;
    VIDEO_FRAME_INFO_S pstUsrPicInfo;

    VoLayer = 0;
    VoChn = 0;

    printf("start read file!\n");
    MEM_DESC mem;
    {
        char *filename = "800x480_420.yuv";
        FILE *fp = NULL;

        ret = openFile(&fp, filename);
        if (ret)
        {
            printf("error openFile!\n");
        }

        ret = buffer_malloc(&mem, 800 * 480 * 1.5, 1024);
        if (ret)
        {
            printf("error buffermalloc!\n");
        }

        unsigned int readLen = fread(mem.vbase, 1, 800 * 480 * 1.5, fp);
        if (readLen != 800 * 480 * 1.5)
        {
            printf("error fread!\n");
            return -1;
        }
    }
    printf("end read file!\n");
    pstUsrPicInfo.u32PoolId = -1; // TODO
    pstUsrPicInfo.stVFrame.enCompressMode = COMPRESS_MODE_NONE;
    pstUsrPicInfo.stVFrame.enVideoFormat = VIDEO_FORMAT_LINEAR;
    pstUsrPicInfo.stVFrame.enPixelFormat = PIXEL_FORMAT_YUV_SEMIPLANAR_420;
    pstUsrPicInfo.stVFrame.u32Width = 800;
    pstUsrPicInfo.stVFrame.u32Height = 480;
    pstUsrPicInfo.stVFrame.u32Field = VIDEO_FIELD_FRAME;
    pstUsrPicInfo.stVFrame.u32PhyAddr[0] = mem.base;               // Y
    pstUsrPicInfo.stVFrame.u32PhyAddr[1] = mem.base + (800 * 480); // UV
    pstUsrPicInfo.stVFrame.u32Stride[0] = 800;                     // Y stride
    pstUsrPicInfo.stVFrame.u32Stride[1] = 800;                     // UV stride
    pstUsrPicInfo.stVFrame.u64pts = 0;

    ret = FH_MPI_VO_SendFrame(VoLayer, VoChn, &pstUsrPicInfo, 0);
    SDK_FUNC_ERROR(model_name, ret);
    printf("FH_MPI_VO_SendFrame!\n");

    return ret;
}

static FH_SINT32 osd_pic_stride(OSD_LAYER_INFO *osd_layer_info)
{
    FH_SINT32 ret = FH_SDK_SUCCESS;

    switch (osd_layer_info->enPixelFormat)
    {
    case PIXEL_FORMAT_RGB_8888:
    case PIXEL_FORMAT_RGB_4444:
    case PIXEL_FORMAT_RGB_1555:
        osd_layer_info->stSizeOsd.u32Width = ALIGN_UP(osd_layer_info->stSizeOsd.u32Width, 16);
        osd_layer_info->stSizeOsd.u32Height = ALIGN_UP(osd_layer_info->stSizeOsd.u32Height, 16);
        break;
    default:
        SDK_ERR_PRT(model_name, "format[%d] not support!\n", osd_layer_info->enPixelFormat);
        ret = -1;
        break;
    }

    return ret;
}

static FH_SINT32 osd_pic_size(OSD_LAYER_INFO *osd_layer_info, FH_SINT32 *size)
{
    FH_SINT32 ret = FH_SDK_SUCCESS;

    switch (osd_layer_info->enPixelFormat)
    {
    case PIXEL_FORMAT_RGB_8888:
        *size = osd_layer_info->stSizeOsd.u32Width * osd_layer_info->stSizeOsd.u32Height * 4;
        break;
    case PIXEL_FORMAT_RGB_4444:
    case PIXEL_FORMAT_RGB_1555:
        *size = osd_layer_info->stSizeOsd.u32Width * osd_layer_info->stSizeOsd.u32Height * 2;
        break;
    default:
        SDK_ERR_PRT(model_name, "format[%d] not support!\n", osd_layer_info->enPixelFormat);
        ret = -1;
        break;
    }

    return ret;
}

FH_SINT32 osd_send_stream(FH_SINT32 layer_id)
{

    FH_SINT32 ret = FH_SDK_SUCCESS;
    GRAPHIC_LAYER GraphicLayer;
    VIDEO_FRAME_INFO_S pstUsrPicInfo;
    FH_SINT32 size = 0;
    MEM_DESC mem;

    GraphicLayer = g_osd_layer_infos[layer_id].GraphicLayer;

    ret = osd_pic_stride(&g_osd_layer_infos[layer_id]);
    SDK_FUNC_ERROR(model_name, ret);

    ret = osd_pic_size(&g_osd_layer_infos[layer_id], &size);
    SDK_FUNC_ERROR(model_name, ret);

    SDK_PRT(model_name, "Start read osd file!\n");
    {
        char *filename = g_osd_layer_infos[layer_id].osd_file;
        FILE *fp = NULL;

        ret = openFile(&fp, filename);
        SDK_FUNC_ERROR(model_name, ret);

        ret = buffer_malloc_withname(&mem, size, 1024, "vo_osd_buf");
        SDK_FUNC_ERROR(model_name, ret);

        unsigned int readLen = fread(mem.vbase, 1, size, fp);
        if (readLen != size)
        {
            SDK_ERR_PRT(model_name, "VOU OSD read file size not correct!!\n");
            return -1;
        }
    }
    SDK_PRT(model_name, "End read osd file!\n");
    pstUsrPicInfo.u32PoolId = -1;
    pstUsrPicInfo.stVFrame.enCompressMode = COMPRESS_MODE_NONE;
    pstUsrPicInfo.stVFrame.enVideoFormat = VIDEO_FORMAT_LINEAR;
    pstUsrPicInfo.stVFrame.enPixelFormat = g_osd_layer_infos[layer_id].enPixelFormat;
    pstUsrPicInfo.stVFrame.u32Width = g_osd_layer_infos[layer_id].stSizeOsd.u32Width;
    pstUsrPicInfo.stVFrame.u32Height = g_osd_layer_infos[layer_id].stSizeOsd.u32Height;
    pstUsrPicInfo.stVFrame.u32Field = VIDEO_FIELD_FRAME;
    pstUsrPicInfo.stVFrame.u32PhyAddr[0] = mem.base;
    pstUsrPicInfo.stVFrame.u32Stride[0] = g_osd_layer_infos[layer_id].stSizeOsd.u32Width;
    pstUsrPicInfo.stVFrame.u64pts = 0;

    ret = FH_MPI_VO_UpdateGraphicLayer(GraphicLayer, &pstUsrPicInfo, 0);
    SDK_FUNC_ERROR(model_name, ret);
    SDK_PRT(model_name, "VOU OSD Show!\n");

    return ret;
}
