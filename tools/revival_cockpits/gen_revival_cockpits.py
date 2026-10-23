#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Based on gen_revival_cockpits.py by SgtBilko76, from OpenXWA-VR
# (https://github.com/SgtBilko76/OpenXWA-VR), GPL-3, by way of OpenXWA's copy
# (the `stage` command, eye offsets and model scale read from Revival's ship
# .def files). Changes for OpenTIE-VR: only the Imperial cockpits (TIE
# Fighter, TIE Interceptor, TIE Bomber, Assault Gunboat), a standard glTF
# frame (OpenTIE-VR's cockpit GLB path), and per-cockpit fixes (FIXES below).
"""Convert X-Wing Revival's TIE cockpits into Aeron flight models for OpenTIE-VR.

X-Wing Revival (https://www.raptor007.com/x-wing-revival/) by Raptor007
(Blair Sherman) ships textured OBJ cockpits, most of them originally made by
the X-Wing Alliance Upgrade team (https://xwaupgrade.com/). OpenTIE-VR bundles
the Imperial ones with Raptor007's permission as its 3D cockpits (VR settings
-> Cockpit: X-Wing Revival; the default when X-Wing Alliance's own cockpits
were not copied over). The models are NOT part of this repository: they are
converted at package time (packaging/quest/tools/package_tie_quest.sh).

  gen_revival_cockpits.py stage <X-Wing Revival zip or folder> <stage dir> [craft ...]
      Copies what the conversion reads (the cockpit OBJ/MTL files, their
      textures, and the cockpit/model_scale lines of the ships' .def files)
      into <stage dir>, laid out like Revival's own folder.

  gen_revival_cockpits.py build <aeron_gltf_cook> <stage dir or Revival folder> <out dir> [craft ...]
      Writes <out dir>/<craft>.glb for every craft (default: all of CRAFTS),
      cooked with aeron_gltf_cook (texture atlases, KTX2). The game reads
      them as app resources cockpit/revival/<craft>.glb
      (src/tie_remaster/flight/xwa_cockpit.c).

Axes: Revival's OBJ space is x right, y up, z forward, model_scale metres per
unit (0.022); the pilot's eye sits at the .def file's `cockpit_vr` (else
`cockpit`) offset in metres. Output vertices are metres with the eye at the
origin in a standard glTF frame (x left, y up, z forward): Aeron's glTF import
((-x, -z, y)) then gives the OPT frame (x right, -y forward, z up) that
OpenTIE-VR's cockpit table uses.

Revival's live screens (special_* materials with .ani textures) become dark
glass: OpenTIE-VR's instrument panels are placed over them instead.
"""
import json
import math
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile

MODEL_SCALE = 0.022
# craft -> (Revival ship .def, fallback OBJ and eye right/up/fwd in metres
# when the .def is missing, texture folder of materials named after their
# texture without an .mtl entry)
CRAFTS = {
    "tiefighter": ("tie-fighter", "tie-fighter_cockpit_vr.obj", (0.0, 0.1, -0.1)),
    "tieinterceptor": ("tie-interceptor", "tie-interceptor_cockpit_vr.obj", (0.0, 0.1, -0.1),
                       "tie-interceptor_cockpit"),
    "tiebomber": ("tie-bomber", "tie-bomber_cockpit_vr.obj", (1.474, 0.1, 1.7152)),
    "gunboat": ("gunboat", "gunboat_cockpit.obj", (0.0, 0.36, 0.98)),
}
# Per-cockpit fixes:
#   fill      a low self-lit floor (emissive = base colour x fill, from a
#             half-size copy of each texture), for an enclosed cockpit that
#             the game's lights leave almost black (the TIE Bomber's pod).
#   grey      materials whose texture is replaced by a grey copy of itself
#             at this mean brightness: the gunboat's side ledges are a beige
#             wood-plank texture (Material029, TEX00028) and a brown framed
#             panel (Material048, TEX00047), out of place in the grey cockpit.
FIXES = {
    "tiebomber": {"fill": 0.22},
    "gunboat": {"grey": {"Material029": 0.38, "Material048": 0.30}},
}
# Lit like the game's ships: Revival's own diffuse (Kd, mostly 0.4) and rough
# surfaces; brighter albedo made the white side consoles glow.
BASE_COLOR_MAX = 0.45
# Self-lit parts (lamps, screens, labels) at a moderate glow.
EMISSIVE = 0.35
ROUGHNESS = 0.8


class Source:
    """An X-Wing Revival folder, a stage folder or the Revival zip: files by
    their path below the game folder (Models/..., Textures/..., Ships/...),
    found case-insensitively."""

    def __init__(self, path):
        self.zip = None
        self.files = {}
        if zipfile.is_zipfile(path):
            self.zip = zipfile.ZipFile(path)
            names = [n for n in self.zip.namelist() if not n.endswith("/")]
            prefix = ""
            for n in names:
                if n.lower().endswith("ships/x-wing.def"):
                    prefix = n[: len(n) - len("ships/x-wing.def")]
                    break
            for n in names:
                if n.startswith(prefix):
                    self.files[n[len(prefix):].lower()] = n
        else:
            self.root = path
            for d, _, fs in os.walk(path):
                for f in fs:
                    full = os.path.join(d, f)
                    rel = os.path.relpath(full, path).replace(os.sep, "/")
                    self.files[rel.lower()] = full

    def find(self, rel):
        return self.files.get(rel.replace("\\", "/").lower())

    def exists(self, rel):
        return self.find(rel) is not None

    def read(self, rel):
        key = self.find(rel)
        if key is None:
            raise FileNotFoundError(rel)
        if self.zip:
            return self.zip.read(key)
        with open(key, "rb") as f:
            return f.read()

    def text(self, rel):
        return self.read(rel).decode("latin-1")


def ship_cockpit(src, name):
    """(obj file, eye offset, model scale) from the craft's Revival .def."""
    ship, obj, eye = CRAFTS[name][:3]
    scale = MODEL_SCALE
    rel = f"Ships/{ship}.def"
    if src.exists(rel):
        found = {}
        for line in src.text(rel).splitlines():
            parts = line.split("#", 1)[0].split()
            if len(parts) >= 5 and parts[0] in ("cockpit", "cockpit_vr"):
                found[parts[0]] = (parts[1], tuple(float(v) for v in parts[2:5]))
            elif len(parts) >= 2 and parts[0] == "model_scale":
                scale = float(parts[1])
        if found:
            obj, eye = found.get("cockpit_vr") or found["cockpit"]
    return obj, eye, scale


def parse_mtl(text):
    mats, cur = {}, None
    for line in text.splitlines():
        parts = line.split("#", 1)[0].split()
        if not parts:
            continue
        key, args = parts[0], parts[1:]
        if key == "newmtl":
            cur = mats.setdefault(" ".join(args), {"d": 1.0, "Ke": (0.0, 0.0, 0.0), "Kd": (0.4, 0.4, 0.4)})
        elif cur is None:
            continue
        elif key == "map_Kd":
            cur["map_Kd"] = args[-1]
        elif key == "map_glow":
            cur["map_glow"] = args[-1]
        elif key == "Kd":
            cur["Kd"] = tuple(float(a) for a in args[:3])
        elif key == "d":
            cur["d"] = float(args[0])
        elif key == "Ke":
            cur["Ke"] = tuple(float(a) for a in args[:3])
    return mats


def parse_obj(text):
    verts, uvs, faces, mtllibs = [], [], [], []
    mtl, smooth = None, 0
    for line in text.splitlines():
        parts = line.split("#", 1)[0].split()
        if not parts:
            continue
        key, args = parts[0], parts[1:]
        if key == "v":
            verts.append(tuple(float(a) for a in args[:3]))
        elif key == "vt":
            uvs.append((float(args[0]), float(args[1]) if len(args) > 1 else 0.0))
        elif key == "f":
            corners = []
            for a in args:
                bits = a.split("/")
                vi = int(bits[0])
                vi = vi - 1 if vi > 0 else len(verts) + vi
                ti = None
                if len(bits) > 1 and bits[1]:
                    ti = int(bits[1])
                    ti = ti - 1 if ti > 0 else len(uvs) + ti
                corners.append((vi, ti))
            for i in range(1, len(corners) - 1):
                faces.append((mtl, smooth, (corners[0], corners[i], corners[i + 1])))
        elif key == "usemtl":
            mtl = " ".join(args)
        elif key == "s":
            smooth = 0 if not args or args[0] == "off" else int(args[0])
        elif key == "mtllib":
            mtllibs.append(" ".join(args))
    return verts, uvs, faces, mtllibs


def craft_materials(src, name, obj_text=None):
    """The OBJ's materials as used by the conversion (missing .mtl entries
    named after their texture filled in), and the OBJ's parse."""
    obj = ship_cockpit(src, name)[0]
    parsed = parse_obj(obj_text if obj_text is not None else src.text(f"Models/{obj}"))
    tex_dir = CRAFTS[name][3] if len(CRAFTS[name]) > 3 else None
    mats = {}
    for lib in parsed[3]:
        if src.exists(f"Models/{lib}"):
            mats.update(parse_mtl(src.text(f"Models/{lib}")))
    used = {f[0] for f in parsed[2]}
    for mtl in used:
        named = re.search(r"_(TEX\d+)\.bmp(_Dark)?$", mtl or "", re.IGNORECASE)
        if mtl not in mats and named and tex_dir:
            # tie-interceptor_cockpit_vr names most materials after their
            # texture ("Material014_TEX00013.bmp") without defining them in
            # its .mtl; Revival draws them untextured.
            dark = 0.2 if named.group(2) else 0.4
            mats[mtl] = {"d": 1.0, "Ke": (0.0, 0.0, 0.0), "Kd": (dark, dark, dark),
                         "map_Kd": f"{tex_dir}/{named.group(1)}.BMP"}
    return mats, used, parsed


def texture_path(src, rel):
    for folder in ("Models", "Textures"):
        if src.exists(f"{folder}/{rel}"):
            return f"{folder}/{rel}"
    return None


def stage(src_path, out_dir, names):
    src = Source(src_path)
    copied = 0

    def put(rel, data):
        nonlocal copied
        path = os.path.join(out_dir, *rel.split("/"))
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(data)
        copied += 1

    for name in names:
        ship = CRAFTS[name][0]
        rel = f"Ships/{ship}.def"
        if src.exists(rel):
            # Only what the conversion reads: the eye lines and the scale.
            keep = [line for line in src.text(rel).splitlines()
                    if line.split()[:1] and line.split()[0] in ("cockpit", "cockpit_vr", "model_scale")]
            put(rel, ("\n".join(keep) + "\n").encode("latin-1"))
        obj = ship_cockpit(src, name)[0]
        obj_text = src.text(f"Models/{obj}")
        put(f"Models/{obj}", obj_text.encode("latin-1"))
        mats, used, parsed = craft_materials(src, name, obj_text)
        for lib in parsed[3]:
            if src.exists(f"Models/{lib}"):
                put(f"Models/{lib}", src.read(f"Models/{lib}"))
        for mtl in used:
            m = mats.get(mtl) or {}
            for key in ("map_Kd", "map_glow"):
                rel = m.get(key)
                if not rel or rel.lower().endswith(".ani"):
                    continue
                path = texture_path(src, rel)
                if path:
                    put(path.split("/", 1)[0] + "/" + rel.replace("\\", "/"), src.read(path))
                else:
                    print(f"{name}: missing texture {rel}", file=sys.stderr)
    print(f"staged {copied} files for {', '.join(names)} in {out_dir}")


def sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def norm(a):
    length = math.sqrt(dot(a, a))
    return (a[0] / length, a[1] / length, a[2] / length) if length > 1e-12 else (0.0, 0.0, 1.0)


def to_gltf(p, eye, scale):
    """OBJ (right, up, fwd) units -> game eye metres -> glTF."""
    right = p[0] * scale - eye[0]
    up = p[1] * scale - eye[1]
    fwd = p[2] * scale - eye[2]
    return (-right, up, fwd)


def convert(name, src, work):
    from PIL import Image, ImageChops

    _, eye, scale = ship_cockpit(src, name)
    mats, _, (verts, uvs, faces, _) = craft_materials(src, name)
    gverts = [to_gltf(v, eye, scale) for v in verts]

    # Face normals, oriented toward the pilot (eye = origin): every cockpit
    # surface is seen from the seat. Smooth within OBJ smoothing groups.
    tris = []
    smooth_acc = {}
    for mtl, smooth, corners in faces:
        p = [gverts[c[0]] for c in corners]
        n = cross(sub(p[1], p[0]), sub(p[2], p[0]))
        if dot(n, n) < 1e-18:
            continue
        centroid = ((p[0][0] + p[1][0] + p[2][0]) / 3, (p[0][1] + p[1][1] + p[2][1]) / 3,
                    (p[0][2] + p[1][2] + p[2][2]) / 3)
        if dot(n, centroid) > 0:  # pointing away from the eye: flip
            corners = (corners[0], corners[2], corners[1])
            n = (-n[0], -n[1], -n[2])
        n = norm(n)
        tris.append((mtl, smooth, corners, n))
        if smooth:
            for c in corners:
                acc = smooth_acc.setdefault((smooth, c[0]), [0.0, 0.0, 0.0])
                acc[0] += n[0]
                acc[1] += n[1]
                acc[2] += n[2]

    # Per material: unique (position, uv, normal) vertices.
    prims = {}
    for mtl, smooth, corners, n in tris:
        prim = prims.setdefault(mtl, {"map": {}, "pos": [], "nrm": [], "uv": [], "idx": []})
        for c in corners:
            vn = n
            if smooth:
                acc = smooth_acc[(smooth, c[0])]
                vn = norm(acc) if dot(acc, n) > 0.5 * math.sqrt(dot(acc, acc)) else n
            uv = uvs[c[1]] if c[1] is not None and c[1] < len(uvs) else (0.0, 0.0)
            key = (c[0], round(uv[0], 6), round(uv[1], 6), round(vn[0], 4), round(vn[1], 4), round(vn[2], 4))
            index = prim["map"].get(key)
            if index is None:
                index = len(prim["pos"])
                prim["map"][key] = index
                prim["pos"].append(gverts[c[0]])
                prim["nrm"].append(vn)
                prim["uv"].append((uv[0], 1.0 - uv[1]))  # glTF v runs down the image
            prim["idx"].append(index)

    images, image_index = [], {}

    def load(rel):
        path = texture_path(src, rel)
        if not path:
            print(f"{name}: missing texture {rel}", file=sys.stderr)
            return None
        from io import BytesIO
        return Image.open(BytesIO(src.read(path)))

    def emissive(kd_rel, glow_rel):
        """Revival's map_glow is a self-lit mask: emission = diffuse * mask."""
        key = ("emissive", kd_rel, glow_rel)
        if key not in image_index:
            kd, mask = load(kd_rel), load(glow_rel)
            if kd is None or mask is None:
                return None
            kd = kd.convert("RGB")
            mask = mask.convert("RGB").resize(kd.size)
            png = f"{name}_{len(images):03d}_e.png"
            ImageChops.multiply(kd, mask).save(os.path.join(work, png))
            image_index[key] = len(images)
            images.append({"uri": png})
        return image_index[key]

    fixes = FIXES.get(name, {})

    def grey(rel, level):
        """A neutral grey copy of a texture, its mean brightness at `level`."""
        key = ("grey", rel)
        if key not in image_index:
            img = load(rel)
            if img is None:
                return None
            img = img.convert("L")
            mean = sum(i * n for i, n in enumerate(img.histogram())) / max(1, img.width * img.height)
            gain = level * 255.0 / max(1.0, mean)
            png = f"{name}_{len(images):03d}_g.png"
            img.point(lambda v: min(255, int(v * gain + 0.5))).convert("RGB").save(os.path.join(work, png))
            image_index[key] = len(images)
            images.append({"uri": png})
        return image_index[key]

    def fill(rel):
        """Half-size copy of a texture for the self-lit fill (FIXES)."""
        key = ("fill", rel)
        if key not in image_index:
            img = load(rel)
            if img is None:
                return None
            img = img.convert("RGB")
            img = img.resize((max(4, img.width // 2), max(4, img.height // 2)), Image.BILINEAR)
            png = f"{name}_{len(images):03d}_f.png"
            img.save(os.path.join(work, png))
            image_index[key] = len(images)
            images.append({"uri": png})
        return image_index[key]

    def image(rel):
        if rel not in image_index:
            img = load(rel)
            if img is None:
                return None
            png = f"{name}_{len(images):03d}.png"
            img.convert("RGB").save(os.path.join(work, png))
            image_index[rel] = len(images)
            images.append({"uri": png})
        return image_index[rel]

    materials, mat_index = [], {}
    bin_data = bytearray()
    buffer_views, accessors, primitives = [], [], []

    def add_view(data, target):
        off = len(bin_data)
        bin_data.extend(data)
        while len(bin_data) % 4:
            bin_data.append(0)
        buffer_views.append({"buffer": 0, "byteOffset": off, "byteLength": len(data), "target": target})
        return len(buffer_views) - 1

    for mtl, prim in prims.items():
        if len(prim["pos"]) > 65535:
            raise SystemExit(f"{name}: material {mtl} has too many vertices")
        if mtl not in mat_index:
            m = mats.get(mtl) or {"d": 1.0, "Ke": (0.0, 0.0, 0.0), "Kd": (0.4, 0.4, 0.4)}
            alpha = m["d"]
            mat = {
                "name": mtl or "default",
                "pbrMetallicRoughness": {"baseColorFactor": [min(k, BASE_COLOR_MAX) for k in m["Kd"]] + [alpha],
                                         "metallicFactor": 0.0, "roughnessFactor": ROUGHNESS},
                "doubleSided": True,
            }
            if alpha < 1.0:
                mat["alphaMode"] = "BLEND"
            screen = m.get("map_Kd", "").lower().endswith(".ani")
            if screen:
                # Revival's live display (health/target/throttle): dark glass
                # here; OpenTIE-VR's instruments are separate panels.
                mat["pbrMetallicRoughness"]["baseColorFactor"] = [0.03, 0.03, 0.035, alpha]
                mat["pbrMetallicRoughness"]["roughnessFactor"] = 0.3
            grey_level = fixes.get("grey", {}).get(mtl)
            if "map_Kd" in m and not screen:
                tex = grey(m["map_Kd"], grey_level) if grey_level else image(m["map_Kd"])
            else:
                tex = None
            if tex is not None:
                mat["pbrMetallicRoughness"]["baseColorTexture"] = {"index": tex}
            glow = emissive(m["map_Kd"], m["map_glow"]) if "map_glow" in m and "map_Kd" in m and not screen else None
            if glow is not None:
                mat["emissiveTexture"] = {"index": glow}
                mat["emissiveFactor"] = [EMISSIVE, EMISSIVE, EMISSIVE]
            elif any(k > 0 for k in m["Ke"]):
                mat["emissiveFactor"] = list(m["Ke"])
            elif tex is not None and fixes.get("fill") and not grey_level:
                lit = fill(m["map_Kd"])
                if lit is not None:
                    f = fixes["fill"]
                    mat["emissiveTexture"] = {"index": lit}
                    mat["emissiveFactor"] = [f * min(k, BASE_COLOR_MAX) / BASE_COLOR_MAX for k in m["Kd"]]
            mat_index[mtl] = len(materials)
            materials.append(mat)
        pos = b"".join(struct.pack("<3f", *p) for p in prim["pos"])
        nrm = b"".join(struct.pack("<3f", *n) for n in prim["nrm"])
        uv = b"".join(struct.pack("<2f", *t) for t in prim["uv"])
        idx = b"".join(struct.pack("<H", i) for i in prim["idx"])
        acc0 = len(accessors)
        accessors.append({"bufferView": add_view(pos, 34962), "componentType": 5126, "count": len(prim["pos"]),
                          "type": "VEC3", "min": [min(p[i] for p in prim["pos"]) for i in range(3)],
                          "max": [max(p[i] for p in prim["pos"]) for i in range(3)]})
        accessors.append({"bufferView": add_view(nrm, 34962), "componentType": 5126, "count": len(prim["nrm"]),
                          "type": "VEC3"})
        accessors.append({"bufferView": add_view(uv, 34962), "componentType": 5126, "count": len(prim["uv"]),
                          "type": "VEC2"})
        accessors.append({"bufferView": add_view(idx, 34963), "componentType": 5123, "count": len(prim["idx"]),
                          "type": "SCALAR"})
        primitives.append({"attributes": {"POSITION": acc0, "NORMAL": acc0 + 1, "TEXCOORD_0": acc0 + 2},
                           "indices": acc0 + 3, "material": mat_index[mtl], "mode": 4})

    textures = [{"source": i} for i in range(len(images))]
    gltf = {
        "asset": {"version": "2.0", "generator": "OpenTIE-VR gen_revival_cockpits.py"},
        "extensionsUsed": ["AERON_flight_model"],
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [
            {"name": f"revival_cockpit_{name}", "children": [1],
             "extensions": {"AERON_flight_model": {"role": "model"}}},
            {"name": "cockpit", "mesh": 0,
             "extensions": {"AERON_flight_model": {"role": "component", "meshType": 0}}},
        ],
        "meshes": [{"name": "cockpit", "primitives": primitives}],
        "materials": materials,
        "images": images,
        "textures": textures,
        "buffers": [{"uri": f"{name}.bin", "byteLength": len(bin_data)}],
        "bufferViews": buffer_views,
        "accessors": accessors,
    }
    gltf_path = os.path.join(work, f"{name}.gltf")
    with open(gltf_path, "w") as f:
        json.dump(gltf, f)
    with open(os.path.join(work, f"{name}.bin"), "wb") as f:
        f.write(bin_data)
    ntri = sum(len(p["idx"]) for p in prims.values()) // 3
    nvert = sum(len(p["pos"]) for p in prims.values())
    return gltf_path, nvert, ntri, len(images)


def build(cook, src_path, out_dir, names):
    src = Source(src_path)
    os.makedirs(out_dir, exist_ok=True)
    work = os.environ.get("REVIVAL_WORK") or tempfile.mkdtemp(prefix="revival_cockpits_")
    os.makedirs(work, exist_ok=True)
    try:
        for name in names:
            gltf, nvert, ntri, nimg = convert(name, src, work)
            glb = os.path.join(out_dir, f"{name}.glb")
            with open(os.path.join(work, name + ".cook.log"), "w") as log:
                subprocess.run([cook, gltf, "--out", glb, "--max-atlas", "2048", "--verbose"], check=True,
                               stdout=log)
            print(f"{name}: {nvert} vertices, {ntri} triangles, {nimg} textures -> {os.path.getsize(glb)} bytes")
    finally:
        if not os.environ.get("REVIVAL_WORK"):
            shutil.rmtree(work, ignore_errors=True)


def crafts(args):
    for name in args:
        if name not in CRAFTS:
            raise SystemExit(f"unknown craft {name} (known: {', '.join(CRAFTS)})")
    return list(args) or list(CRAFTS)


def main():
    args = sys.argv[1:]
    if len(args) >= 3 and args[0] == "stage":
        stage(args[1], args[2], crafts(args[3:]))
    elif len(args) >= 4 and args[0] == "build":
        build(args[1], args[2], args[3], crafts(args[4:]))
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
