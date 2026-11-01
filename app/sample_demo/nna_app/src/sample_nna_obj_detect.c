#include "nna_app/include/sample_common_nna.h"

static char *model_name = MODEL_TAG_DEMO_NNA_OBJ_DET;

static FH_UINT32 g_nna_grpid[MAX_GRP_NUM] = {0};
static FH_SINT32 g_nna_running[MAX_GRP_NUM] = {0};
static FH_SINT32 g_nna_stop[MAX_GRP_NUM] = {0};

static FH_SINT32 draw_box(FH_SINT32 grp_id, FH_SINT32 chn_id, NN_RESULT nn_result, FH_FLOAT conf)
{
    FH_SINT32 ret = FH_SDK_SUCCESS;
    FH_UINT32 chn_w = 0;
    FH_UINT32 chn_h = 0;
    static OSD_GBOX gbox[BOX_MAX_NUM];
    FH_SINT32 i = 0;
    static NN_RESULT last_result = {0};

    if (nn_result.result_det.nn_result_num == 0)
    {
        for (i = 0; i < last_result.result_det.nn_result_num; i++)
        {
            gbox[i].enable = 0;

            ret = sample_set_gbox(grp_id, chn_id, &gbox[i]);
            SDK_FUNC_ERROR_CONTINUE(model_name, ret);
        }

        last_result.result_det.nn_result_num = 0;

        return FH_SDK_SUCCESS;
    }

    SDK_CHECK_MAX_VALID(model_name, nn_result.result_det.nn_result_num, BOX_MAX_NUM, "nn_result_num > BOX_MAX_NUM:%d\n", BOX_MAX_NUM);

    chn_w = get_vpu_chn_w(grp_id, chn_id);
    chn_h = get_vpu_chn_h(grp_id, chn_id);

    for (i = 0; i < nn_result.result_det.nn_result_num; i++)
    {
        gbox[i].gbox_id = i;
        gbox[i].color.fAlpha = 255;
        gbox[i].color.fRed = 255;
        gbox[i].color.fGreen = 0;
        gbox[i].color.fBlue = 0;
        gbox[i].x = nn_result.result_det.nn_result_info[i].box.x * chn_w;
        gbox[i].y = nn_result.result_det.nn_result_info[i].box.y * chn_h;
        gbox[i].w = nn_result.result_det.nn_result_info[i].box.w * chn_w;
        gbox[i].h = nn_result.result_det.nn_result_info[i].box.h * chn_h;

        if (nn_result.result_det.nn_result_info[i].conf < conf)
        {
            gbox[i].enable = 0;
        }
        else
        {
            gbox[i].enable = 1;
        }

        ret = sample_set_gbox(grp_id, chn_id, &gbox[i]);
        SDK_FUNC_ERROR_CONTINUE(model_name, ret);
    }

    if (last_result.result_det.nn_result_num > nn_result.result_det.nn_result_num)
    {
        for (; i < last_result.result_det.nn_result_num; i++)
        {
            gbox[i].enable = 0;

            ret = sample_set_gbox(grp_id, chn_id, &gbox[i]);
            SDK_FUNC_ERROR_CONTINUE(model_name, ret);
        }
    }

    last_result.result_det.nn_result_num = nn_result.result_det.nn_result_num;

    return ret;
}

FH_VOID *nna_obj_detect(FH_VOID *args)
{
    FH_SINT32 ret = FH_SDK_SUCCESS;

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
    FH_FLOAT fps = 0;
    FH_UINT64 time_start = 0;
    FH_UINT64 time_now = 0;
    FH_UINT64 time_bak = 0;
    FH_UINT64 time_lapse = 1000 / FH_NN_DET_FPS;

    vpu_info = get_vpu_config(grp_id);

    if (vpu_info->enable)
    {
        vpu_chn_info = get_vpu_chn_config(grp_id, NN_CHN);
        if (!vpu_chn_info->enable)
        {
            SDK_ERR_PRT(model_name, "VPU[%d] CHN[%d] not enable !\n", grp_id, NN_CHN);
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
#elif defined(FH_NN_PVF_DET_ENABLE)
    nn_type = PERSON_VEHICLE_FIRE_DET;
    strcpy(model_path, "./models/c3det.nbg");
#elif defined(FH_APP_OPEN_LPD_ENABLE)
    nn_type = LPR_DET;
    strcpy(model_path, "./models/lpdet.nbg");
#else
    SDK_ERR_PRT(model_name, "nn_type NULL !\n");
    goto Exit;
#endif

    ret = nna_init(grp_id, &nn_handle, nn_type, model_path, vpu_chn_info->width, vpu_chn_info->height);
    SDK_FUNC_ERROR_GOTO(model_name, ret);

    memset(&nn_input_data, 0, sizeof(nn_input_data));
    nn_input_data.width = vpu_chn_info->width;
    nn_input_data.height = vpu_chn_info->height;
    nn_input_data.stride = vpu_chn_info->width;

    SDK_PRT(model_name, "nna_obj_detect[%d] Start!\n", grp_id);

    g_nna_running[grp_id] = 1;

    memset(thread_name, 0, sizeof(thread_name));
    sprintf(thread_name, "nna_obj_det_%d", grp_id);
    prctl(PR_SET_NAME, thread_name);
    sleep(1);
    time_start = time_bak = time_now = get_ms();
    while (!g_nna_stop[grp_id])
    {
        ret = lock_vpu_stream(grp_id, NN_CHN, &pstVpuframeinfo, 500, &handle_lock, 1);
        SDK_FUNC_ERROR_CONTINUE(model_name, ret);

        time_now = get_ms();
        if (time_now - time_bak > time_lapse)
        {
            time_bak += time_lapse;

            ret = getVPUStreamToNN(pstVpuframeinfo, &nn_input_data);
            if (!ret)
            {
                ret = nna_get_obj_det_result(nn_handle, nn_input_data, &nn_result);
                SDK_FUNC_ERROR_CONTINUE(model_name, ret);
                fps++;

                ret = unlock_vpu_stream(grp_id, NN_CHN, handle_lock, &pstVpuframeinfo);
                SDK_FUNC_ERROR_CONTINUE(model_name, ret);

                ret = draw_box(grp_id, 1, nn_result, 0.9f);
                SDK_FUNC_ERROR_CONTINUE(model_name, ret);

                if (time_now - time_start > 10 * 1000)
                {
                    SDK_PRT(model_name, "NN[%d] Det FPS: %f\n", grp_id, fps / 10);
                    fps = 0;
                    time_start = time_now;
                }
            }
            else
            {
                ret = unlock_vpu_stream(grp_id, NN_CHN, handle_lock, &pstVpuframeinfo);
                SDK_FUNC_ERROR_CONTINUE(model_name, ret);
            }
        }
        else
        {
            ret = unlock_vpu_stream(grp_id, NN_CHN, handle_lock, &pstVpuframeinfo);
            SDK_FUNC_ERROR_CONTINUE(model_name, ret);
        }
    }

    ret = nna_release(grp_id, &nn_handle);
    SDK_FUNC_ERROR_GOTO(model_name, ret);
    SDK_PRT(model_name, "nna_obj_detect[%d] End!\n", grp_id);
Exit:
    g_nna_running[grp_id] = 0;

    return NULL;
}

FH_SINT32 sample_fh_nna_obj_detect_start(FH_SINT32 grp_id)
{
    FH_SINT32 ret = FH_SDK_SUCCESS;
    pthread_t thread;
    pthread_attr_t attr;

    if (g_nna_running[grp_id])
    {
        SDK_PRT(model_name, "nna_obj_detect[%d] already running!\n", grp_id);
        return FH_SDK_SUCCESS;
    }

    g_nna_grpid[grp_id] = grp_id;
    g_nna_stop[grp_id] = 0;

    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
#ifdef __RTTHREAD_OS__
    pthread_attr_setstacksize(&attr, 256 * 1024);
#endif
    if (pthread_create(&thread, &attr, nna_obj_detect, &g_nna_grpid[grp_id]))
    {
        SDK_ERR_PRT(model_name, "pthread_create failed\n");
        goto Exit;
    }

    return ret;

Exit:
    return -1;
}

FH_SINT32 sample_fh_nna_obj_detect_stop(FH_SINT32 grp_id)
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
        SDK_PRT(model_name, "nna_obj_detect[%d] not running!\n", grp_id);
    }

    return ret;
}
