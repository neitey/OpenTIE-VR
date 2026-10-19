#!/usr/bin/env python3
"""Give a compiled (binary) AndroidManifest.xml a new package name and label.

usage: manifest_rename.py in.xml out.xml PACKAGE LABEL

For repacking the OpenXWA-VR APK as OpenTIE-VR without aapt2:
- <manifest package="..."> is set to PACKAGE (so both apps can be installed
  side by side; the app's storage follows the package name),
- <application android:label> becomes the plain string LABEL (it was a
  reference to @string/app_name in resources.arsc),
- the <activity> label, if it has one, likewise.
Class names (org.openxwa.quest.*) stay: they are full names, not relative to
the package.
"""
import struct
import sys

sys.path.insert(0, __file__.rsplit('/', 1)[0])
from manifest_add_permission import RES_XML_START_ELEMENT, build_pool, chunks, read_pool  # noqa: E402

TYPE_STRING = 0x03
LABEL_ID = 0x01010001


def main():
    src, dst, package, label = sys.argv[1:5]
    d = bytearray(open(src, 'rb').read())
    _, file_hsz, _ = struct.unpack_from('<HHI', d, 0)
    pool = read_pool(d, file_hsz)
    strings = list(pool['strings'])
    body_start = file_hsz + pool['size']
    typ, hsz, size = struct.unpack_from('<HHI', d, body_start)
    res_ids = struct.unpack_from('<%dI' % ((size - hsz) // 4), d, body_start + hsz) if typ == 0x0180 else ()

    strings.append(package)
    pkg_idx = len(strings) - 1
    strings.append(label)
    label_idx = len(strings) - 1

    for off, t, h, s in chunks(d, body_start):
        if t != RES_XML_START_ELEMENT:
            continue
        ext = off + 16
        _ns, name, attr_start, attr_size, attr_count = struct.unpack_from('<IIHHH', d, ext)
        el = strings[name]
        for i in range(attr_count):
            a = ext + attr_start + i * attr_size
            _ans, aname, _raw, _vsize, _res0, _dtype, _data = struct.unpack_from('<IIIHBBI', d, a)
            an = strings[aname]
            rid = res_ids[aname] if aname < len(res_ids) else 0
            if el == 'manifest' and an == 'package':
                struct.pack_into('<IIIHBBI', d, a, _ans, aname, pkg_idx, 8, 0, TYPE_STRING, pkg_idx)
                print('package ->', package)
            elif el in ('application', 'activity') and rid == LABEL_ID:
                struct.pack_into('<IIIHBBI', d, a, _ans, aname, label_idx, 8, 0, TYPE_STRING, label_idx)
                print(el, 'label ->', label)

    new_pool = build_pool(pool, strings)
    out = bytearray(d[:file_hsz]) + new_pool + d[body_start:]
    struct.pack_into('<I', out, 4, len(out))
    open(dst, 'wb').write(out)


if __name__ == '__main__':
    main()
