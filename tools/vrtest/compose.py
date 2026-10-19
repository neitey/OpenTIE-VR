#!/usr/bin/env python3
"""Composites a fake-XR capture (TIE_TEST_XR=1) like the OpenXR compositor:
the eye image (projection layer), then hud_world, hud / hud panels, cards,
console, the eye overlay and beams as quads in LOCAL space.

usage: compose.py <capture prefix> [...]
  prefix = capture-dir/f000123[_label] (the part before _xr.txt)
writes <prefix>_comp_left.png, <prefix>_comp_right.png and <prefix>_comp_sbs.png

Approximations: blending in sRGB, nearest sampling, head-locked quads rigidly
attached (no follow smoothing), beams flat (no soft edge filtering). Needs numpy and Pillow.
"""
import math
import sys

import numpy as np
from PIL import Image


def qmat(q):
    x, y, z, w = q
    return np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)],
                     [2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)],
                     [2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)]])


def parse_kv(tokens):
    out = {}
    for t in tokens:
        if "=" in t:
            k, v = t.split("=", 1)
            out[k] = [float(x) for x in v.split(",")]
    return out


def load(path):
    try:
        im = Image.open(path).convert("RGBA")
    except FileNotFoundError:
        return None
    return np.asarray(im).astype(np.float32) / 255.0


def compose(prefix, eye_index, views, layers, images):
    eye = images.get("eye_right" if eye_index else "eye_left")
    if eye is None:
        return None
    h, w = eye.shape[:2]
    v = views[eye_index]
    fl, fr, fu, fd = v["fov"]
    tl, tr, tu, td = math.tan(fl), math.tan(fr), math.tan(fu), math.tan(fd)
    xs = tl + (np.arange(w) + 0.5) / w * (tr - tl)
    ys = tu + (np.arange(h) + 0.5) / h * (td - tu)
    gx, gy = np.meshgrid(xs, ys)
    d_eye = np.stack([gx, gy, -np.ones_like(gx)], -1)
    rot = qmat(v["ori"])
    dirs = d_eye @ rot.T
    eye_pos = np.array(v["pos"])
    head_pos = (np.array(views[0]["pos"]) + np.array(views[1]["pos"])) / 2
    head_rot = qmat(views[0]["ori"])

    def quad(img, rect, pos, rmat, size):
        n = rmat[:, 2]
        dn = dirs @ n
        t = ((pos - eye_pos) @ n) / np.where(np.abs(dn) < 1e-9, 1e-9, dn)
        hit = eye_pos + dirs * t[..., None]
        rel = hit - pos
        lx = rel @ rmat[:, 0]
        ly = rel @ rmat[:, 1]
        u = lx / size[0] + 0.5
        vv = 0.5 - ly / size[1]
        ok = (t > 0) & (u >= 0) & (u < 1) & (vv >= 0) & (vv < 1)
        rx, ry, rw, rh = rect
        px = np.clip((rx + u * rw).astype(int), 0, img.shape[1] - 1)
        py = np.clip((ry + vv * rh).astype(int), 0, img.shape[0] - 1)
        s = img[py, px].copy()
        s[~ok] = 0
        return s

    def over_premult(dst, src):
        a = src[..., 3:4]
        dst[..., :3] = src[..., :3] + dst[..., :3] * (1 - a)

    out = eye.copy()
    out[..., 3] = 1
    screen = images.get("window")
    if screen is not None and any(kind == "backdrop" for kind, _ in layers):
        # Backdrop frames: the composed 2D layers on the virtual screen
        # (engine default 2.1 m ahead, 2.1 m wide), over the eyes.
        sw = 2.1
        sh = sw * screen.shape[0] / screen.shape[1]
        over_premult(out, quad(screen, (0, 0, screen.shape[1], screen.shape[0]), np.array([0.0, 0.0, -2.1]),
                               np.eye(3), (sw, sh)))
    order = ["hud_world", "hud", "hud_panel", "frame", "card", "card2", "console", "eye_overlay", "labels",
             "beam"]
    for name in order:
        for kind, kv in layers:
            if kind != name:
                continue
            if kind in ("hud_world", "hud"):
                img = images.get(kind)
                if img is None or not kv.get("visible", [0])[0]:
                    continue
                pos = np.array(kv["pos"])
                rmat = np.eye(3)
                if kv.get("head_locked", [0])[0]:
                    pos = head_pos + head_rot @ pos
                    rmat = head_rot
                over_premult(out, quad(img, (0, 0, img.shape[1], img.shape[0]), pos, rmat, kv["size"]))
            elif kind == "eye_overlay":
                r = kv.get("rect%d" % eye_index, [0, 0, 0, 0])
                x, y, rw, rh = (int(c) for c in r)
                if rw <= 0 or rh <= 0:
                    x, y, rw, rh = 0, 0, w, h
                region = eye[y:y + rh, x:x + rw]
                a = region[..., 3:4]
                out[y:y + rh, x:x + rw, :3] = region[..., :3] * a + out[y:y + rh, x:x + rw, :3] * (1 - a)
            else:
                if kind == "hud_panel":
                    img = images.get("hud_aux" if int(kv.get("source", [0])[0]) == 1 else "hud")
                elif kind == "beam":
                    img = images.get("beam")
                else:
                    img = images.get(kind)
                if img is None:
                    continue
                eye_sel = int(kv.get("eye", [0])[0])
                if eye_sel and eye_sel != eye_index + 1:
                    continue
                rect = [int(c) for c in kv["rect"]]
                if rect[2] <= 0 or rect[3] <= 0:
                    rect = [0, 0, img.shape[1], img.shape[0]]
                over_premult(out, quad(img, rect, np.array(kv["pos"]), qmat(kv["ori"]), kv["size"]))
    return (np.clip(out[..., :3], 0, 1) * 255).astype(np.uint8)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    for prefix in sys.argv[1:]:
        if prefix.endswith("_xr.txt"):
            prefix = prefix[:-7]
        views = [None, None]
        layers = []
        for line in open(prefix + "_xr.txt"):
            tok = line.split()
            if not tok:
                continue
            if tok[0] in ("view0", "view1"):
                kv = parse_kv(tok[1:])
                views[int(tok[0][4])] = kv
            elif tok[0].startswith("began"):
                if "backdrop=1" in tok:
                    layers.append(("backdrop", {}))
            elif tok[0] != "eye_size":
                layers.append((tok[0], parse_kv(tok[1:])))
        images = {}
        for name in ("eye_left", "eye_right", "hud", "hud_world", "hud_aux", "console", "card", "card2", "frame",
                     "labels", "window", "beam"):
            img = load("%s_%s.png" % (prefix, name))
            if img is not None:
                images[name] = img
        results = []
        for e, side in ((0, "left"), (1, "right")):
            img = compose(prefix, e, views, layers, images)
            if img is not None:
                Image.fromarray(img).save("%s_comp_%s.png" % (prefix, side))
                results.append(img)
        if len(results) == 2:
            Image.fromarray(np.concatenate(results, axis=1)).save(prefix + "_comp_sbs.png")
        print("%s: %d layers, %d eye composites" % (prefix, len(layers), len(results)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
