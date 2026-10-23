#!/usr/bin/env python3
"""OpenTIE-VR's own 3D cockpit shell: a generated single-seat fighter pod.

Original design (not a replica of any film or game cockpit): a faceted
capsule around a seated pilot with a wide three-tier "greenhouse" canopy
(a flat-topped six-sided front pane, open nose facets either side and above
it, big side windows beside the pilot), a sloped dashboard below the sill
carrying the five instrument panels TIE Fighter's own art is shown on, side
consoles, a seat, ribs, pipes, boxes and a few small indicator lights. Dark
grey and black, procedural textures, no text or insignia.

usage: make_cockpit.py --out DIR [--cook AERON_GLTF_COOK] [--glb OUT.glb]
                       [--preview PNG] [--spec]

  --out DIR      writes DIR/opentie_cockpit_fighter.gltf (+ .bin, textures/*.png):
                 plain glTF 2.0 with the AERON_flight_model node contract
  --cook EXE     runs Aeron's glTF cooker on it (aeron/tools/gltf_cook; a build
                 with -DTIE_BUILD_TOOLS=ON makes aeron_gltf_cook), which packs the
                 textures into the KTX2 channel atlases the runtime loader reads
  --glb PATH     cooked output (default resources/cockpit/opentie_cockpit_fighter.glb
                 next to this repo's tools/)
  --preview PNG  software-rendered previews (three-quarter outside, cutaway,
                 pilot's view) for checking the shape without the game
  --spec         print the C initializer for xwa_cockpit.c's k_cockpits[]

Model frame: like the cockpit OPTs xwa_cockpit.c places (X right, -Y forward,
Z up, metres) with the pilot's eye at the origin, converted to glTF axes on
write the way Aeron's loader converts back (aeron x,y,z = -gx, -gz, gy).
Needs numpy and Pillow.
"""
import argparse
import json
import math
import os
import struct
import subprocess
import sys

import numpy as np
from PIL import Image

NAME = "opentie_cockpit_fighter"

# ---------------------------------------------------------------- textures

TEX = 128


def fft_noise(size, beta, seed):
    """Tileable 1/f^beta noise in [-1, 1]."""
    rng = np.random.default_rng(seed)
    white = rng.standard_normal((size, size))
    fy = np.fft.fftfreq(size)[:, None]
    fx = np.fft.fftfreq(size)[None, :]
    f = np.sqrt(fx * fx + fy * fy)
    f[0, 0] = 1.0
    spec = np.fft.fft2(white) / f ** beta
    spec[0, 0] = 0.0
    n = np.real(np.fft.ifft2(spec))
    return n / (np.abs(n).max() + 1e-9)


def to_img(rgb):
    return Image.fromarray(np.clip(rgb, 0, 255).astype(np.uint8), "RGB")


def tex_hull(size=256):
    base = np.array([40.0, 42.0, 46.0])
    img = np.ones((size, size, 3)) * base
    grime = fft_noise(size, 1.6, 1)[..., None]
    img *= 1.0 + 0.16 * grime
    fine = fft_noise(size, 0.6, 2)[..., None]
    img *= 1.0 + 0.05 * fine
    # Irregular panel seams: border, a vertical one in the top half, a
    # horizontal one on the right.
    def seam_v(x, y0, y1):
        img[y0:y1, x:x + 2] *= 0.45
        img[y0:y1, x + 2:x + 3] *= 1.35
    def seam_h(y, x0, x1):
        img[y:y + 2, x0:x1] *= 0.45
        img[y + 2:y + 3, x0:x1] *= 1.35
    seam_v(0, 0, size)
    seam_h(0, 0, size)
    seam_v(size * 5 // 8, 0, size // 2)
    seam_h(size // 2, size * 5 // 8, size)
    seam_h(size * 3 // 4, 0, size * 5 // 8)
    # Fasteners along the seams.
    for k in range(4, size, 16):
        for (y, x) in ((k, 5), (5, k), (k, size * 5 // 8 + 6) if k < size // 2 else (k, 5)):
            img[y:y + 2, x:x + 2] = base * 1.6
    return to_img(img)


def tex_console(size=TEX):
    base = np.array([30.0, 31.0, 34.0])
    img = np.ones((size, size, 3)) * base
    img *= 1.0 + 0.10 * fft_noise(size, 1.2, 3)[..., None]
    step = size // 2
    for k in range(0, size, step):
        img[k:k + 1, :] *= 0.5
        img[:, k:k + 1] *= 0.5
        img[k + 1:k + 2, :] *= 1.3
        img[:, k + 1:k + 2] *= 1.3
    for y in (6, step - 7, step + 6, size - 7):
        for x in (6, step - 7, step + 6, size - 7):
            img[y - 1:y + 1, x - 1:x + 1] = base * 2.0
    return to_img(img)


def tex_frame(size=TEX):
    base = np.array([26.0, 26.0, 29.0])
    img = np.ones((size, size, 3)) * base
    rng = np.random.default_rng(4)
    streak = rng.standard_normal(size)[None, :] * np.ones((size, 1))
    img *= (1.0 + 0.10 * streak[..., None] / 3.0)
    img *= 1.0 + 0.08 * fft_noise(size, 1.4, 5)[..., None]
    return to_img(img)


def tex_bezel(size=64):
    img = np.ones((size, size, 3)) * np.array([11.0, 11.0, 12.0])
    img *= 1.0 + 0.08 * fft_noise(size, 1.0, 6)[..., None]
    return to_img(img)


def tex_seat(size=TEX):
    base = np.array([24.0, 23.0, 23.0])
    y, x = np.mgrid[0:size, 0:size]
    # Diamond quilting: two diagonal line families.
    period = size // 4
    d1 = ((x + y) % period)
    d2 = ((x - y) % period)
    pad = np.minimum(np.minimum(d1, period - d1), np.minimum(d2, period - d2)) / (period / 2.0)
    shade = 0.70 + 0.45 * np.sqrt(np.clip(pad, 0, 1))
    img = base[None, None, :] * shade[..., None]
    stitch = ((d1 == 0) | (d2 == 0)) & ((x // 2) % 2 == 0)
    img[stitch] = base * 1.8
    img *= 1.0 + 0.06 * fft_noise(size, 1.0, 7)[..., None]
    return to_img(img)


def tex_metal(size=TEX):
    base = np.array([66.0, 68.0, 72.0])
    img = np.ones((size, size, 3)) * base
    img *= 1.0 + 0.18 * fft_noise(size, 1.5, 8)[..., None]
    img *= 1.0 + 0.06 * fft_noise(size, 0.5, 9)[..., None]
    return to_img(img)


def tex_floor(size=TEX):
    base = np.array([30.0, 30.0, 32.0])
    y, x = np.mgrid[0:size, 0:size]
    period = 16
    cx = (x % period) - period / 2 + 0.5
    cy = (y % period) - period / 2 + 0.5
    # Raised diamond studs alternating direction per cell (tread plate).
    flip = ((x // period + y // period) % 2) == 0
    u = np.where(flip, cx + cy, cx - cy)
    v = np.where(flip, cx - cy, cx + cy)
    stud = (np.abs(u) < 7) & (np.abs(v) < 1.6)
    img = np.ones((size, size, 3)) * base
    img[stud] *= 1.7
    img *= 1.0 + 0.15 * fft_noise(size, 1.5, 10)[..., None]
    return to_img(img)


# Indicator palette: 4 x 4 cells, the light's UV picks one.
LIGHT_COLORS = {
    "red": (255, 40, 28),
    "green": (40, 255, 90),
    "amber": (255, 165, 30),
    "blue": (90, 170, 255),
    "white": (230, 235, 255),
    "dim_red": (120, 18, 14),
}
LIGHT_CELLS = list(LIGHT_COLORS)


def tex_lights(size=32):
    img = np.zeros((size, size, 3))
    cell = size // 4
    for i, name in enumerate(LIGHT_CELLS):
        cx, cy = i % 4, i // 4
        img[cy * cell:(cy + 1) * cell, cx * cell:(cx + 1) * cell] = LIGHT_COLORS[name]
    return to_img(img)


def light_uv(name):
    i = LIGHT_CELLS.index(name)
    return ((i % 4) + 0.5) / 4.0, ((i // 4) + 0.5) / 4.0


# name: (texture fn, metallic, roughness, emissive factor, double sided, uv
# tile metres). A small emissive factor on the base texture is a dim fill so
# the pod does not go pitch black with the sun behind it; the indicator
# lights glow fully.
FILL = 0.30
MATERIALS = {
    "hull": (tex_hull, 0.35, 0.70, FILL, True, 1.0),
    "frame": (tex_frame, 0.65, 0.45, FILL, False, 0.5),
    "console": (tex_console, 0.30, 0.60, FILL, False, 0.4),
    "bezel": (tex_bezel, 0.10, 0.85, 0.0, False, 0.25),
    "seat": (tex_seat, 0.0, 0.92, FILL, False, 0.30),
    "metal": (tex_metal, 0.80, 0.42, FILL, False, 0.4),
    "floor": (tex_floor, 0.50, 0.65, FILL, False, 0.35),
    "lights": (tex_lights, 0.0, 0.50, 1.0, False, 1.0),
}

# ---------------------------------------------------------------- geometry


def v3(*a):
    return np.array(a, dtype=np.float64)


def unit(v):
    n = np.linalg.norm(v)
    return v / n if n > 1e-12 else v


class Prim:
    def __init__(self, name):
        self.name = name
        self.pos, self.nrm, self.uv, self.idx = [], [], [], []

    def add(self, pts, nrm, uvs):
        base = len(self.pos)
        for p, n, t in zip(pts, nrm, uvs):
            self.pos.append(np.asarray(p, dtype=np.float64))
            self.nrm.append(np.asarray(n, dtype=np.float64))
            self.uv.append(t)
        return base


class Builder:
    def __init__(self):
        self.prims = {m: Prim(m) for m in MATERIALS}

    def box_uv(self, mat, p, n):
        tile = MATERIALS[mat][5]
        a = np.abs(n)
        if a[0] >= a[1] and a[0] >= a[2]:
            u, v = p[1], p[2]
        elif a[1] >= a[2]:
            u, v = p[0], p[2]
        else:
            u, v = p[0], p[1]
        return (u / tile, -v / tile)

    def poly(self, mat, pts, facing=None, uvs=None):
        """Flat convex polygon. `facing`: a vector the front should point along
        (the winding is fixed to match); default the polygon's own winding."""
        pts = [np.asarray(p, dtype=np.float64) for p in pts]
        n = np.zeros(3)
        for i in range(len(pts)):  # Newell normal
            a, b = pts[i], pts[(i + 1) % len(pts)]
            n += np.cross(a, b)
        n = unit(n)
        if facing is not None and np.dot(n, facing) < 0:
            pts = pts[::-1]
            if uvs is not None:
                uvs = uvs[::-1]
            n = -n
        if uvs is None:
            uvs = [self.box_uv(mat, p, n) for p in pts]
        pr = self.prims[mat]
        base = pr.add(pts, [n] * len(pts), uvs)
        for i in range(1, len(pts) - 1):
            pr.idx += [base, base + i, base + i + 1]

    def box(self, mat, center, axes, half, faces="all", uv_const=None):
        """Oriented box; axes = 3 unit column vectors (u, v, w)."""
        c = np.asarray(center, dtype=np.float64)
        u, v, w = (np.asarray(a, dtype=np.float64) for a in axes)
        hu, hv, hw = half
        for axis, sgn in ((0, 1), (0, -1), (1, 1), (1, -1), (2, 1), (2, -1)):
            if faces != "all" and (axis, sgn) not in faces:
                continue
            ax = [u * hu, v * hv, w * hw]
            nrm = ax[axis] * sgn
            o = [ax[k] for k in range(3) if k != axis]
            fc = c + nrm
            pts = [fc - o[0] - o[1], fc + o[0] - o[1], fc + o[0] + o[1], fc - o[0] + o[1]]
            uvs = [uv_const] * 4 if uv_const else None
            self.poly(mat, pts, facing=nrm, uvs=uvs)

    def beam(self, mat, a, b, width, height, up=(0, 0, 1)):
        a, b = np.asarray(a, float), np.asarray(b, float)
        d = b - a
        L = np.linalg.norm(d)
        w = d / L
        up = np.asarray(up, float)
        u = unit(np.cross(w, up))
        if np.linalg.norm(u) < 1e-6:
            u = unit(np.cross(w, v3(1, 0, 0)))
        v = np.cross(u, w)
        self.box(mat, (a + b) / 2, (u, v, w), (width / 2, height / 2, L / 2))

    def cyl(self, mat, a, b, r, segs=10, caps=True):
        a, b = np.asarray(a, float), np.asarray(b, float)
        w = unit(b - a)
        u = unit(np.cross(w, v3(0, 0, 1) if abs(w[2]) < 0.9 else v3(1, 0, 0)))
        v = np.cross(w, u)
        pr = self.prims[mat]
        L = np.linalg.norm(b - a)
        tile = MATERIALS[mat][5]
        ring = []
        for i in range(segs + 1):
            t = 2 * math.pi * i / segs
            ring.append((math.cos(t) * u + math.sin(t) * v, i / segs))
        pts, nrms, uvs = [], [], []
        for dvec, s in ring:
            pts += [a + dvec * r, b + dvec * r]
            nrms += [dvec, dvec]
            uvs += [(s * 2 * math.pi * r / tile, 0.0), (s * 2 * math.pi * r / tile, L / tile)]
        base = pr.add(pts, nrms, uvs)
        for i in range(segs):
            i0, i1, i2, i3 = base + 2 * i, base + 2 * i + 1, base + 2 * i + 2, base + 2 * i + 3
            # outward: (a_i, a_{i+1}, b_{i+1}) has normal along +radial for this frame
            pr.idx += [i0, i2, i3, i0, i3, i1]
        if caps:
            for end, sgn in ((a, -1), (b, 1)):
                cap = [end + d * r for d, _ in ring[:-1]]
                self.poly(mat, cap, facing=w * sgn)

    def light(self, name, center, normal, w=0.016, h=0.010, depth=0.006, up=(0, 0, 1)):
        n = unit(np.asarray(normal, float))
        up = np.asarray(up, float)
        u = unit(np.cross(up, n)) if abs(np.dot(up, n)) < 0.95 else unit(np.cross(v3(0, 1, 0), n))
        v = np.cross(n, u)
        c = np.asarray(center, float) + n * depth / 2
        self.box("lights", c, (u, v, n), (w / 2, h / 2, depth / 2), uv_const=light_uv(name))


# ------------------------------------------------------------ the pod
# Ring profiles (x, z) in metres around the pod, eye at the origin.
# Vertex order: 0 top-left, 1 top-right, 2 right shoulder, 3 right sill,
# 4 right hip, 5 right floor, 6 left floor, 7 left hip, 8 left sill,
# 9 left shoulder. Edge j joins vertex j and j+1.

def mirror_ring(half):
    # half = [top-right, shoulder, sill, hip, floor] for x >= 0
    tr, sh, si, hp, fl = half
    L = lambda p: (-p[0], p[1])
    return [L(tr), tr, sh, si, hp, fl, L(fl), L(hp), L(si), L(sh)]


RING_MAIN = mirror_ring([(0.40, 0.42), (0.74, 0.10), (0.78, -0.22), (0.74, -0.70), (0.46, -1.08)])
RING_NOSE = mirror_ring([(0.26, 0.34), (0.52, 0.08), (0.56, -0.30), (0.52, -0.66), (0.34, -0.96)])

Y_BACK, Y_MID, Y_FRONT, Y_NOSE = 0.62, 0.05, -0.50, -1.02
RINGS = [(Y_BACK, RING_MAIN), (Y_MID, RING_MAIN), (Y_FRONT, RING_MAIN), (Y_NOSE, RING_NOSE)]
# Open (window) edges per segment between consecutive rings.
OPEN = [set(), {1, 2, 8, 9}, {0, 1, 2, 8, 9}]

# Instrument panels: id -> (centre, pitch, yaw, roll, height, assumed aspect
# for the bezel). Pitch 0 = upright facing the pilot, 90 = flat; yaw turns
# the face toward +X. The C table is printed by --spec.
DASH_TOP = v3(0.0, -0.86, -0.28)
DASH_BOT = v3(0.0, -0.60, -0.58)
# Panel pitch of the sloped face: 90 minus its inclination from horizontal.
DASH_PITCH = 90.0 - math.degrees(math.atan2(DASH_TOP[2] - DASH_BOT[2], DASH_BOT[1] - DASH_TOP[1]))


def dash_point(x, s):
    p = DASH_TOP + (DASH_BOT - DASH_TOP) * s
    return v3(x, p[1], p[2])


PANELS = {
    "sensor_fwd": (dash_point(-0.255, 0.47), DASH_PITCH, 0.0, 0.0, 0.16, 1.0),
    "sensor_rear": (dash_point(0.255, 0.47), DASH_PITCH, 0.0, 0.0, 0.16, 1.0),
    "cmd": (dash_point(0.0, 0.50), DASH_PITCH, 0.0, 0.0, 0.17, 1.45),
    "gauges_l": (v3(-0.445, -0.60, -0.47), 42.0, 30.0, 0.0, 0.22, 0.46),
    "gauges_r": (v3(0.445, -0.60, -0.47), 42.0, -30.0, 0.0, 0.22, 0.46),
}


def panel_frame(pitch, yaw, roll):
    """right, down, normal of a panel pose (xwa_cockpit.c's convention)."""
    d = math.radians
    sp, cp = math.sin(d(pitch)), math.cos(d(pitch))
    right = v3(1, 0, 0)
    down = v3(0, sp, -cp)
    normal = v3(0, cp, sp)
    sr, cr = math.sin(d(roll)), math.cos(d(roll))
    r2 = right * cr - down * sr
    d2 = right * sr + down * cr
    sy, cy = math.sin(d(yaw)), math.cos(d(yaw))
    out = []
    for vec in (r2, d2, normal):
        x, y = vec[0], vec[1]
        out.append(v3(x * cy + y * sy, -x * sy + y * cy, vec[2]))
    return out


def ring_point(ring, i, y):
    x, z = ring[i]
    return v3(x, y, z)


def lerp_ring(r0, r1, t):
    return [(a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t) for a, b in zip(r0, r1)]


def ring_at(y):
    for (ya, ra), (yb, rb) in zip(RINGS, RINGS[1:]):
        if yb <= y <= ya:
            return lerp_ring(ra, rb, (ya - y) / (ya - yb))
    return RINGS[0][1] if y > Y_BACK else RINGS[-1][1]


def wall_x(y, z):
    """Inner half width of the pod at (y, z) (right side)."""
    r = ring_at(y)
    side = [r[1], r[2], r[3], r[4], r[5]]  # top-right .. floor-right
    for (xa, za), (xb, zb) in zip(side, side[1:]):
        if zb <= z <= za:
            t = (za - z) / (za - zb) if za != zb else 0
            return xa + (xb - xa) * t
    return side[-1][0] if z < side[-1][1] else side[0][0]


def build():
    b = Builder()
    axis_z = -0.33  # pod axis height (inward normal hint)

    # Hull skin (inward facing; the hull material is double sided so the
    # outside preview shows the shape too).
    for s, ((ya, ra), (yb, rb)) in enumerate(zip(RINGS, RINGS[1:])):
        for j in range(10):
            if j in OPEN[s]:
                continue
            k = (j + 1) % 10
            p = [ring_point(ra, j, ya), ring_point(ra, k, ya), ring_point(rb, k, yb), ring_point(rb, j, yb)]
            c = sum(p) / 4
            b.poly("hull", p, facing=v3(0, c[1], axis_z) - c)
    # Back cap.
    b.poly("hull", [ring_point(RING_MAIN, i, Y_BACK) for i in range(10)], facing=v3(0, -1, 0))
    # Nose cap below the sill (the front pane above it is open).
    b.poly("hull", [ring_point(RING_NOSE, i, Y_NOSE) for i in (3, 4, 5, 6, 7, 8)], facing=v3(0, 1, 0))

    # Canopy frame: hoops at the mid and front rings, the front pane's
    # frame, roof-edge and sill rails, a centre roof spine.
    def path(ring, idxs, y):
        return [ring_point(ring, i, y) for i in idxs]

    hoop = [8, 9, 0, 1, 2, 3]
    for y, ring, size in ((Y_MID, RING_MAIN, 0.065), (Y_FRONT, RING_MAIN, 0.075), (Y_NOSE, RING_NOSE, 0.06)):
        pts = path(ring, hoop, y)
        for a, c in zip(pts, pts[1:]):
            b.beam("frame", a, c, size, size, up=v3(0, 1, 0))
        # joints
        for p in pts:
            b.box("frame", p, (v3(1, 0, 0), v3(0, 1, 0), v3(0, 0, 1)), (size * 0.62,) * 3)
    # Nose sill (the dashboard's front edge) across the front pane.
    b.beam("frame", ring_point(RING_NOSE, 8, Y_NOSE), ring_point(RING_NOSE, 3, Y_NOSE), 0.07, 0.06, up=v3(0, 0, 1))
    for i in (0, 1, 3, 8):
        for (ya, ra), (yb, rb) in ((RINGS[1], RINGS[2]), (RINGS[2], RINGS[3])):
            b.beam("frame", ring_point(ra, i, ya), ring_point(rb, i, yb), 0.055, 0.055, up=v3(0, 0, 1))
    # Roof spine over the head and on to the front ring.
    b.beam("frame", v3(0, Y_BACK - 0.02, 0.405), v3(0, Y_FRONT, 0.405), 0.09, 0.035)

    # Floor plate, a little above the hull floor.
    fz = -1.05
    fl = [v3(-wall_x(Y_BACK, fz), Y_BACK - 0.01, fz), v3(wall_x(Y_BACK, fz), Y_BACK - 0.01, fz),
          v3(wall_x(-0.95, fz + 0.06), -0.95, fz + 0.06), v3(-wall_x(-0.95, fz + 0.06), -0.95, fz + 0.06)]
    b.poly("floor", fl, facing=v3(0, 0, 1))
    # Foot rests (angled plates under the dashboard).
    for sx in (-1, 1):
        c = v3(sx * 0.17, -0.82, -0.93)
        n = unit(v3(0, 0.6, 0.8))
        b.box("metal", c, (v3(1, 0, 0), unit(np.cross(n, v3(1, 0, 0))), n), (0.09, 0.14, 0.012))

    # ---- Dashboard -----------------------------------------------------
    def wx(y, z, inset=0.004):
        return wall_x(y, z) - inset

    top_y, top_z = DASH_TOP[1], DASH_TOP[2]
    bot_y, bot_z = DASH_BOT[1], DASH_BOT[2]
    # Instrument face.
    face = [v3(-wx(top_y, top_z), top_y, top_z), v3(wx(top_y, top_z), top_y, top_z),
            v3(wx(bot_y, bot_z), bot_y, bot_z), v3(-wx(bot_y, bot_z), bot_y, bot_z)]
    fn = panel_frame(DASH_PITCH, 0, 0)[2]
    b.poly("console", face, facing=fn)
    # Glare shield from the front sill back to the face's top edge.
    gz = -0.28
    shield = [v3(-wx(Y_NOSE + 0.02, gz), Y_NOSE + 0.02, gz), v3(wx(Y_NOSE + 0.02, gz), Y_NOSE + 0.02, gz),
              v3(wx(top_y, gz), top_y, gz), v3(-wx(top_y, gz), top_y, gz)]
    b.poly("console", shield, facing=v3(0, 0, 1))
    # Hood lip over the instruments.
    b.beam("frame", v3(-wx(top_y, gz) + 0.02, top_y + 0.015, gz + 0.012), v3(wx(top_y, gz) - 0.02, top_y + 0.015, gz + 0.012),
           0.035, 0.03, up=v3(0, 0, 1))
    # Underside down to the nose cap (knee well roof) and the lower lip.
    uz = -0.66
    under = [v3(-wx(bot_y, bot_z), bot_y, bot_z), v3(wx(bot_y, bot_z), bot_y, bot_z),
             v3(wx(Y_NOSE + 0.02, uz), Y_NOSE + 0.02, uz), v3(-wx(Y_NOSE + 0.02, uz), Y_NOSE + 0.02, uz)]
    b.poly("console", under, facing=v3(0, 0.3, -1))
    b.beam("frame", v3(-wx(bot_y, bot_z) + 0.01, bot_y + 0.01, bot_z - 0.005), v3(wx(bot_y, bot_z) - 0.01, bot_y + 0.01, bot_z - 0.005),
           0.03, 0.03, up=fn)

    # Instrument bezels: the panel plane is their front face.
    for pid, (c, pitch, yaw, roll, h, aspect) in PANELS.items():
        r, d, n = panel_frame(pitch, yaw, roll)
        hw, hh = h * aspect / 2 + 0.018, h / 2 + 0.018
        depth = 0.03 if pid.startswith("gauges") else 0.012
        b.box("bezel", c - n * depth / 2, (r, d, n), (hw, hh, depth / 2))
        # A raised frame strip along the bezel's top and bottom.
        for sgn in (-1, 1):
            b.box("frame", c + d * sgn * (hh - 0.006) + n * 0.004, (r, d, n), (hw, 0.006, 0.004))
        if pid.startswith("gauges"):
            # Pod housing behind the gauge, down onto the side console.
            back = c - n * 0.03
            b.box("console", back - n * 0.05 + v3(0, 0, -0.05), (r, d, n), (hw, hh + 0.02, 0.05))
        # One status light under each panel.
        b.light("green" if pid != "cmd" else "amber", c + d * (hh + 0.012) - n * 0.002, n)

    # Knobs under the sensors, a switch row under the CMD.
    for sx in (-1, 1):
        for dx in (-0.04, 0.04):
            p = dash_point(sx * 0.255 + dx, 0.88)
            b.cyl("metal", p, p + fn * 0.018, 0.011, segs=10)
    r_, d_, _ = panel_frame(DASH_PITCH, 0, 0)
    for i in range(5):
        p = dash_point(-0.08 + i * 0.04, 0.90)
        b.box("metal", p + fn * 0.006, (r_, d_, fn), (0.009, 0.013, 0.006))
        b.box("bezel", p + fn * 0.016 - d_ * 0.004, (r_, d_, fn), (0.003, 0.003, 0.008))

    # Light strip along the glare shield's front edge (between sensors).
    for i, col in enumerate(["red", "amber", "green", "green", "amber", "red"]):
        x = -0.075 + i * 0.03
        b.light(col, v3(x, top_y - 0.03, gz), v3(0, 0, 1), w=0.014, h=0.008)

    # ---- Side consoles ---------------------------------------------------
    cz = -0.53
    for sx in (-1, 1):
        x_in = 0.47
        y0, y1 = -0.62, 0.30
        outer0, outer1 = wx(y0, cz), wx(y1, cz)
        top = [v3(sx * x_in, y0, cz), v3(sx * outer0, y0, cz), v3(sx * outer1, y1, cz), v3(sx * x_in, y1, cz)]
        b.poly("console", top, facing=v3(0, 0, 1))
        # Inner side face down to the floor.
        side = [v3(sx * x_in, y0, cz), v3(sx * x_in, y1, cz), v3(sx * x_in, y1, fz), v3(sx * x_in, y0, fz)]
        b.poly("console", side, facing=v3(-sx, 0, 0))
        # End faces.
        for y, fy in ((y0, -1), (y1, 1)):
            ox = wx(y, cz)
            end = [v3(sx * x_in, y, cz), v3(sx * ox, y, cz), v3(sx * wx(y, fz), y, fz), v3(sx * x_in, y, fz)]
            b.poly("console", end, facing=v3(0, fy, 0))
        # Edge trim.
        b.beam("frame", v3(sx * x_in, y0, cz + 0.008), v3(sx * x_in, y1, cz + 0.008), 0.02, 0.016)
        # Switch rows: little boxes and lights.
        for row, ry in enumerate((-0.34, -0.20, -0.06, 0.08)):
            for col in range(3):
                x = sx * (x_in + 0.05 + col * 0.06)
                if abs(x) > wx(ry, cz) - 0.03:
                    continue
                if (row + col) % 3 == 0:
                    b.light(["green", "amber", "red", "blue"][(row + col) % 4], v3(x, ry, cz), v3(0, 0, 1))
                else:
                    b.box("metal", v3(x, ry, cz + 0.012), (v3(1, 0, 0), v3(0, 1, 0), v3(0, 0, 1)),
                          (0.018, 0.012, 0.012))
                    b.box("bezel", v3(x, ry + 0.004, cz + 0.03), (v3(1, 0, 0), v3(0, 1, 0), v3(0, 0, 1)),
                          (0.004, 0.004, 0.008))
        # A recessed grab handle on the outer wall above the console.
        hy0, hy1 = -0.35, 0.05
        hz = cz + 0.13
        hx = wx(-0.15, hz) - 0.05
        b.cyl("metal", v3(sx * hx, hy0, hz), v3(sx * hx, hy1, hz), 0.012, segs=8)
        for y in (hy0, hy1):
            b.beam("metal", v3(sx * hx, y, hz), v3(sx * (wx(y, hz) + 0.01), y, hz), 0.02, 0.02)

    # ---- Seat --------------------------------------------------------------
    seat_z = -0.78
    b.box("seat", v3(0, 0.08, seat_z - 0.05), (v3(1, 0, 0), v3(0, 1, 0), v3(0, 0, 1)), (0.25, 0.23, 0.05))
    b.box("metal", v3(0, 0.08, seat_z - 0.18), (v3(1, 0, 0), v3(0, 1, 0), v3(0, 0, 1)), (0.18, 0.18, 0.08))
    back_n = unit(v3(0, -1, 0.18))  # reclined a little
    back_up = unit(np.cross(v3(1, 0, 0), back_n))
    back_c = v3(0, 0.36, -0.36)
    b.box("seat", back_c, (v3(1, 0, 0), back_up, back_n), (0.24, 0.40, 0.055))
    b.box("metal", back_c + back_n * -0.075, (v3(1, 0, 0), back_up, back_n), (0.20, 0.36, 0.02))
    # Head rest and side bolsters.
    b.box("seat", back_c + back_up * 0.50 + back_n * 0.02, (v3(1, 0, 0), back_up, back_n), (0.14, 0.09, 0.05))
    for sx in (-1, 1):
        b.box("seat", back_c + v3(sx * 0.27, 0, 0) + back_n * 0.03, (v3(1, 0, 0), back_up, back_n),
              (0.04, 0.32, 0.07))
        b.box("seat", v3(sx * 0.27, 0.08, seat_z - 0.02), (v3(1, 0, 0), v3(0, 1, 0), v3(0, 0, 1)),
              (0.04, 0.22, 0.07))
    # Harness anchor struts from the seat back to the roof.
    b.beam("metal", back_c + back_up * 0.42 + v3(0, 0.06, 0), v3(0, Y_BACK - 0.03, 0.38), 0.05, 0.05)

    # ---- Structure ribs and equipment ----------------------------------------
    # Ribs following the hull below the sill (and over the roof behind the
    # front hoop) at a few stations.
    for y in (-0.22, 0.30):
        r = ring_at(y)
        # Under the sill on both sides; behind the pilot also over the roof.
        for sx in (1, -1):
            a = v3(sx * (r[3][0] - 0.02), y, r[3][1])
            c = v3(sx * (r[4][0] - 0.02), y, r[4][1])
            b.beam("frame", a, c, 0.05, 0.045, up=v3(-sx, 0, 0))
        if y > Y_MID:
            pts = [v3(r[i][0], y, r[i][1]) for i in (8, 9, 0, 1, 2, 3)]
            for a, c in zip(pts, pts[1:]):
                b.beam("frame", a, c, 0.05, 0.05, up=v3(0, 1, 0))
    # Pipes along the lower walls.
    for sx in (-1, 1):
        for k, z in enumerate((-0.80, -0.88)):
            ya, yb = -0.40, Y_BACK - 0.02
            xa = sx * (wall_x(ya, z) - 0.035 - 0.02 * k)
            xb = sx * (wall_x(yb, z) - 0.035 - 0.02 * k)
            b.cyl("metal", v3(xa, ya, z), v3(xb, yb, z), 0.016 if k == 0 else 0.011, segs=8)
            # clamps
            for y in (-0.25, 0.1, 0.45):
                t = (y - ya) / (yb - ya)
                x = xa + (xb - xa) * t
                b.box("frame", v3(x, y, z), (v3(1, 0, 0), v3(0, 1, 0), v3(0, 0, 1)), (0.03, 0.012, 0.03))
    # Rear wall equipment: a hatch frame, two boxes, a vent grille.
    yb = Y_BACK - 0.005
    for x0, x1, z0, z1 in ((-0.30, 0.30, -0.30, 0.30),):
        for a, c in ((v3(x0, yb, z0), v3(x1, yb, z0)), (v3(x1, yb, z0), v3(x1, yb, z1)),
                     (v3(x1, yb, z1), v3(x0, yb, z1)), (v3(x0, yb, z1), v3(x0, yb, z0))):
            b.beam("frame", a + v3(0, -0.015, 0), c + v3(0, -0.015, 0), 0.04, 0.03, up=v3(0, 1, 0))
    for sx in (-1, 1):
        b.box("metal", v3(sx * 0.50, yb - 0.08, -0.52), (v3(1, 0, 0), v3(0, 1, 0), v3(0, 0, 1)), (0.12, 0.08, 0.16))
        b.box("console", v3(sx * 0.50, yb - 0.161, -0.48), (v3(1, 0, 0), v3(0, 1, 0), v3(0, 0, 1)), (0.09, 0.002, 0.06))
        b.light("red" if sx < 0 else "green", v3(sx * 0.50, yb - 0.162, -0.40), v3(0, -1, 0))
    for i in range(6):
        z = -0.75 + i * 0.03
        b.box("frame", v3(0, yb - 0.02, z), (v3(1, 0, 0), v3(0, 1, 0), v3(0, 0, 1)), (0.18, 0.02, 0.006))
    # Overhead panel behind the head with a row of lights.
    oc = v3(0, 0.30, 0.37)
    b.box("console", oc, (v3(1, 0, 0), v3(0, 1, 0), v3(0, 0, 1)), (0.17, 0.13, 0.03))
    for i, col in enumerate(["green", "green", "amber", "blue", "white", "red"]):
        b.light(col, v3(-0.10 + i * 0.04, 0.24, 0.339), v3(0, 0, -1), up=v3(0, 1, 0))
    # Roof lamp: a small strip on the roof spine above the head (out of the
    # forward view).
    b.light("white", v3(0, 0.10, 0.385), v3(0, 0, -1), w=0.08, h=0.010, up=v3(0, 1, 0))
    # Side-wall boxes under the side windows.
    for sx in (-1, 1):
        y, z = 0.38, -0.30
        x = wall_x(y, z) - 0.06
        b.box("metal", v3(sx * x, y, z), (v3(1, 0, 0), v3(0, 1, 0), v3(0, 0, 1)), (0.05, 0.12, 0.08))
        b.light("amber", v3(sx * (x - 0.051), y - 0.06, z + 0.03), v3(-sx, 0, 0))

    return b


# ---------------------------------------------------------------- glTF out

def to_gltf(p):
    """Aeron (x right, y back, z up) to glTF (aeron = -gx, -gz, gy)."""
    return np.stack([-p[..., 0], p[..., 2], -p[..., 1]], axis=-1)


def write_gltf(b, out_dir):
    os.makedirs(os.path.join(out_dir, "textures"), exist_ok=True)
    bin_parts = []
    offset = 0

    def add_blob(data):
        nonlocal offset
        pad = (-offset) % 4
        if pad:
            bin_parts.append(b"\0" * pad)
            offset += pad
        start = offset
        bin_parts.append(data)
        offset += len(data)
        return start

    gltf = {
        "asset": {"version": "2.0", "generator": "OpenTIE-VR tools/cockpit_shell/make_cockpit.py"},
        "extensionsUsed": ["AERON_flight_model"],
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [
            {"name": NAME, "children": [1], "extensions": {"AERON_flight_model": {"role": "model"}}},
            {"name": "shell", "mesh": 0,
             "extensions": {"AERON_flight_model": {"role": "component", "meshType": 0}}},
        ],
        "meshes": [{"name": "shell", "primitives": []}],
        "materials": [], "textures": [], "images": [], "samplers": [{"magFilter": 9729, "minFilter": 9987}],
        "accessors": [], "bufferViews": [], "buffers": [],
    }
    stats = {"vertices": 0, "triangles": 0, "primitives": 0}
    for mi, (mname, (fn, metal, rough, emissive, dsided, _tile)) in enumerate(MATERIALS.items()):
        img = fn()
        rel = f"textures/{NAME}_{mname}.png"
        img.save(os.path.join(out_dir, rel))
        gltf["images"].append({"name": mname, "uri": rel})
        gltf["textures"].append({"source": mi, "sampler": 0})
        mat = {"name": mname, "pbrMetallicRoughness": {
            "baseColorTexture": {"index": mi}, "metallicFactor": metal, "roughnessFactor": rough}}
        if emissive > 0:
            mat["emissiveTexture"] = {"index": mi}
            mat["emissiveFactor"] = [emissive] * 3
        if dsided:
            mat["doubleSided"] = True
        gltf["materials"].append(mat)

        pr = b.prims[mname]
        if not pr.idx:
            continue
        pos = to_gltf(np.array(pr.pos)).astype(np.float32)
        nrm = to_gltf(np.array(pr.nrm)).astype(np.float32)
        uv = np.array(pr.uv, dtype=np.float32)
        idx = np.array(pr.idx, dtype=np.uint32).reshape(-1, 3)
        # glTF fronts are counter-clockwise; the frame change mirrors, so
        # make each triangle's winding agree with its stored normal there.
        g = np.cross(pos[idx[:, 1]] - pos[idx[:, 0]], pos[idx[:, 2]] - pos[idx[:, 0]])
        nsum = nrm[idx[:, 0]] + nrm[idx[:, 1]] + nrm[idx[:, 2]]
        flip = np.einsum("ij,ij->i", g, nsum) < 0
        idx[flip] = idx[flip][:, [0, 2, 1]]
        idx = idx.reshape(-1).astype(np.uint16)
        acc = {}
        for key, arr, typ in (("POSITION", pos, "VEC3"), ("NORMAL", nrm, "VEC3"), ("TEXCOORD_0", uv, "VEC2")):
            start = add_blob(arr.tobytes())
            gltf["bufferViews"].append({"buffer": 0, "byteOffset": start, "byteLength": arr.nbytes, "target": 34962})
            a = {"bufferView": len(gltf["bufferViews"]) - 1, "componentType": 5126, "count": len(arr), "type": typ}
            if key == "POSITION":
                a["min"] = pos.min(axis=0).tolist()
                a["max"] = pos.max(axis=0).tolist()
            gltf["accessors"].append(a)
            acc[key] = len(gltf["accessors"]) - 1
        start = add_blob(idx.tobytes())
        gltf["bufferViews"].append({"buffer": 0, "byteOffset": start, "byteLength": idx.nbytes, "target": 34963})
        gltf["accessors"].append({"bufferView": len(gltf["bufferViews"]) - 1, "componentType": 5123,
                                  "count": len(idx), "type": "SCALAR"})
        gltf["meshes"][0]["primitives"].append({"attributes": acc, "indices": len(gltf["accessors"]) - 1,
                                                "material": mi})
        stats["vertices"] += len(pos)
        stats["triangles"] += len(idx) // 3
        stats["primitives"] += 1
    blob = b"".join(bin_parts)
    with open(os.path.join(out_dir, NAME + ".bin"), "wb") as f:
        f.write(blob)
    gltf["buffers"].append({"uri": NAME + ".bin", "byteLength": len(blob)})
    path = os.path.join(out_dir, NAME + ".gltf")
    with open(path, "w") as f:
        json.dump(gltf, f, indent=1)
    stats["materials"] = len(gltf["materials"])
    return path, stats


# ---------------------------------------------------------------- preview

def look_at(eye, target, up=(0, 0, 1)):
    f = unit(np.asarray(target, float) - eye)
    r = unit(np.cross(f, up))
    u = np.cross(r, f)
    return np.stack([r, u, f])


def mannequin():
    """A seated 1.80 m pilot made of boxes (preview only, for scale)."""
    m = Builder()
    I3 = (v3(1, 0, 0), v3(0, 1, 0), v3(0, 0, 1))
    m.box("metal", v3(0, 0.07, 0.02), I3, (0.08, 0.10, 0.12))           # head, eyes at the origin
    m.box("metal", v3(0, 0.16, -0.40), I3, (0.19, 0.11, 0.27))          # torso
    for sx in (-1, 1):
        m.beam("metal", v3(sx * 0.11, 0.10, -0.70), v3(sx * 0.12, -0.42, -0.63), 0.14, 0.14)  # thigh
        m.beam("metal", v3(sx * 0.12, -0.42, -0.63), v3(sx * 0.15, -0.70, -0.98), 0.11, 0.11)  # shin
        m.beam("metal", v3(sx * 0.21, 0.12, -0.20), v3(sx * 0.24, -0.05, -0.48), 0.08, 0.08)  # upper arm
        m.beam("metal", v3(sx * 0.24, -0.05, -0.48), v3(sx * 0.20, -0.36, -0.46), 0.07, 0.07)  # forearm
    return m.prims["metal"]


def render(b, eye, rot, fov_deg, size=(640, 480), cull=True, clip=None, panels=True, pilot=False):
    """Tiny z-buffer rasterizer in the model frame (flat shaded, textured by
    material average colour, panels drawn as light quads)."""
    W, H = size
    img = np.zeros((H, W, 3)) + np.array([8, 9, 14])
    zbuf = np.full((H, W), np.inf)
    f = 0.5 * W / math.tan(math.radians(fov_deg) / 2)
    light = unit(v3(0.3, 0.5, 0.8))
    tris = []
    for mname, pr in b.prims.items():
        if not pr.idx:
            continue
        tex = np.asarray(MATERIALS[mname][0]())
        col = tex.reshape(-1, 3).mean(axis=0)
        P = np.array(pr.pos)
        N = np.array(pr.nrm)
        I = np.array(pr.idx).reshape(-1, 3)
        dsided = MATERIALS[mname][4]
        for t in I:
            tris.append((P[t], N[t].mean(axis=0), col, mname, dsided))
    if pilot:
        pr = mannequin()
        P, N, I = np.array(pr.pos), np.array(pr.nrm), np.array(pr.idx).reshape(-1, 3)
        for t in I:
            tris.append((P[t], N[t].mean(axis=0), np.array([150, 90, 60]) / 3.0, "pilot", False))
    if panels:
        for pid, (c, pitch, yaw, roll, h, aspect) in PANELS.items():
            r, d, n = panel_frame(pitch, yaw, roll)
            hw, hh = h * aspect / 2, h / 2
            c2 = c + n * 0.003
            q = [c2 - r * hw - d * hh, c2 + r * hw - d * hh, c2 + r * hw + d * hh, c2 - r * hw + d * hh]
            colp = np.array([60, 170, 90]) if pid.startswith("sensor") else np.array([170, 120, 50])
            for tri in ((q[0], q[1], q[2]), (q[0], q[2], q[3])):
                tris.append((np.array(tri), n, colp, "panel", True))
    for P, n, col, mname, dsided in tris:
        if clip is not None and not clip(P.mean(axis=0)):
            continue
        cam = (P - eye) @ rot.T
        if np.any(cam[:, 2] < 0.02):
            continue
        sx = W / 2 + f * cam[:, 0] / cam[:, 2]
        sy = H / 2 - f * cam[:, 1] / cam[:, 2]
        area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0])
        nn = unit(n)
        facing_cam = np.dot(nn, eye - P.mean(axis=0)) > 0
        if cull and not facing_cam and not dsided:
            continue
        if abs(area) < 1e-9:
            continue
        shade = 0.25 + 0.75 * max(0.0, np.dot(nn if facing_cam else -nn, light))
        if mname in ("lights", "panel"):
            shade = 1.0
        c = col * shade * (3.0 if mname not in ("lights", "panel") else 1.0)
        x0, x1 = max(int(sx.min()), 0), min(int(sx.max()) + 1, W - 1)
        y0, y1 = max(int(sy.min()), 0), min(int(sy.max()) + 1, H - 1)
        if x0 > x1 or y0 > y1:
            continue
        gx, gy = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0, y1 + 1) + 0.5)
        w0 = (sx[1] - gx) * (sy[2] - gy) - (sx[2] - gx) * (sy[1] - gy)
        w1 = (sx[2] - gx) * (sy[0] - gy) - (sx[0] - gx) * (sy[2] - gy)
        w2 = (sx[0] - gx) * (sy[1] - gy) - (sx[1] - gx) * (sy[0] - gy)
        inside = ((w0 >= 0) & (w1 >= 0) & (w2 >= 0)) | ((w0 <= 0) & (w1 <= 0) & (w2 <= 0))
        if not inside.any():
            continue
        s = w0 + w1 + w2
        s[s == 0] = 1e-9
        invz = (w0 / cam[0, 2] + w1 / cam[1, 2] + w2 / cam[2, 2]) / s
        z = 1.0 / np.maximum(invz, 1e-9)
        sub = zbuf[y0:y1 + 1, x0:x1 + 1]
        m = inside & (z < sub)
        sub[m] = z[m]
        img[y0:y1 + 1, x0:x1 + 1][m] = c
    return Image.fromarray(np.clip(img, 0, 255).astype(np.uint8))


def previews(b, path):
    views = []
    eye = v3(2.6, -2.4, 1.3)
    views.append(("outside, front three-quarter", render(b, eye, look_at(eye, v3(0, -0.2, -0.35)), 40)))
    eye = v3(-1.9, -1.9, 2.2)
    views.append(("cutaway: canopy and roof removed, seated 1.80 m pilot",
                  render(b, eye, look_at(eye, v3(0, -0.15, -0.55)), 45, clip=lambda c: c[2] < 0.12, pilot=True)))
    e0 = v3(0, 0, 0)
    views.append(("pilot eye, forward (90 deg)", render(b, e0, look_at(e0, v3(0, -1, 0)), 90)))
    views.append(("pilot eye, down 35", render(b, e0, look_at(e0, v3(0, -1, -0.70)), 90)))
    views.append(("pilot eye, left 70", render(b, e0, look_at(e0, v3(-0.94, -0.34, -0.1)), 90)))
    eye = v3(-3.2, -0.25, -0.30)
    views.append(("side section (left half removed), pilot for scale",
                  render(b, eye, look_at(eye, v3(0, -0.25, -0.30)), 38, clip=lambda c: c[0] > 0.0, pilot=True)))
    W, H = views[0][1].size
    sheet = Image.new("RGB", (W * 3, (H + 20) * 2), (0, 0, 0))
    from PIL import ImageDraw
    dr = ImageDraw.Draw(sheet)
    for i, (label, im) in enumerate(views):
        x, y = (i % 3) * W, (i // 3) * (H + 20)
        sheet.paste(im, (x, y + 20))
        dr.text((x + 6, y + 4), label, fill=(220, 220, 220))
    sheet.save(path)


def print_spec():
    order = ["sensor_fwd", "sensor_rear", "cmd", "gauges_l", "gauges_r"]
    ids = {"sensor_fwd": "SENSOR_FWD", "sensor_rear": "SENSOR_REAR", "cmd": "CMD",
           "gauges_l": "GAUGES_L", "gauges_r": "GAUGES_R"}
    for pid in order:
        c, pitch, yaw, roll, h, _ = PANELS[pid]
        print(f"\t\t\t[TIE_COCKPIT_PANEL_{ids[pid]}] = {{ true, {{ {c[0]:.3f}f, {c[1]:.3f}f, {c[2]:.3f}f }}, "
              f"{pitch:.1f}f, {yaw:.1f}f, {roll:.1f}f, {h:.2f}f }},")


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True)
    ap.add_argument("--cook")
    ap.add_argument("--glb", default=os.path.join(here, "..", "..", "resources", "cockpit", NAME + ".glb"))
    ap.add_argument("--preview")
    ap.add_argument("--spec", action="store_true")
    a = ap.parse_args()
    b = build()
    path, stats = write_gltf(b, a.out)
    print(f"{path}: {stats['vertices']} vertices, {stats['triangles']} triangles, "
          f"{stats['primitives']} primitives, {stats['materials']} materials")
    if stats["vertices"] >= 65535:
        sys.exit("too many vertices for the runtime's 16-bit indices")
    if a.spec:
        print_spec()
    if a.preview:
        previews(b, a.preview)
        print("preview:", a.preview)
    if a.cook:
        os.makedirs(os.path.dirname(os.path.abspath(a.glb)), exist_ok=True)
        subprocess.run([a.cook, path, "--out", a.glb, "--quality", "uber"], check=True)
        print(f"{a.glb}: {os.path.getsize(a.glb)} bytes")


if __name__ == "__main__":
    main()
