#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
MC632X 云端人脸 - PC 端接收服务

零硬依赖：只用 Python 标准库，`python face_server.py` 直接能跑。
按装了哪些库自动选择比对引擎，依次降级：

  引擎        需要的包                 能力
  ----------  -----------------------  --------------------------------
  facerec     face_recognition + dlib  128 维特征，1:N 最准（要编译 dlib，py3.13 基本装不上）
  lbph        opencv-contrib-python    LBPH 纹理特征，单人单张即可识别（推荐）
  haar        opencv-python            只检测人脸，不能认人
  none        无                        只收图存盘

⚠ 关键：只有 facerec / lbph 两种引擎才真的会"比对"。
  haar / none 下 analyze() 一律返回 score=0.0 matched=0，
  板端看到的就是 ok=1 match=0 score=0.000 —— 这不是"认错了"，
  而是后端根本没有识别能力。响应里的 "cap" 字段就是用来区分这两种情况的。

用法:
    python face_server.py                     # 监听 0.0.0.0:5000
    python face_server.py --port 8080
    python face_server.py --no-save           # 只看日志不存盘
    python face_server.py --lbph-threshold 85 # LBPH 判定阈值（距离，越小越严）

板子端:
    face url 192.168.137.1 5000 /api/face
    face enroll 张三          # 板子抓一帧 -> POST /api/enroll?name=张三
    face run

接口:
    POST /api/face     body = 现场 JPEG，做识别
    POST /api/enroll   body = 登记照片，?name=张三，存成 known/张三.jpg 并立即生效
    GET  /status       服务能力 / 已登记名单 / 当前引擎
    GET  /api/list     已登记名单

返回 JSON 约定（板端 cloud_face.c 按这个解析）:
    识别: {"ok":1,"face":1,"matched":1,"name":"zhangsan","score":0.93,
           "known":3,"cap":"recognition","engine":"lbph","ms":42}
    登记: {"ok":1,"enrolled":1,"known":3,"msg":"ok","engine":"lbph"}
    ok=0 时附带 "msg" 说明原因
"""
import argparse
import itertools
import json
import os
import re
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

# ---------------- 可选依赖，按能力降级 ----------------
_HAS_NP = False
_HAS_CV2 = False
_HAS_FR = False
_HAS_LBPH = False        # cv2.face 存在（需要 opencv-contrib-python）
_face_cascade = None
_known = {}              # name -> face_recognition encoding
_lbph_samples = {}       # name -> [gray_crop, ...]  (LBPH 用)
_lbph_model = None       # 训练结果，None = 需要重建
_lbph_index = []         # label(int) -> name

ARGS = None

try:
    import numpy as np
    _HAS_NP = True
except Exception:
    np = None

try:
    import cv2
    _HAS_CV2 = True
    _cascade_path = os.path.join(cv2.data.haarcascades,
                                 "haarcascade_frontalface_default.xml")
    if os.path.exists(_cascade_path):
        _face_cascade = cv2.CascadeClassifier(_cascade_path)
except Exception:
    _HAS_CV2 = False

if _HAS_CV2:
    # cv2.face 只在 contrib 版本里有；某些版本要显式 import 才挂上去
    try:
        import cv2.face  # noqa: F401
    except Exception:
        pass
    _HAS_LBPH = hasattr(cv2, "face") and hasattr(getattr(cv2, "face", None),
                                                 "LBPHFaceRecognizer_create")

try:
    import face_recognition
    _HAS_FR = True
except Exception:
    _HAS_FR = False


def engine_name():
    """当前真正生效的比对引擎。网页/串口都靠它判断后端能不能认人。"""
    if _HAS_FR:
        return "facerec"
    if _HAS_LBPH:
        return "lbph"
    if _HAS_CV2:
        return "haar"
    return "none"


def can_recognize():
    """能不能真的认人（而不只是"收到图了"）。"""
    return engine_name() in ("facerec", "lbph")


def cap_name():
    """"cap" 字段：recognition=能认人，detect=只能检测，recv=只能收图。

    板端会把它透传到网页，用来区分"认错了"和"根本没能力认"。"""
    e = engine_name()
    if e in ("facerec", "lbph"):
        return "recognition"
    if e == "haar":
        return "detect"
    return "recv"


# ---------------- 人脸预处理 ----------------
def detect_boxes(img):
    """返回原图坐标下的人脸框 [(x,y,w,h), ...]，按面积从大到小。

    1080p 整帧里人脸往往只占很小一块，直接在原图上跑 Haar 既慢又容易漏；
    先等比缩到长边 800 再检测，然后映射回原坐标。

    三轮，越往后越贵也越宽松：
      1) 800 + 严格     板端 800x480 抓帧基本这一轮就过
      2) 800 + 放宽     侧脸、逆光、戴眼镜
      3) 1400 + 放宽    手机原图（3000+ 像素）缩到 800 后脸只剩几十像素，
                        这一轮把细节找回来。只在原图分辨率高时才跑 ——
                        板端 800x480 的图走不到这里，不会拖慢识别。
    """
    if _face_cascade is None or img is None:
        return []
    h, w = img.shape[:2]
    long_side = max(h, w)

    passes = [(800, 1.10, 5, 40), (800, 1.08, 3, 30)]
    if long_side > 900:
        passes.append((1400, 1.08, 3, 28))

    for side, sf, mn, minpx in passes:
        scale = float(side) / long_side
        if scale < 1.0:
            small = cv2.resize(img, (int(w * scale), int(h * scale)))
        else:
            small, scale = img, 1.0

        gray = cv2.cvtColor(small, cv2.COLOR_BGR2GRAY)
        gray = cv2.equalizeHist(gray)
        faces = _face_cascade.detectMultiScale(
            gray, scaleFactor=sf, minNeighbors=mn, minSize=(minpx, minpx))
        if len(faces) == 0:
            continue

        out = [(int(x / scale), int(y / scale), int(fw / scale), int(fh / scale))
               for (x, y, fw, fh) in faces]
        out.sort(key=lambda b: b[2] * b[3], reverse=True)
        return out

    return []


def crop_gray(img, box, size=128):
    """裁人脸 -> 灰度 -> 直方图均衡 -> 定尺寸。

    LBPH 对光照很敏感，均衡化能明显拉开同一个人 / 不同人的距离。
    外扩 8% 是给下巴和额头留点余量，裁太紧反而丢纹理。
    """
    x, y, w, h = box
    m = int(min(w, h) * 0.08)
    x0, y0 = max(0, x - m), max(0, y - m)
    x1, y1 = min(img.shape[1], x + w + m), min(img.shape[0], y + h + m)
    roi = img[y0:y1, x0:x1]
    if roi.size == 0:
        return None
    gray = cv2.cvtColor(roi, cv2.COLOR_BGR2GRAY)
    gray = cv2.equalizeHist(gray)
    return cv2.resize(gray, (size, size))


# ---------------- LBPH 索引 ----------------
def lbph_rebuild():
    """把所有登记样本训练成一个 LBPH 模型（人少，毫秒级）。"""
    global _lbph_model, _lbph_index
    _lbph_model, _lbph_index = None, []
    names = sorted(_lbph_samples.keys())
    faces, labels = [], []
    for i, nm in enumerate(names):
        for f in _lbph_samples[nm]:
            faces.append(f)
            labels.append(i)
    if not faces:
        return False
    try:
        # radius=1/neighbors=8/grid=8x8 是小样本下比较稳的一组参数
        m = cv2.face.LBPHFaceRecognizer_create(radius=1, neighbors=8,
                                               grid_x=8, grid_y=8)
        m.train(faces, np.array(labels))
        _lbph_model, _lbph_index = m, names
        print(f"[lbph] trained {len(faces)} sample(s), {len(names)} person(s)")
        return True
    except Exception as e:
        print(f"[warn] lbph train failed: {e}")
        return False


def load_known(ref_dir="known"):
    """加载 known/ 目录下已登记的照片，建好识别索引。"""
    global _lbph_model
    _known.clear()
    _lbph_samples.clear()
    _lbph_model = None

    if not os.path.isdir(ref_dir):
        return
    for fn in sorted(os.listdir(ref_dir)):
        if not fn.lower().endswith((".jpg", ".jpeg", ".png")):
            continue
        name = os.path.splitext(fn)[0]
        path = os.path.join(ref_dir, fn)

        if _HAS_FR:
            try:
                img = face_recognition.load_image_file(path)
                encs = face_recognition.face_encodings(img)
                if encs:
                    _known[name] = encs[0]
                    print(f"[ref] {name}: facerec ok")
                else:
                    print(f"[ref] {name}: 没检测到人脸，跳过")
            except Exception as e:
                print(f"[ref] {name}: {e}")

        if not _HAS_FR and _HAS_LBPH and _face_cascade is not None:
            try:
                arr = np.fromfile(path, dtype=np.uint8)
                img = cv2.imdecode(arr, cv2.IMREAD_COLOR)
                if img is None:
                    print(f"[ref] {name}: 解码失败，跳过")
                    continue
                boxes = detect_boxes(img)
                if not boxes:
                    print(f"[ref] {name}: 没检测到人脸，跳过")
                    continue
                g = crop_gray(img, boxes[0])
                if g is not None:
                    _lbph_samples[name] = [g]
                    print(f"[ref] {name}: lbph ok ({boxes[0][2]}x{boxes[0][3]} px)")
            except Exception as e:
                print(f"[ref] {name}: {e}")

    if _HAS_LBPH and not _HAS_FR and _lbph_samples:
        lbph_rebuild()


def ref_count():
    """known/ 目录里的登记照片数。

    注意这是"登记了几个人"，和 len(_known)/len(_lbph_samples)（建好索引的）
    不是一回事：没有识别引擎时照片照样能存，只是没法做 1:N。
    板端网页显示的就是这个数，所以不能用 len(_known)。"""
    d = (ARGS.ref if ARGS else "known")
    try:
        return len([f for f in os.listdir(d)
                    if f.lower().endswith((".jpg", ".jpeg", ".png"))])
    except OSError:
        return 0


def indexed_count():
    """真正进了索引的人数（能被认出来的）。"""
    return len(_known) if _HAS_FR else len(_lbph_index)


# ---------------- 识别 ----------------
def analyze(jpeg_bytes):
    """返回 dict: face, matched, name, score, engine, note, preview

    face  = 画面里有没有人脸
    score = 相似度 0~1，只有 recognition 引擎才有意义
    note  = 人话说明（为什么没匹配上）
    """
    # 每次调用先清掉上一帧的预览图，否则这一帧走异常分支时
    # 会把上一帧画好框的图当成这一帧存出去，看起来像"这帧检测到了"
    analyze.preview = None
    res = {"face": False, "matched": False, "name": "", "score": 0.0,
           "engine": engine_name(), "note": "", "preview": None}

    if not (_HAS_CV2 and _face_cascade is not None):
        res["note"] = "recv-only"
        return res

    try:
        arr = np.frombuffer(jpeg_bytes, dtype=np.uint8)
        img = cv2.imdecode(arr, cv2.IMREAD_COLOR)
        if img is None:
            res["note"] = "jpeg decode failed"
            return res

        boxes = detect_boxes(img)
        if not boxes:
            res["note"] = "no face"
            return res

        res["face"] = True
        box = boxes[0]
        print(f"[detect] {len(boxes)} face(s), main box "
              f"{box[2]}x{box[3]} at ({box[0]},{box[1]}), img "
              f"{img.shape[1]}x{img.shape[0]}")

        if not can_recognize():
            for (x, y, w, h) in boxes:
                cv2.rectangle(img, (x, y), (x + w, y + h), (0, 0, 255), 2)
            res["preview"] = img
            res["note"] = "detect-only"
            return res

        # --- 1) face_recognition：128 维，距离越小越像 ---
        if _HAS_FR and _known:
            rgb = cv2.cvtColor(img, cv2.COLOR_BGR2RGB)
            encs = face_recognition.face_encodings(rgb)
            if not encs:
                res["note"] = "encode failed"
                return res
            names = list(_known.keys())
            best_name, best_score = "", 0.0
            for e in encs:
                dists = face_recognition.face_distance(
                    [_known[n] for n in names], e)
                for n, d in zip(names, dists):
                    score = max(0.0, 1.0 - float(d))
                    if score > best_score:
                        best_name, best_score = n, score
            res["score"] = best_score
            res["matched"] = best_score >= 0.5
            res["name"] = best_name if res["matched"] else ""
            if not res["matched"]:
                res["note"] = f"score {best_score:.3f} < 0.500"
            return res

        # --- 2) LBPH：距离(0~∞) 越小越像，阈值默认 85 ---
        if _HAS_LBPH:
            global _lbph_model
            if not _lbph_samples:
                res["note"] = "no one enrolled"
                return res
            if _lbph_model is None and not lbph_rebuild():
                res["note"] = "lbph train failed"
                return res

            g = crop_gray(img, box)
            if g is None:
                res["note"] = "crop failed"
                return res
            label, dist = _lbph_model.predict(g)
            if label < 0 or label >= len(_lbph_index):
                res["note"] = "lbph label out of range"
                return res
            name = _lbph_index[label]
            # 阈值 85 是拿现场真实抓帧标出来的（tools/received/ 里 14 帧）：
            #   同一个人 666 连拍 8 帧：dist 58.6 ~ 73.3
            #   不同的人         ：dist 133 ~ 137
            # 65 会把 68.3 / 73.3 这两帧真人判成陌生人；85 落在中间偏左，
            # 既兜住同人的抖动，又离不同人还差 48 的余量。
            thr = ARGS.lbph_threshold if ARGS else 85.0
            # 距离 -> 0~1 分数。LBPH 的 dist 没有上界（不同人常见 120~180，
            # 极端情况能到 300+），所以分母取 200 而不是 100：
            #   同人  dist 0~60  -> score 1.00~0.70（看着就像"匹配"）
            #   不同人 dist 120+  -> score 0.40 以下
            # 用 /100 的话，正好卡在阈值的 65 会显示成 0.35 分，
            # 网页上"匹配成功但只有 35 分"，现场会以为识别不可靠。
            res["score"] = max(0.0, min(1.0, 1.0 - float(dist) / 200.0))
            res["matched"] = float(dist) <= thr
            res["name"] = name if res["matched"] else ""
            print(f"[lbph] predict '{name}' dist={dist:.1f} thr={thr:.0f} "
                  f"-> {'MATCH' if res['matched'] else 'unknown'}")
            if not res["matched"]:
                res["note"] = f"dist {dist:.0f} > {thr:.0f}"
            return res

    except Exception as e:
        print(f"[warn] analyze failed: {e}")
        res["note"] = f"analyze error: {e}"
        return res

    return res


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    # 用 itertools.count 发号：__next__ 在 CPython 里是原子的，
    # 而 ThreadingHTTPServer 下 `counter += 1` 并发会撞号，
    # 撞号会让 received/ 里两帧叫同一个文件名、后一帧覆盖前一帧。
    _seq = itertools.count(1)
    counter = 0          # 仅用于 /status 展示

    # ---- 工具 ----
    def _send_json(self, obj, code=200):
        # separators 必须显式给成紧凑格式。
        #
        # json.dumps 默认是 ('， ', ': ')，也就是输出 "name": "zjq" —— 冒号后
        # 有一个空格。板端 cloud_face.c 的 json_get_str() 原来用
        # strstr(json, "\"name\":\"") 硬匹配，带空格就整串找不到，于是：
        #   matched=1、score=0.616 全都正常，唯独 f_nm 是空串，
        #   网页上名字显示"未知"，而且连服务端的 msg（失败原因）也一直是"-"。
        # 板端已经改成跳过空白的写法，这里再压成紧凑格式，双保险：
        # 老固件不重烧也能立刻显示名字。
        body = json.dumps(obj, ensure_ascii=False,
                          separators=(",", ":")).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        print(f"[{time.strftime('%H:%M:%S')}] {self.address_string()} {fmt % args}")

    # ---- 路由 ----
    def do_GET(self):
        if self.path in ("/", "/index.html"):
            caps = [f"engine={engine_name()}", f"cap={cap_name()}"]
            if _HAS_LBPH:
                caps.append("lbph")
            if _HAS_FR:
                caps.append("facerec")
            html = (
                "<html><head><meta charset='utf-8'><title>MC632X face server</title></head>"
                "<body style='font-family:sans-serif;padding:24px'>"
                "<h2>MC632X 云端人脸 - PC 接收服务</h2>"
                f"<p>状态：<b>running</b></p>"
                f"<p>引擎：<code>{engine_name()}</code>　"
                f"能力：<code>{cap_name()}</code></p>"
                f"<p>可识别人数：<b>{indexed_count()}</b> / 已存照片 {ref_count()}</p>"
                f"<p>细节：<code>{', '.join(caps)}</code></p>"
                "<p>POST <code>/api/face</code>，Content-Type: image/jpeg，body 为原始 JPEG。</p>"
                "</body></html>"
            ).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(html)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(html)
        elif self.path == "/status":
            self._send_json({
                "ok": 1,
                "engine": engine_name(),
                "cap": cap_name(),
                "can_recognize": can_recognize(),
                "cv2": _HAS_CV2,
                "cv2face": _HAS_LBPH,
                "face_recognition": _HAS_FR,
                "known": ref_count(),
                "indexed": indexed_count(),
                "names": sorted(_lbph_samples.keys()) if not _HAS_FR else list(_known.keys()),
                "received": Handler.counter,
            })
        elif self.path == "/api/list":
            names = list(_known.keys()) if _HAS_FR else sorted(_lbph_samples.keys())
            self._send_json({"ok": 1, "count": len(names), "names": names})
        else:
            self._send_json({"ok": 0, "msg": "not found"}, 404)

    def do_POST(self):
        t0 = time.time()
        length = int(self.headers.get("Content-Length", 0))
        device = self.headers.get("X-Device", "-")
        ctype = self.headers.get("Content-Type", "-")

        if length <= 0:
            self._send_json({"ok": 0, "msg": "empty body"}, 400)
            return
        if length > 4 * 1024 * 1024:
            self._send_json({"ok": 0, "msg": "body too large"}, 413)
            return

        data = b""
        remain = length
        while remain > 0:
            chunk = self.rfile.read(min(remain, 65536))
            if not chunk:
                break
            data += chunk
            remain -= len(chunk)

        idx = next(Handler._seq)
        Handler.counter = idx

        # 是不是一张 JPEG。
        # 只硬性要求 SOI（FFD8）：板端送的是 VENC 出来的 MJPEG 帧，
        # 万一编码器在尾部补了对齐填充，"必须以 FFD9 结尾"的严格判断会
        # 把好帧判成坏的，现场很难查。EOI 缺失只告警，真正的坏图
        # 后面 cv2.imdecode / face_recognition 会自己报错。
        is_jpeg = data[:2] == b"\xff\xd8"
        if is_jpeg and data[-2:] != b"\xff\xd9":
            print(f"[warn] #{idx} JPEG 尾部不是 FFD9（可能被截断，或编码器补了填充）")

        route = urlparse(self.path).path
        print(f"[recv] #{idx} {route} {length} bytes, jpeg={is_jpeg}, "
              f"device={device}, type={ctype}")

        if not is_jpeg:
            self._send_json({"ok": 0, "msg": "not jpeg",
                             "ms": int((time.time() - t0) * 1000)})
            return

        if route == "/api/enroll":
            self._do_enroll(data, idx, t0)
        else:
            self._do_face(data, idx, t0, device)

    # ---- 登记 ----
    def _do_enroll(self, data, idx, t0):
        q = parse_qs(urlparse(self.path).query)
        name = (q.get("name") or [""])[0].strip()

        if not name:
            self._send_json({"ok": 0, "msg": "no name"})
            return

        safe = re.sub(r"[\\/:*?\"<>|]", "_", name)[:32]
        os.makedirs(ARGS.ref, exist_ok=True)
        path = os.path.join(ARGS.ref, safe + ".jpg")

        # 注意：这里**先不落盘**。同名重新登记时，一张没脸的废片会把
        # 原来那张能用的覆盖掉 —— 于是"重新登记一次"反而把能认的人弄丢了。
        # 先解码 + 检测，确认能用再写文件（见函数末尾）。

        global _lbph_model
        enrolled = 0
        msg = ""
        can_check = (_HAS_CV2 and _face_cascade is not None)
        img = None

        # 先确认这张图里到底有没有脸 —— 无论哪种引擎，这步都必须过。
        # 否则会重演"存盘成功 -> 索引里没有 -> 识别永远失败"的假成功。
        face_ok, crop = False, None
        if can_check:
            try:
                arr = np.frombuffer(data, dtype=np.uint8)
                img = cv2.imdecode(arr, cv2.IMREAD_COLOR)
                if img is not None:
                    boxes = detect_boxes(img)
                    if boxes:
                        face_ok = True
                        crop = crop_gray(img, boxes[0])
                        print(f"[enroll] {name}: face {boxes[0][2]}x{boxes[0][3]} px "
                              f"in {img.shape[1]}x{img.shape[0]} photo")
                    else:
                        print(f"[enroll] {name}: 图里没检测到人脸 "
                              f"({len(data)} bytes, {img.shape[1]}x{img.shape[0]})")
            except Exception as e:
                print(f"[enroll] {name}: decode/detect error: {e}")

        if _HAS_FR:
            if not face_ok:
                msg = "no face in photo"
            else:
                try:
                    # 图已经在上面解过了，别再 load_image_file(path)：
                    # 文件是校验通过之后才写的，这时候可能还不存在。
                    rgb = cv2.cvtColor(img, cv2.COLOR_BGR2RGB)
                    encs = face_recognition.face_encodings(rgb)
                    if encs:
                        _known[name] = encs[0]
                        enrolled, msg = 1, "ok"
                    else:
                        msg = "no face in photo"
                except Exception as e:
                    print(f"[enroll] {name}: encode error: {e}")
                    msg = "encode failed"
        elif _HAS_LBPH:
            if not face_ok or crop is None:
                msg = "no face in photo"
            else:
                _lbph_samples[name] = [crop]
                _lbph_model = None          # 下次识别前重建
                lbph_rebuild()
                enrolled = 1 if name in _lbph_index else 0
                msg = "ok" if enrolled else "lbph train failed"
        elif _HAS_CV2:
            msg = "detect-only"
        else:
            msg = "recv-only"

        # 校验通过才落盘。这样一次失败的"重新登记"不会把原来那张能用的
        # 同名照片冲掉 —— 否则用户重试几次之后就一个都认不出来了。
        # engine=none 时没有检测能力，没得校验，照存不误（至少留个底）。
        if enrolled or not can_check:
            try:
                with open(path, "wb") as f:
                    f.write(data)
                print(f"[enroll] {name} -> {path} ({len(data)} bytes)")
            except OSError as e:
                print(f"[enroll] {name}: 写文件失败 {e}")

        if enrolled:
            print(f"[enroll] {name}: 入库成功（{engine_name()}），"
                  f"当前可识别 {indexed_count()} 人")
        else:
            print(f"[enroll] {name}: 未入库 -> {msg}")

        self._send_json({
            "ok": 1 if enrolled else 0,
            "enrolled": enrolled,
            "name": name,
            "known": ref_count(),
            "indexed": indexed_count(),
            "engine": engine_name(),
            "msg": msg,
            "ms": int((time.time() - t0) * 1000),
        })

    # ---- 识别 ----
    def _do_face(self, data, idx, t0, device):
        saved = ""
        if ARGS.save:
            os.makedirs(ARGS.outdir, exist_ok=True)
            saved = os.path.join(
                ARGS.outdir,
                f"{time.strftime('%Y%m%d_%H%M%S')}_{idx:04d}.jpg")
            with open(saved, "wb") as f:
                f.write(data)
            print(f"[save] {saved}")

        r = analyze(data)

        # 只做检测时，把画了框的预览也存一份（用 imencode 避开中文路径）
        if r["preview"] is not None and saved:
            try:
                okp, buf = cv2.imencode(".jpg", r["preview"])
                if okp:
                    with open(saved.replace(".jpg", "_box.jpg"), "wb") as f:
                        f.write(buf.tobytes())
            except Exception:
                pass
            r["preview"] = None

        resp = {
            "ok": 1,
            "face": 1 if r["face"] else 0,
            "matched": 1 if r["matched"] else 0,
            "name": r["name"],
            "score": round(float(r["score"]), 3),
            "known": ref_count(),
            # indexed 必须回：known 只是 known/ 目录里的照片张数，登记失败的照片
            # 照样落盘。板端优先取 indexed，缺了它就退回 known，网页会显示
            # "3 人"而实际只有 2 个人能被认出来（zdh.jpg 检不到人脸就是这种）。
            "indexed": indexed_count(),
            "engine": r["engine"],
            "cap": cap_name(),
            "ms": int((time.time() - t0) * 1000),
        }
        if r["note"]:
            resp["msg"] = r["note"]
        elif not r["matched"]:
            resp["msg"] = "unknown"

        print(f"[resp] {resp}")
        self._send_json(resp)


def main():
    global ARGS
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=5000)
    ap.add_argument("--outdir", default="received")
    ap.add_argument("--no-save", dest="save", action="store_false")
    ap.add_argument("--ref", default="known", help="已知人脸目录")
    ap.add_argument("--lbph-threshold", type=float, default=85.0,
                    help="LBPH 判定阈值(距离)，越小越严；默认 85")
    ARGS = ap.parse_args()

    load_known(ARGS.ref)

    print("=" * 60)
    print(" MC632X 云端人脸 - PC 服务端")
    print(f" 监听   : http://{ARGS.host}:{ARGS.port}")
    print(f" 接口   : POST /api/face  (Content-Type: image/jpeg)")
    print(f" 引擎   : {engine_name()}   能力: {cap_name()}")
    print(f" 已索引 : {indexed_count()} 人 / 照片 {ref_count()} 张")
    if not can_recognize():
        print(" " + "-" * 56)
        print(" ⚠ 当前后端【不能认人】，只会回 score=0.000 / matched=0。")
        print("   板端会显示 ok=1 match=0 score=0.000 —— 这不是认错，是没引擎。")
        print("   装一个即可（免编译，纯 wheel）：")
        print("     pip install opencv-contrib-python")
        print("   装完务必重启本服务。")
        print(" " + "-" * 56)
    else:
        print(f" LBPH 阈值: {ARGS.lbph_threshold:.0f}（距离，越小越严）")
    print("=" * 60)

    srv = ThreadingHTTPServer((ARGS.host, ARGS.port), Handler)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\n bye")


if __name__ == "__main__":
    main()
