/*
 * talk.h - 网页与板端之间的语音对讲（半双工）
 *
 * 为什么不是"每次说话 open 一次音频"：
 *   本板 ACODEC 每次 FH_AC_AI_Enable 之后，直流伺服要 4.5 秒才回零
 *   （见 audio_capture.h 里那段实测曲线）。如果每按一次都对讲一次开一次，
 *   用户说完要先干等 5 秒才听到声音，完全没法用。
 *   所以这里起一条**常驻采集线程**，把 PCM 连续写进环形缓冲：
 *     · 上行（板 -> 网页）：/api/mic 直接从缓冲里切最近 N 秒，0 等待；
 *     · 下行（网页 -> 板）：/api/talkdown 收到 PCM 立刻走 AO 播。
 *   只有 talk_start 之后的前几秒是"预热"，之后一直是即时的。
 *
 * 代价与边界：
 *   · 与语音识别互斥。talk 常开时 ASR 抢不到 audio_capture 的帧，
 *     所以 talk_start 会先问 voice_cmd_state()，识别在跑就拒绝。
 *   · 半双工。板子只有一路 ACODEC，边录边放会把喇叭的声音灌回麦克风。
 *   · 缓冲 6 秒（384KB .bss）。再长没有意义：网页一次最多播 3 秒。
 */
#ifndef __TALK_H__
#define __TALK_H__

/* 开始/停止。start 会立刻返回，麦克风预热在后台线程里做。
 * 返回：0 成功；-1 打不开音频；-2 已在运行；-6 语音识别正在跑 */
int  talk_start(void);
void talk_stop(void);

/* 1 = 已启动；ready = 1 表示麦克风已过直流收敛、缓冲里有可用数据 */
int  talk_active(void);
int  talk_ready(void);

/* 实时电平 0~100，给网页画音量条（没在跑时返回 0） */
int  talk_level(void);

/* 取最近 sec 秒（1~6）的 PCM（16k/16bit/mono），返回字节数。
 * 数据直接从环形缓冲里给，不拷贝 —— 调用方必须**先发完再取下一次**。
 * 返回 0 表示还没有可用数据（刚 start 或预热中）。 */
int talk_pcm(int sec, const unsigned char **p);

/* 把一段 PCM 播到 LINE_OUT（假设 talk_start 已经打开了 AO）。
 * 阻塞到播完，返回 0 成功，-1 参数错，-2 AO 未使能。 */
int talk_play(const unsigned char *pcm, int len);

/*
 * 入队播放，立刻返回（推荐用这个）。
 *
 * ★ 为什么必须有这一条（2026-10-28 深夜）
 *   talk_play() 内部是按**实时节奏**睡的：每 2048 字节睡 64ms
 *   （audio_capture_play_raw 里那句 rt_thread_mdelay(n/32)）。
 *   网页一次推 12KB，就是 375ms 实打实的阻塞。
 *   webd 是单线程，这 375ms 里 /api/state 轮询、按钮点击、相册请求
 *   **全部排队**——用户感受就是"一开对讲整个页面就不动了"。
 *   现在只做一次 memcpy，真正的播放交给常驻的对讲线程。
 *
 * 返回：>=0 实际入队的字节数（可能小于 len，队列满时截断）；
 *       -1 参数错；-3 对讲没启动。
 */
int talk_push_down(const unsigned char *pcm, int len);

/* 下行队列里还有多少字节没播完（给网页做"板端还在播"的提示） */
int talk_down_pending(void);

#endif /* __TALK_H__ */
