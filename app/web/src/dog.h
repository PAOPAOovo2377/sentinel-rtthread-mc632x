/*
 * dog.h —— 双层看门狗：硬件 WDT + 线程心跳监督
 *
 * 为什么是两层：
 *   只有硬件狗，只能知道"整个系统还活着"，不知道是谁卡了；
 *   只有在 idle 里喂狗更糟 —— idle 能跑只证明"还有人在跑"，
 *   分不清"系统空闲"和"某个高优先级线程死循环"，而且一卡就是硬复位，
 *   现场只能看到"它重启了"，看不到原因。
 *
 * 本方案：
 *   1. 关键线程在各自主循环里调 dog_beat()，只写一个 tick 值，几条指令；
 *   2. 独立线程 dog（prio 20）每秒扫一遍，任何已启动的线程超时
 *      => 打印线程名 + 卡了多久 => 停止喂硬件狗 => 20s 后硬件复位。
 *
 * 于是"卡死"被自动转成"重启 + 一条可查的日志"，而不是永久僵死。
 */
#ifndef __DOG_H__
#define __DOG_H__

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    DOG_TALK = 0,   /* 音频常驻采集，20ms 一拍      */
    DOG_EVI,        /* 证据抓拍线程                 */
    DOG_WEB,        /* HTTP 服务（单线程）          */
    DOG_FACE,       /* 人脸抓帧 / HTTPS 上行        */
    DOG_NN,         /* 人形检测（IVS 主循环）       */
    DOG_NUM
} dog_id_t;

/* 线程心跳上报：在关键线程的主循环里调用，开销约 3 条指令 */
void dog_beat(dog_id_t id);

/* 启动看门狗线程（幂等，重复调用返回 0）。开机流程里调一次即可 */
int  dog_start(void);

/* 现场开关：off 停硬件狗（调试期防止断点被复位），on 重新武装 */
void dog_stop(void);
int  dog_resume(void);

/* 打印各线程心跳年龄与阈值 */
void dog_dump(void);

#ifdef __cplusplus
}
#endif

#endif /* __DOG_H__ */
