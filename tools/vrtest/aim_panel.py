#!/usr/bin/env python3
"""xrpose values that aim a fake controller at a VR panel pixel.
usage: aim_panel.py console|panel px py hand_x hand_y hand_z"""
import math, sys

def facing(yaw_deg, pitch_deg, d):
    y, p = math.radians(yaw_deg), math.radians(pitch_deg)
    dr = (math.sin(y)*math.cos(p), math.sin(p), -math.cos(y)*math.cos(p))
    pos = tuple(c*d for c in dr)
    z = tuple(-c for c in dr)
    x = (z[2], 0.0, -z[0]); l = math.hypot(x[0], x[2]); x = (x[0]/l, 0.0, x[2]/l)
    yv = (z[1]*x[2]-z[2]*x[1], z[2]*x[0]-z[0]*x[2], z[0]*x[1]-z[1]*x[0])
    return pos, x, yv, z

kind = sys.argv[1]
px, py = float(sys.argv[2]), float(sys.argv[3])
hand = tuple(float(v) for v in sys.argv[4:7])
if kind == 'console':
    pos, ax, ay, az = facing(0, -52, 0.52); W, H, wm, hm = 1536, 512, 0.51, 0.17
else:
    pos, ax, ay, az = facing(80, -12, 0.60); W, H, wm, hm = 1024, 768, 0.40, 0.30
lx = (px / W - 0.5) * wm; ly = (0.5 - py / H) * hm
P = tuple(pos[i] + ax[i]*lx + ay[i]*ly for i in range(3))
d = tuple(P[i] - hand[i] for i in range(3)); n = math.sqrt(sum(c*c for c in d)); d = tuple(c/n for c in d)
pitch = math.degrees(math.asin(d[1]))
cp = math.cos(math.radians(pitch))
yaw = math.degrees(math.atan2(-d[0]/cp, -d[2]/cp))
print(f"{hand[0]:.3f} {hand[1]:.3f} {hand[2]:.3f} {yaw:.2f} {pitch:.2f}")
