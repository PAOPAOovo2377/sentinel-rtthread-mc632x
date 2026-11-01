#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
生成编译期默认底图（cloud_face/src/face_ref_jpg.c）

底图 = 要让板子"开机就认得"的那张注册照。生成后编译进固件，放在 .rodata，
不占 RAM。

★ 大多数情况下**不需要**这个脚本：
  板子支持运行时登记，站在摄像头前 `face enroll 张三`，
  或者网页上点"登记当前画面"/"上传照片"，都不用重编固件。
  只有"开机就有一个人、不接受手工登记"的场合才用。

用法:
    python make_ref.py --in zhangsan.jpg
    python make_ref.py --in zhangsan.jpg --max 480     # 长边缩到 480（推荐）
    python make_ref.py --clear                          # 恢复空底图

尺寸建议:
    讯飞要求人脸不小于 30x30 像素。板端抓的是 800x480，人脸大概占 1/3，
    即 ~260x260。所以底图长边 480 已经足够，再大只是白占 flash。
    一张 480x360 的 JPEG 约 20~35KB —— 直接进 .rodata，不膨胀。
    （老版本存 base64，同样一张图要 27~47KB，白扔 1/3 的 flash。）

关于压缩: 装了 Pillow 才能缩放；没装就原样输出（会提示体积）。
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_OUT = os.path.join(HERE, "..", "src", "face_ref_jpg.c")

HEADER = """/*
 * face_ref_jpg.c - 编译期默认底图（注册照），二进制 JPEG
 *
 * 由 tools/make_ref.py 自动生成，请勿手工编辑。
 * 源图: {src}
 * 尺寸: {size}
 * 大小: {nbytes} 字节（.rodata / flash，不占 RAM）
 *
 * 发送时由 cloud_face.c 边读边 base64，不会在内存里出现整段编码结果。
 */
#include <rtthread.h>

const unsigned char cf_ref_jpg[] = {{
"""

FOOTER = """}};

const int cf_ref_jpg_len = {nbytes};
"""

LINE_BYTES = 16


def wrap_c_bytes(data: bytes) -> str:
    """每行 16 字节的 0xNN, —— 缩进按 4 空格，编译期展开成 5 字节/个。"""
    out = []
    for i in range(0, len(data), LINE_BYTES):
        chunk = data[i:i + LINE_BYTES]
        out.append("    " + " ".join("0x%02x," % b for b in chunk))
    return "\n".join(out)


def resize_jpeg(path, max_side):
    """有 Pillow 就缩放；没有就返回原字节。"""
    try:
        from PIL import Image
        import io
    except ImportError:
        print("[warn] 没装 Pillow，跳过缩放（pip install Pillow 可启用）")
        with open(path, "rb") as f:
            return f.read(), "original"

    img = Image.open(path).convert("RGB")
    w, h = img.size
    scale = min(1.0, float(max_side) / float(max(w, h)))
    if scale < 1.0:
        img = img.resize((int(w * scale), int(h * scale)), Image.LANCZOS)

    buf = io.BytesIO()
    img.save(buf, format="JPEG", quality=88, optimize=True)
    return buf.getvalue(), "%dx%d" % img.size


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--in", dest="src", help="源人脸照片 (jpg/png)")
    ap.add_argument("--out", default=DEFAULT_OUT, help="输出 .c 路径")
    ap.add_argument("--max", type=int, default=480, help="长边像素上限，默认 480")
    ap.add_argument("--clear", action="store_true", help="清空底图")
    args = ap.parse_args()

    if args.clear:
        content = HEADER.format(src="-", size="-", nbytes=0)
        content += "    0x00\n"
        content += FOOTER.format(nbytes=0)
        with open(args.out, "w", encoding="utf-8") as f:
            f.write(content)
        print(f"[ok] 已清空底图 -> {args.out}")
        return 0

    if not args.src:
        ap.print_help()
        return 1

    if not os.path.exists(args.src):
        print(f"[err] 找不到文件: {args.src}")
        return 1

    raw, size_info = resize_jpeg(args.src, args.max)

    # 板端单个参考图上限 CF_REF_MAX = 64KB，编译期底图走同一个 base64 通道，
    # 所以同样受这个约束；超了就明显跑不通，不如在这里直接拦住。
    if len(raw) > 64 * 1024:
        print(f"[err] 图片仍有 {len(raw) // 1024}KB，超过板端 64KB 上限。")
        print("      pip install Pillow 后把 --max 调到 320~480 再试。")
        return 1

    if raw[:2] != b"\xff\xd8":
        print("[err] 不是 JPEG（magic 不对）。请用 jpg，或装 Pillow 让脚本转码。")
        return 1

    content = HEADER.format(src=os.path.basename(args.src), size=size_info,
                            nbytes=len(raw))
    content += wrap_c_bytes(raw)
    content += FOOTER.format(nbytes=len(raw))

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as f:
        f.write(content)

    print("=" * 56)
    print(f" 源图     : {args.src}")
    print(f" 缩放后   : {size_info}")
    print(f" JPEG    : {len(raw)} 字节 (.rodata / flash)")
    print(f" 输出     : {args.out}")
    print("=" * 56)
    print("下一步: 重新编译固件（face mode xf 后 face run 验证）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
