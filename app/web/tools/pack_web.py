#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
把 webui/index.html 压成 C 数组（web/src/web_index_gz.c）

板子直接把 gzip 字节发出去，带 Content-Encoding: gzip，由浏览器解压。
MCU 侧不解压 —— 省 RAM 省 CPU，代价只是传输时多 15~20 字节头。

用法:
    python pack_web.py                 # 用默认路径
    python pack_web.py --in a.html --out ../src/web_index_gz.c

改了网页之后必须重跑这个脚本再编译，否则固件里还是旧页面。
"""
import argparse
import gzip
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
WEB_DIR = os.path.dirname(HERE)
DEFAULT_IN = os.path.join(WEB_DIR, "webui", "index.html")
DEFAULT_OUT = os.path.join(WEB_DIR, "src", "web_index_gz.c")

LINE_BYTES = 16

TEMPLATE = """/*
 * web_index_gz.c - 由 tools/pack_web.py 自动生成，请勿手工修改
 *
 * 源文件: webui/index.html
 * 原始大小: {raw} 字节
 * 压缩后  : {gz} 字节 ({ratio:.1f}%)
 *
 * 浏览器以 Content-Encoding: gzip 直接解压，MCU 侧不解压。
 */

#include <rtthread.h>

const unsigned char web_index_gz[] = {{
{body}
}};

const unsigned int web_index_gz_len = {gz};
"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--in", dest="src", default=DEFAULT_IN)
    ap.add_argument("--out", default=DEFAULT_OUT)
    ap.add_argument("--level", type=int, default=9)
    args = ap.parse_args()

    if not os.path.exists(args.src):
        print(f"[err] 找不到 {args.src}")
        return 1

    raw = open(args.src, "rb").read()

    # mtime=0 让同样的输入产出同样的字节，方便 git diff / 复现构建
    buf = gzip.compress(raw, compresslevel=args.level, mtime=0)

    lines = []
    for i in range(0, len(buf), LINE_BYTES):
        chunk = buf[i:i + LINE_BYTES]
        lines.append("    " + "".join("0x%02x," % b for b in chunk))

    body = "\n".join(lines)
    content = TEMPLATE.format(raw=len(raw), gz=len(buf),
                              ratio=len(buf) * 100.0 / max(1, len(raw)),
                              body=body)

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as f:
        f.write(content)

    print("=" * 56)
    print(f" 源文件 : {args.src}")
    print(f" 原始   : {len(raw)} 字节")
    print(f" 压缩后 : {len(buf)} 字节 ({len(buf)*100.0/max(1,len(raw)):.1f}%)")
    print(f" 输出   : {args.out}")
    print("=" * 56)
    print("下一步: 重新编译固件")
    return 0


if __name__ == "__main__":
    sys.exit(main())
