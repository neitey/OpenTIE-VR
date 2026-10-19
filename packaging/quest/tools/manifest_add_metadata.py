#!/usr/bin/env python3
"""Add <meta-data> to the <application> of a compiled (binary) AndroidManifest.xml.

usage: manifest_add_metadata.py in.xml out.xml NAME=VALUE [NAME=VALUE ...]

For repacking an existing APK without aapt2 (like manifest_add_permission.py):
each new element is a copy of the application's first <meta-data> (one with a
string android:value) with the name and value replaced, inserted right after
it. VALUE true/false is stored as a boolean, as aapt2 compiles it; anything
else as a string. Names already present are left alone.

e.g. the Meta Quest OS splash screen (assets/vr_splash.png):
    manifest_add_metadata.py in.xml out.xml com.oculus.ossplash=true
"""
import struct
import sys

from manifest_add_permission import RES_XML_END_ELEMENT, RES_XML_START_ELEMENT, build_pool, chunks, read_pool

TYPE_STRING = 0x03
TYPE_INT_BOOLEAN = 0x12


def attrs(d, off, strings):
    """{attribute name: offset of its 20-byte record} of the start element at off."""
    attr_start, attr_size, attr_count = struct.unpack_from('<HHH', d, off + 16 + 8)
    out = {}
    for i in range(attr_count):
        a = off + 16 + attr_start + i * attr_size
        out[strings[struct.unpack_from('<I', d, a + 4)[0]]] = a - off
    return out


def main():
    src, dst = sys.argv[1], sys.argv[2]
    pairs = [a.split('=', 1) for a in sys.argv[3:]]
    d = bytes(open(src, 'rb').read())
    _, file_hsz, _ = struct.unpack_from('<HHI', d, 0)
    pool = read_pool(d, file_hsz)
    strings = list(pool['strings'])
    meta_idx = strings.index('meta-data')
    app_idx = strings.index('application')
    body_start = file_hsz + pool['size']
    body = d[body_start:]

    depth = 0
    app_depth = None
    template = None  # (start offset, start size, end offset, end size)
    present = set()
    for off, typ, hsz, size in chunks(d, body_start):
        if typ == RES_XML_START_ELEMENT:
            depth += 1
            el = struct.unpack_from('<I', d, off + 16 + 4)[0]
            if el == app_idx:
                app_depth = depth
            if el == meta_idx:
                a = attrs(d, off, strings)
                if 'name' in a:
                    present.add(strings[struct.unpack_from('<I', d, off + a['name'] + 8)[0]])
                if (template is None and app_depth is not None and depth == app_depth + 1 and 'value' in a and
                        d[off + a['value'] + 15] == TYPE_STRING):
                    template = [off, size]
        elif typ == RES_XML_END_ELEMENT:
            el = struct.unpack_from('<I', d, off + 16 + 4)[0]
            if template is not None and len(template) == 2 and el == meta_idx:
                template += [off, size]
            if el == app_idx:
                app_depth = None
            depth -= 1
    if template is None or len(template) != 4:
        sys.exit('no <application> <meta-data> with a string value to copy')
    start_off, start_size, end_off, end_size = template
    start_el = d[start_off:start_off + start_size]
    end_el = d[end_off:end_off + end_size]
    a = attrs(d, start_off, strings)

    insert = bytearray()
    for name, value in pairs:
        if name in present:
            print('already present:', name)
            continue
        el = bytearray(start_el)
        strings.append(name)
        struct.pack_into('<I', el, a['name'] + 8, len(strings) - 1)
        struct.pack_into('<I', el, a['name'] + 16, len(strings) - 1)
        v = a['value']
        if value in ('true', 'false'):
            struct.pack_into('<I', el, v + 8, 0xFFFFFFFF)  # no raw string
            struct.pack_into('<HBBI', el, v + 12, 8, 0, TYPE_INT_BOOLEAN, 0xFFFFFFFF if value == 'true' else 0)
        else:
            strings.append(value)
            struct.pack_into('<I', el, v + 8, len(strings) - 1)
            struct.pack_into('<HBBI', el, v + 12, 8, 0, TYPE_STRING, len(strings) - 1)
        insert += el + end_el
        print('added: %s=%s' % (name, value))

    new_pool = build_pool(pool, strings)
    after = end_off + end_size - body_start
    out = bytearray(d[:file_hsz]) + new_pool + body[:after] + bytes(insert) + body[after:]
    struct.pack_into('<I', out, 4, len(out))
    open(dst, 'wb').write(out)


if __name__ == '__main__':
    main()
