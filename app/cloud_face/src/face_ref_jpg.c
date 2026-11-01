/*
 * face_ref_jpg.c - 编译期默认底图（可选）
 *
 * 由 tools/make_ref.py 生成，默认是空的（长度 0）。
 *
 * 为什么是二进制而不是 base64：
 *   放 .rodata 里只占 flash，不占 RAM；同样一张图，base64 要膨胀 4/3，
 *   而 flash 只剩几十 KB，能省就省。发送时由 cloud_face.c 边读边编码。
 *
 * 正常情况下**不需要**这个文件有内容：板子支持运行时登记
 * （face enroll <名字>，或在网页上点一下），不用重新编译固件。
 * 只有"开机就要有某个人的底图、不接受手工登记"的场合才用 make_ref.py 生成。
 */

#include <rtthread.h>

const unsigned char cf_ref_jpg[] = { 0x00 };
const int           cf_ref_jpg_len = 0;
