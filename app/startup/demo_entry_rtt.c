#include <optparse.h>
#include "sample_common.h"
#include "vlcview/vlcview.h"

typedef unsigned int UINT32;
#define reg_write(addr,value) (*(volatile UINT32 *)(addr)=(value))
#define SET_REG(addr, value)   reg_write(addr, value)

/* 开机自启（startup/boot_auto.c）：
 *   vlcview -p 1246 -> ifconfig/dns -> 时间 -> webd start
 * 以前这几条要手敲，掉电重启就得重来一遍。 */
extern void boot_auto_launch(void);

void user_main(void)
{
#if !defined(FH_FAST_BOOT) && !defined(RPC_MEDIA)
    extern FH_SINT32 fh_nna_module_init(FH_VOID);
    int ret;
    ret = fh_nna_module_init();
    if (ret)
    {
        printf("fh_nna_module_init failed %x !\n", ret);
    }
#endif
SET_REG(0x10200030,0x05);
SET_REG(0x10200034,0x05);
SET_REG(0x10200038,0x05);
SET_REG(0x1020003c,0x05);
SET_REG(0x10200040,0x00);
SET_REG(0x10200044,0x00);
SET_REG(0x10200048,0x05);
SET_REG(0x1020004c,0x05);
SET_REG(0x10200064,0x05);
SET_REG(0x10200068,0x05);
SET_REG(0x1020006c,0x05);
SET_REG(0x10200070,0x05);
SET_REG(0x10200074,0x05);
SET_REG(0x10200078,0x05);
SET_REG(0x1020007c,0x05);
SET_REG(0x10200080,0x05);
SET_REG(0x10200084,0x05);
SET_REG(0x10200088,0x05);
SET_REG(0x102000ac,0x05);
SET_REG(0x102000b0,0x05);
SET_REG(0x102000d4,0x05);
SET_REG(0x102000d8,0x05);

SET_REG(0x0c8010c0,0x0c520c41);
SET_REG(0x0c8010c4,0x00052507);
SET_REG(0x0c8010c8,0x2307b9ab);
SET_REG(0x0c8010cc,0x00000000);
SET_REG(0x0c801008,0xffffffff);

/* 原来这里只是注释掉的一行 _vlcview(NULL,1240)，等于"开机什么都不跑"。
 * 现在换成完整自启：媒体管线 + 固定 IP + DNS + 时间 + web 控制台，
 * 上电即可用，不用再连串口敲命令。
 * 要临时关掉自启，把下面这行注释掉、仍走 msh 手敲即可。 */
boot_auto_launch();
}

FH_VOID vlcview_exit(FH_VOID)
{
#ifdef UVC_ENABLE
    printf("UVC not support exit!\n");
    return;
#endif
    _vlcview_exit();

    buffer_reset_vmm(); /* decomment it after lib reupdate  */
}

FH_VOID vlcview_usage(FH_VOID)
{
    printf("\nusage:  vlcview  [-i: | p: | -q | -h]\n\n");
    printf("        -i ip    :ip address (only for pes)\n");
    printf("        -p port  :port (only for pes and rtsp)\n");
    printf("        -q       :quit\n");
    printf("        -h       :help\n\n");
    printf("example: \n");
    printf("        vlcview -i 10.81.187.8 -p 1234\n");
    printf("        vlcview -q\n");
}

FH_VOID vlcview(int argc, char *argv[])
{
    struct optparse options;
    FH_CHAR *dst_ip = NULL;
    unsigned int port = 1234;
    int cmd;
    int get_cmd = 0;

    optparse_init(&options, argv);
    while ((cmd = optparse(&options, "i:p:qh")) != -1)
    {
        get_cmd = 1;
        switch (cmd)
        {
        case 'i':
        {
            dst_ip = options.optarg;
            break;
        }
        case 'p':
        {
            port = strtol(options.optarg, NULL, 0);
            break;
        }
        case 'q':
        {
            vlcview_exit();
            return;
        }
        default:
        {
            vlcview_usage();
            return;
        }
        }
    }

    if (!get_cmd)
    {
#ifndef UVC_ENABLE
        vlcview_usage();
        return;
#endif
    }

    _vlcview(dst_ip, port);
}

#include <rttshell.h>
SHELL_CMD_EXPORT(vlcview, vlcview());
#ifdef FH_APP_OPEN_AUDIO
SHELL_CMD_EXPORT(sample_audio, sample_audio());
#endif