#!/usr/bin/env python3
"""Add an exported <provider> to the <application> of a compiled (binary) AndroidManifest.xml.

usage: manifest_add_provider.py in.xml out.xml CLASS AUTHORITY

For repacking an existing APK without aapt2 (like manifest_add_permission.py).
The element gets android:name, android:exported="true" and
android:authorities, written in resource-id order as aapt2 does; the
attribute names must already be in the manifest's resource map (they are:
the <activity> uses name/exported and <queries> uses authorities). It is
inserted just before </application>. Does nothing if CLASS is present.

e.g. manifest_add_provider.py in.xml out.xml org.openxwa.quest.SetupProvider org.openxwa.quest.setup
"""
import struct
import sys

from manifest_add_permission import RES_XML_END_ELEMENT, RES_XML_START_ELEMENT, build_pool, chunks, read_pool

TYPE_STRING = 0x03
TYPE_INT_BOOLEAN = 0x12
NO_INDEX = 0xFFFFFFFF
ANDROID_NS = 'http://schemas.android.com/apk/res/android'
ATTR_IDS = {'name': 0x01010003, 'exported': 0x01010010, 'authorities': 0x01010018}


def main():
    src, dst, cls, authority = sys.argv[1:5]
    d = bytes(open(src, 'rb').read())
    _, file_hsz, _ = struct.unpack_from('<HHI', d, 0)
    pool = read_pool(d, file_hsz)
    strings = list(pool['strings'])
    if cls in strings:
        print('already present:', cls)
        open(dst, 'wb').write(d)
        return
    body_start = file_hsz + pool['size']
    # Resource map: string index -> attribute resource id.
    typ, hsz, size = struct.unpack_from('<HHI', d, body_start)
    if typ != 0x0180:
        sys.exit('no resource map after the string pool')
    res_ids = struct.unpack_from('<%dI' % ((size - hsz) // 4), d, body_start + hsz)
    attr_idx = {}
    for name, rid in ATTR_IDS.items():
        found = [i for i, r in enumerate(res_ids) if r == rid and strings[i] == name]
        if not found:
            sys.exit('attribute %s is not in the resource map' % name)
        attr_idx[name] = found[0]
    ns = strings.index(ANDROID_NS)
    el_name = strings.index('provider') if 'provider' in strings else None
    app_idx = strings.index('application')

    app_end = None
    for off, t, h, s in chunks(d, body_start):
        if t == RES_XML_END_ELEMENT and struct.unpack_from('<I', d, off + 16 + 4)[0] == app_idx:
            app_end = off
    if app_end is None:
        sys.exit('no </application>')
    line = struct.unpack_from('<I', d, app_end + 8)[0]

    if el_name is None:
        strings.append('provider')
        el_name = len(strings) - 1
    strings.append(cls)
    cls_idx = len(strings) - 1
    strings.append(authority)
    auth_idx = len(strings) - 1

    attrs = [  # sorted by resource id
        (attr_idx['name'], cls_idx, TYPE_STRING, cls_idx),
        (attr_idx['exported'], NO_INDEX, TYPE_INT_BOOLEAN, 0xFFFFFFFF),
        (attr_idx['authorities'], auth_idx, TYPE_STRING, auth_idx),
    ]
    ext = struct.pack('<IIHHHHHH', NO_INDEX, el_name, 0x14, 0x14, len(attrs), 0, 0, 0)
    for name, raw, dtype, data in attrs:
        ext += struct.pack('<IIIHBBI', ns, name, raw, 8, 0, dtype, data)
    start = struct.pack('<HHIII', RES_XML_START_ELEMENT, 0x10, 16 + len(ext), line, NO_INDEX) + ext
    end = struct.pack('<HHIII', RES_XML_END_ELEMENT, 0x10, 24, line, NO_INDEX) + struct.pack('<II', NO_INDEX, el_name)

    new_pool = build_pool(pool, strings)
    rel = app_end - body_start
    body = d[body_start:]
    out = bytearray(d[:file_hsz]) + new_pool + body[:rel] + start + end + body[rel:]
    struct.pack_into('<I', out, 4, len(out))
    open(dst, 'wb').write(out)
    print('added provider %s (%s)' % (cls, authority))


if __name__ == '__main__':
    main()
